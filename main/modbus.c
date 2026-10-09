#include "modbus.h"
#include "freertos/queue.h"
#include "gatewaymqtt_client.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "esp_log.h"
#include "esp_check.h"

#include "nvs.h"

#include "cJSON.h"

#include "mbcontroller.h"

#include "save_nvs.h"
#include "mqtt_client.h"
#include "gatewaymqtt_client.h"
static const char *TAG = "MODBUS";
extern mqtt_config_t g_cfg;  // 来自 mqtt_client.c

// 全局配置
static ModbusConfig_t g_modbus_cfg;


// Poll Task
static TaskHandle_t g_poll_task_handle = NULL;

static QueueHandle_t g_write_queue = NULL;
/* ============================================================
 * JSON 取值辅助宏
 * ============================================================ */

#define JSON_GET_INT(obj, key, var) do {                            \
    cJSON *json_number_item = cJSON_GetObjectItem((obj), (key));   \
    if (json_number_item != NULL && cJSON_IsNumber(json_number_item)) { \
        (var) = json_number_item->valueint;                         \
    }                                                               \
} while (0)

/* ============================================================
 * NVS -> cJSON -> ModbusConfig_t（一步到位，无字符串中转）
 *
 * JSON 格式：
 * {
 *     "poll_interval_ms": 1000,
 *     "timeout_ms": 5000,
 *     "points": [
 *         {
 *             "slave_id": 1,
 *             "function_code": 3,
 *             "start_address": 0,
 *             "length": 10,
 *             "mqtt_topic": "modbus/device1/holding"
 *         }
 *     ]
 * }
 * ============================================================ */

