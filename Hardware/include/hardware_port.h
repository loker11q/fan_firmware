#ifndef HARDWARE_PORT_H
#define HARDWARE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hardware（硬件层）需要的板级参数。Application 不直接接触 ESP-IDF 类型。 */
typedef struct {
    bool motor_enabled;       /* 是否初始化真正的 PWM 外设。 */
    uint32_t pwm_frequency_hz;/* PWM 频率，单位 Hz。 */
    bool pwm_active_low;      /* PWM 有效电平极性。 */
    uint8_t oled_address;     /* OLED 的 7 位 I²C 地址。 */
    uint8_t oled_width;       /* OLED 水平像素数。 */
    uint8_t oled_height;      /* OLED 垂直像素数。 */
} hw_board_config_t;

/* 初始化外设。0 表示成功，非 0 表示失败。 */
int hw_init(const hw_board_config_t *config);
/* 输出按下状态位图：bit0=KEY1、bit1=KEY2、bit2=KEY3；低有效按键转换成 1。 */
int hw_read_button_mask(uint8_t *pressed_mask);
/* 设置 PWM 占空比，范围 0～100；百分比是逻辑值，极性由驱动转换。 */
int hw_pwm_set_percent(uint8_t duty_percent);
/* 读取校准后的 ADC 引脚电压，单位 mV，结果写入 adc_mv 指向的变量。 */
int hw_battery_adc_mv(uint16_t *adc_mv);
/* 读取并清零本次 FG 脉冲计数；pulse_count 是输出变量地址。 */
int hw_fg_read_and_clear(uint32_t *pulse_count);
void hw_status_led_set(bool on);

/* OLED 不存在时返回 false；调用者可据此跳过绘图。 */
bool hw_display_available(void);
void hw_display_clear(void);
void hw_display_text(uint8_t x, uint8_t y, const char *utf8_text);
void hw_display_pixel(uint8_t x, uint8_t y, bool on);
void hw_display_fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height);
int hw_display_flush(void);

#endif
