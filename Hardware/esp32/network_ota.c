#include "network_port.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart_vfs.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define WIFI_RETRY_DELAY_US 5000000ULL
#define OTA_TIME_SYNC_TIMEOUT_MS 30000U

/* 网络状态会被 Wi-Fi 回调、HTTP 服务器和 UART 命令同时读写。 */
static const char *TAG = "network";
/* provider 是函数指针：HTTP 请求到来时用它向主应用取一份状态快照。 */
static network_status_provider_t s_status_provider;
static void *s_status_context;
static esp_netif_t *s_station_netif;
static esp_timer_handle_t s_retry_timer;
static httpd_handle_t s_http_server;
static bool s_wifi_initialized;
static bool s_console_initialized;
static bool s_sntp_initialized;
static bool s_connected;
static char s_ip[APP_STATUS_IP_LEN];
static char s_ota_state[APP_STATUS_TEXT_LEN] = "IDLE";
static char s_ota_error[APP_STATUS_TEXT_LEN];
static portMUX_TYPE s_network_mux = portMUX_INITIALIZER_UNLOCKED;

/* 把字符串复制到固定长度缓冲区，保证最后一个字节一定是字符串结束符。 */
static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    size_t index = 0U;
    while (index + 1U < capacity && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

void network_port_set_ota_state(const char *state, const char *error)
{
    /* 用短临界区保护共享字符串；不能在锁内做网络访问或长时间等待。 */
    portENTER_CRITICAL(&s_network_mux);
    copy_text(s_ota_state, sizeof(s_ota_state), state);
    copy_text(s_ota_error, sizeof(s_ota_error), error);
    portEXIT_CRITICAL(&s_network_mux);
}

void network_port_get_ota_state(char *state_out,
                                size_t state_out_size,
                                char *error_out,
                                size_t error_out_size)
{
    /* 先在锁内复制到本地数组，再解锁后复制给外部缓冲区。 */
    char state[APP_STATUS_TEXT_LEN] = { 0 };
    char error[APP_STATUS_TEXT_LEN] = { 0 };
    portENTER_CRITICAL(&s_network_mux);
    copy_text(state, sizeof(state), s_ota_state);
    copy_text(error, sizeof(error), s_ota_error);
    portEXIT_CRITICAL(&s_network_mux);
    copy_text(state_out, state_out_size, state);
    copy_text(error_out, error_out_size, error);
}

void network_port_get_link(bool *connected, char *ip_out, size_t ip_out_size)
{
    bool link = false;
    char ip[APP_STATUS_IP_LEN] = { 0 };
    portENTER_CRITICAL(&s_network_mux);
    link = s_connected;
    copy_text(ip, sizeof(ip), s_ip);
    portEXIT_CRITICAL(&s_network_mux);

    /* 输出指针由调用方提供；为空就跳过该项，避免空指针写入。 */
    if (connected != NULL) {
        *connected = link;
    }
    if (ip_out != NULL && ip_out_size > 0U) {
        copy_text(ip_out, ip_out_size, ip);
    }
}

static void wifi_retry_callback(void *argument)
{
    /* esp_timer 回调：只发起一次重连，不在这里循环等待连接成功。 */
    (void)argument;
    if (s_wifi_initialized) {
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "Wi-Fi retry request failed: %s", esp_err_to_name(result));
        }
    }
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    /* ESP-IDF 把事件编号和事件数据传进来；本回调只更新连接状态。 */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_network_mux);
        s_connected = false;
        s_ip[0] = '\0';
        portEXIT_CRITICAL(&s_network_mux);
        if (s_retry_timer != NULL) {
            (void)esp_timer_stop(s_retry_timer);
            (void)esp_timer_start_once(s_retry_timer, WIFI_RETRY_DELAY_US);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected; retry scheduled");
    }
}

