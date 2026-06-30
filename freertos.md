# 手动移植 FreeRTOS 到 STM32 单片机

本文基于 STM32F103C8Tx（Cortex-M3）+ GCC/Makefile 工具链的实际移植经验。

## 1. 项目文件结构

将 FreeRTOS 源码以下列结构放入 STM32CubeMX 生成的工程中：

```
project/
├── Core/
│   ├── Inc/
│   │   └── FreeRTOSConfig.h          # FreeRTOS 配置文件（必须）
│   └── Src/
│       ├── main.c
│       ├── stm32f1xx_it.c            # 中断处理（需修改）
│       ...
├── Drivers/
│   ├── CMSIS/
│   └── STM32F1xx_HAL_Driver/
├── Middlewares/
│   └── FreeRTOS/
│       └── Source/
│           ├── include/              # 19 个头文件
│           ├── portable/
│           │   ├── GCC/ARM_CM3/      # port.c, portmacro.h
│           │   └── MemMang/          # heap_4.c （推荐）
│           ├── tasks.c
│           ├── list.c
│           ├── queue.c
│           ├── timers.c
│           ├── event_groups.c
│           └── stream_buffer.c
├── Makefile
├── startup_stm32f103xb.s
└── STM32F103C8Tx_FLASH.ld
```
CFSR:
✅ IACCVIOL = 0（不是非法指令）
✅ INVSTATE = 0（CPU状态正常）
✅ UNDEFINSTR = 0（不是执行了非法指令）
✅ UNALIGNED = 0（不是未对齐访问）
✅ DIVBYZERO = 0（不是除零）
✅ FORCED = 0（不是其他Fault升级）

BFAR:0xA5A5A5xx  栈溢出（Stack Overflow）
## 2. 需要编译的 FreeRTOS 源文件

共 8 个 `.c` 文件必须加入编译：

| 文件 | 作用 |
|---|---|
| `tasks.c` | 任务创建、调度、删除等核心 API |
| `list.c` | 链表数据结构，内核内部使用 |
| `queue.c` | 队列、信号量、互斥量 |
| `timers.c` | 软件定时器 |
| `event_groups.c` | 事件标志组 |
| `stream_buffer.c` | 流缓冲区和消息缓冲区 |
| `portable/MemMang/heap_4.c` | 堆内存管理（推荐 heap_4） |
| `portable/GCC/ARM_CM3/port.c` | Cortex-M3 移植层（任务切换、中断管理） |

不需要编译 `croutine.c`（协程），除非明确需要。

### Makefile 示例

```makefile
C_SOURCES = \
Core/Src/main.c \
...
Middlewares/FreeRTOS/Source/tasks.c \
Middlewares/FreeRTOS/Source/list.c \
Middlewares/FreeRTOS/Source/queue.c \
Middlewares/FreeRTOS/Source/timers.c \
Middlewares/FreeRTOS/Source/event_groups.c \
Middlewares/FreeRTOS/Source/stream_buffer.c \
Middlewares/FreeRTOS/Source/portable/MemMang/heap_4.c \
Middlewares/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c
```

### Include 路径

```
-IMiddlewares/FreeRTOS/Source/include
-IMiddlewares/FreeRTOS/Source/portable/GCC/ARM_CM3
```

## 3. FreeRTOSConfig.h 关键配置

