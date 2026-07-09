#ifndef __BSP_UART_H__
#define __BSP_UART_H__

#include "main.h"

/* ---- 接收回调：IDLE 中断时调用，参数为本次收到的字节数 ---- */
typedef void (*BSP_UART_RxCallback)(UART_HandleTypeDef *huart, uint16_t len);

/* ---- 发送（阻塞，用于 AT 命令等短数据） ---- */
void BSP_UART_Send(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t len);

/* ---- 注册接收回调（IDLE 中断时调用） ---- */
void BSP_UART_RegisterRxCallback(UART_HandleTypeDef *huart, BSP_UART_RxCallback cb);

/* ---- 启动 DMA 接收（注册回调后调用，数据写入 buf，最大 bufSize 字节） ---- */
void BSP_UART_StartRx_DMA(UART_HandleTypeDef *huart, uint8_t *buf, uint16_t bufSize);

#endif /* __BSP_UART_H__ */
