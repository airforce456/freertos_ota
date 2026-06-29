/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>
#include "debug_uart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Experiment 4: static allocation buffers */
static StackType_t  xStaticStack[256];
static StaticTask_t xStaticTCB;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
/* Experiment 1: LED tasks with different priorities */
void Task_LED_High(void *argument);
void Task_LED_Mid(void *argument);
void Task_LED_Low(void *argument);
/* Experiment 2: self-deleting task */
void Task_SelfDelete(void *argument);
/* Experiment 3: task list reporter */
void Task_ListReporter(void *argument);
/* Experiment 4: static allocation demo */
void Task_StaticDemo(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

int main(void)
{
  /* USER CODE BEGIN 1 */
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  /* USER CODE END 1 */

  /* USER CODE BEGIN 2 */

  /* ---- Init debug UART (queue + TX task + DMA RX) ---- */
  DebugUART_Init();
  DebugUART_StartRx();

  printf("========================================\r\n");
  printf("  FreeRTOS Scheduler Experiments\r\n");
  printf("  STM32F103C8Tx | %lu Hz\r\n", SystemCoreClock);
  printf("========================================\r\n\r\n");

  /* Record heap before any user task */
  unsigned int heapBefore = xPortGetFreeHeapSize();

  /* ---- Experiment 1: 3 LED tasks, different priorities ---- */
  //创建任务的单位是字（word，4 字节）
  xTaskCreate(Task_LED_High, "LED_H", 256, (void *)"PC13[H]", 3, NULL);
  xTaskCreate(Task_LED_Mid,  "LED_M", 256, (void *)"PC14[M]", 2, NULL);
  xTaskCreate(Task_LED_Low,  "LED_L", 256, (void *)"PC15[L]", 1, NULL);

  /* ---- Experiment 2: self-deleting task ---- */
  xTaskCreate(Task_SelfDelete, "SelfDel", 256, NULL, 1, NULL);

  /* ---- Experiment 3: task list reporter (larger stack = more heap cost) ---- */
  xTaskCreate(Task_ListReporter, "ListRpt", 256, NULL, 1, NULL);

  unsigned int heapAfter5Dynamic = xPortGetFreeHeapSize();
  unsigned int dynamicCost = heapBefore - heapAfter5Dynamic;

  /* ---- Experiment 4: static allocation (TCB + stack in BSS, NOT heap) ---- */
  TaskHandle_t xStaticHandle = NULL;
  xStaticHandle = xTaskCreateStatic(Task_StaticDemo, "StaticD", 256, NULL, 2,
                                     xStaticStack, &xStaticTCB);

  unsigned int heapAfterStatic = xPortGetFreeHeapSize();
  unsigned int staticCost = heapAfter5Dynamic - heapAfterStatic;

  printf("=== Heap: Dynamic vs Static ===\r\n");
  printf("  Heap total:       %5u bytes (%u KiB)\r\n",
         configTOTAL_HEAP_SIZE, configTOTAL_HEAP_SIZE / 1024);
  printf("  Before tasks:     %5u bytes free\r\n\r\n", heapBefore);

  printf("  5 dynamic tasks:  %5u bytes cost  (%u free)\r\n",
         dynamicCost, heapAfter5Dynamic);
  printf("    -> avg %u bytes per dynamic task (TCB+stack on heap)\r\n\r\n",
         dynamicCost / 5);

  printf("  1 static task:    %5u bytes cost  (%u free)\r\n",
         staticCost, heapAfterStatic);
  if (xStaticHandle != NULL) {
      printf("    -> CREATED OK, heap saved ~%u bytes vs dynamic\r\n\r\n",
             dynamicCost / 5);
  } else {
      printf("    -> ERROR: xTaskCreateStatic returned NULL!\r\n\r\n");
  }

  vTaskStartScheduler();
  /* USER CODE END 2 */

  /* Infinite loop */
  while (1)
  {
    /* USER CODE BEGIN 3 */
    /* USER CODE END 3 */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* ==================================================================
 *  Static allocation support: provide memory for the FreeRTOS Idle task.
 *  Required when configSUPPORT_STATIC_ALLOCATION is enabled.
 * ================================================================== */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  uxIdleTaskStack[configMINIMAL_STACK_SIZE];

    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer  = uxIdleTaskStack;
    *pulIdleTaskStackSize    = configMINIMAL_STACK_SIZE;
}

/* ==================================================================
 *  Experiment 1: 3 LED tasks with different priorities
 *  Observe preemption: higher priority tasks always run first.
 *
 *  LED_High (prio=3): PC13, blinks every 200ms �?? tight timing
 *  LED_Mid  (prio=2): PC14, blinks every 500ms
 *  LED_Low  (prio=1): PC15, CPU-bound busy-wait �?? gets preempted
 * ================================================================== */

void Task_LED_High(void *argument)
{
    const char *name = (const char *)argument;
    uint32_t count = 0;
    while (1) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        count++;
        printf("[%s] Prio=3 | LED ON=%d | #%lu\r\n",
               name, (int)(count & 1), count);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void Task_LED_Mid(void *argument)
{
    const char *name = (const char *)argument;
    uint32_t count = 0;
    while (1) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_14);
        count++;
        printf("[%s] Prio=2 | LED ON=%d | #%lu\r\n",
               name, (int)(count & 1), count);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void Task_LED_Low(void *argument)
{
    const char *name = (const char *)argument;
    uint32_t count = 0;
    while (1) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_15);
        count++;
        printf("[%s] Prio=1 | Start busy-wait #%lu\r\n", name, count);
        /*
         * CPU-intensive loop �?? higher-priority tasks (LED_High, LED_Mid)
         * will PREEMPT this task when their vTaskDelay expires.
         * Watch the serial output: "Start" and "done" may be separated
         * by higher-priority LED toggles!
         */
        for (volatile uint32_t i = 0; i < 800000; i++) { }
        printf("[%s] Prio=1 | Busy-wait #%lu done\r\n", name, count);
        vTaskDelay(pdMS_TO_TICKS(100));  /* brief yield */
    }
}


/* ==================================================================
 *  Experiment 2: Self-deleting task
 *  Task waits 3 seconds, prints farewell, then deletes itself.
 *  Watch vTaskList output �?? the task disappears after deletion.
 * ================================================================== */

void Task_SelfDelete(void *argument)
{
    (void)argument;
    printf("[SelfDel] Hello! I will delete myself in 3 seconds...\r\n");
    vTaskDelay(pdMS_TO_TICKS(3000));
    printf("[SelfDel] Goodbye! Calling vTaskDelete(NULL)...\r\n");
    vTaskDelete(NULL);
    /* Execution never reaches here */
}


/* ==================================================================
 *  Experiment 3: Task list reporter
 *  Prints all tasks and their states every 5 seconds via vTaskList().
 *  Shows: task name, state (R=Ready, B=Blocked, D=Deleted, S=Suspended),
 *  priority, stack high-water mark, task number.
 * ================================================================== */

void Task_ListReporter(void *argument)
{
    (void)argument;
    char buf[512];

    /* Wait for SelfDel task to disappear first */
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        vTaskList(buf);
        printf("========== Task List ==========\r\n");
        printf("%s\r\n", buf);
        printf("===============================\r\n\r\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


/* ==================================================================
 *  Experiment 4: Static vs dynamic allocation
 *  This task is created with xTaskCreateStatic �?? its TCB and stack
 *  live in BSS (xStaticStack, xStaticTCB), NOT on the FreeRTOS heap.
 *  Compare heap usage printed at boot to see the difference.
 * ================================================================== */

void Task_StaticDemo(void *argument)
{
    (void)argument;
    uint32_t count = 0;
    while (1) {
        printf("[StaticDemo] Running from static memory! count=%lu | heap=%u\r\n",
               count++, (unsigned int)xPortGetFreeHeapSize());
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM2 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM2) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

