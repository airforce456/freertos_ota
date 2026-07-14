/**
 * @file    boot_flash.c
 * @brief   STM32F103 内部 Flash 擦写读（仅操作 APP 区）
 * @note    Flash 写之前必须先擦除（擦除把所有 bit 变 1，写只能把 1 变 0）
 *          擦写期间必须关全局中断
 */
#include "boot_flash.h"
#include "stm32f1xx_hal.h"
#include <string.h>

/* ==================================================================
 *  读一个 32 位字（直接指针解引用）
 * ================================================================== */
uint32_t Boot_FlashReadWord(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* ==================================================================
 *  写一个 32 位字（地址必须已擦除 = 0xFFFFFFFF）
 * ================================================================== */
bool Boot_FlashWriteWord(uint32_t addr, uint32_t data)
{
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data) != HAL_OK)
        return false;

    /* 回读验证 */
    if (Boot_FlashReadWord(addr) != data)
        return false;

    return true;
}

/* ==================================================================
 *  擦除 APP 区全部 52 页（每页 1KB）
 *  耗时约 52 × 20ms ≈ 1 秒
 * ================================================================== */
bool Boot_EraseAppArea(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t pageError = 0;

    HAL_FLASH_Unlock();
    __disable_irq();

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_START_ADDR;
    erase.NbPages     = 44;  /* 44KB / 1KB per page */

    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &pageError);

    __enable_irq();
    HAL_FLASH_Lock();

    if (status != HAL_OK)
        return false;

    /* 验证第一页确实被擦除了 */
    if (Boot_FlashReadWord(APP_START_ADDR) != 0xFFFFFFFF)
        return false;

    return true;
}

/* ==================================================================
 *  写入一块数据到 Flash
 *  流程：解锁 → 关中断 → 按页擦除 → 按字写入 → 开中断 → 上锁
 *
 *  @param addr  Flash 起始地址（必须 4 字节对齐）
 *  @param pBuf  源数据
 *  @param len   字节数（自动补齐到 4 字节边界）
 * ================================================================== */
bool Boot_FlashWriteBuf(uint32_t addr, const uint8_t *pBuf, uint32_t len)
{
    uint32_t word;
    uint32_t i;

    if (pBuf == NULL || len == 0)
        return false;

    if (addr < APP_START_ADDR || addr + len > (APP_END_ADDR + 1))
        return false;  /* 超出 APP 区范围 */

    HAL_FLASH_Lock();    /* 先确保上锁状态 */
    HAL_FLASH_Unlock();
    __disable_irq();

    for (i = 0; i < len; i += 4) {
        /* 拼成 32 位字（处理尾部不足 4 字节的情况） */
        word  = (uint32_t)pBuf[i];
        word |= (i + 1 < len) ? ((uint32_t)pBuf[i + 1] << 8)  : 0xFF000000;
        word |= (i + 2 < len) ? ((uint32_t)pBuf[i + 2] << 16) : 0xFF000000;
        word |= (i + 3 < len) ? ((uint32_t)pBuf[i + 3] << 24) : 0xFF000000;

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK) {
            __enable_irq();
            HAL_FLASH_Lock();
            return false;
        }

        /* 回读验证这个字 */
        if (Boot_FlashReadWord(addr + i) != word) {
            __enable_irq();
            HAL_FLASH_Lock();
            return false;
        }
    }

    __enable_irq();
    HAL_FLASH_Lock();
    return true;
}

/* ==================================================================
 *  验证 APP 区向量表是否合法
 *
 *  向量表前两个条目：
 *    [0] = 初始 SP 值  → 必须在 RAM 范围 (0x20000000 ~ 0x20005000)
 *    [1] = Reset_Handler → 必须在 APP 区且 bit0=1（Thumb 模式）
 *
 *  这只能做"快速合法检查"，不能保证固件功能正常。
 *  完整校验需要 CRC32 比对。
 * ================================================================== */
bool Boot_CheckAppValid(void)
{
    uint32_t sp = Boot_FlashReadWord(APP_START_ADDR);       /* 向量表[0] */
    uint32_t pc = Boot_FlashReadWord(APP_START_ADDR + 4);   /* 向量表[1] */

    /* SP 必须在 RAM 范围内 */
    if (sp < 0x20000000 || sp > 0x20005000)
        return false;

    /* PC 必须在 APP 区 */
    if (pc < APP_START_ADDR || pc > APP_END_ADDR)
        return false;

    /* Cortex-M3 只支持 Thumb 指令，bit0 必须为 1 */
    if ((pc & 0x01) == 0)
        return false;

    return true;
}
