#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H


#include <stdio.h>
#include <stdbool.h>
#include "esp_event.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif



#define WIFI_MAX_RETRY 3

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_SC_DONE_BIT      BIT1
#define WIFI_FAIL_BIT         BIT2
#define SMARTCONFIG_TIMEOUT_MS 60000
extern EventGroupHandle_t wifi_event_group;

void wifi_manager_init(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *ip_buf, size_t buf_size);
esp_err_t wifi_manager_get_ssid(char *ssid_buf, size_t buf_size);
void wifi_manager_start_smartconfig(void);
void wifi_manager_stop_smartconfig(void);




#ifdef __cplusplus
}
#endif

#endif