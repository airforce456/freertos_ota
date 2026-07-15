# Bootloader + APP 双固件调试记录

## 最终分区

```
┌─────────────────────────────┐ 0x08000000
│  Bootloader (20KB)          │  make boot → build_boot/bootloader.hex
├─────────────────────────────┤ 0x08005000
│  APP (44KB)                 │  make app  → build/ota.hex
└─────────────────────────────┘ 0x0800FFFF
```

Bootloader 上电先跑，读 EEPROM 决定启动路径，然后跳转到 APP。APP 从 `0x08005000` 偏移启动，需要设置 `SCB->VTOR = 0x08005000`。

---

## 编译命令

```
make boot   → 生成 build_boot/bootloader.hex (20KB)
make app    → 生成 build/ota.hex (44KB)
```

两个编译产物放在不同目录（`build_boot/` 和 `build/`），互不覆盖。

APP 使用 `-Os` 体积优化，`text=44540` 刚好装进 44KB。Bootloader 也使用 `-Os`，`text≈19KB` 装进 20KB。

---

## 烧录方式

### 必须全片擦除

**每次切换固件类型（单一固件 ↔ Bootloader+APP）时必须全片擦除**，否则 Flash 残留会干扰启动。

### 烧录步骤

1. STM32CubeProgrammer → Erase Chip（全片擦除）
2. Program → 选 `build_boot/bootloader.hex` → Start
3. Program → 选 `build/ota.hex` → Start
4. 重新上电

hex 文件自带地址信息，两个 hex 不会互相覆盖。

---

## 调试过程中遇到的问题

### 问题 1：APP 没烧进去，`0x08005000` 全是 `0xFF`

**现象**：用 VS Code 调试 `"STlink Bootloader"` 配置时，只烧了 Bootloader，APP 区为空。`Boot_CheckAppValid()` 读到 SP=0xFFFFFFFF，校验失败，Bootloader 进入死循环。

**根因**：VS Code 的 `"request": "launch"` 配置每次只烧一个 elf 文件，不会同时烧两个。

**解决**：用 STM32CubeProgrammer 先全片擦除 + 烧两个 hex，再上电测试。或用 VS Code attach 模式只连接不烧录。

---

### 问题 2：APP 启动后卡在 `HAL_Delay` 死循环

**现象**：串口无输出。ST-Link attach 后发现 PC 停在 `HAL_Delay` 的 `while ((HAL_GetTick() - tickstart) < wait)` 循环中。

**根因**：`uwTick` 永远为 0，因为 TIM2 中断没有触发。

**排查过程**：

1. 检查 APP 向量表 → `0x08005000` 处 SP 和 PC 合法 ✅
2. 检查 `stm32f1xx_it.c` → `TIM2_IRQHandler` 存在 ✅
3. 检查 `stm32f1xx_hal_timebase_tim.c` → `HAL_InitTick` 配置正确 ✅
4. 检查 Bootloader 跳转代码 `Boot_JumpToApp()`：

```c
// boot_main.c 第 189 行
__disable_irq();          // ← 关了全局中断！

// 第 201 行
__HAL_RCC_TIM2_CLK_DISABLE();  // ← 关了 TIM2 时钟

// 第 211 行
((void (*)(void))appPc)();     // 跳转到 APP
```

5. APP 的 `HAL_Init()` → `HAL_InitTick()` 重新使能了 TIM2 时钟 ✅
6. **但 `HAL_Init()` 没有重新开全局中断** ❌

**根因总结**：Bootloader 跳转前调了 `__disable_irq()`，APP 继承了这个状态。虽然 TIM2 时钟被重新使能，但 PRIMASK 寄存器仍是 1（全局中断关闭），TIM2 中断永远不会触发 → `uwTick` 永不递增 → `HAL_Delay` 死循环。

**修复**：在 APP 的 `main()` 最开头加：

```c
SCB->VTOR = 0x08005000;   // 向量表偏移
__enable_irq();            // 重新打开全局中断（Bootloader 关的）
```

---

### 问题 3：VS Code 调试只烧一个 elf