esp_err_t modbus_load_config_from_nvs(void)
{
    cJSON *root = load_modbus_config();

    if (root == NULL)
    {
        ESP_LOGW(TAG, "No modbus config in NVS");
        return ESP_ERR_NOT_FOUND;
    }

    memset(&g_modbus_cfg, 0, sizeof(g_modbus_cfg));

    g_modbus_cfg.uart_port = MODBUS_UART_PORT;
    g_modbus_cfg.baudrate  = MODBUS_BAUDRATE;
    g_modbus_cfg.parity    = MB_PARITY_NONE;
    g_modbus_cfg.tx_pin    = MODBUS_TX_GPIO;
    g_modbus_cfg.rx_pin    = MODBUS_RX_GPIO;
    g_modbus_cfg.rts_pin   = -1;  /* UART_PIN_NO_CHANGE */

    g_modbus_cfg.poll_interval_ms = 1000;
    g_modbus_cfg.timeout_ms       = 5000;

    cJSON *uart_cfg = load_uart_config();

    if (uart_cfg != NULL)
    {
        JSON_GET_INT(uart_cfg, "baudrate", g_modbus_cfg.baudrate);
        JSON_GET_INT(uart_cfg, "tx_pin",   g_modbus_cfg.tx_pin);
        JSON_GET_INT(uart_cfg, "rx_pin",   g_modbus_cfg.rx_pin);

        cJSON *par = cJSON_GetObjectItem(uart_cfg, "parity");

        if (par && cJSON_IsString(par) && par->valuestring != NULL)
        {
            if (strcmp(par->valuestring, "even") == 0)
            {
                g_modbus_cfg.parity = MB_PARITY_EVEN;
            }
            else if (strcmp(par->valuestring, "odd") == 0)
            {
                g_modbus_cfg.parity = MB_PARITY_ODD;
            }
        }

        cJSON_Delete(uart_cfg);
    }

    JSON_GET_INT(root, "poll_interval_ms", g_modbus_cfg.poll_interval_ms);

    JSON_GET_INT(root, "timeout_ms", g_modbus_cfg.timeout_ms);

    cJSON *points = cJSON_GetObjectItem(root, "points");

    if (points == NULL || !cJSON_IsArray(points))
    {
        ESP_LOGW(TAG, "No points array in JSON");
        cJSON_Delete(root);
        return ESP_OK;
    }

    int arr_size = cJSON_GetArraySize(points);

    if (arr_size > MODBUS_MAX_POINTS)
    {
        ESP_LOGW(TAG, "Points array size %d exceeds max %d, truncated", arr_size, MODBUS_MAX_POINTS);
        arr_size = MODBUS_MAX_POINTS;
    }

    g_modbus_cfg.point_count = (uint8_t)arr_size;

    for (int i = 0; i < arr_size; i++)
    {
        cJSON *item = cJSON_GetArrayItem(points, i);

        if (item == NULL)
        {
            continue;
        }

        ModbusPoint_t *p = &g_modbus_cfg.points[i];

        JSON_GET_INT(item, "slave_id", p->slave_id);

        JSON_GET_INT(item, "function_code", p->function_code);

        JSON_GET_INT(item, "start_address", p->start_address);

        JSON_GET_INT(item, "length", p->length);

        cJSON *topic = cJSON_GetObjectItem(item, "mqtt_topic");

        if (topic &&
            cJSON_IsString(topic) &&
            topic->valuestring != NULL)
        {
            strncpy(
                p->mqtt_topic,
                topic->valuestring,
                MODBUS_TOPIC_MAX_LEN - 1
            );

            p->mqtt_topic[
                MODBUS_TOPIC_MAX_LEN - 1
            ] = '\0';
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(
        TAG,
        "Modbus config loaded: %d points",
        g_modbus_cfg.point_count
    );

    return ESP_OK;
}
/* ============================================================
 * 对外接口：把写命令交给 Modbus 任务
 * 此函数只入队，不直接操作 UART1。
 * ============================================================ */
esp_err_t modbus_queue_write_register(uint8_t slave_id,
                                      uint16_t address,
                                      uint16_t value)
{
    if (g_write_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ModbusWriteCommand_t command = {
        .slave_id = slave_id,
        .address = address,
        .value = value,
    };

    if (xQueueSend(g_write_queue, &command, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
/* ============================================================
 * 在 Modbus 任务中执行写入
 * 功能码 06：写单个保持寄存器。
 * ============================================================ */
static void modbus_execute_write(const ModbusWriteCommand_t *command)
{
    mb_param_request_t request = {
        .slave_addr = command->slave_id,
        .command = MB_FUNC_WRITE_REGISTER,
        .reg_start = command->address,
        .reg_size = 1,
    };

    uint16_t value = command->value;
    esp_err_t err = mbc_master_send_request(&request, &value);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Write succeeded: slave=%u, address=%u, value=%u",
                 command->slave_id,
                 command->address,
                 command->value);
    }
    else
    {
        ESP_LOGE(TAG,
                 "Write failed: slave=%u, address=%u, value=%u, error=%s",
                 command->slave_id,
                 command->address,
                 command->value,
                 esp_err_to_name(err));
    }

    /* 将 Modbus 写入结果返回给 MQTT */
    if (mqtt_client_is_connected())
    {
        char payload[160];

        int len = snprintf(
            payload,
            sizeof(payload),
            "{\"slave_id\":%u,\"address\":%u,\"value\":%u,"
            "\"success\":%s,\"error\":\"%s\"}",
            command->slave_id,
            command->address,
            command->value,
            (err == ESP_OK) ? "true" : "false",
            esp_err_to_name(err)
        );

        if (len > 0 && len < (int)sizeof(payload))
        {
            mqtt_client_publish(
                g_cfg.publish_topic,
                payload,
                len,
                0,
                0
            );
        }
    }
}
/* ============================================================
 * 打印配置
 * ============================================================ */

void modbus_config_print(void)
{
    ESP_LOGI(TAG, "================ Modbus Config ================" );

    ESP_LOGI(TAG, "uart_port   = %d", g_modbus_cfg.uart_port);
    ESP_LOGI(TAG, "baudrate    = %lu", (unsigned long)g_modbus_cfg.baudrate);
    ESP_LOGI(TAG, "parity      = %d", g_modbus_cfg.parity);
    ESP_LOGI(TAG, "tx_pin      = %d", g_modbus_cfg.tx_pin);
    ESP_LOGI(TAG, "rx_pin      = %d", g_modbus_cfg.rx_pin);
    ESP_LOGI(TAG, "rts_pin     = %d", g_modbus_cfg.rts_pin);

    ESP_LOGI(TAG, "poll_interval_ms = %lu", (unsigned long) g_modbus_cfg.poll_interval_ms);
    ESP_LOGI(TAG, "timeout_ms = %lu", (unsigned long) g_modbus_cfg.timeout_ms );
    ESP_LOGI(TAG, "point_count = %d", g_modbus_cfg.point_count);


    for (int i = 0; i < g_modbus_cfg.point_count; i++)
    {
        ModbusPoint_t *p = &g_modbus_cfg.points[i];

        ESP_LOGI( TAG, "point[%d]: slave=%d func=%d addr=%d len=%d topic=%s",
            i,
            p->slave_id,
            p->function_code,
            p->start_address,
            p->length,
            p->mqtt_topic
        );
    }
    ESP_LOGI( TAG, "================================================" );
}


// 获取配置
const ModbusConfig_t *modbus_get_config(void)
{
    return &g_modbus_cfg;
}


//初始化 Modbus Master 
// ESP-Modbus v1.x
static esp_err_t modbus_master_init(void)
{
    esp_err_t err;

    // 1. 创建 Modbus Master
    void *master_handler = NULL;
    err = mbc_master_init( MB_PORT_SERIAL_MASTER, &master_handler);

    MB_RETURN_ON_FALSE((master_handler != NULL), ESP_ERR_INVALID_STATE, TAG,
                                "mb controller initialization fail.");
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                            "mb controller initialization fail, returns(0x%x).", (int)err);


    // 2. 配置 Modbus RTU（参数来自网页配置）
    mb_communication_info_t comm = {
        .port     = g_modbus_cfg.uart_port,
        .mode     = MB_MODE_RTU,
        .baudrate = g_modbus_cfg.baudrate,
        .parity   = g_modbus_cfg.parity,
    };
    err = mbc_master_setup((void*)&comm);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                            "mb controller setup fail, returns(0x%x).", (int)err);


    //3. 设置 UART GPIO（参数来自网页配置）
    err = uart_set_pin(
        g_modbus_cfg.uart_port,
        g_modbus_cfg.tx_pin,
        g_modbus_cfg.rx_pin,
        g_modbus_cfg.rts_pin,
        UART_PIN_NO_CHANGE
    );
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
        "mb serial set pin failure, uart_set_pin() returned (0x%x).", (int)err);


    // 4. 启动 Modbus
    err = mbc_master_start();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                            "mb controller start fail, returned (0x%x).", (int)err);


    // 5. RS485 自动方向，无需设置半双工 RTS


    ESP_LOGI(TAG, "Modbus Master initialized");

    return ESP_OK;
}


