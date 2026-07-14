/**
 * @file    boot_main.c
 * @brief   Bootloader 主程序
 *
 * 上电流程：
 *   1. 初始化最小系统（HSI 8MHz，不需要外设）
 *   2. 读 EEPROM 获取启动状态
 *   3. 根据 BootCmd 决定：正常跳 APP / OTA 升级 / Golden 恢复
 *   4. 跳转到 APP 执行
 *
 * 本文件不依赖 FreeRTOS，不使用 printf。
 */
#include "stm32f1xx_hal.h"
#include "boot_flash.h"

/* 外部依赖（at24c02.c、w25q64.c 驱动，仅使用其读写接口） */
#include "at24c02.h"
#include "w25q64.h"
#include "bsp_i2c.h"

/* ---- 跳转函数前向声明 ---- */
static void Boot_JumpToApp(uint32_t appAddr);

/* ==================================================================
 *  Bootloader 主入口
 * ================================================================== */
int main(void)
{
    /* ---- 初始化软 I2C（GPIO 配置 PB6/PB7）---- */
    MyI2C_Init();

    /* ---- 初始化 AT24C02 EEPROM ---- */
    AT24C02_Init();

    /* ---- 读取启动命令 ---- */
    uint8_t bootCmd = 0;
    if (!AT24C02_ReadByte(EE_BOOT_CMD, &bootCmd)) {
        /* EEPROM 读失败 → 尝试直接跳 APP */
        goto jump_to_app;
    }

    /* ---- 处理启动命令 ---- */
    switch (bootCmd) {
    case 0:
        /* 正常启动 */
        break;

    case 1: {
        /* OTA 升级：从 W25Q64 搬固件到 APP 区 */
        uint8_t slot = 0;
        AT24C02_ReadByte(EE_FW_SLOT, &slot);

        uint32_t fw_addr = (slot == 0) ? W25Q64_OTA_SLOT_A : W25Q64_OTA_SLOT_B;
        uint32_t fw_size = 0;
        uint32_t fw_crc  = 0;

        /* 读取固件大小和 CRC */
        uint8_t buf4[4];
        AT24C02_ReadBuf(EE_FW_SIZE, buf4, 4);
        fw_size = ((uint32_t)buf4[0] << 24) | ((uint32_t)buf4[1] << 16) |
                  ((uint32_t)buf4[2] << 8)  |  (uint32_t)buf4[3];

        AT24C02_ReadBuf(EE_FW_CRC32, buf4, 4);
        fw_crc = ((uint32_t)buf4[0] << 24) | ((uint32_t)buf4[1] << 16) |
                 ((uint32_t)buf4[2] << 8)  |  (uint32_t)buf4[3];

        if (fw_size == 0 || fw_size > APP_MAX_SIZE) {
            /* 固件大小异常 → 放弃升级 */
            uint8_t zero = 0;
            AT24C02_WriteByte(EE_BOOT_CMD, zero);
            goto jump_to_app;
        }

        /* 校验 W25Q64 上的 CRC32 */
        uint32_t actual_crc = W25Q64_CRC32(fw_addr, fw_size);
        if (actual_crc != fw_crc) {
            /* CRC 不匹配 → 放弃升级 */
            uint8_t zero = 0;
            AT24C02_WriteByte(EE_BOOT_CMD, zero);
            goto jump_to_app;
        }

        /* 擦除 APP 区 */
        if (!Boot_EraseAppArea()) {
            /* 擦除失败 → 重试计数 */
            uint8_t retry = 0;
            AT24C02_ReadByte(EE_RETRY_COUNT, &retry);
            retry++;
            if (retry >= 3) {
                /* 超过 3 次 → Golden 恢复 */
                bootCmd = 2;
            } else {
                AT24C02_WriteByte(EE_RETRY_COUNT, retry);
                /* 复位重试 */
                NVIC_SystemReset();
            }
            break;
        }

        /* 从 W25Q64 逐页搬到内部 Flash */
        {
            uint8_t pageBuf[256];
            bool writeOk = true;

            for (uint32_t offset = 0; offset < fw_size; offset += 256) {
                uint32_t chunk = (fw_size - offset > 256) ? 256 : (fw_size - offset);
                W25Q64_Read(fw_addr + offset, pageBuf, chunk);
                if (!Boot_FlashWriteBuf(APP_START_ADDR + offset, pageBuf, chunk)) {
                    writeOk = false;
                    break;
                }
            }

            if (writeOk) {
                /* 升级成功 → 清零状态 */
                uint8_t zero = 0;
                AT24C02_WriteByte(EE_BOOT_CMD, zero);
                AT24C02_WriteByte(EE_RETRY_COUNT, zero);
                AT24C02_WriteByte(EE_DOWNLOAD_STATUS, zero);
            } else {
                /* 写 Flash 失败 → 重试 */
                uint8_t retry = 0;
                AT24C02_ReadByte(EE_RETRY_COUNT, &retry);
                retry++;
                if (retry >= 3) {
                    bootCmd = 2;  /* 强制 Golden 恢复 */
                } else {
                    AT24C02_WriteByte(EE_RETRY_COUNT, retry);
                    NVIC_SystemReset();
                }
            }
        }
        break;
    }

    case 2:
    default: {
        /* Golden Image 恢复 */
        uint8_t pageBuf[256];

        if (!Boot_EraseAppArea())
            break;

        /* W25Q64_Init 在之前未调用，需确保 SPI1 和 GPIO 已初始化 */
        /* 这里假设 Golden Image 大小 ≤ 60KB */
        bool ok = true;
        for (uint32_t offset = 0; offset < 0xF000; offset += 256) {
            W25Q64_Read(W25Q64_GOLDEN_ADDR + offset, pageBuf, 256);
            if (!Boot_FlashWriteBuf(APP_START_ADDR + offset, pageBuf, 256)) {
                ok = false;
                break;
            }
        }

        if (ok) {
            uint8_t zero = 0;
            AT24C02_WriteByte(EE_BOOT_CMD, zero);
            AT24C02_WriteByte(EE_RETRY_COUNT, zero);
        }
        break;
    }
    }

jump_to_app:
    /* ---- 验证并跳转 APP ---- */
    if (Boot_CheckAppValid()) {
        Boot_JumpToApp(APP_START_ADDR);
    }

    /* APP 不可用 → 死循环（调试时可在这里加 LED 闪烁） */
    while (1);
}

