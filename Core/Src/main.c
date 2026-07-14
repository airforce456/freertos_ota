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
#include "spi.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>
#include "debug_uart.h"
#include "mpu6050.h"
#include "bsp_i2c.h"
#include "bsp_init.h"
#include "oled.h"
#include "event_groups.h"
#include <stdbool.h>
#include "esp8266.h"
#include "onenet.h"
#include "at24c02.h"
#include "w25q64.h"
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
#define EVENT_ACCEL_READY    (1 << 0)
#define EVENT_GYRO_READY     (1 << 1)
QueueHandle_t xSensorQueue = NULL;   /* MPU6050_Data_t: 生产者→消费�?? */
QueueHandle_t xCookedQueue = NULL;   /* Cooked_Data_t: 消费者→消费�?? */
SemaphoreHandle_t xI2CMutex = NULL;  /* 保护�?? I2C 总线 (MPU6050 & OLED 共享) */
EventGroupHandle_t xSensorEvent;
SemaphoreHandle_t MPU_Sem;
/* WiFi 配置见 esp8266.h 中的 ESP8266_WIFI_SSID / ESP8266_WIFI_PWD */

/* OneNET 上报数据（onenet.c 引用，调用 OneNet_SendData 前更新） */
float onenet_ax_g, onenet_ay_g, onenet_az_g;
uint32_t onenet_count;
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
void Task_OneNET_Upload(void *argument);

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
  /* 向量表偏移：APP 从 0x08005000 启动，Bootloader 在 0x08000000 */
//   SCB->VTOR = 0x08005000;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* 提前创建 MPU_Sem，防�? MX_GPIO_Init 使能 EXTI0 后浮�? PA0 触发中断导致空指针崩�? */
  MPU_Sem = xSemaphoreCreateBinary();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  // MX_I2C1_Init();
  DebugUART_Init() ;
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  /* ============================================================
   *  以下为用户自定义初始化代�?? �?? CubeMX 生成时不会被覆盖
   * ============================================================ */

  /* ---- 软 I2C 初始化（接管 PB6/PB7，释放硬件 I2C1） ---- */
  BSP_Init();

  

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

  /* ---- 创建 I2C 总线互斥锁（MPU6050 & OLED & AT24C02 共享） ---- */
  xI2CMutex = xSemaphoreCreateMutex();
  if (xI2CMutex == NULL) {
      printf("[ERR] Failed to create I2C mutex!\r\n");
      while (1) {}
  }

  /* ---- AT24C02 EEPROM 初始化（必须在 xI2CMutex 之后） ---- */
  if (!AT24C02_Init()) {
      printf("[ERR] AT24C02 init failed! Check wiring.\r\n");
  } else {
      printf("[OK] AT24C02 ready\r\n");
  }

  /* ---- W25Q64 SPI Flash 初始化 ---- */
  if (!W25Q64_Init()) {
      printf("[ERR] W25Q64 init failed! Check wiring.\r\n");
  } else {
      uint8_t id[3];
      W25Q64_ReadJEDEC_ID(id);
      printf("[OK] W25Q64 JEDEC ID: %02X %02X %02X\r\n", id[0], id[1], id[2]);
  }

  /* ---- 创建传感器数据管道队列 ---- */
  xSensorQueue = xQueueCreate(8, sizeof(MPU6050_Data_t));     /* 原始数据: 8 �?? */
  xCookedQueue = xQueueCreate(4, sizeof(SensorCooked_t));     /* 处理后数�??: 4 �?? */
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
  /* MPU6050 读取任务（生产�?�，Prio=2�?? */
  xTaskCreate(Task_MPU6050_Read, "MPU_Read", 256,
              (void *)xSensorQueue, 2, NULL);

  /* 数据处理任务（处理层，Prio=2�?? */
  xTaskCreate(Task_DataProcess, "DataProc", 256,
              (void *)&ProcessTaskParam, 2, NULL);

  /* OLED 显示任务（消费者，Prio=3，128w足够 OLED 写屏） */
  xTaskCreate(Task_OLED_Display, "OLED_Disp", 256,
              (void *)xCookedQueue, 3, NULL);

  /* OneNET 周期上传任务（Prio=1，每 5 秒上报一次，128w 足够 MQTT 封包） */
  xTaskCreate(Task_OneNET_Upload, "OneNET_Upl", 256,
              NULL, 1, NULL);



  printf("\r\n========== FreeRTOS + MPU6050 + OLED Data Pipeline ==========\r\n");
  printf("Tasks: MPU_Read(2) -> Queue[8] -> DataProc(2) -> Queue[4] -> OLED_Disp(1)\r\n\r\n");

  /* ---- ESP8266 初始化（NET 包 API：中断逐字节接收 + 轮询等待） ---- */
  printf("[ESP] Waiting for module boot (2s)...\r\n");
  HAL_Delay(2000);

  /* 启动 USART2 中断逐字节接收 */
  ESP8266_StartRx();

  /* 阻塞初始化：AT 检测 → 关回显 → STA 模式 → 连 WiFi */
  ESP8266_Init();

  /* ---- OneNET 云平台接入 ---- */
  printf("[ONENET] Registering device...\r\n");
  if (OneNET_RegisterDevice()) {
      printf("[ONENET] Register OK\r\n");

      if (OneNet_DevLink() == 0) {
          printf("[ONENET] MQTT Link OK\r\n");

          onenet_ax_g = 0; onenet_ay_g = 0;
          onenet_az_g = 0; onenet_count = 0;
          OneNET_Subscribe();
          OneNet_SendData();
      } else {
          printf("[ONENET] MQTT Link FAILED\r\n");
      }
  } else {
      printf("[ONENET] Register device, using default key\r\n");
      /* 若设备已注册过，直接用默认 key 连接 */
      if (OneNet_DevLink() == 0) {
          printf("[ONENET] MQTT Link OK\r\n");
          onenet_ax_g = 0; onenet_ay_g = 0;
          onenet_az_g = 0; onenet_count = 0;
          OneNET_Subscribe();
          OneNet_SendData();
      }
  }
  /* ---- 启动 FreeRTOS 调度器（此调用永不返回） ---- */
  printf("Starting scheduler...\r\n");
  vTaskStartScheduler();

  /* 调度器永远不会返�?? */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 用户循环代码 �?? 调度器启动后不会到达此处 */
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

