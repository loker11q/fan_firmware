#include "app_input.h"
#include "app_status.h"
#include "battery_service.h"
#include "battery_math.h"
#include "fan_core.h"
#include "fan_service.h"
#include "game_engine.h"
#include "hardware_port.h"
#include "network_port.h"
#include "ui_renderer.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define BUTTON_SAMPLE_PERIOD_MS 10U
#define BATTERY_SAMPLE_PERIOD_MS 1000U
#define RPM_SAMPLE_PERIOD_MS 1000U
#define DISPLAY_REFRESH_PERIOD_MS 250U
#define BUTTON_QUEUE_LENGTH 20U
#define APP_EVENT_QUEUE_LENGTH 8U

/* 按键扫描任务通过队列传递这个小结构体；队列会复制结构体内容。 */
typedef struct {
    uint32_t timestamp_ms;
    uint8_t pressed_mask;
} button_sample_t;

static const char *TAG = "fan_app";
/* s_ 开头表示本文件私有的静态状态：生命周期覆盖整个程序运行期。 */
static fan_core_t s_fan;
static app_input_t s_input;
static game_engine_t s_game;
static app_public_status_t s_public_status;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
/* FreeRTOS Queue（队列）句柄；句柄本身指向 RTOS 创建的队列对象。 */
static QueueHandle_t s_button_queue;
static QueueHandle_t s_event_queue;
static uint8_t s_display_page;
static char s_notice[32];
static uint32_t s_notice_until_ms;
static char s_last_error[APP_STATUS_TEXT_LEN];

/* ESP-IDF 计时器返回微秒，这里除以 1000 转为应用使用的毫秒。 */
static uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    /* destination 是写入地址；capacity 必须包含结尾的 '\0' 空间。 */
    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, capacity, "%s", source);
}

static void set_last_error(const char *message)
{
    copy_text(s_last_error, sizeof(s_last_error), message);
}

static void set_notice(const char *message, uint32_t now_ms)
{
    /* 提示信息只保留 1.8 秒，时间到了 OLED 自动恢复正常页面。 */
    copy_text(s_notice, sizeof(s_notice), message);
    s_notice_until_ms = now_ms + 1800U;
}

static const char *active_notice(uint32_t now_ms)
{
    return s_notice[0] != '\0' && (int32_t)(s_notice_until_ms - now_ms) > 0 ?
           s_notice : NULL;
}

static void publish_status(void)
{
    /* 先在本任务的局部变量里组装完整快照，最后一次性发布给其他任务。 */
    app_public_status_t next = { 0 };
    bool wifi_connected = false;
    char ip[APP_STATUS_IP_LEN] = { 0 };
    const esp_app_desc_t *description = esp_app_get_description();

    next.fan_configured = fan_core_is_configured(&s_fan);
    next.fan_running = s_fan.running;
    next.speed_level = s_fan.speed_level;
    next.duty_percent = fan_core_target_duty(&s_fan);
    next.rpm_valid = s_fan.rpm_valid;
    next.rpm = s_fan.rpm;
    next.battery_valid = s_fan.battery_valid;
    next.battery_mv = s_fan.battery_mv;
    next.low_voltage_warning = s_fan.low_voltage_warning;
    copy_text(next.firmware_version, sizeof(next.firmware_version),
              description != NULL ? description->version : "unknown");
    copy_text(next.last_error, sizeof(next.last_error), s_last_error);
    char ota_error[APP_STATUS_TEXT_LEN] = { 0 };
    network_port_get_ota_state(next.ota_state, sizeof(next.ota_state),
                               ota_error, sizeof(ota_error));
    if (ota_error[0] != '\0') {
        copy_text(next.last_error, sizeof(next.last_error), ota_error);
    }
    network_port_get_link(&wifi_connected, ip, sizeof(ip));
    next.wifi_connected = wifi_connected;
    copy_text(next.ip, sizeof(next.ip), ip);

    /* 临界区只保护结构体复制；尽量短，避免其他任务长时间等待。 */
    portENTER_CRITICAL(&s_status_mux);
    s_public_status = next;
    portEXIT_CRITICAL(&s_status_mux);
}

static void status_provider(app_public_status_t *out, void *context)
{
    (void)context;
    if (out == NULL) {
        return;
    }
    /* 网络层给出输出指针；这里复制快照，不把内部全局变量地址交出去。 */
    portENTER_CRITICAL(&s_status_mux);
    *out = s_public_status;
    portEXIT_CRITICAL(&s_status_mux);
}

