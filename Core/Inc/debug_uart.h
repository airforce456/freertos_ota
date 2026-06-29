#ifndef __DEBUG_UART_H__
#define __DEBUG_UART_H__

#include "FreeRTOS.h"
#include "queue.h"

int _write(int file, char *ptr, int len);
void DebugUART_Init(void);
void DebugUART_StartRx(void);

#endif /*__ DEBUG_UART_H__ */
