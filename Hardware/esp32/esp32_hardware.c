#include "hardware_port.h"
#include "ssd1306.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"

/* 当前板级 GPIO 映射（GPIO 编号不是模组排针脚号）。
 * KEY4/GPIO0 和 KEY5/EN 留给 BOOT/RESET，不作为业务按键。
 */
#define FAN_PWM_GPIO GPIO_NUM_4
#define FAN_FG_GPIO GPIO_NUM_35
#define BAT_ADC_CHANNEL ADC_CHANNEL_6
#define KEY_UP_GPIO GPIO_NUM_32
#define KEY_DOWN_GPIO GPIO_NUM_33
#define KEY_RUN_GPIO GPIO_NUM_23
#define STATUS_LED_GPIO GPIO_NUM_2
#define OLED_SDA_GPIO GPIO_NUM_21
#define OLED_SCL_GPIO GPIO_NUM_22

static const char *TAG = "hardware";
/* 这些句柄在初始化后长期有效，供本文件的硬件接口函数复用。 */
static hw_board_config_t s_config;
static bool s_pwm_ready;
static bool s_display_ready;
static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t s_adc_cali;
static i2c_master_bus_handle_t s_i2c_bus;
static pcnt_unit_handle_t s_pcnt_unit;

static int configure_gpio_output(gpio_num_t gpio, int initial_level)
{
    /* 配置输出脚时同时设初始电平，先把 PWM 拉到非有效态。 */
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << (unsigned)gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        return -1;
    }
    return gpio_set_level(gpio, initial_level) == ESP_OK ? 0 : -1;
}

static int configure_buttons(void)
{
    /* 按键外部上拉、按下接地，所以 GPIO 配为输入并启用内部上拉。 */
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << KEY_UP_GPIO) |
                        (1ULL << KEY_DOWN_GPIO) |
                        (1ULL << KEY_RUN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config) == ESP_OK ? 0 : -1;
}

static int configure_pwm(void)
{
    /* active-low 时高电平才是不动作状态；先设置安全电平再初始化 LEDC。 */
    const int inactive_level = s_config.pwm_active_low ? 1 : 0;
    if (configure_gpio_output(FAN_PWM_GPIO, inactive_level) != 0) {
        return -1;
    }
    if (!s_config.motor_enabled || s_config.pwm_frequency_hz == 0U) {
        /* 电机未使能时保留普通 GPIO 的安全电平，不启动 LEDC PWM。 */
        s_pwm_ready = false;
        return 0;
    }

    /* LEDC 是 ESP32 的 PWM 外设；10 位分辨率对应 0～1023 的计数值。 */
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = s_config.pwm_frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_config) != ESP_OK) {
        return -1;
    }
    const ledc_channel_config_t channel_config = {
        .gpio_num = FAN_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = s_config.pwm_active_low ? 1023U : 0U,
        .hpoint = 0U,
    };
    if (ledc_channel_config(&channel_config) != ESP_OK) {
        return -1;
    }
    s_pwm_ready = true;
    return 0;
}

static int configure_adc(void)
{
    /* ADC1 单次采样；只有校准驱动可用时才向上层报告可信毫伏值。 */
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_config, &s_adc_unit) != ESP_OK) {
        return -1;
    }
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc_unit, BAT_ADC_CHANNEL,
                                   &channel_config) != ESP_OK) {
        return -1;
    }

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&calibration_config,
                                             &s_adc_cali) != ESP_OK) {
        /* 不用未经校准的原始码值冒充电压，读数标成无效更安全。 */
        ESP_LOGW(TAG, "ADC calibration unavailable; battery value will be marked invalid");
        s_adc_cali = NULL;
    }
#else
    s_adc_cali = NULL;
#endif
    return 0;
}

static int configure_fg_counter(void)
{
    /* PCNT（脉冲计数器）累计 FG 边沿；每秒读取后清零一次。 */
    const pcnt_unit_config_t unit_config = {
        .high_limit = 32767,
        .low_limit = -1,
    };
    if (pcnt_new_unit(&unit_config, &s_pcnt_unit) != ESP_OK) {
        return -1;
    }
    const pcnt_chan_config_t channel_config = {
        .edge_gpio_num = FAN_FG_GPIO,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t channel = NULL;
    if (pcnt_new_channel(s_pcnt_unit, &channel_config, &channel) != ESP_OK) {
        return -1;
    }
    if (pcnt_channel_set_edge_action(channel,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
        pcnt_channel_set_level_action(channel,
                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_KEEP) != ESP_OK) {
        return -1;
    }
    const pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000U,
    };
    if (pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_config) != ESP_OK ||
        pcnt_unit_enable(s_pcnt_unit) != ESP_OK ||
        pcnt_unit_clear_count(s_pcnt_unit) != ESP_OK ||
        pcnt_unit_start(s_pcnt_unit) != ESP_OK) {
        return -1;
    }
    return 0;
}

