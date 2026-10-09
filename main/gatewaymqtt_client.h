#ifndef ESP32_MQTT_CLIENT_H
#define ESP32_MQTT_CLIENT_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_client_init(void);
int mqtt_client_publish(const char *topic, const char *data, int len, int qos, int retain);
int mqtt_client_publish_json(const char *topic, const cJSON *json_obj, int qos, int retain);
bool mqtt_client_is_connected(void);
void mqtt_client_deinit(void);
esp_err_t mqtt_client_init_local(void); /* modern prototype: no old-style parameter declarations */

/* ================================================================
 * 内部配置结构（与 HTML 的 getMqttConfig() 字段一一对应）
 * ================================================================ */
typedef struct {
    bool   enabled;
    char   broker[64];
    int    port;
    char   client_id[32];
    char   username[32];
    char   password[32];
    char   publish_topic[64];
    char   subscribe_topic[64];
    int    qos;
    int    keep_alive;
} mqtt_config_t;
#ifdef __cplusplus
}
#endif

#endif
