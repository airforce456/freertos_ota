/**
 * 文件名： esp8266.c
 * 说明：   ESP8266 简单驱动（HAL 移植版）
 *         中断逐字节接收 + 轮询等待模式，匹配 NET 包 onenet.c 调用约定
 */
#include "esp8266.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ---- 全局接收缓冲（ISR 填充） ---- */
unsigned char esp8266_buf[512];
volatile unsigned short esp8266_cnt    = 0;
volatile unsigned short esp8266_cntPre = 0;
static uint8_t esp8266_rxByte;    /* HAL_UART_Receive_IT 单字节接收缓冲 */

/* ---- 内部延时（阻塞，10ms 粒度，调度器启动前可用） ---- */
static void DelayXms(unsigned short ms)
{
    HAL_Delay(ms);
}

/* ---- UART 发送（阻塞） ---- */
static void Usart_SendString(UART_HandleTypeDef *huart, unsigned char *data, unsigned short len)
{
    HAL_UART_Transmit(huart, data, len, HAL_MAX_DELAY);
}

/* ==================================================================
 *  ESP8266_Clear — 清空接收缓冲区
 * ================================================================== */
void ESP8266_Clear(void)
{
    memset(esp8266_buf, 0, sizeof(esp8266_buf));
    esp8266_cnt = 0;
}

/* ==================================================================
 *  ESP8266_WaitRecive — 检测是否收到完整一帧
 *  原理：中断把字节写入 esp8266_buf 并递增 esp8266_cnt，
 *        当 esp8266_cnt 不再增长时认为接收完毕。
 * ================================================================== */
static _Bool ESP8266_WaitRecive(void)
{
    if (esp8266_cnt == 0)
        return 1;   /* 还没收到数据 */

    if (esp8266_cnt == esp8266_cntPre) {
        esp8266_cnt = 0;    /* 计数稳定 → 接收完毕 */
        return 0;           /* REV_OK */
    }

    esp8266_cntPre = esp8266_cnt;
    return 1;               /* REV_WAIT */
}

/* ==================================================================
 *  ESP8266_SendCmd — 发送 AT 命令，阻塞等待回复
 *  返回 0=成功（回复中包含 res） 1=超时
 * ================================================================== */
_Bool ESP8266_SendCmd(char *cmd, char *res)
{
    unsigned char timeOut = 200;   /* 200 × 10ms = 2 秒超时 */

    Usart_SendString(&huart2, (unsigned char *)cmd, strlen(cmd));

    while (timeOut--) {
        if (ESP8266_WaitRecive() == 0) {
            if (strstr((const char *)esp8266_buf, res) != NULL) {
                ESP8266_Clear();
                return 0;   /* 成功 */
            }
        }
        DelayXms(10);
    }
    return 1;   /* 超时 */
}

/* ==================================================================
 *  ESP8266_SendData — 发送 TCP 数据
 *  先发 AT+CIPSEND=len，等 '>' 提示符后发送原始数据
 * ================================================================== */
void ESP8266_SendData(unsigned char *data, unsigned short len)
{
    char cmdBuf[32];

    ESP8266_Clear();
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+CIPSEND=%d\r\n", len);
    if (ESP8266_SendCmd(cmdBuf, ">") == 0) {
        Usart_SendString(&huart2, data, len);
    }
}

/* ==================================================================
 *  ESP8266_GetIPD — 等待 +IPD TCP 传入数据
 *  返回指向数据内容的指针（esp8266_buf 内部偏移），超时返回 NULL
 * ================================================================== */
unsigned char *ESP8266_GetIPD(unsigned short timeOut)
{
    char *ptrIPD = NULL;

    do {
        if (ESP8266_WaitRecive() == 0) {
            ptrIPD = strstr((char *)esp8266_buf, "IPD,");
            if (ptrIPD == NULL) {
                /* IPD 可能延迟，继续等 */
            } else {
                ptrIPD = strchr(ptrIPD, ':');
                if (ptrIPD != NULL) {
                    ptrIPD++;
                    return (unsigned char *)ptrIPD;
                }
                return NULL;
            }
        }
        DelayXms(5);
    } while (timeOut--);

    return NULL;
}

/* ==================================================================
 *  ESP8266_Init — 上电初始化（阻塞，直到连上 WiFi）
 *  流程：AT 检测 → 关回显 → STA 模式 → DHCP → 连 WiFi
 * ================================================================== */
void ESP8266_Init(void)
{
    char cmdBuf[128];

    ESP8266_Clear();

    printf("[ESP] 1. AT\r\n");
    while (ESP8266_SendCmd("AT\r\n", "OK"))
        DelayXms(500);

    printf("[ESP] 2. ATE0\r\n");
    while (ESP8266_SendCmd("ATE0\r\n", "OK"))
        DelayXms(500);

    printf("[ESP] 3. CWMODE=1\r\n");
    while (ESP8266_SendCmd("AT+CWMODE=1\r\n", "OK"))
        DelayXms(500);

    printf("[ESP] 4. CWDHCP\r\n");
    while (ESP8266_SendCmd("AT+CWDHCP=1,1\r\n", "OK"))
        DelayXms(500);

    printf("[ESP] 5. CWJAP\r\n");
    snprintf(cmdBuf, sizeof(cmdBuf), "AT+CWJAP=\"%s\",\"%s\"\r\n",
             ESP8266_WIFI_SSID, ESP8266_WIFI_PWD);
    while (ESP8266_SendCmd(cmdBuf, "GOT IP"))
        DelayXms(500);

    printf("[ESP] Init OK\r\n");
}

/* ==================================================================
 *  HAL_UART_RxCpltCallback — USART2 每收到一个字节的回调
 * ================================================================== */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        if (esp8266_cnt >= sizeof(esp8266_buf))
            esp8266_cnt = 0;
        esp8266_buf[esp8266_cnt++] = esp8266_rxByte;
        /* 继续接收下一个字节 */
        HAL_UART_Receive_IT(&huart2, &esp8266_rxByte, 1);
    }
}

/* ==================================================================
 *  ESP8266_StartRx — 启动中断接收（MX_USART2_UART_Init 之后调用）
 * ================================================================== */
void ESP8266_StartRx(void)
{
    HAL_UART_Receive_IT(&huart2, &esp8266_rxByte, 1);
}
