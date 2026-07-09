#include "bsp_uart.h"

/* ---- 注册的接收回调（每个 UART 一个） ---- */
static BSP_UART_RxCallback rxCallback1 = NULL;
static BSP_UART_RxCallback rxCallback2 = NULL;

/* ---- DMA 缓冲区信息（IDLE 中断后自动重启用） ---- */
static uint8_t *dmaBuf1 = NULL, *dmaBuf2 = NULL;
static uint16_t dmaSize1 = 0,  dmaSize2 = 0;

/* ---- 发送（阻塞） ---- */
void BSP_UART_Send(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(huart, (uint8_t *)data, len, HAL_MAX_DELAY);
}

/* ---- 注册接收回调 ---- */
void BSP_UART_RegisterRxCallback(UART_HandleTypeDef *huart, BSP_UART_RxCallback cb)
{
    if (huart->Instance == USART1) {
        rxCallback1 = cb;
    } else if (huart->Instance == USART2) {
        rxCallback2 = cb;
    }
}

/* ---- 启动 DMA 接收（同时记录缓冲区信息以便自动重启） ---- */
void BSP_UART_StartRx_DMA(UART_HandleTypeDef *huart, uint8_t *buf, uint16_t bufSize)
{
    if (huart->Instance == USART1) {
        dmaBuf1  = buf;
        dmaSize1 = bufSize;
    } else if (huart->Instance == USART2) {
        dmaBuf2  = buf;
        dmaSize2 = bufSize;
    }
    HAL_UARTEx_ReceiveToIdle_DMA(huart, buf, bufSize);
}

/* ---- USART IDLE 中断处理（stm32f1xx_it.c 中调用） ---- */
void BSP_UART_HandleIdle(UART_HandleTypeDef *huart, uint16_t rxLen)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_14);  /* LED 确认 ISR 到达 */
    BSP_UART_RxCallback cb  = NULL;
    uint8_t          *restartBuf  = NULL;
    uint16_t          restartSize = 0;

    if (huart->Instance == USART1) {
        cb          = rxCallback1;
        restartBuf  = dmaBuf1;
        restartSize = dmaSize1;
    } else if (huart->Instance == USART2) {
        cb          = rxCallback2;
        restartBuf  = dmaBuf2;
        restartSize = dmaSize2;
    }
    if (cb) {
        cb(huart, rxLen);
    }
    /* 关键：HAL_UARTEx_ReceiveToIdle_DMA 要求 RxState == READY，
     * 但 IDLE 事件后 RxState 仍为 BUSY_RX，必须先复位再重启 */
    if (restartBuf && restartSize > 0) {
        huart->RxState = HAL_UART_STATE_READY;
        HAL_UARTEx_ReceiveToIdle_DMA(huart, restartBuf, restartSize);
    }
}