static void ip_event_handler(void *argument,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)argument;
    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP ||
        event_data == NULL) {
        return;
    }

    /* event_data 只在本次回调期间有效，因此马上把 IP 复制为普通字符串。 */
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    char ip[APP_STATUS_IP_LEN] = { 0 };
    (void)snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    portENTER_CRITICAL(&s_network_mux);
    s_connected = true;
    copy_text(s_ip, sizeof(s_ip), ip);
    portEXIT_CRITICAL(&s_network_mux);
    if (s_retry_timer != NULL) {
        (void)esp_timer_stop(s_retry_timer);
    }
    ESP_LOGI(TAG, "Wi-Fi connected; address=%s", ip);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    /* HTTP 任务进入此处理函数时，向主应用取状态副本，不直接访问风扇内部变量。 */
    app_public_status_t status = { 0 };
    if (s_status_provider != NULL) {
        s_status_provider(&status, s_status_context);
    }

    bool connected = false;
    char ip[APP_STATUS_IP_LEN] = { 0 };
    char ota_state[APP_STATUS_TEXT_LEN] = { 0 };
    char ota_error[APP_STATUS_TEXT_LEN] = { 0 };
    network_port_get_link(&connected, ip, sizeof(ip));
    portENTER_CRITICAL(&s_network_mux);
    copy_text(ota_state, sizeof(ota_state), s_ota_state);
    copy_text(ota_error, sizeof(ota_error), s_ota_error);
    portEXIT_CRITICAL(&s_network_mux);

    /* cJSON_CreateObject 在堆上分配 JSON 节点；发送后必须释放。 */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "out of memory");
    }
    cJSON_AddStringToObject(root, "firmware", status.firmware_version);
    cJSON_AddBoolToObject(root, "fan_configured", status.fan_configured);
    cJSON_AddBoolToObject(root, "fan_running", status.fan_running);
    cJSON_AddNumberToObject(root, "speed_level", status.speed_level);
    cJSON_AddNumberToObject(root, "duty_percent", status.duty_percent);
    /* 传感器数据无效时返回 JSON null，而不是伪造 0 RPM/0 mV。 */
    if (status.rpm_valid) {
        cJSON_AddNumberToObject(root, "rpm", status.rpm);
    } else {
        cJSON_AddNullToObject(root, "rpm");
    }
    if (status.battery_valid) {
        cJSON_AddNumberToObject(root, "battery_mv", status.battery_mv);
    } else {
        cJSON_AddNullToObject(root, "battery_mv");
    }
    cJSON_AddBoolToObject(root, "low_voltage_warning",
                          status.low_voltage_warning);
    cJSON_AddBoolToObject(root, "wifi_connected", connected);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ota_state", ota_state);
    cJSON_AddStringToObject(root, "last_error",
                            ota_error[0] != '\0' ? ota_error : status.last_error);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "JSON encoding failed");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    const esp_err_t result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return result;
}

