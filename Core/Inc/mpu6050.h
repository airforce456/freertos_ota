#ifndef __MPU6050_H__
#define __MPU6050_H__

#include "main.h"
#include "soft_i2c.h"

/* MPU6050 I2C 地址 (7位: 0x68, AD0 接 GND) */
#define MPU6050_ADDR           0x68

/* 寄存器定义 */
#define MPU6050_REG_WHO_AM_I   0x75        /* 器件 ID，应为 0x68 */
#define MPU6050_REG_PWR_MGMT_1 0x6B        /* 电源管理 */
#define MPU6050_REG_SMPRT_DIV  0x19        /* 采样率分频 */
#define MPU6050_REG_CONFIG     0x1A        /* 低通滤波器 */
#define MPU6050_REG_GYRO_CONFIG 0x1B       /* 陀螺仪量程 */
#define MPU6050_REG_ACCEL_CONFIG 0x1C      /* 加速度计量程 */
#define MPU6050_REG_ACCEL_XOUT_H 0x3B      /* 加速度计 X 高字节（共 6 字节） */
#define MPU6050_REG_GYRO_XOUT_H  0x43      /* 陀螺仪 X 高字节（共 6 字节） */

/* ---- 原始传感器数据结构体（队列传递） ---- */
typedef struct {
    int16_t ax, ay, az;     /* 加速度原始值（±16384 = ±1g @ ±2g 量程） */
    int16_t gx, gy, gz;     /* 角速度原始值（±131 = ±1°/s @ ±250°/s 量程） */
    int16_t temp;           /* 温度原始值 */
} MPU6050_Data_t;

/* ---- 处理后的数据结构体（浮点，物理单位） ---- */
typedef struct {
    float ax_g, ay_g, az_g;        /* 加速度 (g) */
    float gx_dps, gy_dps, gz_dps;  /* 角速度 (°/s) */
    uint32_t count;                /* 采样序号 */
} SensorCooked_t;

/* 初始化 MPU6050：唤醒 + 配置量程 + 采样率 */
SoftI2C_Status MPU6050_Init(void);

/* 读取 WHO_AM_I 寄存器，应返回 0x68 */
uint8_t MPU6050_ReadWhoAmI(void);

/* 读加速度 + 角速度 + 温度（14 字节） */
SoftI2C_Status MPU6050_ReadAll(MPU6050_Data_t *data);

/* 零偏校准：采集 N 个样本取平均，结果存于 offset 参数中 */
void MPU6050_Calibrate(int16_t *offset_ax, int16_t *offset_ay, int16_t *offset_az);

#endif /* __MPU6050_H__ */
