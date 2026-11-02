#ifndef APP_STATUS_H
#define APP_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#define APP_STATUS_VERSION_LEN 24U /* 固件版本字符数组长度，包含结尾的 0。 */
#define APP_STATUS_IP_LEN 16U      /* IPv4 字符串长度，包含结尾的 0。 */
#define APP_STATUS_TEXT_LEN 32U    /* 错误/OTA 描述的字符数组长度。 */

/* 给 OLED 和 HTTP 接口读取的只读快照。
 * 指针不会暴露给调用者；状态通过结构体复制，减少跨任务共享风险。
 */
typedef struct {
    char firmware_version[APP_STATUS_VERSION_LEN]; /* 固件版本字符串。 */
    char ip[APP_STATUS_IP_LEN];                     /* Wi-Fi 获得的 IPv4 地址。 */
    char last_error[APP_STATUS_TEXT_LEN];           /* 最近一次错误；空串表示无错误。 */
    char ota_state[APP_STATUS_TEXT_LEN];            /* OTA 流程状态，例如 IDLE。 */
    bool fan_configured;                            /* 电机参数是否完整有效。 */
    bool fan_running;                               /* 软件风扇运行标志。 */
    uint8_t speed_level;                            /* 当前档位，1～10。 */
    uint8_t duty_percent;                           /* 目标 PWM 百分比。 */
    bool rpm_valid;                                 /* false 时 rpm 字段应忽略。 */
    uint32_t rpm;                                   /* FG 计算出的每分钟转数。 */
    bool battery_valid;                             /* false 时 battery_mv 字段应忽略。 */
    uint16_t battery_mv;                            /* 电池总电压，单位 mV。 */
    bool low_voltage_warning;                       /* 是否触发已配置的电池告警。 */
    bool wifi_connected;                            /* Wi-Fi 是否已连接并取得地址。 */
} app_public_status_t;

#endif
