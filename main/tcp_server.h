#ifndef ESP32C_GATEWAY_TCP_SERVER_H_
#define ESP32C_GATEWAY_TCP_SERVER_H_

#include "esp_err.h"
#include "driver/uart.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 NVS 读取端口配置并启动 TCP Server
 *
 * NVS 中默认读取键名 "tcp_port"（uint16_t），命名空间 "tcp_cfg"。
 * 若 NVS 中不存在该键，则使用默认值 8080。
 *
 * @param uart_port  绑定的 UART 端口号
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tcp_server_start(uart_port_t uart_port);

/**
 * @brief 停止 TCP Server
 */
esp_err_t tcp_server_stop(void);

/**
 * @brief 查询 TCP Server 是否正在运行
 */
bool tcp_server_is_running(void);

/**
 * @brief 将端口号保存到 NVS（需在 tcp_server_start 之前调用）
 *
 * @param port  要保存的端口号
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tcp_server_set_port(uint16_t port);

#ifdef __cplusplus
}
#endif

#endif