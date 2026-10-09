#include "uart.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "uart_manager";
static bool s_initialized[UART_NUM_MAX] = {false};

// JSON 取值辅助：字段存在且为数字才赋值，否则保留默认值
#define JSON_GET_INT(obj, key, var) do { \
    cJSON *item = cJSON_GetObjectItem(obj, key); \
    if (item && cJSON_IsNumber(item)) { \
        var = item->valueint; \
    } \
} while(0)

/*
{
    "baudrate": 115200,
    "data_bits": 8,
    "parity": "none",
    "stop_bits": 1,
    "protocol": "transparent",
    "rx_pin": 4,
    "tx_pin": 5
}
*/
esp_err_t uart_manager_init_from_json(const char *json_str, uart_port_t uart_port)
{
    // ===== 1. 参数校验：空指针直接拒绝 =====
    if (json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // ===== 2. 解析 JSON =====
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    // ===== 3. 初始化全部配置参数为默认值 =====
    // JSON 中不传的字段保持默认值，避免使用未初始化变量
    int port        = uart_port;             // 指定串口
    int baud_rate   = 115200;                // 默认波特率
    int data_bits   = UART_DATA_8_BITS;      // 默认 8 位数据
    int stop_bits   = UART_STOP_BITS_1;      // 默认 1 位停止位
    int parity      = UART_PARITY_DISABLE;   // 默认无校验
    int flow_ctrl   = UART_HW_FLOWCTRL_DISABLE; // 默认无硬件流控
    int rx_pin      = UART_PIN_NO_CHANGE;    // 默认不修改 RX 引脚
    int tx_pin      = UART_PIN_NO_CHANGE;    // 默认不修改 TX 引脚
    int rx_buf_size = 1024;                  // 默认接收缓冲区大小
    int tx_buf_size = 1024;                  // 默认发送缓冲区大小
    int data_bits_value = 8;
    int stop_bits_value = 1;

    // ===== 4. 从 JSON 中提取整数字段（字段存在才覆盖默认值） =====
    JSON_GET_INT(root, "baudrate",    baud_rate);
    JSON_GET_INT(root, "data_bits",   data_bits_value);
    JSON_GET_INT(root, "stop_bits",   stop_bits_value);
    JSON_GET_INT(root, "flow_ctrl",   flow_ctrl);
    JSON_GET_INT(root, "rx_pin",      rx_pin);
    JSON_GET_INT(root, "tx_pin",      tx_pin);
    JSON_GET_INT(root, "rx_buf_size", rx_buf_size);
    JSON_GET_INT(root, "tx_buf_size", tx_buf_size);

    // ===== 5. 根据协议决定是否由 uart_manager 初始化 UART =====
    // UART0：透传模式，走 USB-CDC，引脚由硬件固定不需配置
    // UART1：Modbus RTU 外设，由 modbus 模块通过 FreeModbus 初始化
    cJSON *proto = cJSON_GetObjectItem(root, "protocol");
    if (proto && proto->valuestring && strcmp(proto->valuestring, "modbus_rtu") == 0) {
        ESP_LOGI(TAG, "Modbus RTU mode: UART will be initialized by modbus module");
        cJSON_Delete(root);
        return ESP_OK;
    }

    // ===== 6. parity 字段：字符串 → ESP-IDF 常量的转换 =====
    // 前端下发 "none"/"even"/"odd"，需映射为 UART_PARITY_xxx 枚举值
    // "none" 时保持默认值 UART_PARITY_DISABLE，无需额外处理
    cJSON *par = cJSON_GetObjectItem(root, "parity");
    if (par && par->valuestring) {
        if (strcmp(par->valuestring, "even") == 0) {
            parity = UART_PARITY_EVEN;
        } else if (strcmp(par->valuestring, "odd") == 0) {
            parity = UART_PARITY_ODD;
        }
    }

    // JSON 解析完毕，立即释放内存，避免不必要的内存占用
    cJSON_Delete(root);

    // ===== 7. 已初始化路径：热更新运行参数 =====
    // 适用于运行时通过下发 JSON 动态修改波特率等配置的场景
    // 不需要重新安装驱动，直接调用 IDF 的 setter 接口即可
    if (s_initialized[port]) {
        uart_set_baudrate(port, baud_rate);
        uart_set_word_length(port, data_bits);
        uart_set_stop_bits(port, stop_bits);
        uart_set_parity(port, parity);
        uart_set_hw_flow_ctrl(port, flow_ctrl, 0);
        ESP_LOGI(TAG, "UART%d hot reconfig OK: baud=%d", port, baud_rate);
        return ESP_OK;
    }
/*
 * 数据位：网页数值转换为 ESP-IDF 枚举。
 */
    switch (data_bits_value)
    {
        case 5:
            data_bits = UART_DATA_5_BITS;
            break;

        case 6:
            data_bits = UART_DATA_6_BITS;
            break;

        case 7:
            data_bits = UART_DATA_7_BITS;
            break;

        case 8:
            data_bits = UART_DATA_8_BITS;
            break;

        default:
            ESP_LOGE(TAG,
                    "Invalid UART data bits: %d",
                    data_bits_value);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
    }


    /*
    * 停止位：网页数值转换为 ESP-IDF 枚举。
    */
    switch (stop_bits_value)
    {
        case 1:
            stop_bits = UART_STOP_BITS_1;
            break;

        case 2:
            stop_bits = UART_STOP_BITS_2;
            break;

        default:
            ESP_LOGE(TAG,
                    "Invalid UART stop bits: %d",
                    stop_bits_value);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
    }
    // ===== 8. 首次初始化路径：完整配置 + 引脚 + 驱动安装 =====

    // 8.1 组装 uart_config_t 结构体
    uart_config_t uart_cfg = {
        .baud_rate  = baud_rate,
        .data_bits  = data_bits,
        .stop_bits  = stop_bits,
        .parity     = parity,
        .flow_ctrl  = flow_ctrl,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 8.2 应用参数配置（波特率、数据位、停止位、校验、流控）
    esp_err_t ret = uart_param_config(port, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "param_config failed: %d", ret);
        return ret;
    }

    // 8.3 配置引脚
    // UART0（USB-CDC）引脚由 ROM 固件/芯片内部固定，重复设置会覆盖系统配置
    // 仅当非 UART0 时才调用 uart_set_pin 配置外设 TX/RX 引脚
    if (port != UART_NUM_0) {
        ret = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set_pin failed: %d", ret);
            return ret;
        }
    }

    // 8.4 安装 UART 驱动（分配 FIFO 缓冲区、中断等资源）
    ret = uart_driver_install(port, rx_buf_size, tx_buf_size, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "driver_install failed: %d", ret);
        return ret;
    }

    // 8.5 标记初始化完成，防止后续调用走热更新路径
    s_initialized[port] = true;
    ESP_LOGI(TAG, "UART%d init OK: baud=%d, data=%d, stop=%d, parity=%d",
             port, baud_rate, data_bits, stop_bits, parity);
    return ESP_OK;
}

esp_err_t uart_manager_deinit(uart_port_t port)
{
    esp_err_t ret = uart_driver_delete(port);
    if (ret == ESP_OK) {
        s_initialized[port] = false;
        ESP_LOGI(TAG, "UART%d deinit OK", port);
    }
    return ret;
}

int uart_manager_send(uart_port_t port, const uint8_t *data, int len)
{
    return uart_write_bytes(port, data, len);
}

int uart_manager_read(uart_port_t port, uint8_t *buf, int max_len, TickType_t timeout)
{
    return uart_read_bytes(port, buf, max_len, timeout);
}