// 读取一个 Point
esp_err_t modbus_read_point(const ModbusPoint_t *point, void *data, uint16_t data_len)
{
    if (point == NULL || data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t need_bytes;
    bool is_holding;
    switch (point->function_code) {
        case MB_FUNC_READ_COILS:
        case MB_FUNC_READ_DISCRETE_INPUTS:
            need_bytes = (point->length + 7) / 8;
            is_holding = false;
            break;
        case MB_FUNC_READ_HOLDING_REGISTER:
        case MB_FUNC_READ_INPUT_REGISTER:
            need_bytes = point->length * sizeof(uint16_t);
            is_holding = true;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported function code: %d", point->function_code);
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (point->length == 0 || need_bytes > data_len)
    {
        ESP_LOGE(TAG, "data_len=%u < need_bytes=%u", data_len, need_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    mb_param_request_t request = {
        .slave_addr = point->slave_id,
        .command = point->function_code,
        .reg_start = point->start_address,
        .reg_size = point->length,
    };

    memset(data, 0, need_bytes);

    esp_err_t err = mbc_master_send_request(&request, data);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Modbus request failed: slave=%d func=%d addr=%d len=%d err=%s",
            point->slave_id,
            point->function_code,
            point->start_address,
            point->length,
            esp_err_to_name(err)
        );

        return err;
    }

    for (int i = 0; i < point->length; i++)
    {
        if (is_holding)
        {
            uint16_t *reg = (uint16_t *)data;
            ESP_LOGI(TAG, "slave=%d addr=%d data[%d]=0x%04X",
                point->slave_id, point->start_address + i, i, reg[i]);
        }
        else
        {
            uint8_t *bits = (uint8_t *)data;
            ESP_LOGI(TAG, "slave=%d addr=%d data[%d]=0x%02X",
                point->slave_id, point->start_address + i, i, bits[i]);
        }
    }

    return ESP_OK;
}


/* ============================================================
 * Poll Task
 * ============================================================ */

static void modbus_poll_task(void *arg)
{
    /*
     * 最大 125 个寄存器
     *
     * FC03 / FC04 的一次读取长度
     * 不超过 MODBUS_MAX_REGISTERS
     */

    uint8_t *data =malloc(MODBUS_MAX_REGISTERS *sizeof(uint16_t));


    if (data == NULL)
    {
        ESP_LOGE(TAG, "poll task malloc failed" );

        g_poll_task_handle = NULL;

        vTaskDelete(NULL);

        return;
    }


    while (1)
    {
        // 依次执行 points[]
        for (int i = 0; i < g_modbus_cfg.point_count; i++)
        {
            ModbusPoint_t *point = &g_modbus_cfg.points[i];

            ESP_LOGI( TAG, "poll point[%d]", i );

            esp_err_t err = modbus_read_point( point, data, MODBUS_MAX_REGISTERS * sizeof(uint16_t) );//按照当前采集规则读取modbus从设备数据

            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "point[%d] read failed", i);
                continue;
            }

            // MQTT 发布：snprintf 拼 JSON，零堆分配，轮询不碎片化
            if (mqtt_client_is_connected())
            {
                uint16_t *regs = (uint16_t *)data;
                char payload[512] = {0};
                int off = 0;

                /*
                第1步:  拼  {"s":1,"a":0,"d":[
                第2步:  拼  123,456,789          （循环，第一个不加逗号）
                第3步:  拼  ]}
                结果:   {"s":1,"a":0,"d":[123,456,789]}
                */
                off = snprintf(payload, sizeof(payload),
                               "{\"s\":%d,\"a\":%d,\"d\":[",
                               point->slave_id, point->start_address);

                for (int j = 0; j < point->length && off < (int)sizeof(payload) - 10; j++)
                {
                    off += snprintf(payload + off, sizeof(payload) - off,
                                    "%s%d", (j > 0) ? "," : " ", regs[j]);
                }
                off += snprintf(payload + off, sizeof(payload) - off, "]}");

                mqtt_client_publish(point->mqtt_topic, payload, off, 0, 0);
            }
        }

        /* 等待 MQTT 写命令；等不到就按原来的间隔开始下一轮采集 */
        ModbusWriteCommand_t command;

        if (xQueueReceive(g_write_queue,
                        &command,
                        pdMS_TO_TICKS(g_modbus_cfg.poll_interval_ms)) == pdTRUE)
        {
            modbus_execute_write(&command);

            /* 处理等待期间已排队的其他命令 */
            while (xQueueReceive(g_write_queue, &command, 0) == pdTRUE)
            {
                modbus_execute_write(&command);
            }
        }
    }
}


