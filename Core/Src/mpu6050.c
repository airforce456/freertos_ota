#include "mpu6050.h"

/* 超时时间（tick 数） */
#define I2C_TIMEOUT  100

/* ---- 私有辅助函数：F1 的 HAL_I2C_Mem_Read/Write 有重复起始 bug ----
 * 改用两步法：先 Master_Transmit 写寄存器号，再 Master_Receive 读数据   */

// uint8_t MyI2C_ReadWhoAmI(void)
// {
//     uint8_t ID = 0;
    
//     MyI2C_Init();
    
//     /* ---- 写操作：设备地址(写) + 寄存器地址 ---- */
//     MyI2C_Start();
//     MyI2C_SendByte(0xD0);           /* 设备地址 + W */
//     if(MyI2C_ReciveAck())           /* ACK=0 成功，非0则NACK */
//     {
//         MyI2C_Stop();
//         return 0;                   /* 设备无应答 */
//     }
//     MyI2C_SendByte(0x75);           /* WHO_AM_I 寄存器 */
//     if(MyI2C_ReciveAck())
//     {
//         MyI2C_Stop();
//         return 0;
//     }
//     MyI2C_Stop();
    
//     /* ---- 读操作：设备地址(读) + 读取数据 ---- */
//     MyI2C_Start();
//     MyI2C_SendByte(0xD1);           /* 设备地址 + R */
//     if(MyI2C_ReciveAck())
//     {
//         MyI2C_Stop();
//         return 0;
//     }
//     ID = MyI2C_RecvByte(1);         /* 最后一个字节发 NACK */
//     MyI2C_Stop();
    
//     return ID;
// }


// 写单个寄存器
SoftI2C_Status I2C_WriteReg(uint8_t devAddr,uint8_t regAddr,uint8_t val)
{
    MyI2C_Start();
    MyI2C_SendByte(devAddr | 0x00);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_SendByte(regAddr);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_SendByte(val);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_Stop();
    return SOFT_I2C_OK;

}
//读单个寄存器
SoftI2C_Status I2C_ReadReg(uint8_t devAddr, uint8_t regAddr, uint8_t *val)
{
    //1.写寄存器地址
    MyI2C_Start();
    MyI2C_SendByte(devAddr | 0x00);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_SendByte(regAddr);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_Stop();


    
    // 第2步：读数据

    MyI2C_Start();
    MyI2C_SendByte(devAddr | 0x01) ;
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    *val = MyI2C_RecvByte(1);
    MyI2C_Stop();
    return SOFT_I2C_OK;

}

SoftI2C_Status I2C_ReadRegs(uint8_t devAddr, uint8_t regAddr, uint8_t *buf, uint8_t len)
{
    //1.写寄存器地址
    MyI2C_Start();
    MyI2C_SendByte(devAddr | 0x00);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_SendByte(regAddr);
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    MyI2C_Stop();


    
    // 第2步：读数据

    MyI2C_Start();
    MyI2C_SendByte(devAddr | 0x01) ;
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        return SOFT_I2C_ERROR;
    }
    for(uint8_t i=0;i<len;i++)
    {
        buf[i]=MyI2C_RecvByte(i==len-1?1:0);
    }
    
    MyI2C_Stop();
    return SOFT_I2C_OK;
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

    /* 3. 采样率 = 1kHz / (1 + SMPRT_DIV) = 1kHz / 4 = 250Hz */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_SMPRT_DIV, 3) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;

    /* 4. 加速度计量程 ±2g */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_ACCEL_CONFIG, 0x00) != SOFT_I2C_OK)
        return SOFT_I2C_ERROR;

    /* 5. 陀螺仪量程 ±250°/s */
    if (I2C_WriteReg(MPU6050_ADDR, MPU6050_REG_GYRO_CONFIG, 0x00) != SOFT_I2C_OK)
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
