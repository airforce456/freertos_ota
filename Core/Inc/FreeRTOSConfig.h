#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#define vPortSVCHandler     SVC_Handler

#define xPortPendSVHandler  PendSV_Handler

#define xPortSysTickHandler SysTick_Handler
/* 所有配置宏都写在这里 */
#include "stm32f1xx_hal.h"
// FreeRTOS 运行时配置 运行时的频率时钟
#define configCPU_CLOCK_HZ    ( SystemCoreClock )

//第一个配置：调度方式
#define configUSE_PREEMPTION 1
//什么叫 Idle Task？ 空闲任务
#define configUSE_IDLE_HOOK 0
//第三个配置：Tick Hook，Tick Hook 是在每次系统时钟中断时调用的函数
#define configUSE_TICK_HOOK 0

#define configTICK_RATE_HZ            ( ( TickType_t )1000 )

/* 最多支持多少个优先级 */
#define configMAX_PRIORITIES          32

/* Idle Task栈大小（单位：word，不是byte） */
#define configMINIMAL_STACK_SIZE      128

//堆大小
#define configTOTAL_HEAP_SIZE (10*1024)

/* Cortex-M3使用32位Tick */
#define configUSE_16_BIT_TICKS        0

/* 中断优先级位数 */  //STM32F103NVIC支持4bit
#define configPRIO_BITS               4

/* 最低中断优先级 */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15

/* FreeRTOS可管理的最高中断优先级 */  //优先级高于5（数值更小，例如0、1、2、3、4）的中断，不能调用任何 FreeRTOS API。
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

/* 真正写入NVIC寄存器的优先级 */
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8-configPRIO_BITS) )

#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8-configPRIO_BITS) )

/* 启用 FreeRTOS API 函数 */
#define INCLUDE_vTaskDelay             1
#define INCLUDE_vTaskSuspend           1
#define INCLUDE_vTaskDelete            1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define configCHECK_FOR_STACK_OVERFLOW    2
/* 启用静态内存分配 (xTaskCreateStatic) */
#define configSUPPORT_STATIC_ALLOCATION         1

/* 启用任务跟踪和格式化输出 (vTaskList) */
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    1

#endif