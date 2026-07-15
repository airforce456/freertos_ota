#ifndef __OLED_H__
#define __OLED_H__

#include "main.h"
#include "soft_i2c.h"
#include "stdio.h"
#include "math.h"
/* MPU6050 I2C 地址 (AD0 接 GND) */
#define OLED_ADDR           0x3C   
#define OLED_CMD            0x00
#define OLED_DATA           0x40
#define OLED_DISPLAY_OFF  0xAE
#define OLED_DISPLAY_ON   0xAF


void OLED_Clear(void);

void OLED_WriteCmd(uint8_t cmd);
void OLED_WriteData(uint8_t data);
void OLED_WriteCmdBuf(const uint8_t *cmds,uint8_t len);
void OLED_Init(void);
void OLED_SetCursor(uint8_t page, uint8_t col);
void OLED_Clear(void);
void OLED_ShowString(uint8_t page,uint8_t col, const char *str);
void OLED_WriteDataBuf(const uint8_t *data,uint8_t len);
void OLED_Fill(uint8_t val);
void OLED_ShowNum(uint8_t page,uint8_t col, float  data);

#endif /* __OLED_H__ */
