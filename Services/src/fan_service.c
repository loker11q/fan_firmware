#include "fan_service.h"
#include "hardware_port.h"

/* Service（服务层）连接业务规则和硬件接口，不计算档位规则也不操作寄存器。 */
int fan_service_apply_pwm(const fan_core_t *fan)
{
    /* 配置不完整时也明确要求输出 0%，避免旧 PWM 状态残留。 */
    if (fan == 0 || !fan_core_is_configured(fan)) {
        return hw_pwm_set_percent(0U);
    }
    return hw_pwm_set_percent(fan_core_target_duty(fan));
}

int fan_service_set_output_safe(void)
{
    /* 启动失败、PWM 配置错误等异常路径都会调用这个安全输出函数。 */
    return hw_pwm_set_percent(0U);
}
