#ifndef FAN_CORE_H
#define FAN_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* 这一层只保存风扇规则，不操作 ESP32 GPIO，因此可以在电脑或其他 MCU 上复用。 */
#define FAN_SPEED_LEVELS 10U

typedef struct {
    bool motor_enabled;              /* 只有确认电机参数后才允许开启输出。 */
    uint32_t pwm_frequency_hz;       /* PWM 频率，单位 Hz；0 表示没有配置。 */
    uint8_t max_duty_percent;        /* 最高档允许的占空比，范围 1～100。 */
    bool pwm_active_low;             /* true 表示低电平有效，具体要看电机手册。 */
    uint16_t pulses_per_revolution;  /* FG 每转脉冲数；0 表示不能计算 RPM。 */
    uint16_t battery_warn_mv;        /* 电池告警阈值，单位 mV；0 表示关闭。 */
    uint16_t battery_stop_mv;        /* 电池停机阈值，单位 mV；0 表示关闭。 */
} fan_config_t;

typedef struct {
    fan_config_t config;             /* 初始化时复制进来的参数。 */
    bool running;                    /* 软件运行状态，不代表硬件已经断电。 */
    bool battery_valid;              /* false 时不要把 battery_mv 当成有效读数。 */
    bool low_voltage_warning;         /* 当前电压是否低于已配置的告警阈值。 */
    bool low_voltage_stop;            /* 当前电压是否触发停机保护。 */
    bool rpm_valid;                  /* FG 参数/采样有效时才为 true。 */
    uint8_t speed_level;              /* 当前档位，范围 1～10。 */
    uint8_t duty_percent;             /* 当前计算出的目标 PWM 百分比。 */
    uint16_t battery_mv;              /* 电池总电压，单位 mV，不是电量百分比。 */
    uint32_t rpm;                     /* 转速，单位转/分钟。 */
} fan_core_t;

/* 初始化结构体。fan 是写入目标的地址；config 是只读输入，可以为 NULL。 */
void fan_core_init(fan_core_t *fan, const fan_config_t *config);
/* 检查使能、频率和最高占空比是否组成一套有效的电机配置。 */
bool fan_core_is_configured(const fan_core_t *fan);
/* 请求启动或停止；启动条件不满足时返回 false，并保持安全输出状态。 */
bool fan_core_set_running(fan_core_t *fan, bool running);
/* direction 大于 0 加档、小于 0 减档；档位会限制在 1～10。 */
void fan_core_adjust_level(fan_core_t *fan, int8_t direction);
/* 更新电池测量结果；valid=false 表示本次 ADC 读数无效。 */
void fan_core_update_battery(fan_core_t *fan, bool valid, uint16_t battery_mv);
/* 更新 FG 转速；FG PPR 未配置时 RPM 会标为无效。 */
void fan_core_update_rpm(fan_core_t *fan, bool valid, uint32_t rpm);
/* 读取当前应输出的占空比；未运行或未配置时返回 0。 */
uint8_t fan_core_target_duty(const fan_core_t *fan);

#endif
