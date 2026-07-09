#include "esp8266.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ==================================================================
 *  Parser — 纯函数，不依赖句柄
 * ================================================================== */

ESP_RespType_t ESP_ParseLine(const char *line, uint16_t len,
                             uint8_t **outData, uint16_t *outLen)
{
    (void)len;

    /* "OK\r\n" — 排除 "SEND OK" */
    if (strstr(line, "OK") && !strstr(line, "SEND OK"))  return ESP_RESP_OK;
    if (strstr(line, "SEND OK"))                         return ESP_RESP_SEND_OK;
    if (strstr(line, "ERROR"))                           return ESP_RESP_ERROR;
    if (strchr(line, '>'))                               return ESP_RESP_SEND_PROMPT;
    if (strstr(line, "busy"))                            return ESP_RESP_BUSY;
    if (strstr(line, "FAIL"))                            return ESP_RESP_FAIL;
    if (strstr(line, "ready"))                           return ESP_RESP_READY;
    if (strstr(line, "CONNECT") && !strstr(line, "FAIL")) return ESP_RESP_CONNECT;
    if (strstr(line, "CLOSED"))                          return ESP_RESP_CLOSED;

    /* "+IPD,<len>:<data>" — 提取 TCP 数据指针和长度 */
    if (strstr(line, "+IPD,")) {
        const char *p = strstr(line, "+IPD,") + 5;
        int dataLen = 0;
        while (*p >= '0' && *p <= '9') { dataLen = dataLen * 10 + (*p - '0'); p++; }
        if (*p == ':') p++;
        if (outData) *outData = (uint8_t *)p;
        if (outLen)  *outLen  = (uint16_t)dataLen;
        return ESP_RESP_IPD;
    }
    return ESP_RESP_NONE;
}

/* ==================================================================
 *  ISR 回调 — 只搬运字节，行边界识别，通知句柄
 *  注意：本函数在 USART IDLE 中断上下文中执行！
 * ================================================================== */

static void ESP_UART_RxISR(UART_HandleTypeDef *huart, uint16_t len)
{
    extern ESP_HandleTypeDef espWifi;  /* App 层全局句柄 */
    ESP_HandleTypeDef *h = &espWifi;

    if (huart != h->huart) return;
    if (len > sizeof(h->rxDmaBuf)) len = sizeof(h->rxDmaBuf);

    for (uint16_t i = 0; i < len; i++) {
        uint8_t ch = h->rxDmaBuf[i];

        if (ch == '\n') {
            h->rxLineBuf[h->rxLinePos] = '\0';
            ESP_RespType_t r = ESP_ParseLine((const char *)h->rxLineBuf,
                                             h->rxLinePos, NULL, NULL);
            h->rxLinePos = 0;

            /* 匹配目标 → 标记命令完成 */
            if (!h->cmdDone && r != ESP_RESP_NONE) {
                h->cmdResult = r;
                h->cmdDone   = true;
            }

            /* 用户要了响应缓冲 → 保存本条原始行 */
            if (h->cmdRespBuf && h->cmdRespSize > 0 && r != ESP_RESP_NONE) {
                uint16_t remain = h->cmdRespSize - strlen(h->cmdRespBuf) - 1;
                if (remain > 0) {
                    strncat(h->cmdRespBuf, (const char *)h->rxLineBuf, remain);
                    strncat(h->cmdRespBuf, "\r\n",
                            h->cmdRespSize - strlen(h->cmdRespBuf) - 1);
                }
            }

            /* 上层的网络事件回调 */
            if (h->onEventCallback) {
                if (r == ESP_RESP_OK)      h->onEventCallback(h, 0);
                if (r == ESP_RESP_CLOSED)  h->onEventCallback(h, ESP_EVENT_TCP_CLOSED);
            }
        } else if (ch != '\r' && h->rxLinePos < sizeof(h->rxLineBuf) - 1) {
            h->rxLineBuf[h->rxLinePos++] = ch;
        }
    }
}

/* ==================================================================
 *  初始化句柄 + 注册回调 + 启动 DMA
 * ================================================================== */

void ESP_Init(ESP_HandleTypeDef *h, UART_HandleTypeDef *huart)
{
    memset(h, 0, sizeof(*h));
    h->huart     = huart;
    h->initState = ESP_STATE_RESET;

    BSP_UART_RegisterRxCallback(huart, ESP_UART_RxISR);
    BSP_UART_StartRx_DMA(huart, h->rxDmaBuf, sizeof(h->rxDmaBuf));
}

/* ==================================================================
 *  发送 AT 命令并阻塞等待结果
 *  FreeRTOS 环境下应改用 TaskNotify，当前为兼容裸机使用轮询
 * ================================================================== */