```c
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ⚠️ 必须包含 HAL 头文件，获取 SystemCoreClock */
#include "stm32f1xx_hal.h"

/* 系统时钟频率 */
#define configCPU_CLOCK_HZ              ( SystemCoreClock )

/* === 调度配置 === */
#define configUSE_PREEMPTION            1       /* 抢占式调度 */
#define configUSE_IDLE_HOOK             0       /* 空闲任务钩子 */
#define configUSE_TICK_HOOK             0       /* 设为 0，HAL 时基由独立定时器提供 */
#define configTICK_RATE_HZ              ( ( TickType_t )1000 )  /* 1ms 心跳 */
#define configMAX_PRIORITIES            32      /* 最大优先级数 */
#define configMINIMAL_STACK_SIZE        128     /* Idle 任务栈大小（字） */
#define configTOTAL_HEAP_SIZE           ( 10 * 1024 )  /* 堆大小 10KB */
#define configUSE_16_BIT_TICKS          0       /* 32 位 Tick 计数器 */

/* === Cortex-M3 中断优先级 === */
#define configPRIO_BITS                 4       /* STM32F1 NVIC 4bit */

/* 最低优先级（数值最大） */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15

/* FreeRTOS 可管理的中断上限（优先级 0-4 不能调用 FreeRTOS API） */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

/* 写入 NVIC 寄存器时左移到高 4 位 */
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8-configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8-configPRIO_BITS) )

/* === ⚠️ ISR 名称映射（关键！忘记会导致崩溃） === */
#define vPortSVCHandler                SVC_Handler
#define xPortPendSVHandler             PendSV_Handler
#define xPortSysTickHandler            SysTick_Handler

/* === ⚠️ 启用用到的 API 函数（忘记会导致链接失败） === */
#define INCLUDE_vTaskDelay              1
#define INCLUDE_vTaskSuspend            1
#define INCLUDE_xTaskGetCurrentTaskHandle 1

#endif /* FREERTOS_CONFIG_H */
```

### 中断优先级原理

STM32F103 的 NVIC 使用 4 位优先级（configPRIO_BITS = 4），但寄存器是 8 位宽，所以配置值需要左移 4 位写入：

```
用户配置       →  写入 NVIC
0x0F  (15)        0xF0  → 最低优先级（FreeRTOS 内核使用）
0x05  (5)         0x50  → 可管理中断上限
0x00  (0)         0x00  → 最高优先级（不能被 FreeRTOS 抢占）
```

**规则**：中断优先级数值 ≥ 5 的 ISR 可以调用 FreeRTOS API；优先级 0-4 的 ISR 不能调用。

## 4. 中断向量映射（最关键步骤）

### 问题

FreeRTOS 的 Cortex-M3 移植层 `port.c` 中定义了三个关键 ISR：

| FreeRTOS 函数 | 功能 |
|---|---|
| `vPortSVCHandler()` | SVC — 启动第一个任务 |
| `xPortPendSVHandler()` | PendSV — 任务上下文切换 |
| `xPortSysTickHandler()` | SysTick — 系统心跳 |

但启动文件 `startup_stm32f103xb.s` 的向量表中使用的是 CMSIS 标准名称：
`SVC_Handler`、`PendSV_Handler`、`SysTick_Handler`。

### 解决方案

**步骤 A** — 在 `FreeRTOSConfig.h` 中添加 `#define` 映射：

```c
#define vPortSVCHandler                SVC_Handler
#define xPortPendSVHandler             PendSV_Handler
#define xPortSysTickHandler            SysTick_Handler
```

这样 `port.c` 中的函数名经预处理器替换后，与向量表中的名称匹配。

**步骤 B** — 从 `stm32f1xx_it.c` 中移除冲突的空 ISR：

CubeMX 生成的 `stm32f1xx_it.c` 中包含这三个 ISR 的空壳实现。映射宏生效后，`port.c` 会提供同名函数，与 `it.c` 中的定义产生重复定义。必须用 `#if 0` 包裹或直接删除：

```c
#if 0  /* FreeRTOS 已通过 port.c 提供这些 ISR */
void SVC_Handler(void)      { /* 空壳 — 会导致崩溃 */ }
void PendSV_Handler(void)   { /* 空壳 — 会导致崩溃 */ }
void SysTick_Handler(void)  { /* 空壳 — 会导致崩溃 */ }
#endif
```

### 崩溃现象

如果忘记这一步，`vTaskStartScheduler()` 触发 SVC 后，CPU 跳转到空壳 ISR，返回时 PC 跳到非法地址，导致 **HardFault → Double Fault → CPU Lockup**：

```
Program received signal SIGINT, Interrupt.
0xf3af4804 in ?? ()                          ← 非法地址
[stm32f1x.cpu] clearing lockup after double fault
```

## 5. HAL 时基与 FreeRTOS SysTick 的冲突

### 问题

