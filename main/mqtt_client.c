#include "mqtt_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "modbus.h"
#include "esp_log.h"
#include "gatewaymqtt_client.h"  

#include "save_nvs.h"

static const char *TAG = "MQTT";



/* ================================================================
 * 静态变量
 * ================================================================ */
static esp_mqtt_client_handle_t g_client = NULL;
 mqtt_config_t            g_cfg    = {0};
static bool                     g_connected = false;
/* ============================================================
 * 解析 MQTT 写寄存器指令，并交给 Modbus 任务
 * 消息示例：{"slave_id":1,"address":0,"value":123}
 * ============================================================ */
static void mqtt_queue_modbus_command(esp_mqtt_event_handle_t event)
{
    char message[128];

    /* MQTT 数据不保证以 '\0' 结尾，因此先复制到本地数组 */
    if (event->data_len <= 0 ||
        event->data_len >= (int)sizeof(message) ||
        event->current_data_offset != 0 ||
        event->data_len != event->total_data_len)
    {
        ESP_LOGW(TAG, "Modbus command is too long or incomplete");
        return;
    }

    memcpy(message, event->data, event->data_len);
    message[event->data_len] = '\0';

    cJSON *root = cJSON_Parse(message);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "Invalid Modbus command JSON");
        return;
    }

    cJSON *slave = cJSON_GetObjectItem(root, "slave_id");
    cJSON *address = cJSON_GetObjectItem(root, "address");
    cJSON *value = cJSON_GetObjectItem(root, "value");

    /* 只接受合法的整数范围，避免错误数据被截断后发给从站 */
    bool valid =
        cJSON_IsNumber(slave) &&
        cJSON_IsNumber(address) &&
        cJSON_IsNumber(value) &&
        slave->valuedouble == slave->valueint &&
        address->valuedouble == address->valueint &&
        value->valuedouble == value->valueint &&
        slave->valueint >= 1 &&
        slave->valueint <= 247 &&
        address->valueint >= 0 &&
        address->valueint <= 65535 &&
        value->valueint >= 0 &&
        value->valueint <= 65535;

    if (!valid)
    {
        ESP_LOGW(TAG, "Invalid Modbus slave, address or value");
        cJSON_Delete(root);
        return;
    }

    esp_err_t err = modbus_queue_write_register(
        (uint8_t)slave->valueint,
        (uint16_t)address->valueint,
        (uint16_t)value->valueint
    );

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Cannot queue Modbus command: %s",
                 esp_err_to_name(err));
    }
}
/* ================================================================
 * MQTT 事件回调
 * ================================================================ */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Broker connected");
        g_connected = true;

        // 连上后订阅控制指令 topic（如果配置了）
        if (strlen(g_cfg.subscribe_topic) > 0) {
            int msg_id = esp_mqtt_client_subscribe(
                g_client, g_cfg.subscribe_topic, g_cfg.qos
            );
            ESP_LOGI(TAG, "Subscribed to '%s', msg_id=%d",
                     g_cfg.subscribe_topic, msg_id);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Broker disconnected");
        g_connected = false;
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Received: topic=%.*s, data=%.*s",
                 event->topic_len, event->topic,
                 event->data_len,  event->data);
        mqtt_queue_modbus_command(event);

        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ================================================================
 * 从 NVS 解析配置
 * ================================================================ */
static esp_err_t mqtt_parse_config(cJSON *root)
{
    if (root == NULL) return ESP_ERR_INVALID_ARG;

    memset(&g_cfg, 0, sizeof(g_cfg));

    cJSON *item;

    item = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(item))  g_cfg.enabled = cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) g_cfg.enabled = (item->valueint != 0);

    item = cJSON_GetObjectItem(root, "broker");
    if (cJSON_IsString(item))
        strncpy(g_cfg.broker, item->valuestring, sizeof(g_cfg.broker) - 1);

    item = cJSON_GetObjectItem(root, "port");
    if (cJSON_IsNumber(item)) g_cfg.port = item->valueint;
    if (g_cfg.port == 0) g_cfg.port = 1883;     // 默认 1883

    item = cJSON_GetObjectItem(root, "client_id");
    if (cJSON_IsString(item))
        strncpy(g_cfg.client_id, item->valuestring, sizeof(g_cfg.client_id) - 1);

    item = cJSON_GetObjectItem(root, "username");
    if (cJSON_IsString(item))
        strncpy(g_cfg.username, item->valuestring, sizeof(g_cfg.username) - 1);

    item = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(item))
        strncpy(g_cfg.password, item->valuestring, sizeof(g_cfg.password) - 1);

    item = cJSON_GetObjectItem(root, "publish_topic");
    if (cJSON_IsString(item))
        strncpy(g_cfg.publish_topic, item->valuestring, sizeof(g_cfg.publish_topic) - 1);

    item = cJSON_GetObjectItem(root, "subscribe_topic");
    if (cJSON_IsString(item))
        strncpy(g_cfg.subscribe_topic, item->valuestring, sizeof(g_cfg.subscribe_topic) - 1);

    item = cJSON_GetObjectItem(root, "qos");
    if (cJSON_IsNumber(item)) g_cfg.qos = item->valueint;

    item = cJSON_GetObjectItem(root, "keep_alive");
    if (cJSON_IsNumber(item)) g_cfg.keep_alive = item->valueint;
    if (g_cfg.keep_alive == 0) g_cfg.keep_alive = 60;

    return ESP_OK;
}
/* ================================================================
 * 初始化local MQTT 客户端（不读取 NVS，使用固定参数）
 * ================================================================ */
