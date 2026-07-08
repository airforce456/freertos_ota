#ifndef __OLED_H__
#define __OLED_H__

#include "main.h"
#include "soft_i2c.h"

/* SSD1306 I2C 地址 (7位: 0x3C) */
#define OLED_ADDR   0x3C

/* 控制字节 */
#define OLED_CMD    0x00   /* 命令模式 */
#define OLED_DATA   0x40   /* 数据模式 */

/* 6x8 字模表 (ASCII 32~126, 共95个字符, 每字符6字节) */
extern const uint8_t OLED_F6x8[][6];

/* 初始化 SSD1306 (先唤醒再写配置序列) */
void OLED_Init(void);

/* 设光标位置 (page: 0~7, col: 0~127) */
void OLED_SetCursor(uint8_t page, uint8_t col);

/* 单字节命令 / 数据 */
void OLED_WriteCmd(uint8_t cmd);
void OLED_WriteData(uint8_t data);

/* 批量命令 / 数据 (一次 I2C Start/Stop 发送多个字节) */
void OLED_WriteCmdBuf(const uint8_t *cmds, uint8_t len);
void OLED_WriteDataBuf(const uint8_t *data, uint8_t len);

/* 清屏 / 全屏填充 */
void OLED_Clear(void);
void OLED_Fill(uint8_t val);

/* 显示字符串 (只支持 ASCII 可打印字符 32~126) */
void OLED_ShowString(uint8_t page, uint8_t col, const char *str);

/* 显示浮点数 (3位小数) */
void OLED_ShowNum(uint8_t page, uint8_t col, float data);

#endif /* __OLED_H__ */
