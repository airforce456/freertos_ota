/**
 * @file    at24c02.c
 * @brief   AT24C02 256-byte I2C EEPROM driver implementation
 * @note    Uses soft I2C (PB6/PB7), shared with OLED & MPU6050.
 *          Caller must hold xI2CMutex before calling any function.
 */
#include "at24c02.h"
#include "bsp_i2c.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

extern SemaphoreHandle_t xI2CMutex;

/* ==================================================================
 *  Low-level: write one byte
 *  AT24C02 write sequence: Start → DevAddr(W) → MemAddr → Data → Stop
 *  Must wait up to 5ms for internal write cycle after Stop.
 * ================================================================== */
bool AT24C02_WriteByte(uint8_t addr, uint8_t data)
{
    bool ok = false;

    if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) != pdPASS)
        return false;

    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR << 1);          /* Device addr + Write */
    if (MyI2C_ReciveAck()) goto exit;
    MyI2C_SendByte(addr);                        /* Memory address */
    if (MyI2C_ReciveAck()) goto exit;
    MyI2C_SendByte(data);                        /* Data byte */
    if (MyI2C_ReciveAck()) goto exit;
    ok = true;

exit:
    MyI2C_Stop();
    xSemaphoreGive(xI2CMutex);

    /* AT24C02 internal write cycle: max 5ms */
    vTaskDelay(pdMS_TO_TICKS(6));
    return ok;
}

/* ==================================================================
 *  Read one byte
 *  Sequence: Start → DevAddr(W) → MemAddr → ReStart → DevAddr(R) → Data(NACK) → Stop
 * ================================================================== */
bool AT24C02_ReadByte(uint8_t addr, uint8_t *pData)
{
    bool ok = false;

    if (pData == NULL) return false;
    if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) != pdPASS)
        return false;

    /* Dummy write to set internal address pointer */
    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR << 1);
    if (MyI2C_ReciveAck()) goto exit;
    MyI2C_SendByte(addr);
    if (MyI2C_ReciveAck()) goto exit;

    /* Repeated Start + Read */
    MyI2C_Start();
    MyI2C_SendByte((AT24C02_ADDR << 1) | 0x01);  /* Device addr + Read */
    if (MyI2C_ReciveAck()) goto exit;
    *pData = MyI2C_RecvByte(1);                    /* NACK = last byte */
    ok = true;

exit:
    MyI2C_Stop();
    xSemaphoreGive(xI2CMutex);
    return ok;
}

/* ==================================================================
 *  Write buffer with page boundary handling
 *  AT24C02 page = 8 bytes. If write crosses page boundary,
 *  split into multiple page writes.
 * ================================================================== */
bool AT24C02_WriteBuf(uint8_t addr, const uint8_t *pBuf, uint16_t len)
{
    uint16_t remain = len;
    uint8_t offset = 0;

    if (pBuf == NULL || addr + len > AT24C02_TOTAL_SIZE)
        return false;

    while (remain > 0) {
        uint8_t page_remain = AT24C02_PAGE_SIZE - (addr % AT24C02_PAGE_SIZE);
        uint8_t chunk = (remain < page_remain) ? (uint8_t)remain : page_remain;

        if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) != pdPASS)
            return false;

        MyI2C_Start();
        MyI2C_SendByte(AT24C02_ADDR << 1);
        if (MyI2C_ReciveAck()) { MyI2C_Stop(); xSemaphoreGive(xI2CMutex); return false; }
        MyI2C_SendByte(addr);
        if (MyI2C_ReciveAck()) { MyI2C_Stop(); xSemaphoreGive(xI2CMutex); return false; }

        for (uint8_t i = 0; i < chunk; i++) {
            MyI2C_SendByte(pBuf[offset + i]);
            if (MyI2C_ReciveAck()) { MyI2C_Stop(); xSemaphoreGive(xI2CMutex); return false; }
        }

        MyI2C_Stop();
        xSemaphoreGive(xI2CMutex);

        /* Wait for internal write cycle (max 5ms per page) */
        vTaskDelay(pdMS_TO_TICKS(6));

        offset += chunk;
        addr   += chunk;
        remain -= chunk;
    }
    return true;
}

/* ==================================================================
 *  Read buffer (AT24C02 supports sequential read with auto-increment)
 * ================================================================== */
bool AT24C02_ReadBuf(uint8_t addr, uint8_t *pBuf, uint16_t len)
{
    uint16_t i;

    if (pBuf == NULL || addr + len > AT24C02_TOTAL_SIZE)
        return false;

    if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) != pdPASS)
        return false;

    /* Dummy write to set address */
    MyI2C_Start();
    MyI2C_SendByte(AT24C02_ADDR << 1);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); xSemaphoreGive(xI2CMutex); return false; }
    MyI2C_SendByte(addr);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); xSemaphoreGive(xI2CMutex); return false; }

    /* Repeated Start + sequential read */
    MyI2C_Start();
    MyI2C_SendByte((AT24C02_ADDR << 1) | 0x01);
    if (MyI2C_ReciveAck()) { MyI2C_Stop(); xSemaphoreGive(xI2CMutex); return false; }

    for (i = 0; i < len - 1; i++) {
        pBuf[i] = MyI2C_RecvByte(0);   /* ACK = continue reading */
    }
    pBuf[i] = MyI2C_RecvByte(1);       /* NACK = last byte */

    MyI2C_Stop();
    xSemaphoreGive(xI2CMutex);
    return true;
}

/* ==================================================================
 *  Initialize OTA metadata area
 *  Checks magic number; if invalid, writes default values.
 * ================================================================== */
bool AT24C02_Init(void)
{
    uint32_t magic = 0;
    uint8_t buf[4];

    /* Read existing magic */
    if (!AT24C02_ReadBuf(EE_MAGIC, buf, 4))
        return false;
    magic = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
            ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];

    if (magic == 0xA5A5A5A5)
        return true;  /* Already initialized */

    /* First-time init: write magic + defaults */
    uint8_t init_buf[4];
    init_buf[0] = 0xA5; init_buf[1] = 0xA5;
    init_buf[2] = 0xA5; init_buf[3] = 0xA5;
    if (!AT24C02_WriteBuf(EE_MAGIC, init_buf, 4))
        return false;

    /* BootCmd = 0 (normal boot) */
    uint8_t zero = 0;
    if (!AT24C02_WriteByte(EE_BOOT_CMD, zero))
        return false;

    /* Version string = "v1.0.0" */
    const char *ver = "v1.0.0";
    return AT24C02_WriteBuf(EE_VERSION_STR, (const uint8_t *)ver, strlen(ver));
}