static esp_err_t start_status_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }
    /* 这个服务器只注册一个 GET 状态接口，不注册风扇控制写接口。 */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80U;
    config.stack_size = 4096U;
    esp_err_t result = httpd_start(&s_http_server, &config);
    if (result != ESP_OK) {
        s_http_server = NULL;
        return result;
    }
    const httpd_uri_t status_uri = {
        .uri = "/api/v1/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };
    result = httpd_register_uri_handler(s_http_server, &status_uri);
    if (result != ESP_OK) {
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    return result;
}

static void copy_wifi_field(uint8_t *destination,
                            size_t destination_size,
                            const char *source)
{
    /* ESP-IDF Wi-Fi 配置使用字节数组；这里按目标数组容量截断并补 0。 */
    if (destination == NULL || destination_size == 0U) {
        return;
    }
    const size_t length = source == NULL ? 0U : strlen(source);
    const size_t copied = length < destination_size - 1U ?
                          length : destination_size - 1U;
    if (copied > 0U) {
        memcpy(destination, source, copied);
    }
    destination[copied] = 0U;
}

int network_port_start(network_status_provider_t provider, void *context)
{
    /* 保存回调和上下文指针，稍后收到 HTTP 请求时再调用。 */
    s_status_provider = provider;
    s_status_context = context;
    /* 默认 SSID 为空：不启动 Wi-Fi，风扇、按键和 OLED 仍可独立使用。 */
    if (CONFIG_FAN_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is not configured; network API is disabled");
        return 0;
    }

    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(result));
        return -1;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop initialization failed: %s", esp_err_to_name(result));
        return -1;
    }

    /* STA（Station，终端模式）让 ESP32 连接路由器，而不是创建热点。 */
    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == NULL) {
        ESP_LOGE(TAG, "could not create Wi-Fi station interface");
        return -1;
    }
    const wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_init);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(result));
        return -1;
    }
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL);
    if (result == ESP_OK) {
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            ip_event_handler, NULL);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi event registration failed: %s", esp_err_to_name(result));
        return -1;
    }

    /* 断线后用软件定时器延迟重连，避免在事件回调里不停循环。 */
    const esp_timer_create_args_t retry_timer_args = {
        .callback = wifi_retry_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    result = esp_timer_create(&retry_timer_args, &s_retry_timer);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi retry timer creation failed: %s", esp_err_to_name(result));
        return -1;
    }

    /* 配置结构先清零，再复制凭据；超长凭据直接报错，不静默截断。 */
    wifi_config_t wifi_config = { 0 };
    if (strlen(CONFIG_FAN_WIFI_SSID) >= sizeof(wifi_config.sta.ssid) ||
        strlen(CONFIG_FAN_WIFI_PASSWORD) >= sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "Wi-Fi SSID/password is longer than the ESP32 field limit");
        return -1;
    }
    copy_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid),
                    CONFIG_FAN_WIFI_SSID);
    copy_wifi_field(wifi_config.sta.password, sizeof(wifi_config.sta.password),
                    CONFIG_FAN_WIFI_PASSWORD);
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(result));
        return -1;
    }
    s_wifi_initialized = true;

    result = start_status_server();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "status HTTP server start failed: %s", esp_err_to_name(result));
        return -1;
    }
    ESP_LOGI(TAG, "status API ready at GET /api/v1/status");
    return 0;
}

static esp_err_t synchronize_clock(void)
{
    /* HTTPS 证书校验依赖正确时间；设备刚启动时先通过 SNTP 校时。 */
    if (!s_sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        const esp_err_t result = esp_netif_sntp_init(&config);
        if (result != ESP_OK) {
            return result;
        }
        s_sntp_initialized = true;
    }
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(OTA_TIME_SYNC_TIMEOUT_MS));
}

