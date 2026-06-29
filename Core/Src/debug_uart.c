#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "usart.h"
#include "debug_uart.h"

#define UART_TX_QUEUE_LEN   16    /* queue depth: buffer 16 messages */
#define UART_TX_BUF_SIZE    64    /* max bytes per message */

static QueueHandle_t     xUartTxQueue      = NULL;
static SemaphoreHandle_t xTxDmaSemaphore   = NULL;  /* DMA TX complete signal */

typedef struct {
    char data[UART_TX_BUF_SIZE];
    uint16_t len;
} UartTxMsg_t;

/* forward declaration */
void vUartTxTask(void *pvParameters);

/* ===== Init debug UART (create queue + TX task + DMA semaphore) ===== */
void DebugUART_Init(void)
{
    if (xUartTxQueue != NULL) return;  /* already initialized */

    /* Create binary semaphore for DMA TX complete signaling */
    xTxDmaSemaphore = xSemaphoreCreateBinary();
    if (xTxDmaSemaphore == NULL) return;

    xUartTxQueue = xQueueCreate(UART_TX_QUEUE_LEN, sizeof(UartTxMsg_t));
    if (xUartTxQueue != NULL) {
        xTaskCreate(vUartTxTask, "UartTX", 256, NULL, 1, NULL);
    }
}

/* ===== _write: printf redirect to UART ===== */
int _write(int file, char *ptr, int len)
{
    (void)file;

    /* Before scheduler starts, fallback to blocking polling transmit */
    if (xUartTxQueue == NULL) {
        HAL_UART_Transmit_DMA(&huart1, (uint8_t *)ptr, len);
        return len;
    }

    /* Push data chunks into queue */
    size_t offset = 0;
    while (offset < (size_t)len) {
        UartTxMsg_t msg;
        size_t chunk = (size_t)len - offset;
        if (chunk > UART_TX_BUF_SIZE) chunk = UART_TX_BUF_SIZE;
        memcpy(msg.data, ptr + offset, chunk);
        msg.len = (uint16_t)chunk;

        if (xQueueSend(xUartTxQueue, &msg, pdMS_TO_TICKS(10)) != pdPASS) {
            break;  /* drop remaining data if queue full */
        }
        offset += chunk;
    }
    return len;
}

/* ===== UART TX task: dequeues messages, sends via DMA ===== */
void vUartTxTask(void *pvParameters)
{
    (void)pvParameters;
    UartTxMsg_t msg;

    while (1) {
        /* Block until a message arrives */
        if (xQueueReceive(xUartTxQueue, &msg, portMAX_DELAY) == pdPASS) {
            /* Start DMA transfer (non-blocking) */
            if (HAL_UART_Transmit_DMA(&huart1, (uint8_t *)msg.data, msg.len) == HAL_OK) {
                /* Wait for DMA to finish before sending next message */
                xSemaphoreTake(xTxDmaSemaphore, portMAX_DELAY);
            }
        }
    }
}

/* ===== HAL callback: DMA TX complete → signal semaphore from ISR ===== */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(xTxDmaSemaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* ==================================================================
 *  DMA RX (USART1 IDLE detection)
 *  Receives variable-length data from host PC.
 *  HAL_UARTEx_ReceiveToIdle_DMA → IDLE interrupt → RxEventCallback
 * ================================================================== */

#define UART_RX_BUF_SIZE    256

static uint8_t uart_rx_buf[UART_RX_BUF_SIZE];

/* Start DMA reception with IDLE detection */
void DebugUART_StartRx(void)
{
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uart_rx_buf, UART_RX_BUF_SIZE);
}

/* HAL callback: DMA RX complete (full buffer or IDLE detected) */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1 && Size > 0) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        /* Echo received data back as-is (proof of RX functionality) */
        for (uint16_t i = 0; i < Size; ) {
            UartTxMsg_t msg;
            uint16_t chunk = Size - i;
            if (chunk > UART_TX_BUF_SIZE) chunk = UART_TX_BUF_SIZE;
            memcpy(msg.data, (char *)&uart_rx_buf[i], chunk);
            msg.len = chunk;
            xQueueSendFromISR(xUartTxQueue, &msg, &xHigherPriorityTaskWoken);
            i += chunk;
        }

        /* Restart DMA reception for next frame */
        HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uart_rx_buf, UART_RX_BUF_SIZE);

        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
