#ifndef BATTERY_SERVICE_H
#define BATTERY_SERVICE_H

#include "fan_core.h"

#include <stdbool.h>

/* 采 ADC、换算分压前电压并更新 fan 状态；返回本次读数是否有效。 */
bool battery_service_sample(fan_core_t *fan);

#endif
