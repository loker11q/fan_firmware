#ifndef FAN_SERVICE_H
#define FAN_SERVICE_H

#include "fan_core.h"

/* 将业务层算出的目标占空比交给硬件接口；异常时返回非 0。 */
int fan_service_apply_pwm(const fan_core_t *fan);
/* 不依赖电机配置，直接请求硬件输出 0% 作为安全动作。 */
int fan_service_set_output_safe(void);

#endif
