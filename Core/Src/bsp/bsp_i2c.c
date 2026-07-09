#include "bsp_i2c.h"
#include "main.h"

uint8_t MyI2C_R_SDA(void) {
    return (GPIOB->IDR & GPIO_PIN_7) ? 1 : 0;
}

void MyI2C_Init(void)
{
    I2C1->CR1 &= ~I2C_CR1_PE;    // 关掉 I2C1 硬件外设，释放 PB6/PB7
    I2C1->CR1 &= ~I2C_CR1_PE;

    GPIO_InitTypeDef cfg = {0};
    cfg.Pin   = SOFT_I2C_SCL_PIN | SOFT_I2C_SDA_PIN;
    cfg.Mode  = GPIO_MODE_OUTPUT_OD;    // 普通 GPIO 开漏，不是复用
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SOFT_I2C_SCL_PORT, &cfg);
    SCL_High();
    SDA_High();
}

void MyI2C_Start(void)
{
    /* 确保总线空闲 */
    SDA_High();
    I2C_Delay_US();
    SCL_High();
    I2C_Delay_US();
    /* SCL 为高时 SDA 从高拉低 = Start */
    SDA_Low();
    I2C_Delay_US();
    SCL_Low();
}



void MyI2C_Stop(void)
{
    SDA_Low();
    I2C_Delay_US();
    SCL_High();
    I2C_Delay_US();
    /* SCL 为高时 SDA 从低拉高 = Stop */
    SDA_High();
    I2C_Delay_US();
}

void MyI2C_SendByte(uint8_t Byte)
{
    for(uint8_t i=0;i<8;i++)
    {
        if ((Byte >> (7 - i)) & 0x01)
        {
            SDA_High();
        }
        else
        {
            SDA_Low();
        }
        I2C_Delay_US();
        SCL_High();
        I2C_Delay_US();
        SCL_Low();
        I2C_Delay_US();
    }
    // MyI2C_ReciveAck(); 
    /* 注意：SendByte 不内部读 ACK，由调用者自行处理 */
}
uint8_t MyI2C_RecvByte(uint8_t ack)
{
    uint8_t ReceiveByte=0;
    SDA_High();

    for(uint8_t i=0;i<8;i++)
    {
        SCL_High();
        I2C_Delay_US();
        if(MyI2C_R_SDA())
        {
            ReceiveByte |= (0x80>>i);
        }
        SCL_Low();
        I2C_Delay_US();
    }
    MyI2C_SendAck(ack);   /* ack=0: 继续读; ack=1: 最后一个字节(NACK) */
    return ReceiveByte;
}

uint8_t MyI2C_ReciveAck(void)
{
    uint8_t Ack;

    SDA_High();
    SCL_High();
    I2C_Delay_US();
    Ack = MyI2C_R_SDA();
    
    SCL_Low();
    return Ack;
}

void MyI2C_SendAck(uint8_t byte)
{
    if(byte)
    {
        SDA_High();
    }
    else
    {
        SDA_Low();
    }
    SCL_High();
    I2C_Delay_US();
    SCL_Low();

}

uint8_t MyI2C_ReadWhoAmI(void)
{
    uint8_t ID = 0;
    
    MyI2C_Init();
    
    /* ---- 写操作：设备地址(写) + 寄存器地址 ---- */
    MyI2C_Start();
    MyI2C_SendByte(0xD0);           /* 设备地址 + W */
    if(MyI2C_ReciveAck())           /* ACK=0 成功，非0则NACK */
    {
        MyI2C_Stop();
        // return 0;                   /* 设备无应答 */
    }
    MyI2C_SendByte(0x75);           /* WHO_AM_I 寄存器 */
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        // return 0;
    }
    MyI2C_Stop();
    
    /* ---- 读操作：设备地址(读) + 读取数据 ---- */
    MyI2C_Start();
    MyI2C_SendByte(0xD1);           /* 设备地址 + R */
    if(MyI2C_ReciveAck())
    {
        MyI2C_Stop();
        // return 0;
    }
    ID = MyI2C_RecvByte(1);         /* 最后一个字节发 NACK */
    MyI2C_Stop();
    
    return ID;
}
