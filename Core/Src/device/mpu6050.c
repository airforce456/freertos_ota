#include "mpu6050.h"
#include <stdio.h>
#include <string.h>

/* ---- 私有辅助函数：用软 I2C 封装 I2C 读写，替代 HAL I2C（避免 F1 硬件 RESTART bug） ---- */
extern SemaphoreHandle_t MPU_Sem;

/* 写单个寄存器：发设备地址+W → 寄存器号 → 数据值 */
static SoftI2C_Status I2C_WriteReg(uint8_t devAddr, uint8_t regAddr, uint8_t val)
{
    MyI2C_Start();
    MyI2C_SendByte(devAddr << 1);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); return SOFT_I2C_ERROR; }
    MyI2C_SendByte(regAddr);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); return SOFT_I2C_ERROR; }
    MyI2C_SendByte(val);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); return SOFT_I2C_ERROR; }
    MyI2C_Stop();
    return SOFT_I2C_OK;
}

/* 读多个寄存器：先发设备地址+W → 寄存器号 → Stop → 再发设备地址+R → 读len字节 */
static SoftI2C_Status I2C_ReadRegs(uint8_t devAddr, uint8_t regAddr,
                                    uint8_t *buf, uint8_t len)
{
    /* 第一步：写寄存器号 */
    MyI2C_Start();
    MyI2C_SendByte(devAddr << 1);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); return SOFT_I2C_ERROR; }
    MyI2C_SendByte(regAddr);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); return SOFT_I2C_ERROR; }
    MyI2C_Stop();

    /* 第二步：读数据 */
    MyI2C_Start();
    MyI2C_SendByte((devAddr << 1) | 1);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); return SOFT_I2C_ERROR; }
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = MyI2C_RecvByte(i == (len - 1) ? 1 : 0);  /* 最后一字节发NACK */
    }
    MyI2C_Stop();
    return SOFT_I2C_OK;
}

/* 读单个寄存器 */
static SoftI2C_Status I2C_ReadReg(uint8_t devAddr, uint8_t regAddr, uint8_t *val)
{
    return I2C_ReadRegs(devAddr, regAddr, val, 1);
}

/* ===== 初始化 MPU6050 ===== */
SoftI2C_Status MPU6050_Init(void)
{
    uint8_t reg;

    /* 1. 检查 WHO_AM_I */
    if (I2C_ReadReg(MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &reg) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;
    if (reg != 0x68) return SOFT_I2C_ERROR;

    /* 2. 唤醒（清除 SLEEP 位） */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_PWR_MGMT_1, 0x00) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;

    /* 3. 采样率 = 1kHz / (1 + SMPRT_DIV) = 1kHz / 20 = 50Hz */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_SMPRT_DIV, 49) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;

    /* 4. 加速度计量程 ±2g */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_ACCEL_CONFIG, 0x00) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;

    /* 5. 陀螺仪量程 ±250°/s */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_GYRO_CONFIG, 0x00) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;
    if (I2C_WriteReg(MPU6050_ADDR, INT_PIN_CFG, 0x10) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;
    if (I2C_WriteReg(MPU6050_ADDR, INT_ENABLE, 0x01) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;
    return SOFT_I2C_OK;
}

/* ===== 读 WHO_AM_I ===== */
uint8_t MPU6050_ReadWhoAmI(void)
{
    uint8_t reg;
    if (I2C_ReadReg(MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &reg) == SOFT_I2C_OK)
        return reg;
    return 0;
}

/* ===== 读全部传感器数据（加速度 + 温度 + 角速度，共 14 字节） ===== */
SoftI2C_Status MPU6050_ReadAll(MPU6050_Data_t *data)
{
    uint8_t buf[14];

    if (I2C_ReadRegs(MPU6050_ADDR, MPU6050_REG_ACCEL_XOUT_H, buf, 14) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;

    /* 大端拼接 */
    data->ax   = (int16_t)((buf[0]  << 8) | buf[1]);
    data->ay   = (int16_t)((buf[2]  << 8) | buf[3]);
    data->az   = (int16_t)((buf[4]  << 8) | buf[5]);
    data->temp = (int16_t)((buf[6]  << 8) | buf[7]);
    data->gx   = (int16_t)((buf[8]  << 8) | buf[9]);
    data->gy   = (int16_t)((buf[10] << 8) | buf[11]);
    data->gz   = (int16_t)((buf[12] << 8) | buf[13]);

    return SOFT_I2C_OK;
}

/* ===== 零偏校准：采集 100 个样本取平均（必须在调度器启动前调用，使用 HAL_Delay） ===== */
void MPU6050_Calibrate(int16_t *offset_ax, int16_t *offset_ay, int16_t *offset_az)
{
    MPU6050_Data_t raw;
    int32_t sum_ax = 0, sum_ay = 0, sum_az = 0;

    printf("[MPU] Calibrating (keep sensor still)...\r\n");
    for (int i = 0; i < 100; i++) {
        if (MPU6050_ReadAll(&raw) == SOFT_I2C_OK) {
            sum_ax += raw.ax;
            sum_ay += raw.ay;
            sum_az += raw.az - 16384;  /* 减去 1g 重力 */
        }
        HAL_Delay(10);  /* 使用 HAL_Delay（调度器启动前 vTaskDelay 不可用） */
    }
    *offset_ax = (int16_t)(sum_ax / 100);
    *offset_ay = (int16_t)(sum_ay / 100);
    *offset_az = (int16_t)(sum_az / 100);
    printf("[MPU] Calibration done: bias_ax=%d, bias_ay=%d, bias_az=%d\r\n",
           *offset_ax, *offset_ay, *offset_az);
}
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_0) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(MPU_Sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_14);
    
  }
}