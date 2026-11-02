#include "fan_core.h"

#include <stddef.h>
#include <string.h>

/* 本文件只判断配置和计算目标状态，不直接写 GPIO/PWM 寄存器。 */
static bool fan_config_valid(const fan_config_t *config)
{
    /* config 是指针；先判空，才能安全读取 config->成员。 */
    return config != NULL && config->motor_enabled &&
           config->pwm_frequency_hz > 0U &&
           config->max_duty_percent > 0U &&
           config->max_duty_percent <= 100U;
}

void fan_core_init(fan_core_t *fan, const fan_config_t *config)
{
    if (fan == NULL) {
        return;
    }
    /* memset 把结构体所有字段清零，保证启动时 running 和 PWM 都是关闭状态。 */
    memset(fan, 0, sizeof(*fan));
    if (config != NULL) {
        /* 这里是结构体内容复制，不保存调用者 config 指针。 */
        fan->config = *config;
    }
    /* 档位从 1 开始；尚未启动时目标占空比仍然是 0。 */
    fan->speed_level = 1U;
}

bool fan_core_is_configured(const fan_core_t *fan)
{
    return fan != NULL && fan_config_valid(&fan->config);
}

bool fan_core_set_running(fan_core_t *fan, bool running)
{
    if (fan == NULL) {
        return false;
    }
    /* 启动前再检查一次安全条件；停止请求则始终允许执行。 */
    if (running && (!fan_core_is_configured(fan) || fan->low_voltage_stop)) {
        fan->running = false;
        fan->duty_percent = 0U;
        return false;
    }
    fan->running = running;
    fan->duty_percent = fan_core_target_duty(fan);
    return true;
}

void fan_core_adjust_level(fan_core_t *fan, int8_t direction)
{
    if (fan == NULL || direction == 0) {
        return;
    }
    /* 加减档都限制边界，避免 uint8_t 溢出或出现 0 档。 */
    if (direction > 0 && fan->speed_level < FAN_SPEED_LEVELS) {
        fan->speed_level++;
    } else if (direction < 0 && fan->speed_level > 1U) {
        fan->speed_level--;
    }
    fan->duty_percent = fan_core_target_duty(fan);
}

void fan_core_update_battery(fan_core_t *fan, bool valid, uint16_t battery_mv)
{
    if (fan == NULL) {
        return;
    }
    fan->battery_valid = valid;
    fan->battery_mv = valid ? battery_mv : 0U;
    fan->low_voltage_warning = false;
    fan->low_voltage_stop = false;
    /* 无效测量不参与比较；只有明确配置停机阈值时才采取停机保护。 */
    if (!valid) {
        if (fan->config.battery_stop_mv > 0U) {
            fan->low_voltage_stop = true;
            fan->running = false;
            fan->duty_percent = 0U;
        }
        return;
    }

    const uint16_t warn_mv = fan->config.battery_warn_mv;
    const uint16_t stop_mv = fan->config.battery_stop_mv;
    /* 阈值为 0 表示关闭该项判断，避免在参数未知时猜测电池保护电压。 */
    if (warn_mv > 0U && battery_mv <= warn_mv) {
        fan->low_voltage_warning = true;
    }
    if (stop_mv > 0U && battery_mv <= stop_mv) {
        fan->low_voltage_stop = true;
        fan->running = false;
        fan->duty_percent = 0U;
    }
}

void fan_core_update_rpm(fan_core_t *fan, bool valid, uint32_t rpm)
{
    if (fan == NULL) {
        return;
    }
    /* 没有确认 FG 每转脉冲数时，数字即使算出来也不能标成有效 RPM。 */
    fan->rpm_valid = valid && fan->config.pulses_per_revolution > 0U;
    fan->rpm = fan->rpm_valid ? rpm : 0U;
}

uint8_t fan_core_target_duty(const fan_core_t *fan)
{
    if (fan == NULL || !fan->running || !fan_core_is_configured(fan) ||
        fan->low_voltage_stop) {
        return 0U;
    }
    /* 按档位线性缩放：最高占空比 × 当前档位 ÷ 10，并做四舍五入。 */
    const uint32_t scaled = (uint32_t)fan->config.max_duty_percent *
                            (uint32_t)fan->speed_level;
    return (uint8_t)((scaled + (FAN_SPEED_LEVELS / 2U)) / FAN_SPEED_LEVELS);
}