STM32 HAL 库依赖 `HAL_IncTick()` 提供 `HAL_Delay()` 等延时功能。默认情况下 HAL 使用 SysTick 作为时基源，但 FreeRTOS 的 `#define xPortSysTickHandler SysTick_Handler` 会让 port.c 接管 SysTick ISR。如果 SysTick ISR 中不调用 `HAL_IncTick()`，`HAL_Delay()` 就会卡死。

### 推荐方案：CubeMX 配置独立定时器作为 HAL 时基

在 CubeMX 中将 HAL 时基源从 SysTick 改为硬件定时器（如 TIM2），让 FreeRTOS 独占 SysTick，HAL 使用独立定时器。

**CubeMX 操作步骤：**

1. 打开 `ota.ioc` → **Pinout & Configuration** 标签页
2. **System Core** → **SYS** → **Timebase Source** 下拉选择 `TIM2`
3. 按 `Ctrl+S` 保存，CubeMX 会自动重新生成代码

**CubeMX 自动生成的内容：**

| 文件 | 内容 |
|---|---|
| `Core/Src/stm32f1xx_hal_timebase_tim.c` | 覆盖 `HAL_InitTick()`，用 TIM2 替代 SysTick；同时提供 `HAL_SuspendTick()` / `HAL_ResumeTick()` |
| `Core/Src/main.c` | 添加 `HAL_TIM_PeriodElapsedCallback()`，在其中调用 `HAL_IncTick()` |
| `Core/Src/stm32f1xx_it.c` | 添加 `TIM2_IRQHandler()`，调用 `HAL_TIM_IRQHandler(&htim2)` |
| `ota.ioc` | `NVIC.TimeBase=TIM2_IRQn`、`NVIC.TimeBaseIP=TIM2`、`VP_SYS_VS_tim2.Mode=TIM2` |

**CubeMX 生成后需要手动删除的重复内容：**

如果在 CubeMX 配置前手动写过临时的 `HAL_InitTick()` 或 `HAL_TIM_Base_MspInit()`，生成后需要删除，否则与 `stm32f1xx_hal_timebase_tim.c` 冲突（重复定义）。CubeMX 生成的代码全部位于 `USER CODE BEGIN/END` 块**之外**，而用户手写的临时代码通常在 `USER CODE` 块内，手动删除对应块即可。

**原理：**

```
HAL_Init() 调用 HAL_InitTick()
    → stm32f1xx_hal_timebase_tim.c 中的 HAL_InitTick() 配置 TIM2 产生 1ms 中断
    → TIM2 每次溢出触发 TIM2_IRQHandler()（在 stm32f1xx_it.c 中）
    → HAL_TIM_IRQHandler() 处理中断 → HAL_TIM_PeriodElapsedCallback()（在 main.c 中）
    → HAL_IncTick() 递增 HAL 时基
    
FreeRTOS SysTick（独立，互不干扰）
    → port.c 中的 SysTick_Handler() → xTaskIncrementTick() → 任务调度
```

**注意事项：**

- `configUSE_TICK_HOOK` 必须设为 `0`（不再需要 tick hook）
- 不要手写 `vApplicationTickHook()`
- TIM2 的中断优先级由 CubeMX 配置为 15（最低），与 FreeRTOS 内核中断同级，不会干扰调度

## 6. 启用 FreeRTOS API（INCLUDE_* 宏）

FreeRTOS 的部分 API 函数受 `INCLUDE_*` 宏控制，未定义或定义为 0 时，这些函数不会被编译，链接时出现 `undefined reference` 错误。

### 常见错误

```
undefined reference to `vTaskDelay'
```

### 必须按需启用的宏

```c
#define INCLUDE_vTaskDelay              1   /* vTaskDelay() — 任务延时 */
#define INCLUDE_vTaskSuspend            1   /* vTaskSuspend() / vTaskResume() */
#define INCLUDE_xTaskGetCurrentTaskHandle 1 /* xTaskGetCurrentTaskHandle() */
```

**原则**：用到哪些 API，就启用对应的 `INCLUDE_*` 宏。完整的宏列表见 FreeRTOS 官方文档。

## 7. 内存管理

推荐使用 `heap_4.c`（支持内存碎片合并），在 `FreeRTOSConfig.h` 中配置堆大小：

```c
#define configTOTAL_HEAP_SIZE  ( 10 * 1024 )  /* 10KB 堆空间 */
```

堆大小根据创建的任务数量、栈大小、队列和信号量数量估算。

## 8. main.c 中的任务创建流程

```c
#include "FreeRTOS.h"
#include "task.h"

