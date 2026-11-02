#ifndef BATTERY_MATH_H
#define BATTERY_MATH_H

#include <stdbool.h>
#include <stdint.h>

/* 电池测量换算函数：只做数学计算，不调用 ADC 外设。
 * V2.4 分压电阻为上臂 200 kΩ、下臂 47 kΩ。
 */
/* 输入 ADC 引脚电压（mV），返回分压前的系统电池总电压（mV）。 */
uint16_t battery_math_adc_to_system_mv(uint16_t adc_mv);
/* 按脉冲数、采样时间和每转脉冲数计算 RPM。
 * rpm_out 是调用方提供的输出变量地址；参数无效时返回 false。
 */
bool battery_math_pulses_to_rpm(uint32_t pulse_count,
                                uint32_t interval_ms,
                                uint16_t pulses_per_revolution,
                                uint32_t *rpm_out);

#endif
