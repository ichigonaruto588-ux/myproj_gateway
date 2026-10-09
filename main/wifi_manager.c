#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
EventGroupHandle_t wifi_event_group = NULL;
static const char *TAG = "wifi_mgr";
static int s_retry_count = 0;
static bool s_smartconfig_running = false;


bool wifi_manager_is_connected(void)
{
    if (wifi_event_group == NULL) return false;
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}


static void sc_task(void *param)
{
    esp_err_t ret;

    // 清除上一次可能残留的配网完成标志
    xEventGroupClearBits(wifi_event_group, WIFI_SC_DONE_BIT);

    // 选择 ESPTouch 配网协议
    ret = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Set SmartConfig type failed: %s", esp_err_to_name(ret));
        goto task_exit;
    }

    // 使用默认参数启动 SmartConfig
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ret = esp_smartconfig_start(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Start SmartConfig failed: %s", esp_err_to_name(ret));
        goto task_exit;
    }

    ESP_LOGI(TAG, "SmartConfig started, timeout: %d ms", SMARTCONFIG_TIMEOUT_MS);

    // 等待配网完成，最多等待 60 秒；等待成功后自动清除完成位
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_SC_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(SMARTCONFIG_TIMEOUT_MS));

    if (bits & WIFI_SC_DONE_BIT)
    {
        ESP_LOGI(TAG, "SmartConfig finished");
    }
    else
    {
        ESP_LOGW(TAG, "SmartConfig timeout, please retry");
    }

    // 成功或超时后，都停止本次配网
    ret = esp_smartconfig_stop();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Stop SmartConfig failed: %s", esp_err_to_name(ret));
    }

    task_exit:
        // 清除运行状态并删除当前任务
        s_smartconfig_running = false;
        vTaskDelete(NULL);
}

void wifi_manager_start_smartconfig(void)
{
    if (s_smartconfig_running) 
    {
        ESP_LOGW(TAG, "SmartConfig already running");
        return;
    }
    s_smartconfig_running = true;

     BaseType_t result =xTaskCreate(sc_task, "sc_task", 4096, NULL, 3, NULL);
    if (result != pdPASS) 
    {
        s_smartconfig_running = false;
        ESP_LOGE(TAG, "Create SmartConfig task failed");
    }
}

void wifi_manager_stop_smartconfig(void)
{
    if (!s_smartconfig_running) return;
    esp_smartconfig_stop();
    s_smartconfig_running = false;
    ESP_LOGI(TAG, "SmartConfig stopped");
}
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        nvs_handle_t handle;
        wifi_config_t wifi_config;
        bool has_saved = false;

        if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
            size_t len = sizeof(wifi_config_t);
            if (nvs_get_blob(handle, "sta_cfg", &wifi_config, &len) == ESP_OK) {
                ESP_LOGI(TAG, "found saved WiFi config, auto-connecting...");
                esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                esp_wifi_connect();
                has_saved = true;
            }
            nvs_close(handle);
        }

        if (!has_saved) {
            ESP_LOGI(TAG, "no saved WiFi config, starting SmartConfig...");
            wifi_manager_start_smartconfig();
        }

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", disconn->reason);
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            if (s_smartconfig_running) {
                return;
            }

            if (s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;

                ESP_LOGI(TAG, "retry WiFi: %d/%d",
                        s_retry_count, WIFI_MAX_RETRY);

                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "WiFi retry failed, starting SmartConfig");

                s_retry_count = 0;
                wifi_manager_start_smartconfig();
            }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "SmartConfig scan done");

    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "SmartConfig found channel");

    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config = {0};
        uint8_t ssid[33] = {0};
        uint8_t password[65] = {0};

        memset(&wifi_config, 0, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SmartConfig got SSID:%s", ssid);

        nvs_handle_t handle;
        if (nvs_open("wifi_cfg", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_blob(handle, "sta_cfg", &wifi_config, sizeof(wifi_config_t));
            nvs_commit(handle);
            nvs_close(handle);
            ESP_LOGI(TAG, "WiFi config saved to NVS");
        }

        esp_err_t disconnect_ret = esp_wifi_disconnect();
        if (disconnect_ret != ESP_OK &&
            disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "WiFi disconnect failed: %s",
                    esp_err_to_name(disconnect_ret));
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());

    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        ESP_LOGI(TAG, "SmartConfig ACK done");
        xEventGroupSetBits(wifi_event_group, WIFI_SC_DONE_BIT);
    }
}


void wifi_manager_init(void)
{
    wifi_event_group = xEventGroupCreate();

    //第一步：初始化TCP/IP协议栈
    ESP_ERROR_CHECK(esp_netif_init());
    //第二步：创建默认系统事件循环（WiFi/IP/SmartConfig 事件都通过它分发）
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //第三步：创建 STA 模式的默认网络接口，绑定 WiFi STA 与 TCP/IP 协议栈
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    //关闭 WiFi 省电模式
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi manager initialized");
}
esp_err_t wifi_manager_get_ip(char *ip_buf, size_t buf_size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) return ESP_FAIL;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return ESP_FAIL;

    snprintf(ip_buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_manager_get_ssid(char *ssid_buf, size_t buf_size)
{
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return ESP_FAIL;
    snprintf(ssid_buf, buf_size, "%s", (char *)cfg.sta.ssid);
    return ESP_OK;
}