/* ⚠️ 前向声明：任务函数必须在 xTaskCreate 调用之前声明 */
void StartTask1(void *argument);

int main(void)
{
    HAL_Init();                    /* HAL 初始化（通过 TIM2 提供 1ms 时基） */
    SystemClock_Config();          /* 时钟配置 */
    MX_GPIO_Init();                /* 外设初始化 */
    MX_USART1_UART_Init();

    /* 创建任务 */
    xTaskCreate(
        StartTask1,                /* 任务函数 */
        "Task1",                   /* 任务名（调试用） */
        128,                       /* 栈大小（字，不是字节！） */
        NULL,                      /* 参数 */
        1,                         /* 优先级 */
        NULL                       /* 任务句柄 */
    );

    /* 启动调度器（此函数不会返回） */
    vTaskStartScheduler();

    /* 永远不会运行到这里 */
    while (1) {}
}

/* 任务实现 */
void StartTask1(void *argument)
{
    while (1)
    {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        vTaskDelay(500);           /* 延时 500ms（500 ticks） */
    }
}
```

> **注意**：`configMINIMAL_STACK_SIZE` 和 `xTaskCreate` 的栈大小参数单位都是 **字（word）**，不是字节。对于 Cortex-M3，1 word = 4 bytes。

## 9. 常见问题速查

| 现象 | 原因 | 解决 |
|---|---|---|
| 编译：`'StartTask1' undeclared` | 任务函数定义在 `xTaskCreate` 调用之后 | 在文件开头添加前向声明 |
| 链接：`undefined reference to 'vTaskDelay'` | `INCLUDE_vTaskDelay` 未定义 | 在 FreeRTOSConfig.h 中添加 `#define INCLUDE_vTaskDelay 1` |
| 链接：`cannot find entry symbol Reset_Handler` | 启动文件没有参与编译（Makefile 中 ASM_SOURCES 被注释） | 拆分 Makefile 中 `# ASM sources` 和 `ASM_SOURCES = \` 为两行 |
| 运行：上电后 HardFault / Lockup | SVC/PendSV/SysTick ISR 映射未生效 | 确保 FreeRTOSConfig.h 中三个 `#define` 映射宏已启用，且 stm32f1xx_it.c 中的空 ISR 已用 `#if 0` 禁用 |
| 运行：`HAL_Delay()` 卡死 | HAL 时基源与 FreeRTOS 共用 SysTick 导致 HAL_IncTick 未被调用 | 在 CubeMX 中将 SYS → Timebase Source 设为 TIM2，让 HAL 使用独立定时器 |
| 链接：`multiple definition of SVC_Handler` | port.c 的定义与 stm32f1xx_it.c 冲突 | 用 `#if 0` 禁用 it.c 中的重复定义 |
| 链接：`multiple definition of HAL_InitTick` | CubeMX 生成的 `stm32f1xx_hal_timebase_tim.c` 与用户手写的 `HAL_InitTick()` 冲突 | 删除用户手写的版本（CubeMX 已提供） |
| 链接：`multiple definition of htim2` | main.c 和 `stm32f1xx_hal_timebase_tim.c` 中重复定义 `TIM_HandleTypeDef htim2` | 删除 main.c 中 `USER CODE PV` 内的 `htim2` 声明 |

## 10. 移植检查清单

