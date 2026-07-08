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
#include "oled.h"
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
QueueHandle_t xSensorQueue = NULL;   /* MPU6050_Data_t: 生产者→消费�? */
QueueHandle_t xCookedQueue = NULL;   /* Cooked_Data_t: 消费者→消费�? */
SemaphoreHandle_t xI2CMutex = NULL;  /* 保护�? I2C 总线 (MPU6050 & OLED 共享) */
typedef struct
{
    QueueHandle_t xSensorQueue;
    QueueHandle_t xCookedQueue;
    int16_t ax_offset;
    int16_t ay_offset;
    int16_t az_offset;
} TaskParam_t;
TaskParam_t ProcessTaskParam;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Task_MPU6050_Read(void *argument);
void Task_OLED_Display(void *argument);
void Task_DataProcess(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
uint8_t rebyte=0;
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */

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

/* ===== 2.1A: MPU6050 读取任务（生产�?�） ===== */
void Task_MPU6050_Read(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    MPU6050_Data_t data;
    uint32_t count = 0;

    while (1) {
        /* 获取 I2C 总线�? */
        if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(200)) == pdPASS) {
            if (MPU6050_ReadAll(&data) == SOFT_I2C_OK) {
                count++;
                if (xQueueSend(q, &data, pdMS_TO_TICKS(100)) != pdPASS) {
                    printf("[MPU] Queue full, dropped #%lu\r\n", count);
                } else {
                    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);  /* PC13 活动指示 */
                }
            }
            xSemaphoreGive(xI2CMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ===== 2.1B: OLED 显示任务（消费�?�） ===== */
void Task_OLED_Display(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    SensorCooked_t data;
    uint32_t count = 0;
    /* 清屏后显�? hello */
    OLED_Fill(0x00);
    OLED_ShowString(0, 0, "hello");
    printf("[OLED] Task started, q=%p\r\n", (void*)q);
    while (1) {
        if (xQueueReceive(q, &data, portMAX_DELAY) == pdPASS) {
            count++;
            /* 获取 I2C 总线锁，保护 OLED 写操�? */
            if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdPASS) {
                OLED_ShowNum(2, 1, data.ax_g);
                OLED_ShowNum(3, 1, data.ay_g);
                OLED_ShowNum(4, 1, data.az_g);
                OLED_ShowNum(4, 100, (float)count);
                xSemaphoreGive(xI2CMutex);
            }
            printf("[OLED] #%lu ax=%+.3f ay=%+.3f az=%+.3f\r\n",
                   count, data.ax_g, data.ay_g, data.az_g);
        }
    }
}

void Task_DataProcess(void *argument)
{
    TaskParam_t *param = (TaskParam_t *)argument;

    QueueHandle_t xSensorQueue = param->xSensorQueue;
    QueueHandle_t xCookedQueue = param->xCookedQueue;
    MPU6050_Data_t data;
    SensorCooked_t cooked;

    /* 校准已在 main() 中完成，直接进入处理循环 */
    while(1)
    {
        if (xQueueReceive(xSensorQueue, &data, portMAX_DELAY) == pdPASS) {
            cooked.ax_g  = (float)(data.ax - param->ax_offset) / 16384.0f;
            cooked.ay_g  = (float)(data.ay - param->ay_offset) / 16384.0f;
            cooked.az_g  = (float)(data.az - param->az_offset) / 16384.0f;
            xQueueSend(xCookedQueue, &cooked, pdMS_TO_TICKS(100));
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

