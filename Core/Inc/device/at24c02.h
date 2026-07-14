/**
 * @file    at24c02.h
 * @brief   AT24C02 256-byte I2C EEPROM driver
 * @note    Shares I2C1 bus (PB6=SCL, PB7=SDA) with OLED & MPU6050.
 *          Address: 0x50 (A0=A1=A2=GND)
 *          Page size: 8 bytes, total 256 bytes (32 pages)
 */
#ifndef __AT24C02_H__
#define __AT24C02_H__

#include <stdint.h>
#include <stdbool.h>

#define AT24C02_ADDR        0x50    /* 7-bit I2C address */
#define AT24C02_PAGE_SIZE   8       /* bytes per page write */
#define AT24C02_TOTAL_SIZE  256     /* total EEPROM size */

/* ---- OTA metadata addresses (see EEPROM layout in docs) ---- */
#define EE_MAGIC            0x00    /* 4 bytes: 0xA5A5A5A5 */
#define EE_BOOT_CMD         0x04    /* 1 byte: 0=normal, 1=upgrade, 2=golden recovery */
#define EE_FW_SIZE          0x05    /* 4 bytes */
#define EE_FW_CRC32         0x09    /* 4 bytes */
#define EE_FW_VERSION       0x0D    /* 4 bytes */
#define EE_DOWNLOAD_STATUS  0x11    /* 1 byte: 0=idle, 1=downloading, 2=done, 3=verify fail */
#define EE_RETRY_COUNT      0x12    /* 1 byte */
#define EE_FW_SLOT          0x13    /* 1 byte: 0=SlotA, 1=SlotB */
#define EE_VERSION_STR      0x20    /* 16 bytes: version string e.g. "v1.0.0" */

/**
 * @brief  Write one byte to EEPROM
 * @param  addr   EEPROM internal address (0~255)
 * @param  data   byte to write
 * @return true=success, false=error
 */
bool AT24C02_WriteByte(uint8_t addr, uint8_t data);

/**
 * @brief  Read one byte from EEPROM
 * @param  addr   EEPROM internal address (0~255)
 * @param  pData  output pointer
 * @return true=success, false=error
 */
bool AT24C02_ReadByte(uint8_t addr, uint8_t *pData);

/**
 * @brief  Write multiple bytes (handles page boundary)
 * @param  addr   start address
 * @param  pBuf   data buffer
 * @param  len    number of bytes
 * @return true=success, false=error
 */
bool AT24C02_WriteBuf(uint8_t addr, const uint8_t *pBuf, uint16_t len);

/**
 * @brief  Read multiple bytes
 * @param  addr   start address
 * @param  pBuf   output buffer
 * @param  len    number of bytes
 * @return true=success, false=error
 */
bool AT24C02_ReadBuf(uint8_t addr, uint8_t *pBuf, uint16_t len);

/**
 * @brief  Initialize EEPROM with default OTA metadata
 * @note   Writes magic + BootCmd=0 + version="v1.0.0" if magic mismatch.
 * @return true=init OK, false=write error
 */
bool AT24C02_Init(void);

#endif /* __AT24C02_H__ */
