#ifndef __ESP8266_H__
#define __ESP8266_H__

#include "main.h"

/* 全局接收缓冲区（ISR 逐字节填充） */
extern unsigned char esp8266_buf[512];

/* WiFi 配置 */
#ifndef ESP8266_WIFI_SSID
#define ESP8266_WIFI_SSID   "Xiaomi 13"
#endif
#ifndef ESP8266_WIFI_PWD
#define ESP8266_WIFI_PWD    "123456789"
#endif

/* ---- API（匹配 NET 包 onenet.c 的调用约定） ---- */

/* 上电初始化：AT 检测 → 关回显 → STA 模式 → 连 WiFi（阻塞，直到连上） */
void ESP8266_Init(void);

/* 清空接收缓冲区 */
void ESP8266_Clear(void);

/* 发送 AT 命令，阻塞等待回复中包含 res 字符串，返回 0=成功 1=超时 */
_Bool ESP8266_SendCmd(char *cmd, char *res);

/* 发送 TCP 数据（先发 AT+CIPSEND，等 '>' 后发原始数据） */
void ESP8266_SendData(unsigned char *data, unsigned short len);

/* 等待 TCP 传入数据（+IPD），返回指向数据内容的指针，超时返回 NULL
 * timeOut：超时次数，每次约 10ms */
unsigned char *ESP8266_GetIPD(unsigned short timeOut);

/* 启动 USART2 中断逐字节接收（MX_USART2_UART_Init 之后调用一次） */
void ESP8266_StartRx(void);

#endif /* __ESP8266_H__ */
