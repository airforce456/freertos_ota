/**
 * @file    at24c02.c
 * @brief   AT24C02 256-byte I2C EEPROM driver implementation
 * @note    Uses soft I2C (PB6/PB7), shared with OLED & MPU6050.
 *          APP mode: protected by xI2CMutex, uses vTaskDelay.
 *          BOOT mode (#define BOOTLOADER_BUILD): no RTOS, uses HAL_Delay.
 */
#include "at24c02.h"
#include "bsp_i2c.h"
#include <string.h>

#ifdef BOOTLOADER_BUILD
  /* Bootloader: single-threaded, no lock needed.
   * Use simple NOP-loop delay (no HAL_Delay, no TIM2 dependency).
   * HSI 8MHz, ~5 cycles per loop → ~200000 iterations ≈ 1ms */
  static void Boot_DelayMs(uint32_t ms) {
      for (uint32_t i = 0; i < ms; i++) {
          for (volatile uint32_t j = 0; j < 2000; j++);
      }
  }
  #define EE_LOCK()       (true)
  #define EE_UNLOCK()
  #define EE_DELAY_MS(ms) Boot_DelayMs(ms)
#else
  /* APP: FreeRTOS mutex protection */
  #include "FreeRTOS.h"
    #include "task.h"
  #include "semphr.h"
  extern SemaphoreHandle_t xI2CMutex;

    static void EE_AppDelayMs(uint32_t ms)
    {
            if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
                    vTaskDelay(pdMS_TO_TICKS(ms));
            } else {
                    for (uint32_t i = 0; i < ms; i++) {
                            for (volatile uint32_t j = 0; j < 2000; j++);
                    }
            }
    }

  #define EE_LOCK()       (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdPASS)
  #define EE_UNLOCK()     xSemaphoreGive(xI2CMutex)
    #define EE_DELAY_MS(ms) EE_AppDelayMs(ms)
#endif

/* ==================================================================
 *  Low-level: write one byte
 * ================================================================== */
bool AT24C02_WriteByte(uint8_t addr, uint8_t data)
{
    bool ok = false;

    if (!EE_LOCK()) return false;

    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR << 1);
    if (MyI2C_ReciveAck()) goto exit;
    MyI2C_SendByte(addr);
    if (MyI2C_ReciveAck()) goto exit;
    MyI2C_SendByte(data);
    if (MyI2C_ReciveAck()) goto exit;
    ok = true;

exit:
    MyI2C_Stop();
    EE_UNLOCK();
    EE_DELAY_MS(6);  /* internal write cycle max 5ms */
    return ok;
}

/* ==================================================================
 *  Read one byte
 * ================================================================== */
bool AT24C02_ReadByte(uint8_t addr, uint8_t *pData)
{
    bool ok = false;

    if (pData == NULL) return false;
    if (!EE_LOCK()) return false;

    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR << 1);
    if (MyI2C_ReciveAck()) goto exit;
    MyI2C_SendByte(addr);
    if (MyI2C_ReciveAck()) goto exit;

    MyI2C_Start();
    MyI2C_SendByte((AT24C02_ADDR << 1) | 0x01);
    if (MyI2C_ReciveAck()) goto exit;
    *pData = MyI2C_RecvByte(1);
    ok = true;

exit:
    MyI2C_Stop();
    EE_UNLOCK();
    return ok;
}

/* ==================================================================
 *  Write buffer (handles page boundary)
 * ================================================================== */
bool AT24C02_WriteBuf(uint8_t addr, const uint8_t *pBuf, uint16_t len)
{
    //希望实现一个函数用于一块pBuf，用于告诉数据在哪，的数据写入，并且给出了需要写入的长度为len
    uint16_t remain = len;
    uint16_t offset =0;
    //判断是否合法
    if(pBuf ==NULL ||addr+len>AT24C02_TOTAL_SIZE)
        return false;
    //判断是否还有剩余没有写入
    
    while(remain>0)
    {
        uint8_t page_remain = AT24C02_PAGE_SIZE - (addr % AT24C02_PAGE_SIZE);
        //判断当前页能否写入完毕传入的若不能则分页写入,chunk为了表示当前页需要写入多少
        uint8_t chunk = (remain>page_remain)? page_remain : remain;
        //如果当前页写不满就使用remain 如果当前页写满了就使用pageremain   后面一次进行循环
        //拿I2c锁开始写入
       if (!EE_LOCK()) return false;

        MyI2C_Start();
        //设备名寄存器
        MyI2C_SendByte(AT24C02_ADDR << 1);
        if (MyI2C_ReciveAck())  { MyI2C_Stop(); EE_UNLOCK(); return false; }
        //设备写寄存器
        MyI2C_SendByte(addr);
        if (MyI2C_ReciveAck())  { MyI2C_Stop(); EE_UNLOCK(); return false; }
        //设备写的数据值
        for(uint8_t i=0;i<chunk;i++)
        {
            MyI2C_SendByte(pBuf[offset+i]);
            if (MyI2C_ReciveAck())  { MyI2C_Stop(); EE_UNLOCK(); return false; }
        }
        MyI2C_Stop();
        EE_UNLOCK();
        EE_DELAY_MS(6);
        addr=addr+chunk;
        offset+=chunk;
        remain=remain-chunk;
    }
return true;
}

/* ==================================================================
 *  Read buffer (sequential read with auto-increment)
 * ================================================================== */
bool AT24C02_ReadBuf(uint8_t addr, uint8_t *pBuf, uint16_t len)
{
    uint16_t i;

    if (pBuf == NULL || addr + len > AT24C02_TOTAL_SIZE)
        return false;

    if (!EE_LOCK()) return false;

    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR << 1);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); EE_UNLOCK(); return false; }
    MyI2C_SendByte(addr);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); EE_UNLOCK(); return false; }

    MyI2C_Start();
    MyI2C_SendByte((AT24C02_ADDR << 1) | 0x01);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); EE_UNLOCK(); return false; }

    for (i = 0; i < len - 1; i++) {
        pBuf[i] = MyI2C_RecvByte(0);
    }
    pBuf[i] = MyI2C_RecvByte(1);

    MyI2C_Stop();
    EE_UNLOCK();
    return true;
}

/* ==================================================================
 *  Initialize OTA metadata area
 * ================================================================== */
bool AT24C02_Init(void)
{
    uint32_t magic = 0;
    uint8_t buf[4];

    if (!AT24C02_ReadBuf(EE_MAGIC, buf, 4))
        return false;
    magic = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
            ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];

    if (magic == 0xA5A5A5A5)
        return true;

    uint8_t init_buf[4];
    init_buf[0] = 0xA5; init_buf[1] = 0xA5;
    init_buf[2] = 0xA5; init_buf[3] = 0xA5;
    if (!AT24C02_WriteBuf(EE_MAGIC, init_buf, 4))
        return false;

    uint8_t zero = 0;
    if (!AT24C02_WriteByte(EE_BOOT_CMD, zero))
        return false;

    /* 版本号 V1.0.3（OTA 测试用） */
    const char *ver = "V1.0.3";
    uint8_t ver_buf[16];
    memset(ver_buf, 0, sizeof(ver_buf));
    memcpy(ver_buf, ver, strlen(ver));
    return AT24C02_WriteBuf(EE_VERSION_STR, ver_buf, sizeof(ver_buf));
}
