#ifndef __ESP8266_H__
#define __ESP8266_H__

#include "main.h"
#include "bsp_uart.h"
#include <stdbool.h>

/* ==================================================================
 *  响应类型（Parser 层的输出）
 * ================================================================== */
typedef enum {
    ESP_RESP_NONE       = 0,   /* 未识别 / 不完整行 */
    ESP_RESP_OK,               /* "OK\r\n" */
    ESP_RESP_ERROR,            /* "ERROR\r\n" */
    ESP_RESP_SEND_OK,          /* "SEND OK\r\n" */
    ESP_RESP_SEND_PROMPT,      /* ">" 发送提示符 */
    ESP_RESP_BUSY,             /* "busy p..." */
    ESP_RESP_CONNECT,          /* "CONNECT\r\n" / 连接成功 */
    ESP_RESP_CLOSED,           /* "CLOSED\r\n" */
    ESP_RESP_IPD,              /* "+IPD,len:data" TCP 数据 */
    ESP_RESP_READY,            /* "ready\r\n" 上电就绪 */
    ESP_RESP_FAIL,             /* "FAIL\r\n" */
} ESP_RespType_t;

/* ==================================================================
 *  初始化状态机
 * ================================================================== */
typedef enum {
    ESP_STATE_RESET,            /* 等待上电 ready */
    ESP_STATE_AT_CHECK,         /* AT 检测 */
    ESP_STATE_ECHO_OFF,         /* 关回显 ATE0 */
    ESP_STATE_STA_MODE,         /* AT+CWMODE=1 */
    ESP_STATE_WIFI_CONNECTING,  /* AT+CWJAP 连接中 */
    ESP_STATE_GET_IP,           /* AT+CIFSR 查询 IP */
    ESP_STATE_READY,            /* 全部就绪 */
    ESP_STATE_ERROR,            /* 某步失败，等待重试 */
} ESP_InitState_t;

/* ==================================================================
 *  网络事件位（供 App 层 EventGroup 使用）
 * ================================================================== */
#define ESP_EVENT_WIFI_CONNECTED    (1 << 0)
#define ESP_EVENT_GOT_IP            (1 << 1)
#define ESP_EVENT_TCP_CONNECTED     (1 << 2)
#define ESP_EVENT_TCP_CLOSED        (1 << 3)

/* ==================================================================
 *  驱动句柄（封装所有状态，不写死 UART）
 * ================================================================== */
typedef struct ESP_Handle {
    UART_HandleTypeDef   *huart;          /* 不写死 huart2       */
    uint8_t               rxDmaBuf[256];  /* DMA 接收缓冲        */
    uint8_t               rxLineBuf[128]; /* 行组装缓冲区        */
    uint16_t              rxLinePos;      /* 当前行缓冲写入位置  */

    /* ---- 命令同步 ---- */
    volatile bool         cmdDone;        /* 异步等待完成标志    */
    volatile ESP_RespType_t cmdResult;     /* 本次命令的响应类型  */
    char                 *cmdRespBuf;     /* 用户传入的响应缓冲  */
    uint16_t              cmdRespSize;    /* 响应缓冲大小        */

    /* ---- 初始化状态机 ---- */
    ESP_InitState_t       initState;
    uint8_t               initRetry;

    /* ---- 上层回调（App 层注册，用于通知网络事件） ---- */
    void (*onEventCallback)(struct ESP_Handle *h, uint32_t event);
} ESP_HandleTypeDef;

/* ==================================================================
 *  API
 * ================================================================== */

/* 初始化句柄 + 注册回调 + 启动 DMA 接收 */
void ESP_Init(ESP_HandleTypeDef *h, UART_HandleTypeDef *huart);

/* 异步发送 AT 命令，阻塞等待 cmdResult，超时返回 ESP_TIMEOUT */
ESP_RespType_t ESP_SendCmd(ESP_HandleTypeDef *h, const char *cmd,
                           char *response, uint16_t respSize,
                           uint32_t timeout_ms);

/* 运行一次初始化状态机，返回 true 表示已进入 READY 状态 */
bool ESP_RunInitStateMachine(ESP_HandleTypeDef *h);

/* 便捷封装 */
bool ESP_ConnectWiFi(ESP_HandleTypeDef *h, const char *ssid, const char *pwd);
bool ESP_GetIP(ESP_HandleTypeDef *h, char *ipBuf, uint16_t bufSize);
bool ESP_TCPConnect(ESP_HandleTypeDef *h, const char *host, uint16_t port);
bool ESP_TCPSend(ESP_HandleTypeDef *h, const uint8_t *data, uint16_t len);

/* 注册网络事件回调 */
void ESP_RegisterEventCallback(ESP_HandleTypeDef *h,
                               void (*cb)(ESP_HandleTypeDef *h, uint32_t event));

/* ==================================================================
 *  Parser（内部使用，也可被上层直接调用）
 * ================================================================== */

/* 喂一行原始数据，返回解析结果类型。+IPD 的 data 存入 *outData */
ESP_RespType_t ESP_ParseLine(const char *line, uint16_t len,
                             uint8_t **outData, uint16_t *outLen);

#endif /* __ESP8266_H__ */