/* ===== 2.1A: MPU6050 读取任务（生产�?�） ===== */
void Task_MPU6050_Read(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    MPU6050_Data_t data;
    uint32_t count = 0;

    while (1) {
        /* 获取 I2C 总线�? */
         if(xSemaphoreTake(MPU_Sem, portMAX_DELAY) == pdTRUE)
        {
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
          
        }
    }
}

/* ===== 2.1B: OLED 显示任务（消费�?�） ===== */
void Task_OLED_Display(void *argument)
{
    QueueHandle_t q = (QueueHandle_t)argument;
    SensorCooked_t data;
    uint32_t count = 0;
    /* 清屏后显�?? hello */
    OLED_Fill(0x00);
    OLED_ShowString(0, 0, "hello");
    printf("[OLED] Task started, q=%p\r\n", (void *)q);
    while (1) {
        if (xQueueReceive(q, &data, portMAX_DELAY) == pdPASS) {
            count++;
            /* 获取 I2C 总线锁，保护 OLED 写操�?? */
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
            cooked.count++;
            xQueueSend(xCookedQueue, &cooked, pdMS_TO_TICKS(100));

            /* 同步更新 OneNET 上报数据 */
            onenet_ax_g  = cooked.ax_g;
            onenet_ay_g  = cooked.ay_g;
            onenet_az_g  = cooked.az_g;
            onenet_count = cooked.count;
        }
    }
}
/* ===== OneNET 周期上传任务 ===== */
void Task_OneNET_Upload(void *argument)
{
    (void)argument;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("[ONENET] Uploading...\r\n");
        OneNet_SendData();
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
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