static void ota_worker(void *argument)
{
    (void)argument;
    /* 该函数运行在专用 FreeRTOS 任务里，避免阻塞主业务任务。 */
    network_port_set_ota_state("TIME_SYNC", "");
    const esp_err_t time_result = synchronize_clock();
    if (time_result != ESP_OK) {
        ESP_LOGE(TAG, "system time sync failed: %s", esp_err_to_name(time_result));
        network_port_set_ota_state("FAILED", "time sync failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "starting HTTPS OTA from the configured URL");
    network_port_set_ota_state("DOWNLOADING", "");
    /* HTTPS 客户端启用 ESP-IDF 证书包，校验服务器证书链。 */
    const esp_http_client_config_t http_config = {
        .url = CONFIG_FAN_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };
    const esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    /* ESP-IDF OTA API 将镜像写入非当前运行的应用槽，并检查镜像。 */
    const esp_err_t result = esp_https_ota(&ota_config);
    if (result == ESP_OK) {
        network_port_set_ota_state("REBOOTING", "");
        ESP_LOGI(TAG, "image verified and selected; restarting");
        vTaskDelay(pdMS_TO_TICKS(100U));
        esp_restart();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGE(TAG, "HTTPS OTA failed: %s; current firmware remains selected",
             esp_err_to_name(result));
    network_port_set_ota_state("FAILED", esp_err_to_name(result));
    vTaskDelete(NULL);
}

static bool ota_is_in_progress(const char *state)
{
    /* 用状态字符串判断是否已有一次升级正在执行，避免重复启动任务。 */
    return strcmp(state, "STARTING") == 0 || strcmp(state, "TIME_SYNC") == 0 ||
           strcmp(state, "DOWNLOADING") == 0 || strcmp(state, "REBOOTING") == 0;
}

static int ota_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* 先检查 URL、HTTPS、Wi-Fi 和风扇状态；都满足才创建下载任务。 */
    if (CONFIG_FAN_OTA_URL[0] == '\0') {
        printf("OTA URL is not configured. Set it in menuconfig first.\n");
        network_port_set_ota_state("NO_URL", "");
        return 1;
    }
    if (strncmp(CONFIG_FAN_OTA_URL, "https://", 8U) != 0) {
        printf("OTA requires an HTTPS URL.\n");
        network_port_set_ota_state("FAILED", "HTTPS URL required");
        return 1;
    }
    bool connected = false;
    network_port_get_link(&connected, NULL, 0U);
    if (!connected) {
        printf("Wi-Fi is not connected; OTA was not started.\n");
        network_port_set_ota_state("OFFLINE", "Wi-Fi disconnected");
        return 1;
    }

    char current_state[APP_STATUS_TEXT_LEN] = { 0 };
    network_port_get_ota_state(current_state, sizeof(current_state), NULL, 0U);
    if (ota_is_in_progress(current_state)) {
        printf("OTA is already in progress.\n");
        return 1;
    }
    network_port_set_ota_state("STARTING", "");
    vTaskDelay(pdMS_TO_TICKS(50U));
    app_public_status_t status = { 0 };
    if (s_status_provider != NULL) {
        s_status_provider(&status, s_status_context);
    }
    if (status.fan_running) {
        printf("Stop the fan before starting OTA.\n");
        network_port_set_ota_state("IDLE", "fan is running");
        return 1;
    }

    /* 任务栈大小单位为字节；耗时下载独立运行，主任务继续喂看门狗。 */
    if (xTaskCreate(ota_worker, "ota_worker", 10240U, NULL, 5U, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not create OTA worker task");
        network_port_set_ota_state("FAILED", "OTA task allocation failed");
        return 1;
    }
    printf("OTA worker started; follow progress in the UART log.\n");
    return 0;
}

int network_port_register_ota_command(void)
{
    if (s_console_initialized) {
        return 0;
    }
    /*
     * 先创建 UART REPL（交互命令行）。ESP-IDF 会在这里初始化 console；
     * 若提前调用 esp_console_init()，REPL 内部会再次初始化，导致失败。
     */
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "fan>";
    esp_console_repl_t *repl = NULL;
    esp_err_t result = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (result != ESP_OK) {
        /*
         * REPL 初始化失败时，ESP-IDF 可能已切换 UART 输出方式并随后删除驱动。
         * 先恢复直接写 UART 的模式，再打印错误，避免日志又调用已删除的驱动。
         */
        uart_vfs_dev_use_nonblocking(uart_config.channel);
        ESP_LOGE(TAG, "UART console creation failed: %s", esp_err_to_name(result));
        return -1;
    }

    /* REPL 初始化完 console 后，再注册 OTA 命令，供用户在串口输入 ota。 */
    const esp_console_cmd_t command = {
        .command = "ota",
        .help = "Download the configured HTTPS firmware image",
        .hint = NULL,
        .func = ota_command,
        .argtable = NULL,
    };
    const esp_err_t register_result = esp_console_cmd_register(&command);
    if (register_result != ESP_OK) {
        ESP_LOGE(TAG, "OTA command registration failed: %s",
                 esp_err_to_name(register_result));
    }

    result = esp_console_start_repl(repl);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "UART console start failed: %s", esp_err_to_name(result));
        return -1;
    }
    s_console_initialized = true;
    if (register_result == ESP_OK) {
        ESP_LOGI(TAG, "UART command ready; type 'ota' to update firmware");
        return 0;
    }
    return -1;
}
