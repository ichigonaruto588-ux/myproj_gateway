#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include "wifi_manager.h"
#include "cJSON.h"
#include "save_nvs.h"
#include "uart.h"
#include "tcp_server.h"
#include "gatewaymqtt_client.h"
#include "modbus.h"
#include "webserver.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "app_main";

typedef enum
{
    APP_MODE_TRANSPARENT = 0,
    APP_MODE_GATEWAY
} app_mode_t;
/**
 * @brief 从 UART 配置中识别设备运行模式
 *
 * protocol = "modbus_rtu"  -> 网关模式
 * 其他值或缺少字段         -> 默认透传模式
 */
static app_mode_t get_app_mode(const cJSON *uart_config)
{
    const cJSON *protocol =
        cJSON_GetObjectItemCaseSensitive(uart_config, "protocol");

    if (cJSON_IsString(protocol) &&
        protocol->valuestring != NULL &&
        strcmp(protocol->valuestring, "modbus_rtu") == 0)
    {
        return APP_MODE_GATEWAY;
    }

    return APP_MODE_TRANSPARENT;
}
/**
 * @brief 启动 UART-TCP 透传模式
 */
static esp_err_t start_transparent_mode(const cJSON *uart_config)
{
    if (uart_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting UART-TCP transparent mode");

    char *uart_json = cJSON_PrintUnformatted(uart_config);

    if (uart_json == NULL)
    {
        ESP_LOGE(TAG, "UART config JSON conversion failed");
        return ESP_ERR_NO_MEM;
    }

    /*
     * 初始化 UART1。
     */
    esp_err_t ret = uart_manager_init_from_json(
        uart_json,
        UART_NUM_1
    );

    free(uart_json);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "UART1 init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /*
     * 启动 TCP Server，并将数据转发到 UART1。
     */
    ret = tcp_server_start(UART_NUM_1);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "TCP Server start failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "UART-TCP transparent mode started");

    return ESP_OK;
}


/**
 * @brief 启动 Modbus-MQTT 网关模式
 */
static esp_err_t start_gateway_mode(void)
{
    ESP_LOGI(TAG, "Starting Modbus-MQTT gateway mode");

    /*
     * Modbus 模块负责初始化 UART1。
     */
    esp_err_t ret = modbus_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Modbus init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /*
     * 启动 MQTT 客户端。
     */
    ret = mqtt_client_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "MQTT init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Modbus-MQTT gateway mode started");

    return ESP_OK;
}


/**
 * @brief 根据配置启动选中的业务模式
 */
static esp_err_t start_business_mode(
    app_mode_t app_mode,
    const cJSON *uart_config
)
{
    switch (app_mode)
    {
        case APP_MODE_TRANSPARENT:
            return start_transparent_mode(uart_config);

        case APP_MODE_GATEWAY:
            return start_gateway_mode();

        default:
            ESP_LOGE(TAG, "Unknown application mode: %d", app_mode);
            return ESP_ERR_INVALID_ARG;
    }
}
static void wifi_init_and_wait(void)
{
    //第二步：开启 SmartConfig 配网
    wifi_manager_init();

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    char ip[16] = {0};
    char ssid[33] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_ssid(ssid, sizeof(ssid));
    ESP_LOGI(TAG, "WiFi ready: SSID=%s, IP=%s", ssid, ip);
}

static void uart_config_init(void)
{
    //第三步：从 NVS 恢复 UART 配置并初始化
    cJSON *uart_cfg = load_uart_config();
    if (uart_cfg) {
        ESP_LOGI(TAG, "Restoring UART config");
    } else {
        ESP_LOGW(TAG, "No UART config in NVS, skip");
        return;
    }
}
static void start_work_mode()
{

    cJSON *uart_cfg = load_uart_config();
    if (uart_cfg) 
    {
        ESP_LOGI(TAG, "Restoring UART config");
    } 
    else
    {
        ESP_LOGW(TAG, "No UART config in NVS, skip");
        return;
    }


    app_mode_t app_mode = get_app_mode(uart_cfg);

   //启动业务模式（透传或网关）
    esp_err_t ret = start_business_mode(app_mode, uart_cfg);
    cJSON_Delete(uart_cfg);
    uart_cfg = NULL;
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                "Business mode start failed: %s",
                esp_err_to_name(ret));
        /*
        * Web Server 已经启动，所以即使业务启动失败，
        * 仍然可以打开网页修改配置。
        */
        return;
    }
}
void app_main(void)
{
        //第一步：开启 NVS 存储
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI(TAG, "===== Industrial Gateway Starting =====");

    //第二步：开启 SmartConfig 配网
    wifi_init_and_wait();//开启wifi 打开配网


    //第四步：启动 Web Server（用于配置 UART、TCP、MQTT、Modbus 等）
    ret = webserver_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Web server init failed: %s", esp_err_to_name(ret));
        return;
    }

    //启动业务模式（透传或网关）根据网页配置的信息选择工作模式然后执行对应的启动
    start_work_mode();


}
