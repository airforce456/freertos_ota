/**
 * @file    w25q64.h
 * @brief   W25Q64 8MB SPI NOR Flash driver
 * @note    SPI1: PA5=SCK, PA6=MISO, PA7=MOSI, PA4=CS (GPIO Output)
 *          Sector: 4KB, Block: 64KB, Total: 8MB (128 blocks)
 */
#ifndef __W25Q64_H__
#define __W25Q64_H__

#include <stdint.h>
#include <stdbool.h>

/* ---- Flash geometry ---- */
#define W25Q64_SECTOR_SIZE      4096    /* 4KB  (minimum erase unit) */
#define W25Q64_BLOCK_SIZE       65536   /* 64KB */
#define W25Q64_TOTAL_SIZE       8388608 /* 8MB  (128 blocks, 2048 sectors) */
#define W25Q64_PAGE_SIZE        256     /* 256 bytes per program page */

/* ---- OTA partition layout in W25Q64 ---- */
#define W25Q64_GOLDEN_ADDR      0x000000    /* Golden Image: 60KB backup */
#define W25Q64_OTA_SLOT_A       0x00F000    /* OTA Slot A: 60KB */
#define W25Q64_OTA_SLOT_B       0x01E000    /* OTA Slot B: 60KB */
#define W25Q64_OTA_SLOT_SIZE    0x00F000    /* 60KB per slot */

/* ---- Status Register bits ---- */
#define W25Q64_SR_BUSY          0x01        /* Write In Progress */
#define W25Q64_SR_WEL           0x02        /* Write Enable Latch */

/**
 * @brief  Initialize W25Q64 (CS pin high, verify JEDEC ID)
 * @return true=success, false=ID mismatch or SPI error
 */
bool W25Q64_Init(void);

/**
 * @brief  Read JEDEC Manufacturer + Device ID (3 bytes)
 * @param  pID  output buffer [3]: Manufacturer, MemoryType, Capacity
 */
void W25Q64_ReadJEDEC_ID(uint8_t *pID);

/**
 * @brief  Read data from flash
 * @param  addr  24-bit flash address
 * @param  pBuf  output buffer
 * @param  len   bytes to read
 */
void W25Q64_Read(uint32_t addr, uint8_t *pBuf, uint32_t len);

/**
 * @brief  Write one page (up to 256 bytes, must not cross page boundary)
 * @note   Caller must ensure: (addr % 256) + len <= 256
 *          and must call W25Q64_WriteEnable() first.
 * @param  addr  24-bit flash address
 * @param  pBuf  data to write
 * @param  len   bytes (≤ 256)
 */
void W25Q64_PageProgram(uint32_t addr, const uint8_t *pBuf, uint16_t len);

/**
 * @brief  Write arbitrary length (handles page boundaries internally)
 * @param  addr  start address
 * @param  pBuf  data
 * @param  len   total bytes
 * @return true=success, false=error
 */
bool W25Q64_Write(uint32_t addr, const uint8_t *pBuf, uint32_t len);

/**
 * @brief  Erase one sector (4KB)
 * @param  addr  sector-aligned address (addr % 4096 == 0)
 */
void W25Q64_SectorErase(uint32_t addr);

/**
 * @brief  Erase one 64KB block
 * @param  addr  block-aligned address (addr % 65536 == 0)
 */
void W25Q64_BlockErase(uint32_t addr);

/**
 * @brief  Full chip erase (takes ~40 seconds!)
 * @note   USE WITH CAUTION. Prefer SectorErase for OTA.
 */
void W25Q64_ChipErase(void);

/**
 * @brief  Wait until flash is not busy
 */
void W25Q64_WaitBusy(void);

/**
 * @brief  Enable write latch (must call before Program/Erase)
 */
void W25Q64_WriteEnable(void);

/**
 * @brief  Read Status Register 1
 */
uint8_t W25Q64_ReadSR(void);

/**
 * @brief  Calculate CRC32 over a flash region
 * @param  addr  start address
 * @param  len   bytes to checksum
 * @return CRC32 value
 */
uint32_t W25Q64_CRC32(uint32_t addr, uint32_t len);

#endif /* __W25Q64_H__ */
