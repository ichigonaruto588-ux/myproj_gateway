#include "save_nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "nvs";

// 通用：存 cJSON 对象 → NVS
static esp_err_t nvs_save_json(const char *ns, const char *key, cJSON *obj)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(ns, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    char *json_str = cJSON_PrintUnformatted(obj);
    ret = nvs_set_str(handle, key, json_str);

    nvs_commit(handle);
    nvs_close(handle);
    free(json_str);
    return ret;
}

// 通用：从 NVS 读 → cJSON 对象（调用方负责 cJSON_Delete）
static cJSON *nvs_load_json(const char *ns, const char *key)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) return NULL;

    size_t len = 0;
    if (nvs_get_str(handle, key, NULL, &len) != ESP_OK) {
        nvs_close(handle);
        return NULL;
    }

    char *buf = malloc(len);
    if (!buf) { nvs_close(handle); return NULL; }

    nvs_get_str(handle, key, buf, &len);
    nvs_close(handle);

    cJSON *obj = cJSON_Parse(buf);
    free(buf);
    return obj;
}

// ———————————————— UART ————————————————

esp_err_t save_uart_config(const char *json_str)
{
    if (json_str == NULL) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "uart JSON parse failed");
        return ESP_FAIL;
    }

    esp_err_t ret = nvs_save_json("uart_cfg", "config", root);
    cJSON_Delete(root);
    return ret;
}

cJSON *load_uart_config(void)
{
    return nvs_load_json("uart_cfg", "config");
}

// ———————————————— TCP ————————————————

esp_err_t save_tcp_config(const char *json_str)
{
    if (json_str == NULL) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "tcp JSON parse failed");
        return ESP_FAIL;
    }

    esp_err_t ret = nvs_save_json("tcp_cfg", "config", root);
    cJSON_Delete(root);
    return ret;
}

cJSON *load_tcp_config(void)
{
    return nvs_load_json("tcp_cfg", "config");
}

// ———————————————— MQTT ————————————————

esp_err_t save_mqtt_config(const char *json_str)
{
    if (json_str == NULL) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "mqtt JSON parse failed");
        return ESP_FAIL;
    }

    esp_err_t ret = nvs_save_json("mqtt_cfg", "config", root);
    cJSON_Delete(root);
    return ret;
}

cJSON *load_mqtt_config(void)
{
    return nvs_load_json("mqtt_cfg", "config");
}

// ———————————————— Modbus ————————————————

esp_err_t save_modbus_config(const char *json_str)
{
    if (json_str == NULL) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "modbus JSON parse failed");
        return ESP_FAIL;
    }

    esp_err_t ret = nvs_save_json("modbus_cfg", "config", root);
    cJSON_Delete(root);
    return ret;
}

cJSON *load_modbus_config(void)
{
    return nvs_load_json("modbus_cfg", "config");
}