/* ============================================================
 * 启动轮询任务
 * ============================================================ */

esp_err_t modbus_start_poll_task(void)
{
    if (g_poll_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }


    BaseType_t ret =xTaskCreate(
                                modbus_poll_task,
                                "modbus_poll",
                                4096,
                                NULL,
                                5,
                                &g_poll_task_handle
                                );


    if (ret != pdPASS)
    {
        g_poll_task_handle = NULL;

        ESP_LOGE(TAG,"create modbus poll task failed");

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


/* ============================================================
 * 停止轮询任务
 * ============================================================ */

esp_err_t modbus_stop_poll_task(void)
{
    if (g_poll_task_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    //全局先置空，别人再调直接挡在入口；局部变量兜底，该删的任务还能删到。
    TaskHandle_t task = g_poll_task_handle;
    g_poll_task_handle = NULL;

    vTaskDelete(task);

    return ESP_OK;
}

/* ============================================================
 * 使用本地固定配置初始化 Modbus
 * 不从 NVS 读取配置
 * ============================================================ */
esp_err_t modbus_init_local(void)
{
    esp_err_t err;

    if (g_poll_task_handle != NULL || g_write_queue != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Loading local Modbus configuration");

    /* 清空旧配置 */
    memset(&g_modbus_cfg, 0, sizeof(g_modbus_cfg));

    /* UART1 本地固定配置 */
    g_modbus_cfg.uart_port = UART_NUM_1;
    g_modbus_cfg.baudrate  = 115200;
    g_modbus_cfg.parity    = MB_PARITY_NONE;
    g_modbus_cfg.tx_pin    = 0;
    g_modbus_cfg.rx_pin    = 1;
    g_modbus_cfg.rts_pin   = UART_PIN_NO_CHANGE;

    /* 轮询配置 */
    g_modbus_cfg.poll_interval_ms = 1000;
    g_modbus_cfg.timeout_ms       = 5000;

    /*
     * 本地测试点位：
     * 从站地址：1
     * 功能码：03，读取保持寄存器
     * 起始地址：0
     * 数量：10个寄存器
     * MQTT上报主题：test/status
     */
    g_modbus_cfg.point_count = 1;

    g_modbus_cfg.points[0].slave_id       = 1;
    g_modbus_cfg.points[0].function_code  = MB_FUNC_READ_HOLDING_REGISTER;
    g_modbus_cfg.points[0].start_address  = 0;
    g_modbus_cfg.points[0].length         = 10;

    snprintf(
        g_modbus_cfg.points[0].mqtt_topic,
        sizeof(g_modbus_cfg.points[0].mqtt_topic),
        "%s",
        "test/status"
    );

    /* 打印本地配置 */
    modbus_config_print();

    /* 初始化 Modbus Master */
    err = modbus_master_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Local Modbus master init failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* 创建 MQTT → Modbus 写命令队列 */
    g_write_queue = xQueueCreate(
        8,
        sizeof(ModbusWriteCommand_t)
    );

    if (g_write_queue == NULL)
    {
        ESP_LOGE(TAG, "Create Modbus write queue failed");
        return ESP_ERR_NO_MEM;
    }

    /* 启动 Modbus 轮询任务 */
    err = modbus_start_poll_task();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Start local Modbus poll task failed: %s",
                 esp_err_to_name(err));

        vQueueDelete(g_write_queue);
        g_write_queue = NULL;

        return err;
    }

    ESP_LOGI(TAG, "Modbus initialized successfully with local configuration");

    return ESP_OK;
}
/* ============================================================
 * 总初始化
 * ============================================================ */

esp_err_t modbus_init(void)
{
    esp_err_t err;

    //1. NVS -> ModbusConfig_t
    err = modbus_load_config_from_nvs();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "load modbus config from NVS failed");
        return err;
    }

    //2. 打印配置
    modbus_config_print();

    //3. 初始化 Modbus Master
    err = modbus_master_init();////配置modbus使用到的串口 并且启动mudbus 
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,"modbus master init failed");

        return err;
    }

    /* 创建 MQTT → Modbus 命令队列 */
    g_write_queue = xQueueCreate(8, sizeof(ModbusWriteCommand_t));

    if (g_write_queue == NULL)
    {
    ESP_LOGE(TAG, "Create Modbus write queue failed");
    return ESP_ERR_NO_MEM;
    }

    //4. 创建轮询任务
    err = modbus_start_poll_task();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,"start poll task failed");
        return err;
    }

    ESP_LOGI(TAG,"Modbus initialized successfully");

    return ESP_OK;
}