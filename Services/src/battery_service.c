#include "battery_service.h"
#include "battery_math.h"
#include "hardware_port.h"

/* 读取一次电池电压：硬件层给 ADC 脚电压，本层换算成电池总电压。 */
bool battery_service_sample(fan_core_t *fan)
{
    if (fan == 0) {
        return false;
    }
    /* adc_mv 是本函数的局部变量；地址传给硬件层，让它把结果写回来。 */
    uint16_t adc_mv = 0U;
    if (hw_battery_adc_mv(&adc_mv) != 0) {
        fan_core_update_battery(fan, false, 0U);
        return false;
    }
    /* 分压电阻把高电池电压缩小后送到 GPIO34，所以这里要反向换算。 */
    fan_core_update_battery(fan, true,
                            battery_math_adc_to_system_mv(adc_mv));
    return true;
}
