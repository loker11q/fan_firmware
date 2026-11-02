#ifndef NETWORK_PORT_H
#define NETWORK_PORT_H

#include "app_status.h"

#include <stdbool.h>
#include <stddef.h>

/* 网络层需要状态时调用此函数；out 指向调用方提供的状态结构体。 */
typedef void (*network_status_provider_t)(app_public_status_t *out,
                                          void *context);

/* 启动 Wi-Fi 和只读状态接口；SSID 为空时网络功能保持关闭。 */
int network_port_start(network_status_provider_t provider, void *context);
/* 复制连接状态和 IP；输出缓冲区由调用方分配，避免返回内部静态指针。 */
void network_port_get_link(bool *connected, char *ip_out, size_t ip_out_size);
/* 注册 H6 UART 控制台的 ota 命令。 */
int network_port_register_ota_command(void);
/* 更新/读取 OTA 状态。字符串会复制到调用者给定的缓冲区。 */
void network_port_set_ota_state(const char *state, const char *error);
void network_port_get_ota_state(char *state_out,
                                size_t state_out_size,
                                char *error_out,
                                size_t error_out_size);

#endif