**现象**：用 VS Code `"STlink APP"` 调试时，只烧了 `ota.elf`，没烧 Bootloader。CPU 从 `0x08000000` 读向量表，那里是空的（`0xFF`），直接跑飞。

**解决**：创建 `"STlink APP (attach)"` 配置，用 `"request": "attach"` 代替 `"request": "launch"`。attach 模式只连接目标板，不烧录，不 reset。

```json
{
    "name": "STlink APP (attach)",
    "executable": "${workspaceRoot}\\build\\ota.elf",
    "request": "attach",   // ← 关键：不烧录
    "type": "cortex-debug",
    "servertype": "openocd",
    ...
}
```

---

## 启动完整流程

```
上电
  ↓
Bootloader @ 0x08000000
  ├─ SystemInit() → HSI 8MHz
  ├─ main()
  │   ├─ MyI2C_Init()          (软 I2C, PB6/PB7)
  │   ├─ AT24C02_Init()        (EEPROM 初始化, 用 Boot_DelayMs)
  │   ├─ 读 EEPROM[BootCmd]
  │   │   ├─ 0: 正常 → 跳到 jump_to_app
  │   │   ├─ 1: OTA 升级 → 从 W25Q64 搬固件
  │   │   └─ 2: Golden 恢复
  │   └─ Boot_CheckAppValid()
  │       └─ 合法 → Boot_JumpToApp(0x08005000)
  │           ├─ __disable_irq()
  │           ├─ 关所有外设时钟 (GPIO/SPI/USART/TIM2/DMA)
  │           ├─ __set_MSP(appSp)
  │           ├─ SCB->VTOR = appAddr
  │           └─ ((void(*)(void))appPc)()
  ↓
APP @ 0x08005000
  ├─ Reset_Handler → SystemInit → main()
  ├─ SCB->VTOR = 0x08005000
  ├─ __enable_irq()             ← 关键！Bootloader 关的
  ├─ HAL_Init() → HAL_InitTick() → TIM2 1ms 时基启动
  ├─ SystemClock_Config()
  ├─ MX_GPIO/USART/DMA/SPI_Init()
  ├─ BSP_Init() → OLED_Init() → MPU6050_Init()
  ├─ AT24C02_Init(), W25Q64_Init()
  ├─ FreeRTOS 对象创建 + 任务创建
  ├─ ESP8266_Init() → OneNET 注册/连接
  └─ vTaskStartScheduler() → 永不返回
```

---

## 关键设计决策

| 决策 | 原因 |
|------|------|
| Bootloader 不用 HAL_Delay | Bootloader 没有 TIM2 时基，用 NOP-loop 延时 (`Boot_DelayMs`) |
| at24c02.c 条件编译 | `BOOTLOADER_BUILD` 宏切换 RTOS/裸机模式（信号量 vs 无锁，vTaskDelay vs HAL_Delay） |
| w25q64.c 条件编译 | Bootloader 用手写 SPI 驱动（`boot_spi.c`），APP 用 HAL SPI |
| 全片擦除再烧录 | 避免旧固件残留导致向量表错乱 |
| APP 用 `-Os` 优化 | 44KB 装不下 `-Og` 的 48KB 代码 |
| `SCB->VTOR` + `__enable_irq` | Bootloader 跳转后 APP 必须恢复中断和向量表 |

---

## 每次 CubeMX 重新生成后的检查清单

- [ ] `stm32f1xx_hal_timebase_tim.c` 是否被覆盖（TIM2 InitTick 的 AutoReloadPreload 和时钟屏障）
- [ ] `stm32f1xx_it.c` 中的 `TIM2_IRQHandler` 是否还在
- [ ] `main.c` 中的 `SCB->VTOR` 和 `__enable_irq` 是否还在
- [ ] Makefile 末尾的 boot/app target 是否被覆盖
- [ ] 链接脚本 `STM32F103C8Tx_BOOT.ld` 和 `STM32F103C8Tx_APP.ld` 不会被 CubeMX 覆盖（CubeMX 只管 `STM32F103C8Tx_FLASH.ld`）
