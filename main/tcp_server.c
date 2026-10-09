#include "tcp_server.h"
#include "uart.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "cJSON.h"
#include "save_nvs.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t g_clients_mutex = NULL;
static const char *TAG = "tcp_server";

#define NVS_NAMESPACE  "tcp_cfg"
#define NVS_KEY_PORT   "tcp_port"
#define DEFAULT_PORT   8080

// 客户端链表节点
typedef struct client_node {
    int sock;
    struct client_node *next;
} client_node_t;

static volatile bool g_running = false;
static int g_listen_sock = -1;
static uart_port_t g_uart_port;

static client_node_t *g_head = NULL;            // 客户端链表头
static QueueHandle_t g_uart_queue = NULL;       // UART → TCP 数据队列

static TaskHandle_t g_accept_handle = NULL;
static TaskHandle_t g_forward_handle = NULL;
static TaskHandle_t g_uart_read_handle = NULL;

#define UART_QUEUE_SIZE 8    // 队列深度
#define UART_BUF_SIZE   256   // 每包最大长度

// UART 数据包
typedef struct {
    int len;
    uint8_t data[UART_BUF_SIZE];
} uart_packet_t;

// ============================
// 链表操作
// ============================
static void add_client(int sock)
{

         // 未初始化锁时，不操作链表
    if (g_clients_mutex == NULL)
    {
        ESP_LOGE(TAG, "Clients mutex is not initialized");
        return;
    }

    // 获得锁后，才允许访问链表
    xSemaphoreTake(g_clients_mutex, portMAX_DELAY);


    client_node_t *node = calloc(1, sizeof(client_node_t));
    node->sock = sock;
    node->next = g_head;
    g_head = node;
    ESP_LOGI(TAG, "Client added, sock=%d", sock);


    
    // 未找到对应客户端，也要释放锁
    xSemaphoreGive(g_clients_mutex);
}

static void remove_client(int sock)
{
     // 未初始化锁时，不操作链表
    if (g_clients_mutex == NULL)
    {
        ESP_LOGE(TAG, "Clients mutex is not initialized");
        return;
    }

    // 获得锁后，才允许访问链表
    xSemaphoreTake(g_clients_mutex, portMAX_DELAY);


    client_node_t **pp = &g_head;
    while (*pp) {
        if ((*pp)->sock == sock) {
            client_node_t *del = *pp;
            *pp = del->next;
            close(del->sock);
            free(del);
            ESP_LOGI(TAG, "Client removed, sock=%d", sock);
            break;
        }
        pp = &(*pp)->next;
    }
         xSemaphoreGive(g_clients_mutex);
}

// ============================
// 构建 select 的 fd_set
// ============================
static int build_fd_set(fd_set *fds)
{
    FD_ZERO(fds);
    int max_fd = -1;

    client_node_t *p = g_head;
    while (p) {
        FD_SET(p->sock, fds);
        if (p->sock > max_fd) max_fd = p->sock;
        p = p->next;
    }
    return max_fd;
}

// ============================
// 群发数据给所有客户端
// ============================
static void broadcast_to_all(const uint8_t *data, int len)
{
    client_node_t *p = g_head;
    client_node_t *prev = NULL;

    while (p) {
        client_node_t *next = p->next;
        int ret = send(p->sock, data, len, 0);
        ESP_LOGI(TAG,
         "TCP send: sock=%d, len=%d, ret=%d",
         p->sock,
         len,
         ret);
        if (ret < 0) {
            // 发送失败，移除客户端
            if (prev) prev->next = next;
            else     g_head = next;
            close(p->sock);
            free(p);
        } else {
            prev = p;
        }
        p = next;
    }
}

// ============================
// Accept 任务：等待客户端连接
// ============================
static void tcp_accept_task(void *arg)
{
    ESP_LOGI(TAG, "tcp_accept_task started");
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client = accept(g_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            if (!g_running) break;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        add_client(client);
    }

    ESP_LOGI(TAG, "tcp_accept_task exit");
    vTaskDelete(NULL);
}

// ============================
// UART 读任务：收数据 → 入队
// ============================
static void uart_read_task(void *arg)
{
    while (1) {
        uart_packet_t pkt;
        int len = uart_manager_read(g_uart_port, pkt.data, UART_BUF_SIZE,
                                     pdMS_TO_TICKS(20));
        if (len > 0) {
            ESP_LOGI(TAG, "UART RX: %d bytes", len);
            pkt.len = len;
            BaseType_t result = xQueueSend(
                g_uart_queue,
                &pkt,
                0
             );
            ESP_LOGI(TAG,"UART queue send result: %d",result);
        }
    }
}