static void refresh_display(uint32_t now_ms)
{
    /* 根据输入模式决定画主页面、游戏菜单还是游戏画面。 */
    if (s_input.mode == APP_INPUT_MODE_GAME_MENU) {
        ui_renderer_draw_game_menu(&s_game);
    } else if (s_input.mode == APP_INPUT_MODE_GAME) {
        ui_renderer_draw_game(&s_game);
    } else {
        /* OLED 渲染期间使用本地副本，避免边画边被其他任务改字段。 */
        app_public_status_t snapshot;
        portENTER_CRITICAL(&s_status_mux);
        snapshot = s_public_status;
        portEXIT_CRITICAL(&s_status_mux);
        ui_renderer_draw_main(&snapshot, s_display_page, active_notice(now_ms));
    }
}

static void apply_fan_output(void)
{
    /* 所有 PWM 更新都走 Service；硬件错误时立即停止并再次请求 0%。 */
    if (fan_service_apply_pwm(&s_fan) != 0) {
        set_last_error("PWM output failed");
        (void)fan_core_set_running(&s_fan, false);
        (void)fan_service_set_output_safe();
    }
}

static void return_to_main(uint32_t now_ms)
{
    s_game.phase = GAME_PHASE_MENU;
    app_input_set_mode(&s_input, APP_INPUT_MODE_MAIN);
    set_notice("已退出游戏", now_ms);
}

static void handle_app_event(const app_event_t *event)
{
    if (event == NULL) {
        return;
    }
    /* 事件由 app_input 状态机生成；本函数集中把事件变成业务动作。 */
    const uint32_t now_ms = event->timestamp_ms;
    switch (event->type) {
    case APP_EVENT_SPEED_UP:
        fan_core_adjust_level(&s_fan, 1);
        apply_fan_output();
        break;
    case APP_EVENT_SPEED_DOWN:
        fan_core_adjust_level(&s_fan, -1);
        apply_fan_output();
        break;
    case APP_EVENT_PAGE_TOGGLE:
        s_display_page ^= 1U;
        s_notice[0] = '\0';
        break;
    case APP_EVENT_FAN_TOGGLE:
        if (!s_fan.running) {
            char ota_state[APP_STATUS_TEXT_LEN] = { 0 };
            network_port_get_ota_state(ota_state, sizeof(ota_state), NULL, 0U);
            if (strcmp(ota_state, "STARTING") == 0 ||
                strcmp(ota_state, "TIME_SYNC") == 0 ||
                strcmp(ota_state, "DOWNLOADING") == 0 ||
                strcmp(ota_state, "REBOOTING") == 0) {
                set_notice("OTA中暂不可启动", now_ms);
                break;
            }
        }
        if (!fan_core_set_running(&s_fan, !s_fan.running)) {
            set_notice("电机参数未配置", now_ms);
        } else {
            s_notice[0] = '\0';
        }
        apply_fan_output();
        break;
    case APP_EVENT_GAME_ENTER:
        if (s_fan.running) {
            set_notice("请先停止风扇", now_ms);
        } else {
            s_game.phase = GAME_PHASE_MENU;
            app_input_set_mode(&s_input, APP_INPUT_MODE_GAME_MENU);
            s_notice[0] = '\0';
        }
        break;
    case APP_EVENT_GAME_MENU_PREV:
        game_engine_menu_move(&s_game, -1);
        break;
    case APP_EVENT_GAME_MENU_NEXT:
        game_engine_menu_move(&s_game, 1);
        break;
    case APP_EVENT_GAME_SELECT:
        game_engine_start(&s_game, now_ms);
        app_input_set_mode(&s_input, APP_INPUT_MODE_GAME);
        break;
    case APP_EVENT_GAME_LEFT:
        game_engine_input(&s_game, GAME_INPUT_LEFT, now_ms);
        break;
    case APP_EVENT_GAME_RIGHT:
        game_engine_input(&s_game, GAME_INPUT_RIGHT, now_ms);
        break;
    case APP_EVENT_GAME_ACTION:
        game_engine_input(&s_game, GAME_INPUT_ACTION, now_ms);
        break;
    case APP_EVENT_GAME_BACK:
        s_game.phase = GAME_PHASE_MENU;
        app_input_set_mode(&s_input, APP_INPUT_MODE_GAME_MENU);
        break;
    case APP_EVENT_GAME_EXIT:
        return_to_main(now_ms);
        break;
    case APP_EVENT_NONE:
    default:
        break;
    }
}

static void process_button_sample(const button_sample_t *sample)
{
    /* 输入状态机读取样本并生成事件；数组在栈上，处理完本次调用即失效。 */
    app_event_t generated[APP_INPUT_MAX_EVENTS];
    const size_t count = app_input_update(&s_input, sample->pressed_mask,
                                          sample->timestamp_ms, generated);
    for (size_t i = 0U; i < count; ++i) {
        if (xQueueSend(s_event_queue, &generated[i], 0U) != pdTRUE) {
            set_last_error("button event queue full");
        }
    }
}