/* ==================================================================
 *  跳转到 APP
 *
 *  步骤：
 *   1. 关全局中断（APP 会自己开）
 *   2. 设向量表偏移（其实不需要，APP 会自己设 SCB->VTOR）
 *   3. 设 MSP 为 APP 向量表的第一个字
 *   4. 跳转到 APP 的 Reset_Handler
 * ================================================================== */
static void Boot_JumpToApp(uint32_t appAddr)
{
    uint32_t appSp  = *(volatile uint32_t *)appAddr;        /* 向量表[0] = SP */
    uint32_t appPc  = *(volatile uint32_t *)(appAddr + 4);  /* 向量表[1] = PC */

    /* 关全局中断 */
    __disable_irq();

    /* 复位所有外设到上电默认状态（避免 APP 初始化时冲突） */
    /* 复位 RCC（恢复默认时钟配置）*/
    /* 注意：这里不调 HAL_DeInit，因为 Bootloader 不用 HAL */
    __HAL_RCC_GPIOA_CLK_DISABLE();
    __HAL_RCC_GPIOB_CLK_DISABLE();
    __HAL_RCC_GPIOC_CLK_DISABLE();
    __HAL_RCC_SPI1_CLK_DISABLE();
    __HAL_RCC_USART1_CLK_DISABLE();
    __HAL_RCC_USART2_CLK_DISABLE();
    __HAL_RCC_I2C1_CLK_DISABLE();
    __HAL_RCC_TIM2_CLK_DISABLE();
    __HAL_RCC_DMA1_CLK_DISABLE();

    /* 设 MSP */
    __set_MSP(appSp);

    /* 设向量表偏移（APP 启动后会自己设，但先设了更安全） */
    SCB->VTOR = appAddr;

    /* 跳转：把 PC 值加载到 PC 寄存器 */
    ((void (*)(void))appPc)();

    /* 不会执行到这里 */
}
