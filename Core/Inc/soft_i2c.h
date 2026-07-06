#ifndef __SOFT_I2C_H__
#define __SOFT_I2C_H__
#include "main.h"

#define SOFT_I2C_SCL_PORT   GPIOB
#define SOFT_I2C_SCL_PIN    GPIO_PIN_6
#define SOFT_I2C_SDA_PORT   GPIOB
#define SOFT_I2C_SDA_PIN    GPIO_PIN_7


#define SCL_High()  GPIOB->BSRR = GPIO_PIN_6
#define SCL_Low()   GPIOB->BRR = GPIO_PIN_6
#define SDA_High()  GPIOB->BSRR = GPIO_PIN_7
#define SDA_Low()   GPIOB->BRR = GPIO_PIN_7



uint8_t MyI2C_R_SDA(void); 

#define I2C_Delay_US()  { for (volatile int i = 0; i < 5; i++); }  /* 简单延时，约 8~10us (确保 > 标准模式 4.7us) */
void MyI2C_Init(void);


void MyI2C_Start(void);
void MyI2C_Stop(void);
void MyI2C_SendByte(uint8_t byte);
uint8_t MyI2C_RecvByte(uint8_t ack);
uint8_t MyI2C_ReciveAck(void);
void MyI2C_SendAck(uint8_t byte);
uint8_t MyI2C_ReadWhoAmI(void);

#endif