static void drain_app_events(void)
{
    /* 零等待地取出当前队列中已有事件，直到队列暂时为空。 */
    app_event_t event;
    while (xQueueReceive(s_event_queue, &event, 0U) == pdTRUE) {
        handle_app_event(&event);
    }
}

static void button_scan_task(void *argument)
{
    (void)argument;
    /* 独立的 RTOS 任务每 10 ms 读按键；不在此任务修改风扇业务状态。 */
    TickType_t wake_tick = xTaskGetTickCount();
    for (;;) {
        button_sample_t sample = { 0 };
        sample.timestamp_ms = app_now_ms();
        if (hw_read_button_mask(&sample.pressed_mask) == 0) {
            /* xQueueSend 按 sizeof(button_sample_t) 复制 sample 的内容。 */
            if (xQueueSend(s_button_queue, &sample,
                           pdMS_TO_TICKS(BUTTON_SAMPLE_PERIOD_MS)) != pdTRUE) {
                ESP_LOGW(TAG, "button sample queue full; sample dropped");
            }
        }
        vTaskDelayUntil(&wake_tick, pdMS_TO_TICKS(BUTTON_SAMPLE_PERIOD_MS));
    }
}

static void app_task(void *argument)
{
    (void)argument;
    /* app_task 是业务状态的主要所有者，避免多个任务同时改 s_fan/s_game。 */
    const uint32_t now_at_start = app_now_ms();
    uint32_t last_battery_ms = now_at_start;
    uint32_t last_rpm_ms = now_at_start;
    uint32_t last_render_ms = 0U;
    /* 把当前任务注册到任务看门狗；循环末尾必须定期 reset。 */
    (void)esp_task_wdt_add(NULL);

    for (;;) {
        button_sample_t sample;
        if (xQueueReceive(s_button_queue, &sample,
                          pdMS_TO_TICKS(BUTTON_SAMPLE_PERIOD_MS)) == pdTRUE) {
            process_button_sample(&sample);
            while (xQueueReceive(s_button_queue, &sample, 0U) == pdTRUE) {
                process_button_sample(&sample);
            }
            drain_app_events();
        }

        /* 下面按时间差调度采样和绘图；没有用长延时阻塞游戏/按键处理。 */
        const uint32_t now_ms = app_now_ms();
        if ((uint32_t)(now_ms - last_battery_ms) >= BATTERY_SAMPLE_PERIOD_MS) {
            last_battery_ms = now_ms;
            (void)battery_service_sample(&s_fan);
            apply_fan_output();
        }
        if ((uint32_t)(now_ms - last_rpm_ms) >= RPM_SAMPLE_PERIOD_MS) {
            last_rpm_ms = now_ms;
            uint32_t pulse_count = 0U;
            uint32_t rpm = 0U;
            /* 先取 1 秒脉冲数，再根据 PPR 换算 RPM；PPR=0 时结果无效。 */
            const bool have_count = hw_fg_read_and_clear(&pulse_count) == 0;
            const bool valid = have_count && battery_math_pulses_to_rpm(
                pulse_count, RPM_SAMPLE_PERIOD_MS,
                s_fan.config.pulses_per_revolution, &rpm);
            fan_core_update_rpm(&s_fan, valid, rpm);
        }

        game_engine_tick(&s_game, now_ms);
        publish_status();
        if ((uint32_t)(now_ms - last_render_ms) >= DISPLAY_REFRESH_PERIOD_MS) {
            last_render_ms = now_ms;
            refresh_display(now_ms);
        }
        /* 告诉任务看门狗：本轮主循环正常完成。 */
        (void)esp_task_wdt_reset();
    }
}

static bool app_startup_selftest(bool hardware_initialized)
{
    /* OTA 首次启动自检只检查必要模块、按键可读和 PWM 安全零输出。 */
    if (!hardware_initialized || s_button_queue == NULL || s_event_queue == NULL ||
        hw_pwm_set_percent(0U) != 0 || fan_core_target_duty(&s_fan) != 0U) {
        return false;
    }
    uint8_t button_mask = 0U;
    return hw_read_button_mask(&button_mask) == 0;
}

