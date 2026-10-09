#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================
 * Modbus 硬件参数
 * ========================= */

#define MODBUS_UART_PORT        UART_NUM_1
#define MODBUS_BAUDRATE         9600
#define MODBUS_TX_GPIO          6
#define MODBUS_RX_GPIO          7
#define MODBUS_RTS_GPIO         10

/* =========================
 * Modbus 协议限制
 * ========================= */

#define MODBUS_MAX_POINTS       8
#define MODBUS_MAX_REGISTERS    64
#define MODBUS_TOPIC_MAX_LEN    64

#define MODBUS_WRITE_RESULT_TOPIC "test/write_result"

/* UART 校验方式 */
#define MB_PARITY_EVEN UART_PARITY_EVEN
#define MB_PARITY_ODD  UART_PARITY_ODD

/* Modbus 标准功能码 */
#define MB_FUNC_READ_COILS               0x01
#define MB_FUNC_READ_DISCRETE_INPUTS     0x02
#define MB_FUNC_READ_HOLDING_REGISTER    0x03
#define MB_FUNC_READ_INPUT_REGISTER      0x04
#define MB_FUNC_WRITE_REGISTER           0x06
/* =========================
 * 单个 Modbus 采集点
 * ========================= */

typedef struct
{
    uint8_t  slave_id;
    uint8_t  function_code;
    uint16_t start_address;
    uint16_t length;

    char mqtt_topic[MODBUS_TOPIC_MAX_LEN];
} ModbusPoint_t;

/* =========================
 * Modbus 总配置
 * ========================= */

typedef struct
{
    uint8_t  uart_port;
    uint32_t baudrate;
    uint8_t  parity;
    int      tx_pin;
    int      rx_pin;
    int      rts_pin;

    uint32_t poll_interval_ms;
    uint32_t timeout_ms;

    uint8_t point_count;
    ModbusPoint_t points[MODBUS_MAX_POINTS];
} ModbusConfig_t;

/* =========================
 * API
 * ========================= */

esp_err_t modbus_load_config_from_nvs(void);
void      modbus_config_print(void);
const ModbusConfig_t *modbus_get_config(void);
esp_err_t modbus_init(void);
esp_err_t modbus_start_poll_task(void);
esp_err_t modbus_stop_poll_task(void);
esp_err_t modbus_init_local(void);
/* 接收一条写单个保持寄存器的命令，交由 Modbus 任务执行 */
esp_err_t modbus_queue_write_register(uint8_t slave_id,
                                      uint16_t address,
                                      uint16_t value);

/* ============================================================
 * MQTT 下发的 Modbus 写命令
 * 队列保存具体数值，不保存 MQTT 回调里的临时数据指针。
 * ============================================================ */
typedef struct
{
    uint8_t slave_id;
    uint16_t address;
    uint16_t value;
} ModbusWriteCommand_t;                                     
#ifdef __cplusplus
}
#endif

#endif