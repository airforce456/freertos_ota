# STM32 + ESP8266 + OneNET 云平台 完整调试记录

## 项目概况

在 STM32F103C8T6 + FreeRTOS 项目中，从零集成 ESP8266 WiFi 模块并接入 OneNET MQTT 云平台，实现 MPU6050 传感器数据周期上报。

---

## 第一阶段：ESP8266 初始化超时

### 现象

```
[ESP] Init FAILED (timeout)
```

ESP8266 的 AT 命令永远无响应，初始化状态机卡在第一步。

### 调试过程

**1. 轮询收发测试**

怀疑 ESP 根本没收到命令。绕开 DMA/ISR，直接用轮询方式在 USART2 上发送 `AT\r\n`，然后检查 RXNE 标志读取回复。

结果：成功收到 `AT\r\n\r\nOK\r\n`，证明 ESP8266 硬件、接线（PA2→ESP RX, PA3→ESP TX）、波特率 115200 完全正常。

**2. DMA + IDLE 中断检查**

配置 DMA 接收，读取 USART2 CR1 和 DMA CNDTR 寄存器：

- CR1 = 0x201C（IDLEIE=1）→ IDLE 中断已使能
- CNDTR = 256 → 256（收到 10 字节）→ DMA 正常收数据
- DMA 缓冲区内容：`AT\r\n\r\nOK\r\n` → 数据正确

结论：DMA 工作正常，但 ISR 没有被触发（CNDTR 未被 ISR 重启为 256）。

**3. GPIO 翻转确认 ISR 入口**

在 `USART2_IRQHandler` 入口无条件翻转 PC13。发现 PC13 在超时循环结束后才闪烁，说明 ISR 在 busy-wait 期间被延迟。

**4. 定位根因：BASEPRI 中断屏蔽**

FreeRTOS 在 Cortex-M3 上每次进入临界区时，将 `BASEPRI` 设为 0x50（对应 NVIC 优先级 5），屏蔽所有优先级 ≥5 的中断。

```
NVIC 优先级         BASEPRI=0x50 屏蔽？
─────────────────────────────────────────
USART2  = 5 (0x50)  ✅ 被屏蔽
EXTI0   = 5 (0x50)  ✅ 临界区内屏蔽自己
```

EXIT0（MPU6050 INT）每 4ms 触发一次（250Hz），其 ISR 调用 `xSemaphoreGiveFromISR` → `portSET_INTERRUPT_MASK_FROM_ISR()` → BASEPRI 拉高。如果 USART2 IDLE 恰好在此时触发，ISR 被挂起延迟。

### 修复

| 文件 | 改动 | 原因 |
|------|------|------|
| `usart.c` | USART2 优先级 5→**4** | BASEPRI=0x50 不屏蔽优先级 4（0x40） |
| `bsp_uart.c` | IDLE 回调后 `huart->RxState = READY` 再重启 DMA | HAL_UARTEx_ReceiveToIdle_DMA 要求状态为 READY |
| `mpu6050.c` | SMPRT_DIV 3→19（250Hz→50Hz） | 降低 EXIT0 中断频率 |

---

## 第二阶段：ESP8266 驱动重构（NET 包架构）

### 原因

原先使用 DMA + IDLE 中断 + 复杂解析器的驱动架构在 ISR 时机问题上不稳定。NET 包使用更简单可靠的中断逐字节接收 + 轮询等待模式。

### 新架构

```
USART2 RXNE 中断 → HAL_UART_RxCpltCallback → esp8266_buf[esp8266_cnt++]
                                                      ↓
ESP8266_SendCmd() 发 AT 命令后轮询 esp8266_cnt，停增即认为回复完毕
```

### 移植文件

| 文件 | 说明 |
|------|------|
| `esp8266.h/c` | 重写，使用 `HAL_UART_Receive_IT` 逐字节 + `HAL_UART_RxCpltCallback` |
| `stm32f1xx_it.c` | USART2 ISR 恢复 `HAL_UART_IRQHandler` |

### 最终中断布局

```
USART1 = 0  （不受 BASEPRI 影响，printf 畅通）
USART2 = 4  （不受 BASEPRI 影响，ISR 可靠触发）
EXTI0  = 5  （可调用 FreeRTOS FromISR API）
```

---

## 第三阶段：OneNET 云平台接入

### 移植文件