// ============================
// 主转发任务：select 监听所有客户端
// ============================
static void forward_task(void *arg)
{
    fd_set read_fds;

    while (g_running) 
    {
        int max_fd = build_fd_set(&read_fds);
        if (max_fd < 0) 
        {
            // 没有客户端，等一等再检查
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 }; // 200ms 超时
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready < 0) 
        {
            if (!g_running) break;
            continue;
        }

        // === select 返回：处理可读的客户端 ===
        if (ready > 0) 
        {
            client_node_t *p = g_head;

            while (p)
            {
                client_node_t *next = p->next;

                if (FD_ISSET(p->sock, &read_fds))
                {
                    uint8_t buf[512];
                    int len = recv(p->sock, buf, sizeof(buf), 0);

                    if (len <= 0)
                    {
                        // 客户端断开
                        remove_client(p->sock);
                    } 
                    else
                    {
                        // TCP 收到 → 发到 UART
                        uart_manager_send(g_uart_port, buf, len);
                    }
                } 
                p = next;
            }
        }

        // === 检查 UART 队列：有数据 → 群发所有客户端 ===
        uart_packet_t pkt;
        while (xQueueReceive(g_uart_queue, &pkt, 0) == pdTRUE) 
        {
            ESP_LOGI(TAG, "UART queue -> TCP: %d bytes",pkt.len);
            broadcast_to_all(pkt.data, pkt.len);
        }
    }

    ESP_LOGI(TAG, "Forward task exit");
    vTaskDelete(NULL);
}

// ============================
// 启停
// ============================

esp_err_t tcp_server_set_port(uint16_t port)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u16(handle, NVS_KEY_PORT, port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write port failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Port %u saved to NVS", port);
    return ESP_OK;
}

static uint16_t load_tcp_server_port(void)
{
    uint16_t port = DEFAULT_PORT;

    // 读取网页保存的 TCP JSON 配置
    cJSON *tcp_config = load_tcp_config();
    if (tcp_config == NULL)
    {
        ESP_LOGW(TAG, "No TCP config, use default port %u", DEFAULT_PORT);
        return DEFAULT_PORT;
    }

    // 读取监听端口数组
    cJSON *listen_ports = cJSON_GetObjectItemCaseSensitive(
        tcp_config,
        "listen_ports"
    );

    if (!cJSON_IsArray(listen_ports))
    {
        ESP_LOGW(TAG, "Invalid listen_ports, use default port %u", DEFAULT_PORT);
        cJSON_Delete(tcp_config);
        return DEFAULT_PORT;
    }

    // 当前程序只使用第一个监听端口
    cJSON *first_port = cJSON_GetArrayItem(listen_ports, 0);

    if (cJSON_IsNumber(first_port) &&
        first_port->valueint >= 1 &&
        first_port->valueint <= 65535)
    {
        port = (uint16_t)first_port->valueint;
    }
    else
    {
        ESP_LOGW(TAG, "Invalid TCP port, use default port %u", DEFAULT_PORT);
    }

    cJSON_Delete(tcp_config);

    ESP_LOGI(TAG, "Read TCP port %u from config", port);
    return port;
}

esp_err_t tcp_server_start(uart_port_t uart_port)
{
    if (g_running) {
        ESP_LOGW(TAG, "TCP server already running");
        return ESP_OK;
    }

    g_uart_port = uart_port;

    uint16_t port = load_tcp_server_port();// 从 NVS 或网页配置读取端口

    g_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(g_listen_sock);
        return ESP_FAIL;
    }

    if (listen(g_listen_sock, 5) < 0) {
        ESP_LOGE(TAG, "listen failed");
        close(g_listen_sock);
        return ESP_FAIL;
    }

    g_uart_queue = xQueueCreate(UART_QUEUE_SIZE, sizeof(uart_packet_t));

    g_running = true;

// 创建客户端链表的互斥锁
if (g_clients_mutex == NULL)
{
    g_clients_mutex = xSemaphoreCreateMutex();
    if (g_clients_mutex == NULL)
    {
        ESP_LOGE(TAG, "Create clients mutex failed");
        close(g_listen_sock);
        g_listen_sock = -1;
        return ESP_ERR_NO_MEM;
    }
}

BaseType_t ret1 = xTaskCreate(
    tcp_accept_task,
    "tcp_accept",
    4096,
    NULL,
    4,
    &g_accept_handle
);

BaseType_t ret2 = xTaskCreate(
    forward_task,
    "tcp_fwd",
    4096,
    NULL,
    5,
    &g_forward_handle
);

BaseType_t ret3 = xTaskCreate(
    uart_read_task,
    "uart_read",
    4096,
    NULL,
    4,
    &g_uart_read_handle
);

ESP_LOGI(TAG, "task create result: accept=%ld, forward=%ld, uart=%ld",
         (long)ret1, (long)ret2, (long)ret3);

    ESP_LOGI(TAG, "TCP Server started on port %d", port);
    return ESP_OK;
}

esp_err_t tcp_server_stop(void)
{
    g_running = false;

    // 先关 listen socket，让 accept() 报错退出
    if (g_listen_sock >= 0) {
        close(g_listen_sock);
        g_listen_sock = -1;
    }

    // 他杀三个任务
    if (g_accept_handle)    { vTaskDelete(g_accept_handle);    g_accept_handle = NULL; }
    if (g_forward_handle)   { vTaskDelete(g_forward_handle);   g_forward_handle = NULL; }
    if (g_uart_read_handle) { vTaskDelete(g_uart_read_handle); g_uart_read_handle = NULL; }

    // 清空链表
    client_node_t *p = g_head;
    while (p) {
        client_node_t *next = p->next;
        close(p->sock);
        free(p);
        p = next;
    }
    g_head = NULL;

    if (g_uart_queue) {
        vQueueDelete(g_uart_queue);
        g_uart_queue = NULL;
    }

    ESP_LOGI(TAG, "TCP Server stopped");
    return ESP_OK;
}

bool tcp_server_is_running(void)
{
    return g_running;
}