static int configure_display(void)
{
    /* I²C 总线和 OLED 控制器可能缺席；失败时上层仍可继续运行。 */
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7U,
        .flags.enable_internal_pullup = false,
    };
    if (i2c_new_master_bus(&bus_config, &s_i2c_bus) != ESP_OK) {
        return -1;
    }
    /* 先试配置地址，再尝试常见的另一个地址 0x3C/0x3D。 */
    uint8_t address = s_config.oled_address;
    if (ssd1306_attach(s_i2c_bus, address,
                       s_config.oled_width, s_config.oled_height) != 0) {
        address = address == 0x3CU ? 0x3DU : 0x3CU;
        if (ssd1306_attach(s_i2c_bus, address,
                           s_config.oled_width, s_config.oled_height) != 0) {
            ESP_LOGW(TAG, "SSD1306 not found at 0x3C or 0x3D; continuing without OLED");
            return -1;
        }
    }
    ESP_LOGI(TAG, "OLED found at 0x%02X (%u x %u)", address,
             s_config.oled_width, s_config.oled_height);
    return 0;
}

int hw_init(const hw_board_config_t *config)
{
    if (config == NULL) {
        return -1;
    }
    /* 复制配置到本文件的静态存储区，后续驱动回调会读取它。 */
    s_config = *config;
    if (configure_pwm() != 0 ||
        configure_buttons() != 0 ||
        configure_gpio_output(STATUS_LED_GPIO, 0) != 0) {
        ESP_LOGE(TAG, "GPIO/PWM initialization failed");
        return -1;
    }
    if (configure_adc() != 0) {
        ESP_LOGE(TAG, "Battery ADC initialization failed");
    }
    if (configure_fg_counter() != 0) {
        ESP_LOGW(TAG, "FG counter unavailable");
        s_pcnt_unit = NULL;
    }
    /* OLED 初始化失败不返回硬件总初始化失败，其他功能继续工作。 */
    s_display_ready = configure_display() == 0;
    return 0;
}

int hw_read_button_mask(uint8_t *pressed_mask)
{
    if (pressed_mask == NULL) {
        return -1;
    }
    const int up_level = gpio_get_level(KEY_UP_GPIO);
    const int down_level = gpio_get_level(KEY_DOWN_GPIO);
    const int run_level = gpio_get_level(KEY_RUN_GPIO);
    if (up_level < 0 || down_level < 0 || run_level < 0) {
        return -1;
    }
    /* 低电平代表按下；把三个 GPIO 电平打包成一个字节交给按键状态机。 */
    *pressed_mask = (uint8_t)((up_level == 0 ? 0x01U : 0U) |
                              (down_level == 0 ? 0x02U : 0U) |
                              (run_level == 0 ? 0x04U : 0U));
    return 0;
}

int hw_pwm_set_percent(uint8_t duty_percent)
{
    if (duty_percent > 100U) {
        return -1;
    }
    /* LEDC 未启动时只接受 0%；非零输出会返回错误，防止意外驱动电机。 */
    if (!s_pwm_ready) {
        return duty_percent == 0U ? 0 : -1;
    }
    /* 业务层始终用“有效占空比”；若信号低有效，物理波形要取反。 */
    const uint8_t physical_percent = s_config.pwm_active_low ?
                                     (uint8_t)(100U - duty_percent) : duty_percent;
    const uint32_t duty = ((uint32_t)physical_percent * 1023U + 50U) / 100U;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) != ESP_OK) {
        return -1;
    }
    return 0;
}

int hw_battery_adc_mv(uint16_t *adc_mv)
{
    if (adc_mv == NULL || s_adc_unit == NULL || s_adc_cali == NULL) {
        return -1;
    }
    /* raw 是 ADC 原始码；经校准函数转换后才成为引脚毫伏值。 */
    int raw = 0;
    int millivolts = 0;
    if (adc_oneshot_read(s_adc_unit, BAT_ADC_CHANNEL, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(s_adc_cali, raw, &millivolts) != ESP_OK ||
        millivolts < 0 || millivolts > UINT16_MAX) {
        return -1;
    }
    *adc_mv = (uint16_t)millivolts;
    return 0;
}

int hw_fg_read_and_clear(uint32_t *pulse_count)
{
    if (pulse_count == NULL || s_pcnt_unit == NULL) {
        return -1;
    }
    /* 读取采样窗口内的计数，然后清零，下一窗口从 0 开始。 */
    int count = 0;
    if (pcnt_unit_get_count(s_pcnt_unit, &count) != ESP_OK || count < 0 ||
        pcnt_unit_clear_count(s_pcnt_unit) != ESP_OK) {
        return -1;
    }
    *pulse_count = (uint32_t)count;
    return 0;
}

void hw_status_led_set(bool on)
{
    (void)gpio_set_level(STATUS_LED_GPIO, on ? 1 : 0);
}

bool hw_display_available(void)
{
    return s_display_ready;
}

void hw_display_clear(void)
{
    if (s_display_ready) ssd1306_clear();
}

void hw_display_text(uint8_t x, uint8_t y, const char *utf8_text)
{
    if (s_display_ready) ssd1306_draw_text(x, y, utf8_text);
}

void hw_display_pixel(uint8_t x, uint8_t y, bool on)
{
    if (s_display_ready) ssd1306_draw_pixel(x, y, on);
}

void hw_display_fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height)
{
    if (s_display_ready) ssd1306_fill_rect(x, y, width, height);
}

int hw_display_flush(void)
{
    if (!s_display_ready) {
        return -1;
    }
    /* 连续传输失败后关闭 OLED 输出，避免反复错误影响其他任务。 */
    const int result = ssd1306_flush();
    if (result != 0) {
        s_display_ready = false;
        ESP_LOGW(TAG, "OLED transfer failed; disabling display output");
    }
    return result;
}
