#ifndef __MPU6050_H__
#define __MPU6050_H__

#include "main.h"
#include "i2c.h"

/* MPU6050 I2C 地址 (AD0 接 GND) */
#define MPU6050_ADDR           0x68 << 1   /* HAL 库需要左移 1 位 */

/* 寄存器定义 */
#define MPU6050_REG_WHO_AM_I   0x75        /* 器件 ID，应为 0x68 */
#define MPU6050_REG_PWR_MGMT_1 0x6B        /* 电源管理 */
#define MPU6050_REG_SMPRT_DIV  0x19        /* 采样率分频 */
#define MPU6050_REG_CONFIG     0x1A        /* 低通滤波器 */
#define MPU6050_REG_GYRO_CONFIG 0x1B       /* 陀螺仪量程 */
#define MPU6050_REG_ACCEL_CONFIG 0x1C      /* 加速度计量程 */
#define MPU6050_REG_ACCEL_XOUT_H 0x3B      /* 加速度计 X 高字节（共 6 字节） */
#define MPU6050_REG_GYRO_XOUT_H  0x43      /* 陀螺仪 X 高字节（共 6 字节） */

/* 传感器数据结构体 */
typedef struct {
    int16_t ax, ay, az;     /* 加速度原始值（±16384 = ±1g @ ±2g 量程） */
    int16_t gx, gy, gz;     /* 角速度原始值（±131 = ±1°/s @ ±250°/s 量程） */
    int16_t temp;           /* 温度原始值 */
} MPU6050_Data_t;

/* 初始化 MPU6050：唤醒 + 配置量程 + 采样率 */
HAL_StatusTypeDef MPU6050_Init(void);

/* 读取 WHO_AM_I 寄存器，应返回 0x68 */
uint8_t MPU6050_ReadWhoAmI(void);

/* 读加速度 + 角速度 + 温度（14 字节） */
HAL_StatusTypeDef MPU6050_ReadAll(MPU6050_Data_t *data);

#endif /* __MPU6050_H__ */
