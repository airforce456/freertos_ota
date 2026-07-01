#include "mpu6050.h"

/* 超时时间（tick 数） */
#define I2C_TIMEOUT  100

/* ---- 私有辅助函数：F1 的 HAL_I2C_Mem_Read/Write 有重复起始 bug ----
 * 改用两步法：先 Master_Transmit 写寄存器号，再 Master_Receive 读数据   */

static HAL_StatusTypeDef I2C_WriteReg(uint8_t devAddr, uint8_t regAddr, uint8_t val)
{
    uint8_t buf[2] = { regAddr, val };
    return HAL_I2C_Master_Transmit(&hi2c1, devAddr, buf, 2, I2C_TIMEOUT);
}

static HAL_StatusTypeDef I2C_ReadRegs(uint8_t devAddr, uint8_t regAddr,
                                       uint8_t *buf, uint8_t len)
{
    HAL_StatusTypeDef rc;
    rc = HAL_I2C_Master_Transmit(&hi2c1, devAddr, &regAddr, 1, I2C_TIMEOUT);
    if (rc != HAL_OK) return rc;
    return HAL_I2C_Master_Receive(&hi2c1, devAddr, buf, len, I2C_TIMEOUT);
}

static HAL_StatusTypeDef I2C_ReadReg(uint8_t devAddr, uint8_t regAddr, uint8_t *val)
{
    return I2C_ReadRegs(devAddr, regAddr, val, 1);
}

/* ===== 初始化 MPU6050 ===== */
HAL_StatusTypeDef MPU6050_Init(void)
{
    uint8_t reg;

    /* 1. 检查 WHO_AM_I */
    if (I2C_ReadReg(MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &reg) != HAL_OK)
        return HAL_ERROR;
    if (reg != 0x68) return HAL_ERROR;

    /* 2. 唤醒（清除 SLEEP 位） */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_PWR_MGMT_1, 0x00) != HAL_OK)
        return HAL_ERROR;

    /* 3. 采样率 = 1kHz / (1 + SMPRT_DIV) = 1kHz / 4 = 250Hz */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_SMPRT_DIV, 3) != HAL_OK)
        return HAL_ERROR;

    /* 4. 加速度计量程 ±2g */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_ACCEL_CONFIG, 0x00) != HAL_OK)
        return HAL_ERROR;

    /* 5. 陀螺仪量程 ±250°/s */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_GYRO_CONFIG, 0x00) != HAL_OK)
        return HAL_ERROR;

    return HAL_OK;
}

/* ===== 读 WHO_AM_I ===== */
uint8_t MPU6050_ReadWhoAmI(void)
{
    uint8_t reg;
    if (I2C_ReadReg(MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &reg) == HAL_OK)
        return reg;
    return 0;
}

/* ===== 读全部传感器数据（加速度 + 温度 + 角速度，共 14 字节） ===== */
HAL_StatusTypeDef MPU6050_ReadAll(MPU6050_Data_t *data)
{
    uint8_t buf[14];

    if (I2C_ReadRegs(MPU6050_ADDR, MPU6050_REG_ACCEL_XOUT_H, buf, 14) != HAL_OK)
        return HAL_ERROR;

    /* 大端拼接 */
    data->ax   = (int16_t)((buf[0]  << 8) | buf[1]);
    data->ay   = (int16_t)((buf[2]  << 8) | buf[3]);
    data->az   = (int16_t)((buf[4]  << 8) | buf[5]);
    data->temp = (int16_t)((buf[6]  << 8) | buf[7]);
    data->gx   = (int16_t)((buf[8]  << 8) | buf[9]);
    data->gy   = (int16_t)((buf[10] << 8) | buf[11]);
    data->gz   = (int16_t)((buf[12] << 8) | buf[13]);

    return HAL_OK;
}
