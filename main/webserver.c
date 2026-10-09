#include "webserver.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include <string.h>
#include <stdlib.h>
#include "uart.h"
#include "save_nvs.h"
#include "wifi_manager.h"

static const char *TAG = "webserver";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ============================================================
//  GET /  —  首页
// ============================================================
static esp_err_t root_get_handler(httpd_req_t *req)
{
    size_t html_size = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)index_html_start, html_size);
    return ESP_OK;
}

// ============================================================
//  POST /api/config/uart  —  UART 配置
// ============================================================
static esp_err_t uart_config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

   // uart_manager_init_from_json(buf,UART_NUM_1);

    save_uart_config(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  POST /api/config/tcp  —  TCP 配置
// ============================================================
static esp_err_t tcp_config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        ESP_LOGE(TAG, "TCP config JSON save failed: empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = save_tcp_config(buf);
    
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
        ESP_LOGE(TAG, "TCP config JSON save failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }


    ESP_LOGI(TAG, "TCP config JSON saved successfully: %s", buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  POST /api/config/mqtt  —  MQTT 配置
// ============================================================
static esp_err_t mqtt_config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = save_mqtt_config(buf);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  POST /api/config/modbus  —  Modbus 配置
// ============================================================
static esp_err_t modbus_config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = save_modbus_config(buf);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"msg\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  GET /api/config  —  加载全部配置
// ============================================================
static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *uart_cfg   = load_uart_config();
    cJSON *tcp_cfg    = load_tcp_config();
    cJSON *mqtt_cfg   = load_mqtt_config();
    cJSON *modbus_cfg = load_modbus_config();

    cJSON_AddItemToObject(root, "uart",   uart_cfg   ? uart_cfg   : cJSON_CreateNull());
    cJSON_AddItemToObject(root, "tcp",    tcp_cfg    ? tcp_cfg    : cJSON_CreateNull());
    cJSON_AddItemToObject(root, "mqtt",   mqtt_cfg   ? mqtt_cfg   : cJSON_CreateNull());
    cJSON_AddItemToObject(root, "modbus", modbus_cfg ? modbus_cfg : cJSON_CreateNull());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

// ============================================================
//  GET /api/wifi/status  —  WiFi 状态
// ============================================================
static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    bool connected = wifi_manager_is_connected();
    cJSON_AddBoolToObject(root, "connected", connected);

    if (connected) {
        char ip[32] = "---";
        char ssid[64] = "---";
        wifi_manager_get_ip(ip, sizeof(ip));
        wifi_manager_get_ssid(ssid, sizeof(ssid));
        cJSON_AddStringToObject(root, "ip",   ip);
        cJSON_AddStringToObject(root, "ssid", ssid);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

// ============================================================
//  POST /api/smartconfig/start  —  启动 SmartConfig
// ============================================================
static esp_err_t smartconfig_start_post_handler(httpd_req_t *req)
{
    wifi_manager_start_smartconfig();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  POST /api/smartconfig/stop  —  停止 SmartConfig
// ============================================================
static esp_err_t smartconfig_stop_post_handler(httpd_req_t *req)
{
    wifi_manager_stop_smartconfig();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  POST /api/ota/start  —  OTA（占位，待实现）
// ============================================================
static esp_err_t ota_start_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"pending\",\"msg\":\"OTA not implemented yet\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
//  POST /api/reboot  —  重启设备
// ============================================================
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"msg\":\"rebooting\"}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ============================================================
//  注册所有路由
// ============================================================

static const httpd_uri_t uri_root = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = root_get_handler,
};

static const httpd_uri_t uri_uart = {
    .uri     = "/api/config/uart",
    .method  = HTTP_POST,
    .handler = uart_config_post_handler,
};

static const httpd_uri_t uri_tcp = {
    .uri     = "/api/config/tcp",
    .method  = HTTP_POST,
    .handler = tcp_config_post_handler,
};

static const httpd_uri_t uri_mqtt = {
    .uri     = "/api/config/mqtt",
    .method  = HTTP_POST,
    .handler = mqtt_config_post_handler,
};

static const httpd_uri_t uri_modbus = {
    .uri     = "/api/config/modbus",
    .method  = HTTP_POST,
    .handler = modbus_config_post_handler,
};

static const httpd_uri_t uri_config = {
    .uri     = "/api/config",
    .method  = HTTP_GET,
    .handler = config_get_handler,
};

static const httpd_uri_t uri_wifi = {
    .uri     = "/api/wifi/status",
    .method  = HTTP_GET,
    .handler = wifi_status_get_handler,
};

static const httpd_uri_t uri_sc_start = {
    .uri     = "/api/smartconfig/start",
    .method  = HTTP_POST,
    .handler = smartconfig_start_post_handler,
};

static const httpd_uri_t uri_sc_stop = {
    .uri     = "/api/smartconfig/stop",
    .method  = HTTP_POST,
    .handler = smartconfig_stop_post_handler,
};

static const httpd_uri_t uri_ota = {
    .uri     = "/api/ota/start",
    .method  = HTTP_POST,
    .handler = ota_start_post_handler,
};

static const httpd_uri_t uri_reboot = {
    .uri     = "/api/reboot",
    .method  = HTTP_POST,
    .handler = reboot_post_handler,
};

// ============================================================
static httpd_handle_t s_webserver = NULL;
esp_err_t webserver_start(void)
{
    // 防止重复启动
    if (s_webserver != NULL)
    {
        ESP_LOGW(TAG, "Web server is already running");
        return ESP_OK;
    }

    // 每项包含请求路径、HTTP 方法和处理函数
    const httpd_uri_t *uri_handlers[] =
    {
        &uri_root,
        &uri_uart,
        &uri_tcp,
        &uri_mqtt,
        &uri_modbus,
        &uri_config,
        &uri_wifi,
        &uri_sc_start,
        &uri_sc_stop,
        &uri_ota,
        &uri_reboot
    };

    const size_t uri_count = sizeof(uri_handlers) / sizeof(uri_handlers[0]);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // 连接达到上限时，允许清理最久未使用的连接
    config.lru_purge_enable = true;

    // 根据实际接口数量设置注册容量
    config.max_uri_handlers = (uint16_t)uri_count;

    ESP_LOGI(TAG, "Starting web server on port %u, URI count: %u",
             (unsigned int)config.server_port, (unsigned int)uri_count);

    // 先使用局部句柄，全部注册成功后再记录到全局
    httpd_handle_t new_server = NULL;
    esp_err_t err = httpd_start(&new_server, &config);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return err;
    }

    // 逐个注册接口，并检查注册结果
    for (size_t i = 0; i < uri_count; ++i)
    {
        const httpd_uri_t *uri_handler = uri_handlers[i];

        err = httpd_register_uri_handler(new_server, uri_handler);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register URI '%s', method=%d: %s",
                     uri_handler->uri, (int)uri_handler->method, esp_err_to_name(err));

            // 注册失败时停止服务器，避免留下部分接口
            esp_err_t stop_err = httpd_stop(new_server);
            if (stop_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to clean up web server: %s", esp_err_to_name(stop_err));
            }

            return err;
        }

        ESP_LOGI(TAG, "Registered URI: %s, method=%d",uri_handler->uri, (int)uri_handler->method);
    }

    // 全部注册成功后保存服务器句柄
    s_webserver = new_server;

    ESP_LOGI(TAG, "Web server started successfully with %u URI handlers",(unsigned int)uri_count);

    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    if (s_webserver == NULL) {
        ESP_LOGW(TAG, "Web server is not running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping web server");

    esp_err_t err = httpd_stop(s_webserver);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to stop web server: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    s_webserver = NULL;

    ESP_LOGI(TAG, "Web server stopped");
    return ESP_OK;
}