static void check_ota_first_boot(bool selftest_ok)
{
    /* 只有新槽处于 PENDING_VERIFY（待验证）时，才确认或回滚它。 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }

#if CONFIG_FAN_SELFTEST_FORCE_FAIL
    selftest_ok = false;
#endif

    if (selftest_ok) {
        const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
        if (result == ESP_OK) {
            network_port_set_ota_state("CONFIRMED", "");
            ESP_LOGI(TAG, "OTA first-boot self-test passed; image confirmed");
        } else {
            ESP_LOGE(TAG, "could not confirm OTA image: %s", esp_err_to_name(result));
            esp_restart();
        }
    } else {
        ESP_LOGE(TAG, "OTA first-boot self-test failed; rolling back");
        network_port_set_ota_state("ROLLBACK", "self-test failed");
        const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "rollback request returned: %s", esp_err_to_name(result));
        esp_restart();
    }
}

void app_main(void)
{
    /* ESP-IDF 启动入口。顺序：NVS -> 配置/状态 -> 硬件 -> 队列/任务 -> 网络。 */
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS（非易失性存储）版本不兼容时，擦除仅针对 NVS 分区后重试。 */
        ESP_LOGW(TAG, "NVS format/version issue; erasing NVS and retrying");
        if (nvs_flash_erase() == ESP_OK) {
            nvs_result = nvs_flash_init();
        }
    }
    if (nvs_result != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(nvs_result));
        set_last_error("NVS init failed");
    }

    /* CONFIG_ 项由 menuconfig 生成；默认保持电机 PWM 禁止。 */
    /* 关闭的 Kconfig 布尔项不会出现在 sdkconfig.h，所以先按“未勾选”初始化。 */
    bool motor_enabled = false;
    bool pwm_active_low = false;
#ifdef CONFIG_FAN_MOTOR_ENABLE
    motor_enabled = true;
#endif
#ifdef CONFIG_FAN_PWM_ACTIVE_LOW
    pwm_active_low = true;
#endif
    const fan_config_t fan_config = {
        .motor_enabled = motor_enabled,
        .pwm_frequency_hz = (uint32_t)CONFIG_FAN_PWM_FREQ_HZ,
        .max_duty_percent = (uint8_t)CONFIG_FAN_PWM_MAX_DUTY_PERCENT,
        .pwm_active_low = pwm_active_low,
        .pulses_per_revolution = (uint16_t)CONFIG_FAN_FG_PULSES_PER_REV,
        .battery_warn_mv = (uint16_t)CONFIG_FAN_BATTERY_WARN_MV,
        .battery_stop_mv = (uint16_t)CONFIG_FAN_BATTERY_STOP_MV,
    };
    const hw_board_config_t board_config = {
        .motor_enabled = fan_config.motor_enabled,
        .pwm_frequency_hz = fan_config.pwm_frequency_hz,
        .pwm_active_low = fan_config.pwm_active_low,
        .oled_address = (uint8_t)CONFIG_FAN_OLED_ADDRESS,
        .oled_width = (uint8_t)CONFIG_FAN_OLED_WIDTH,
        .oled_height = (uint8_t)CONFIG_FAN_OLED_HEIGHT,
    };
    /* 先初始化纯 C 状态，再初始化硬件，启动后第一件事是请求安全 PWM。 */
    fan_core_init(&s_fan, &fan_config);
    app_input_init(&s_input);
    game_engine_init(&s_game, esp_random());
    const int hardware_result = hw_init(&board_config);
    (void)fan_service_set_output_safe();
    if (hardware_result != 0) {
        set_last_error("hardware init failed");
        ESP_LOGE(TAG, "hardware initialization failed; PWM remains off");
    }

    /* 队列按“一个元素的字节数”创建；读写双方必须使用相同结构体大小。 */
    s_button_queue = xQueueCreate(BUTTON_QUEUE_LENGTH, sizeof(button_sample_t));
    s_event_queue = xQueueCreate(APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    publish_status();

    const bool queue_ready = s_button_queue != NULL && s_event_queue != NULL;
    if (!queue_ready) {
        set_last_error("queue create failed");
        (void)fan_service_set_output_safe();
        ESP_LOGE(TAG, "unable to create application queues");
        check_ota_first_boot(false);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    /* scan 任务采样按键；app 任务消费样本并运行业务逻辑。 */
    const BaseType_t scan_task_created = xTaskCreate(
        button_scan_task, "button_scan", 3072U, NULL, 4U, NULL);
    const BaseType_t app_task_created = xTaskCreate(
        app_task, "fan_app", 6144U, NULL, 5U, NULL);
    const bool tasks_ready = scan_task_created == pdPASS && app_task_created == pdPASS;
    if (!tasks_ready) {
        set_last_error("task create failed");
        (void)fan_service_set_output_safe();
        ESP_LOGE(TAG, "unable to create application tasks");
    }

    /* 新 OTA 固件确认前先做自检；普通首次烧录不处于待验证状态，会直接跳过。 */
    const bool startup_ok = tasks_ready && nvs_result == ESP_OK &&
        app_startup_selftest(hardware_result == 0);
    check_ota_first_boot(startup_ok);

    if (network_port_start(status_provider, NULL) != 0) {
        set_last_error("network init failed");
    }
    if (network_port_register_ota_command() != 0) {
        set_last_error("UART console init failed");
    }
    publish_status();
    ESP_LOGI(TAG, "fan firmware started; motor output defaults to zero");
}