- [ ] FreeRTOS 源码 8 个 `.c` 文件加入编译
- [ ] `FreeRTOSConfig.h` 放在 Core/Inc/ 下
- [ ] 三个 `#define` ISR 映射宏已启用
- [ ] `stm32f1xx_it.c` 中 SVC/PendSV/SysTick ISR 已用 `#if 0` 禁用
- [ ] `configCPU_CLOCK_HZ` 正确引用 `SystemCoreClock`
- [ ] `configPRIO_BITS` = 4（STM32F1）
- [ ] `INCLUDE_vTaskDelay` 等 API 宏已启用
- [ ] `configUSE_TICK_HOOK = 0`（HAL 时基由独立定时器提供）
- [ ] CubeMX 中 SYS → Timebase Source 已设为 TIM2（或其他定时器）
- [ ] CubeMX 生成后删除了 main.c 中手写的 `HAL_InitTick()`、`HAL_TIM_Base_MspInit()`、`MX_TIM2_Init()`
- [ ] CubeMX 生成后删除了 main.c 中 `USER CODE PV` 内重复的 `htim2` 声明
- [ ] 任务函数有前向声明
- [ ] Makefile / 工程配置中 Include 路径正确
- [ ] Makefile 中 `ASM_SOURCES` 未被注释掉（CubeMX bug 会导致粘连）
- [ ] 启动文件 `.s` 参与编译

## 11. CubeMX 重新生成后的手动修复

每次在 CubeMX 中修改外设配置并重新生成代码后，以下内容会被覆盖，**必须手动修复**：

### 11.1 stm32f1xx_it.c — 三个 FreeRTOS ISR

CubeMX 每次都会重新生成 SVC_Handler、PendSV_Handler、SysTick_Handler 的空壳函数。必须用 `#if 0 ... #endif` 重新包裹：

```c
#if 0  /* FreeRTOS: xxx_Handler is provided by port.c via #define */
void SVC_Handler(void)      { /* ... */ }
void PendSV_Handler(void)   { /* ... */ }
void SysTick_Handler(void)  { /* ... */ }
#endif
```

> **后果**：如果不做这一步，上电直接 HardFault → Double Fault → CPU Lockup。

### 11.2 Makefile — `ASM_SOURCES` 行粘连

CubeMX 3.15.2 生成的 Makefile 存在 bug：`# ASM sources` 注释和 `ASM_SOURCES = \` 会被合并到同一行：

```makefile
# ASM sourcesASM_SOURCES =  \    ← 错误！ASM_SOURCES 被注释掉
```

必须手动拆分为两行：

```makefile
# ASM sources
ASM_SOURCES =  \
```

> **后果**：如果不修复，启动文件不被汇编，链接时报 `cannot find entry symbol Reset_Handler`，`.text` 段只有几十字节。

### 11.3 清理手写时基代码（仅初次迁移时）

如果从 "Tick Hook 方案" 迁移到 "TIM2 时基方案"，CubeMX 首次生成 `stm32f1xx_hal_timebase_tim.c` 后需检查 main.c 的 `USER CODE` 块中是否残留以下内容，有则删除：

- `TIM_HandleTypeDef htim2;`（与 timebase 文件冲突）
- `HAL_StatusTypeDef HAL_InitTick(...)`（与 timebase 文件冲突）
- `void HAL_TIM_Base_MspInit(...)`（timebase 文件已处理时钟使能）
- `static void MX_TIM2_Init(void)`（不再需要）
- `MX_TIM2_Init()` 调用（不再需要）

### 11.4 不被 CubeMX 覆盖的文件

以下文件不在 CubeMX 生成范围内，**修改一次即可，不会被覆盖**：

- `FreeRTOSConfig.h`（手动创建的文件，CubeMX 不会管）
- `Middlewares/FreeRTOS/` 整个目录（手动添加的中间件）
- Makefile 中手动添加的 FreeRTOS 源文件和 include 路径

### 11.5 利用 `KeepUserCode` 保护

`.ioc` 文件中 `ProjectManager.KeepUserCode=true` 确保 `USER CODE BEGIN/END` 块内的代码在重新生成时被保留。因此：

- ✅ FreeRTOS Includes、`xTaskCreate`、`vTaskStartScheduler` 等写在 `USER CODE` 块内 → 安全
- ❌ `stm32f1xx_it.c` 中的 SVC/PendSV/SysTick ISR 不在 `USER CODE` 块内 → 会被覆盖
- ❌ Makefile 中的 `ASM_SOURCES` 不在保护范围内 → 会被覆盖
