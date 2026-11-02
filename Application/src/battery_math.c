#include "battery_math.h"

#include <limits.h>

/* 把 ADC 引脚电压恢复成分压前电池电压，参数和返回值都以 mV 为单位。 */
uint16_t battery_math_adc_to_system_mv(uint16_t adc_mv)
{
    /* Vbat = Vadc * (200 + 47) / 47; round to nearest millivolt. */
    /* 先转为 32 位整数再乘，避免 16 位中间结果溢出；+23 是四舍五入补偿。 */
    const uint32_t scaled = (uint32_t)adc_mv * 247U + 23U;
    const uint32_t result = scaled / 47U;
    return result > UINT16_MAX ? UINT16_MAX : (uint16_t)result;
}

bool battery_math_pulses_to_rpm(uint32_t pulse_count,
                                uint32_t interval_ms,
                                uint16_t pulses_per_revolution,
                                uint32_t *rpm_out)
{
    /* 除数为 0 或输出指针为空都不能计算；调用者仍保留原变量。 */
    if (rpm_out == NULL || interval_ms == 0U || pulses_per_revolution == 0U) {
        return false;
    }
    /* RPM = 脉冲数 × 60000 ms/min ÷ (采样毫秒 × 每转脉冲数)。 */
    /* 用 64 位中间变量，避免乘法先在 32 位范围内溢出。 */
    const uint64_t numerator = (uint64_t)pulse_count * 60000ULL;
    const uint64_t denominator = (uint64_t)interval_ms * pulses_per_revolution;
    const uint64_t result = (numerator + denominator / 2ULL) / denominator;
    /* *rpm_out 表示写入指针所指向的变量。超范围时饱和到最大值。 */
    *rpm_out = result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
    return true;
}