esp_err_t mqtt_client_init_local(void)
{
    if (g_client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // 现有事件回调会用这两个值：连上后订阅 test/cmd
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.subscribe_topic, sizeof(g_cfg.subscribe_topic), "%s", "test/cmd");
    snprintf(g_cfg.publish_topic, sizeof(g_cfg.publish_topic), "%s", "test/status");
    g_cfg.qos = 0;
    g_connected = false;

    // 先使用固定参数，不读取 NVS
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://192.168.5.20:1883",
        .credentials.client_id = "esp32c3_test_01",
        .session.keepalive = 60,
    };

    g_client = esp_mqtt_client_init(&mqtt_cfg);
    if (g_client == NULL) {
        ESP_LOGE(TAG, "Create MQTT client failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        g_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register MQTT event failed");
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
        return err;
    }

    err = esp_mqtt_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Start MQTT client failed");
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started; waiting for Broker connection");
    return ESP_OK;
}

/* ================================================================
 * 初始化
 * ================================================================ */
esp_err_t mqtt_client_init(void)
{
    // ===== 1. NVS → cJSON =====
    cJSON *root = load_mqtt_config();
    if (root == NULL) {
        ESP_LOGE(TAG, "load_mqtt_config failed");
        return ESP_ERR_NOT_FOUND;
    }

    // ===== 2. cJSON → mqtt_config_t =====
    esp_err_t err = mqtt_parse_config(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse mqtt config failed");
        return err;
    }

    if (!g_cfg.enabled) {
        ESP_LOGI(TAG, "MQTT disabled, skip");
        return ESP_OK;
    }

    // ===== 3. 拼接 Broker URI =====
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", g_cfg.broker, g_cfg.port);
    ESP_LOGI(TAG, "Broker: %s, client_id: %s", uri, g_cfg.client_id);

    // ===== 4. 构造 ESP-IDF MQTT 配置 =====
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials = {
            .client_id = g_cfg.client_id,
            .username  = strlen(g_cfg.username) > 0 ? g_cfg.username : NULL,
            .authentication.password = strlen(g_cfg.password) > 0 ? g_cfg.password : NULL,
        },
        .session.keepalive = g_cfg.keep_alive,
    };

    // ===== 5. 创建并启动 =====
    g_client = esp_mqtt_client_init(&mqtt_cfg);
    if (g_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_err_t ret = esp_mqtt_client_start(g_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

/* ================================================================
 * 发布
 * ================================================================ */
int mqtt_client_publish(const char *topic, const char *data, int len,
                        int qos, int retain)
{
    if (g_client == NULL || !g_connected) return -1;

    int msg_id = esp_mqtt_client_publish(g_client, topic, data, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish to '%s' failed", topic);
    }
    return msg_id;
}

int mqtt_client_publish_json(const char *topic, const cJSON *json_obj,
                             int qos, int retain)
{
    if (json_obj == NULL) return -1;

    char *str = cJSON_PrintUnformatted(json_obj);
    if (str == NULL) return -1;

    int msg_id = mqtt_client_publish(topic, str, (int)strlen(str), qos, retain);
    free(str);
    return msg_id;
}

/* ================================================================
 * 查询连接状态
 * ================================================================ */
bool mqtt_client_is_connected(void)
{
    return g_connected;
}

/* ================================================================
 * 销毁
 * ================================================================ */
void mqtt_client_deinit(void)
{
    if (g_client) {
        esp_mqtt_client_stop(g_client);
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
    }
    g_connected = false;
    ESP_LOGI(TAG, "MQTT client deinit");
}