| 文件 | 来源 | 说明 |
|------|------|------|
| `onenet.c/h` | 改编自 NET 包 | 去 SPL、适配 HAL、`printf` 替代 `UsartPrintf` |
| `base64.c/h` | NET 包直接复制 | Base64 编解码 |
| `hmac_sha1.c/h` | NET 包直接复制 | HMAC-SHA1 签名 |
| `mqttkit.c/h` | NET 包直接复制 | MQTT 协议封包/解包 |
| `cjson.c/h` | NET 包直接复制 | JSON 解析 |
| `Common.h` | NET 包直接复制 | MQTT 公共定义 |

### 接入流程

```
ESP8266_Init()          → AT 检测 → 关回显 → STA 模式 → 连 WiFi
OneNET_RegisterDevice() → HTTP POST 到 OneNET 注册设备（一次性）
OneNet_DevLink()        → MQTT CONNECT 到 mqtts.heclouds.com:1883
OneNET_Subscribe()      → 订阅 $sys/{pid}/{dev}/thing/property/set
OneNet_SendData()       → MQTT PUBLISH 上报传感器数据
```

### 遇到的坑

1. **6002 端口被手机热点拦截** → 改为 `mqtts.heclouds.com:1883`
2. **`%f` 格式化在 onenet.c 上下文失效** → 改用整型毫g值（×1000）
3. **OneNET 只上传一次** → 没有周期任务调用，创建 `Task_OneNET_Upload`

---

## 第四阶段：内存溢出（RAM EXHAUSTION）

### 现象

```
region `RAM' overflowed by 536 bytes
```

链接时 `._user_heap_stack` 段超出 STM32F103C8 的 20KB RAM。

### 分析

STM32F103C8T6 总 RAM：20KB。BSS 段占用约 11KB（含 FreeRTOS 堆 10KB、系统堆 512B、系统栈 1024B），加上各种队列、信号量、任务栈，总计超出。

### 修复

| 文件 | 改动 | 节省 |
|------|------|------|
| `STM32F103C8Tx_FLASH.ld` | `_Min_Heap_Size` 0x200→**0x000** | 512B |
| `STM32F103C8Tx_FLASH.ld` | `_Min_Stack_Size` 0x400→**0x200** | 512B |
| `mqttkit.c` | `malloc`/`free` → `pvPortMalloc`/`vPortFree` | MQTT 走 FreeRTOS 堆 |
| `onenet.c` | 同上 + `#include "FreeRTOS.h"` | 声明 pvPortMalloc/vPortFree |

系统堆省下 512 字节，系统栈省下 512 字节，总计节省 1024 字节。MQTT 的动态分配走 FreeRTOS 堆（10KB），不再依赖独立系统堆。

---

## 最终系统架构

### 任务优先级

```
OLED_Disp    (Prio=3)    1280B 栈    OLED 实时显示加速度
MPU_Read     (Prio=2)    1024B 栈    MPU6050 传感器读取（INT 驱动 @50Hz）
DataProcess  (Prio=2)    1024B 栈    原始值→物理量转换
UartTX       (Prio=2)    1024B 栈    DMA 串口发送
UartRX       (Prio=2)    1024B 栈    DMA 串口接收
OneNET_Upl   (Prio=1)    1024B 栈    每 5 秒上传一次 OneNET
```

### 数据流

```
MPU6050 INT(50Hz) → EXIT0 ISR → MPU_Sem → MPU_Read → xSensorQueue[8]
                                                          ↓
                                                     DataProcess
                                                    ↓              ↓
                                               xCookedQueue[4]   onenet_* 全局变量
                                                    ↓              ↓
                                               OLED_Disp         OneNET_Upl(5s)
```

### 中断布局

```
USART1    = 0    printf 串口，最高优先级
USART2    = 4    ESP8266 通信，不受 BASEPRI 影响
EXTI0     = 5    MPU6050 INT，可调 FromISR API
TIM2      = ?    HAL 时基
```

---

## 关键技术点总结

| 知识点 | 说明 |
|--------|------|
| BASEPRI 中断屏蔽 | `configMAX_SYSCALL_INTERRUPT_PRIORITY=5` 时，BASEPRI=0x50 屏蔽优先级 ≥5 的中断 |
| 中断优先级规划 | 需 FreeRTOS API 的中断优先级 ≥5，不需 API 的可以 <5 避免被 BASEPRI 屏蔽 |
| 二值信号量 vs 互斥量 | ISR 只能用信号量通知任务；互斥量带优先级继承，用于保护共享资源 |
| STM32F103C8 RAM | 仅 20KB，需严格控制 BSS、堆、栈大小 |
| malloc 重定向 | `#define malloc pvPortMalloc` 让第三方库走 FreeRTOS 堆管理 |
| OneNET MQTT 端口 | 6002 可能被网络拦截，推荐 1883（标准 MQTT）或使用域名 |
