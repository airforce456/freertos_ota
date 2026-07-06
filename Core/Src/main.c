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
#include "i2c.h"
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
#include "mpu6050.h"
#include "soft_i2c.h"
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
QueueHandle_t xSensorQueue = NULL;   /* MPU6050_Data_t: 生产者→消费者 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Task_MPU6050_Read(void *argument);
void Task_OLED_Display(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
uint8_t rebyte=0;
/* USER CODE END 0 */

int main(void)
{
  /* USER CODE BEGIN 1 */
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();
  rebyte=MyI2C_ReadWhoAmI();
  /* USER CODE END 1 */
  
  /* USER CODE BEGIN 2 */

  DebugUART_Init();

  printf("========================================\r\n");
  printf("  2.1 Queue: MPU6050 Sensor Pipeline\r\n");
  printf("  STM32F103C8Tx | %lu Hz\r\n", SystemCoreClock);
  printf("========================================\r\n\r\n");

  /* ---- I2C 总线扫描，确认设备在线 ---- */
  printf("I2C Scan: ");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
      if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1), 2, 10) == HAL_OK) {
          printf("0x%02X ", addr);
          found++;
      }
  }
  if (found == 0) {
      printf("NO DEVICE FOUND!\r\n");
  } else {
      printf("(%d device(s))\r\n", found);
  }
  printf("[SoftI2C] WHO_AM_I = 0x%02X (expect 0x68)\r\n", rebyte);

  /* 扫描后复位 I2C 状态机，否则后续 HAL_I2C_Mem_Read 会脏 */
  HAL_I2C_DeInit(&hi2c1);
  MX_I2C1_Init();

  /* 手动读 WHO_AM_I：先写寄存器号，再读（F1 的 I2C 外设 Mem_Read 有重复起始问题） */
  {
      uint8_t reg = 0x75, who;
      HAL_StatusTypeDef rc;
      rc = HAL_I2C_Master_Transmit(&hi2c1, 0x68 << 1, &reg, 1, 100);
      if (rc == HAL_OK) {
          rc = HAL_I2C_Master_Receive(&hi2c1, 0x68 << 1, &who, 1, 100);
      }
      printf("[MPU] Raw WHO_AM_I: rc=%d val=0x%02X (expect rc=0 val=0x68)\r\n",
             (int)rc, who);
  }

  /* MPU6050 硬件初始化 */
  if (MPU6050_Init() == HAL_OK) {
      printf("[MPU6050] Init OK!\r\n\r\n");
  } else {
      printf("[MPU6050] Init FAILED!\r\n\r\n");
  }

  /* 创建队列：8 个 MPU6050_Data_t 槽位 */
  xSensorQueue = xQueueCreate(8, sizeof(MPU6050_Data_t));
  if (xSensorQueue == NULL) {
      printf("[Queue] Create FAILED!\r\n");
  } else {
      printf("[Queue] Created (8 slots x %u bytes)\r\n", (unsigned)sizeof(MPU6050_Data_t));
  }

  /* 生产者 (Prio=2) + 消费者 (Prio=1) — 都传队列句柄 */
  BaseType_t rc;
  rc = xTaskCreate(Task_MPU6050_Read, "MPU_Read", 512, (void*)xSensorQueue, 2, NULL);
  printf("[Task] MPU_Read create: %s\r\n", rc == pdPASS ? "OK" : "FAIL");
  rc = xTaskCreate(Task_OLED_Display, "OLED_Disp", 512, (void*)xSensorQueue, 1, NULL);
  printf("[Task] OLED_Disp create: %s\r\n", rc == pdPASS ? "OK" : "FAIL");

  printf("\r\n--- Starting scheduler ---\r\n\r\n");
  vTaskStartScheduler();
  /* USER CODE END 2 */

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
 *  Static allocation support for Idle task
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
 *  Stack overflow hook (required by configCHECK_FOR_STACK_OVERFLOW=2)
 * ================================================================== */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("\r\n!!! STACK OVERFLOW: %s !!!\r\n", pcTaskName);
    __disable_irq();
    while (1) {}
}

/* ===== 2.1A: MPU6050 读取任务（生产者） ===== */
void Task_MPU6050_Read(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    MPU6050_Data_t data;
    uint32_t count = 0;

    while (1) {
        if (MPU6050_ReadAll(&data) == HAL_OK) {
            count++;
            if (xQueueSend(q, &data, pdMS_TO_TICKS(100)) != pdPASS) {
                printf("[MPU] Queue full, dropped #%lu\r\n", count);
            } else {
                HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);  /* PC13 活动指示 */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ===== 2.1B: OLED 显示任务（消费者） ===== */
void Task_OLED_Display(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    MPU6050_Data_t data;
    uint32_t count = 0;

    printf("[OLED] Task started, q=%p\r\n", (void*)q);
    while (1) {
        if (xQueueReceive(q, &data, portMAX_DELAY) == pdPASS) {
            count++;
            printf("[OLED] #%lu ax=%+6d ay=%+6d az=%+6d\r\n",
                   count, data.ax, data.ay, data.az);
        }
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

