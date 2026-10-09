#ifndef UART_MANAGER_H
#define UART_MANAGER_H

#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uart_manager_init_from_json(const char *json_str, uart_port_t port);

esp_err_t uart_manager_deinit(uart_port_t port);

int uart_manager_send(uart_port_t port, const uint8_t *data, int len);

int uart_manager_read(uart_port_t port, uint8_t *buf, int max_len, TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif