// boot_flash.h
#ifndef __BOOT_FLASH_H__
#define __BOOT_FLASH_H__

#include <stdint.h>
#include <stdbool.h>

/* APP 区地址范围 */
#define APP_START_ADDR   0x08005000
#define APP_END_ADDR     0x0800FFFF   /* 64KB 末尾 */
#define APP_MAX_SIZE     (44 * 1024)  /* 44KB */

/* STM32F103 Flash 页大小 = 1KB (HAL 已有定义 FLASH_PAGE_SIZE) */

/**
 * @brief 擦除 APP 区（全部 52KB，52 个页）
 * @return true=成功
 */
bool Boot_EraseAppArea(void);

/**
 * @brief 按 32 位字写入 Flash（地址必须已擦除）
 * @param addr  Flash 地址（必须 4 字节对齐）
 * @param data  32 位数据
 * @return true=成功
 */
bool Boot_FlashWriteWord(uint32_t addr, uint32_t data);

/**
 * @brief 从 Flash 读一个 32 位字
 */
uint32_t Boot_FlashReadWord(uint32_t addr);

/**
 * @brief 写入一块数据（自动分页擦除 + 按字写入）
 * @param addr  起始地址
 * @param pBuf  数据指针
 * @param len   字节数
 * @return true=成功
 */
bool Boot_FlashWriteBuf(uint32_t addr, const uint8_t *pBuf, uint32_t len);

/**
 * @brief 验证 APP 区向量表是否合法
 *        检查 SP 初始值是否在 RAM 范围内
 * @return true=合法
 */
bool Boot_CheckAppValid(void);

#endif
