#ifndef SAVE_NVS_H
#define SAVE_NVS_H

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t save_uart_config(const char *json_str);
esp_err_t save_tcp_config(const char *json_str);
esp_err_t save_mqtt_config(const char *json_str);
esp_err_t save_modbus_config(const char *json_str);

cJSON *load_uart_config(void);
cJSON *load_tcp_config(void);
cJSON *load_mqtt_config(void);
cJSON *load_modbus_config(void);

#ifdef __cplusplus
}
#endif

#endif