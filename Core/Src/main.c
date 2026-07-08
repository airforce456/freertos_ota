/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
QueueHandle_t xSensorQueue = NULL;   /* MPU6050_Data_t: 生产者→消费者 */
QueueHandle_t xCookedQueue = NULL;   /* Cooked_Data_t: 消费者→消费者 */
SemaphoreHandle_t xI2CMutex = NULL;  /* 保护软 I2C 总线 (MPU6050 & OLED 共享) */
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
uint8_t rebyte = 0;
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* 此处可放少量初始化代码（CubeMX 会在 HAL_Init() 之前执行） */
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_DMA_Init();
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  /* 注意：不调用 MX_I2C1_Init() — 使用软 I2C */

  /* USER CODE BEGIN 2 */
  /* ============================================================
   *  以下为用户自定义初始化代码 — CubeMX 生成时不会被覆盖
   * ============================================================ */

  /* ---- 软 I2C 初始化（接管 PB6/PB7，释放硬件 I2C1） ---- */
  MyI2C_Init();

  /* ---- OLED 初始化 ---- */
  OLED_Init();

  /* ---- MPU6050 初始化 ---- */
  if (MPU6050_Init() != SOFT_I2C_OK) {
      printf("[ERR] MPU6050 init failed! Check wiring.\r\n");
      while (1) {}
  }
  printf("[OK] MPU6050 WHO_AM_I = 0x%02X\r\n", MPU6050_ReadWhoAmI());

  /* ---- 零偏校准（校准时传感器需保持静止） ---- */
  int16_t offset_ax = 0, offset_ay = 0, offset_az = 0;
  MPU6050_Calibrate(&offset_ax, &offset_ay, &offset_az);

  /* ---- 创建 I2C 总线互斥锁（MPU6050 & OLED 共享软 I2C） ---- */
  xI2CMutex = xSemaphoreCreateMutex();
  if (xI2CMutex == NULL) {
      printf("[ERR] Failed to create I2C mutex!\r\n");
      while (1) {}
  }

  /* ---- 创建传感器数据管道队列 ---- */
  xSensorQueue = xQueueCreate(8, sizeof(MPU6050_Data_t));     /* 原始数据: 8 槽 */
  xCookedQueue = xQueueCreate(4, sizeof(SensorCooked_t));     /* 处理后数据: 4 槽 */
  if (xSensorQueue == NULL || xCookedQueue == NULL) {
      printf("[ERR] Failed to create queues!\r\n");
      while (1) {}
  }

  /* ---- 初始化任务参数结构体 ---- */
  ProcessTaskParam.xSensorQueue = xSensorQueue;
  ProcessTaskParam.xCookedQueue = xCookedQueue;
  ProcessTaskParam.ax_offset = offset_ax;
  ProcessTaskParam.ay_offset = offset_ay;
  ProcessTaskParam.az_offset = offset_az;

  /* ---- 创建 3 级流水线任务 ---- */
  /* MPU6050 读取任务（生产者，Prio=2） */
  xTaskCreate(Task_MPU6050_Read, "MPU_Read", 256,
              (void *)xSensorQueue, 2, NULL);

  /* 数据处理任务（处理层，Prio=2） */
  xTaskCreate(Task_DataProcess, "DataProc", 512,
              (void *)&ProcessTaskParam, 2, NULL);

  /* OLED 显示任务（消费者，Prio=1） */
  xTaskCreate(Task_OLED_Display, "OLED_Disp", 256,
              (void *)xCookedQueue, 1, NULL);

  /* ---- 初始化调试串口，启动 UART TX/RX 任务 ---- */
  DebugUART_Init();

  printf("\r\n========== FreeRTOS + MPU6050 + OLED Data Pipeline ==========\r\n");
  printf("Tasks: MPU_Read(2) -> Queue[8] -> DataProc(2) -> Queue[4] -> OLED_Disp(1)\r\n\r\n");

  /* ---- 启动 FreeRTOS 调度器（此调用永不返回） ---- */
  vTaskStartScheduler();

  /* 调度器永远不会返回 */
  while (1) {}
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 用户循环代码 — 调度器启动后不会到达此处 */
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

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
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
 *  Stack overflow hook
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
        /* 获取 I2C 总线锁 */
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

/* ===== 2.1B: OLED 显示任务（消费者） ===== */
void Task_OLED_Display(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    SensorCooked_t data;
    uint32_t count = 0;
    /* 清屏后显示 hello */
    OLED_Fill(0x00);
    OLED_ShowString(0, 0, "hello");
    printf("[OLED] Task started, q=%p\r\n", (void *)q);
    while (1) {
        if (xQueueReceive(q, &data, portMAX_DELAY) == pdPASS) {
            count++;
            /* 获取 I2C 总线锁，保护 OLED 写操作 */
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

/* ===== 数据处理任务 ===== */
void Task_DataProcess(void *argument)
{
    TaskParam_t *param = (TaskParam_t *)argument;

    QueueHandle_t xSensorQueue = param->xSensorQueue;
    QueueHandle_t xCookedQueue = param->xCookedQueue;
    MPU6050_Data_t data;
    SensorCooked_t cooked;

    /* 校准已在 main() 中完成，直接进入处理循环 */
    while (1)
    {
        if (xQueueReceive(xSensorQueue, &data, portMAX_DELAY) == pdPASS) {
            cooked.ax_g = (float)(data.ax - param->ax_offset) / 16384.0f;
            cooked.ay_g = (float)(data.ay - param->ay_offset) / 16384.0f;
            cooked.az_g = (float)(data.az - param->az_offset) / 16384.0f;
            xQueueSend(xCookedQueue, &cooked, pdMS_TO_TICKS(100));
        }
    }
}
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
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
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