ESP_RespType_t ESP_SendCmd(ESP_HandleTypeDef *h, const char *cmd,
                           char *response, uint16_t respSize,
                           uint32_t timeout_ms)
{
    h->cmdDone     = false;
    h->cmdResult   = ESP_RESP_NONE;
    h->cmdRespBuf  = response;
    h->cmdRespSize = respSize;
    if (response && respSize > 0) response[0] = '\0';

    /* 发送命令 */
    if (cmd != NULL) {
        BSP_UART_Send(h->huart, (const uint8_t *)cmd, strlen(cmd));
    }

    /* 等待 ISR 回调填充 cmdDone */
    uint32_t t0 = HAL_GetTick();
    while (!h->cmdDone) {
        if (HAL_GetTick() - t0 > timeout_ms) {
            return ESP_RESP_NONE;
        }
    }
    return h->cmdResult;
}

/* ==================================================================
 *  初始化状态机 — App 层循环调用直至返回 true
 * ================================================================== */

bool ESP_RunInitStateMachine(ESP_HandleTypeDef *h)
{
    ESP_RespType_t r;

    switch (h->initState) {
    case ESP_STATE_RESET:
        r = ESP_SendCmd(h, "AT\r\n", NULL, 0, 3000);
        if (r == ESP_RESP_OK) {
            h->initState = ESP_STATE_ECHO_OFF;
            h->initRetry = 0;
        } else if (++h->initRetry > 3) {
            h->initState = ESP_STATE_ERROR;
        }
        break;

    case ESP_STATE_ECHO_OFF:
        r = ESP_SendCmd(h, "ATE0\r\n", NULL, 0, 2000);
        h->initState = (r == ESP_RESP_OK) ? ESP_STATE_STA_MODE : ESP_STATE_ERROR;
        break;

    case ESP_STATE_STA_MODE:
        r = ESP_SendCmd(h, "AT+CWMODE=1\r\n", NULL, 0, 2000);
        h->initState = (r == ESP_RESP_OK) ? ESP_STATE_READY : ESP_STATE_ERROR;
        break;

    case ESP_STATE_READY:
        return true;

    case ESP_STATE_ERROR:
    default:
        break;
    }
    return false;
}

/* ==================================================================
 *  便捷封装
 * ================================================================== */

bool ESP_ConnectWiFi(ESP_HandleTypeDef *h, const char *ssid, const char *pwd)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    ESP_RespType_t r = ESP_SendCmd(h, cmd, NULL, 0, 15000);
    if (r == ESP_RESP_OK && h->onEventCallback) {
        h->onEventCallback(h, ESP_EVENT_WIFI_CONNECTED);
    }
    return (r == ESP_RESP_OK);
}

bool ESP_GetIP(ESP_HandleTypeDef *h, char *ipBuf, uint16_t bufSize)
{
    ESP_RespType_t r = ESP_SendCmd(h, "AT+CIFSR\r\n", ipBuf, bufSize, 5000);
    if (r == ESP_RESP_OK && h->onEventCallback) {
        h->onEventCallback(h, ESP_EVENT_GOT_IP);
    }
    return (r == ESP_RESP_OK);
}

bool ESP_TCPConnect(ESP_HandleTypeDef *h, const char *host, uint16_t port)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", host, port);

    /* 第一步：发命令，等 OK（命令语法正确） */
    ESP_RespType_t r = ESP_SendCmd(h, cmd, NULL, 0, 10000);
    if (r != ESP_RESP_OK) return false;

    /* 第二步：等实际的 CONNECT 或 ERROR/CLOSED（ESP 异步返回） */
    r = ESP_SendCmd(h, NULL, NULL, 0, 15000);
    if (r == ESP_RESP_CONNECT || r == ESP_RESP_OK) {
        if (h->onEventCallback) h->onEventCallback(h, ESP_EVENT_TCP_CONNECTED);
        return true;
    }
    return false;
}

bool ESP_TCPSend(ESP_HandleTypeDef *h, const uint8_t *data, uint16_t len)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u\r\n", len);

    /* 等 > 提示符 */
    ESP_RespType_t r = ESP_SendCmd(h, cmd, NULL, 0, 5000);
    if (r != ESP_RESP_SEND_PROMPT) return false;

    /* 发实际数据 */
    BSP_UART_Send(h->huart, data, len);

    /* 等 SEND OK */
    r = ESP_SendCmd(h, NULL, NULL, 0, 10000);
    return (r == ESP_RESP_SEND_OK);
}

void ESP_RegisterEventCallback(ESP_HandleTypeDef *h,
                               void (*cb)(ESP_HandleTypeDef *h, uint32_t event))
{
    h->onEventCallback = cb;
}

/* espWifi 在 App 层 (main.c) 中定义，此处通过 ISR 内 extern 引用 */
