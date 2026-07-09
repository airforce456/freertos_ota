#ifndef __SOFT_I2C_H__
#define __SOFT_I2C_H__
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* I2C bus mutex — shared by MPU6050 and OLED (both use PB6/PB7 soft I2C) */
extern SemaphoreHandle_t xI2CMutex;

#define SOFT_I2C_SCL_PORT   GPIOB
#define SOFT_I2C_SCL_PIN    GPIO_PIN_6
#define SOFT_I2C_SDA_PORT   GPIOB
#define SOFT_I2C_SDA_PIN    GPIO_PIN_7


#define SCL_High()  GPIOB->BSRR = GPIO_PIN_6
#define SCL_Low()   GPIOB->BRR = GPIO_PIN_6
#define SDA_High()  GPIOB->BSRR = GPIO_PIN_7
#define SDA_Low()   GPIOB->BRR = GPIO_PIN_7
typedef enum {
    SOFT_I2C_OK    = 0,
    SOFT_I2C_ERROR = 1
} SoftI2C_Status;


uint8_t MyI2C_R_SDA(void); 

/* I2C 半周期延时，目标 ~5us（100kHz I2C 标准模式半周期 5us） */
/* HSI 8MHz + -Og: volatile for 循环每轮约 1.25us，15 轮 ≈ 19us */
/* 若不稳定可微调: 12~18 之间，宁大勿小 */
#define I2C_Delay_US()  { for (volatile int i = 0; i < 15; i++); }
void MyI2C_Init(void);


void MyI2C_Start(void);
void MyI2C_Stop(void);
void MyI2C_SendByte(uint8_t byte);
uint8_t MyI2C_RecvByte(uint8_t ack);
uint8_t MyI2C_ReciveAck(void);
void MyI2C_SendAck(uint8_t byte);
uint8_t MyI2C_ReadWhoAmI(void);

#endif
