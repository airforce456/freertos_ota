# STM32F103C8T6 OTA 远程升级项目完整文档

> 项目结构、驱动原理、Bootloader 流程、OTA 升级链路详解

---

## 目录

- [一、AT24C02 EEPROM 驱动详解](#一at24c02-eeprom-驱动详解)
- [二、W25Q64 SPI Flash 驱动详解](#二w25q64-spi-flash-驱动详解)
- [三、ESP8266 WiFi 驱动详解](#三esp8266-wifi-驱动详解)
- [四、OneNET MQTT 云平台接入](#四onenet-mqtt-云平台接入)
- [五、Bootloader 设计与实现](#五bootloader-设计与实现)
- [六、OTA 远程升级完整流程](#六ota-远程升级完整流程)
- [七、Bootloader + APP 双固件分区](#七bootloader--app-双固件分区)
- [八、调试问题记录](#八调试问题记录)
- [九、关键设计决策与检查清单](#九关键设计决策与检查清单)

---

# Bootloader + APP 双固件调试记录

---

## 一、AT24C02 EEPROM 驱动详解

### 硬件基础

AT24C02 是一个 **256 字节（2Kbit）的 I2C EEPROM** 芯片，7 位 I2C 地址为 `0x50`（A0/A1/A2 引脚都接地）。

```
引脚连接：
  PB6 (SCL) ── AT24C02 SCL
  PB7 (SDA) ── AT24C02 SDA

I2C 总线上共享设备：
  AT24C02 (0x50) + OLED SSD1306 (0x3C) + MPU6050 (0x68)
```

**关键特性**：
- 页大小 = **8 字节**（一次最多连续写 8 字节，跨页会被截断回卷）
- 写一个字节后需要 **等 5ms**（内部写周期），这段时间芯片不响应 I2C 请求
- 读没有页限制，可以连续读整个 256 字节

### I2C 通信协议（软 I2C）

项目使用软件模拟 I2C（`bsp_i2c.c`，PB6=SCL, PB7=SDA），不依赖硬件 I2C 外设。GPIO 模拟时序：

```
起始信号: SDA 高→低 时 SCL 为高
停止信号: SDA 低→高 时 SCL 为高
发送 1 位: SCL 低 → 设 SDA → SCL 高 → SCL 低
接收 1 位: SCL 低 → 释放 SDA → SCL 高 → 读 SDA → SCL 低
ACK:  第 9 个时钟 SDA 被从机拉低 = 成功
NACK: 第 9 个时钟 SDA 保持高 = 无应答/读取结束
```

### AT24C02 读写时序

**写一个字节**（`AT24C02_WriteByte`）：

```
[Start] → [设备地址(W)=0xA0] → [ACK] → [内存地址] → [ACK] → [数据] → [ACK] → [Stop]
                                                                              ↓
                                                                    等 6ms (内部写周期)
```

核心代码：
```c
MyI2C_Start();
MyI2C_SendByte(AT24C02_ADDR << 1);   // 0x50 << 1 = 0xA0 (写模式)
if (MyI2C_ReciveAck()) goto exit;     // 无 ACK → 芯片忙或不存在
MyI2C_SendByte(addr);                 // EEPROM 内部地址 (0~255)
if (MyI2C_ReciveAck()) goto exit;
MyI2C_SendByte(data);                 // 要写入的数据
if (MyI2C_ReciveAck()) goto exit;
ok = true;
exit:
MyI2C_Stop();
EE_DELAY_MS(6);                       // 等内部写周期（最长 5ms）
```

**读一个字节**（`AT24C02_ReadByte`）：

```
[Start] → [设备地址(W)=0xA0] → [ACK] → [内存地址] → [ACK] →
[Restart] → [设备地址(R)=0xA1] → [ACK] → [读数据] → [NACK] → [Stop]
```

**为什么要 Restart？** 因为要先发内存地址（写模式），再读数据（读模式），中间不能 Stop，否则芯片会丢失地址指针。

```c
MyI2C_Start();
MyI2C_SendByte(AT24C02_ADDR << 1);           // 0xA0 (写模式，设地址)
MyI2C_SendByte(addr);
// 注意：这里不 Stop！直接 Restart
MyI2C_Start();
MyI2C_SendByte((AT24C02_ADDR << 1) | 0x01);   // 0xA1 (读模式)
*pData = MyI2C_RecvByte(1);                   // NACK = 最后一个字节
MyI2C_Stop();
```

### 页边界处理（关键！）

AT24C02 每页 8 字节。如果从地址 `0x06` 开始写 4 字节，实际会写到 `0x06, 0x07`，然后**回卷到页开头**覆盖 `0x00, 0x01`！

`AT24C02_WriteBuf` 的解决方案：
```c
while (remain > 0) {
    uint8_t page_remain = 8 - (addr % 8);      // 当前页还剩多少
    uint8_t chunk = min(remain, page_remain);   // 本次最多写这么多

    // 写 chunk 字节到当前页 ...
    EE_DELAY_MS(6);  // 等内部写周期

    offset += chunk;
    addr   += chunk;
    remain -= chunk;
}
```

### 条件编译：一份代码同时支持 APP 和 Bootloader

`at24c02.c` 同时被 APP（FreeRTOS）和 Bootloader（裸机）编译，通过 `BOOTLOADER_BUILD` 宏切换行为：

```c
#ifdef BOOTLOADER_BUILD
  // ========== Bootloader 版本 ==========
  // 单线程，不需要锁
  #define EE_LOCK()       (true)
  #define EE_UNLOCK()
  // 没有 TIM2 时基 → 用 NOP 循环延时
  static void Boot_DelayMs(uint32_t ms) {
      for (uint32_t i = 0; i < ms; i++)
          for (volatile uint32_t j = 0; j < 2000; j++);
  }
  #define EE_DELAY_MS(ms) Boot_DelayMs(ms)

#else
  // ========== APP 版本 ==========
  // FreeRTOS 多任务 → 互斥锁保护 I2C 总线
  extern SemaphoreHandle_t xI2CMutex;
  #define EE_LOCK()   (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdPASS)
  #define EE_UNLOCK()  xSemaphoreGive(xI2CMutex)

  // 智能延时：调度器启动前用 NOP，启动后用 vTaskDelay
  static void EE_AppDelayMs(uint32_t ms) {
      if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
          vTaskDelay(pdMS_TO_TICKS(ms));  // 不占 CPU
      else
          for (...) NOP;                   // 调度器未启动，只能空转
  }
  #define EE_DELAY_MS(ms) EE_AppDelayMs(ms)
#endif
```

**为什么 Bootloader 不能用 vTaskDelay？** Bootloader 不链接 FreeRTOS 库。

**为什么 APP 在调度器启动前不能用 vTaskDelay？** `vTaskDelay` 依赖调度器运行，调度器未启动时调用会 HardFault。

### EEPROM 存储布局

```
地址     大小    字段              说明
0x00     4B     Magic             0xA5A5A5A5（判断是否初始化过）
0x04     1B     BootCmd           0=正常启动 1=OTA升级 2=Golden恢复
0x05     4B     FW_Size           固件文件大小（大端序）
0x09     4B     FW_CRC32          固件 CRC32 校验值
0x0D     4B     FW_Version        固件版本号（数字）
0x11     1B     Download_Status   0=空闲 1=下载中 2=完成 3=校验失败
0x12     1B     Retry_Count       升级失败重试次数
0x13     1B     FW_Slot           0=SlotA 1=SlotB（W25Q64 存储位置）
0x20    16B     Version_Str       版本字符串 "V1.0.3"（\0 填充）
```

### 批量写：`AT24C02_WriteBuf`

```c
while (remain > 0) {
    uint8_t page_remain = 8 - (addr % 8);      // 当前页剩余空间
    uint8_t chunk = min(remain, page_remain);   // 本次最多写这么多

    EE_LOCK();
    MyI2C_Start();
    MyI2C_SendByte(0xA0);              // 设备地址(写)
    MyI2C_SendByte(addr);              // 内存地址
    for (i=0; i<chunk; i++)
        MyI2C_SendByte(pBuf[offset+i]);// 连续发数据
    MyI2C_Stop();
    EE_UNLOCK();
    EE_DELAY_MS(6);                    // 等内部写周期

    offset += chunk; addr += chunk; remain -= chunk;
}
```

**举例**：从地址 `0x06` 写 6 字节 `ABCDEF`：
- 第一轮：`page_remain = 2`，`chunk = 2`，写入 `AB` 到 `0x06,0x07`，等 6ms
- 第二轮：`addr=0x08`，`page_remain = 8`，`chunk = 4`，写入 `CDEF` 到 `0x08~0x0B`

### 批量读：`AT24C02_ReadBuf`（连续读，无需分页）

AT24C02 支持连续读：设好起始地址后，每读一个字节芯片自动把内部地址 +1。

```c
EE_LOCK();

// Dummy write：设地址指针（写模式）
MyI2C_Start();
MyI2C_SendByte(0xA0);    // 设备地址(写)
MyI2C_SendByte(addr);    // 起始地址

// Restart + 连续读
MyI2C_Start();
MyI2C_SendByte(0xA1);    // 设备地址(读)

for (i = 0; i < len - 1; i++)
    pBuf[i] = MyI2C_RecvByte(0);   // ACK = 继续发
pBuf[i] = MyI2C_RecvByte(1);       // NACK = 最后一个，停止

MyI2C_Stop();
EE_UNLOCK();
```

ACK/NACK 的区别：
- `MyI2C_RecvByte(0)` → 读完后发 ACK（SDA 拉低），告诉芯片"继续发下一个字节"
- `MyI2C_RecvByte(1)` → 读完后发 NACK（SDA 高），告诉芯片"这是最后一个，别发了"

### 初始化：`AT24C02_Init`

```c
bool AT24C02_Init(void) {
    // 1. 读 Magic（地址 0x00 处 4 字节，大端序拼成 32 位）
    读 EE_MAGIC → magic

    // 2. Magic 匹配 → 已初始化，跳过（避免不必要的 EEPROM 磨损）
    if (magic == 0xA5A5A5A5) return true;

    // 3. 首次初始化
    写 Magic = 0xA5A5A5A5          // 标记"已初始化"
    写 BootCmd = 0                 // 正常启动模式
    写 Version_Str = "V1.0.3"      // 16 字节，\0 填充
}
```

**为什么版本号写 16 字节而不是 6 字节？** 保证后续升级时 `OTA_Finish` 写新版本号（如 `"V1.1"` 5 字节）能完全覆盖旧数据，不留残留字符导致乱码。

**Magic 值的作用**：判断 EEPROM 是否是第一次使用。全新芯片全 `0xFF` → Magic 不匹配 → 自动写默认值。AT24C02 有 100 万次写寿命，避免每次启动都重写。

---

## 二、W25Q64 SPI Flash 驱动详解

### 硬件基础

W25Q64 是 **8MB（64Mbit）SPI NOR Flash**，通过 SPI1 接口通信。

```
引脚连接：
  PA4 (CS)    ── W25Q64 /CS   (片选，GPIO 输出)
  PA5 (SCK)   ── W25Q64 CLK   (SPI1 时钟)
  PA6 (MISO)  ── W25Q64 DO    (SPI1 主入从出)
  PA7 (MOSI)  ── W25Q64 DI    (SPI1 主出从入)

SPI 模式: Mode 0 (CPOL=0, CPHA=0)，空闲时 CLK 低电平，第一个边沿采样
```

**W25Q64 内部结构**：

```
┌─────────────────────────┐
│ 8MB = 128 个 Block       │  每个 Block = 64KB (最小擦除单元之一)
│   Block 0  (64KB)       │
├─────────────────────────┤
│ 每个 Block = 16 个 Sector │  每个 Sector = 4KB (常用擦除单元)
│   Sector 0  (4KB)       │
├─────────────────────────┤
│ 每个 Sector = 16 个 Page  │  每个 Page = 256 字节 (编程单元)
│   Page 0  (256B)        │
└─────────────────────────┘
```

**关键约束**：
- **写之前必须擦除**：Flash 只能把 1 变成 0，要把 0 变回 1 必须擦除整个区域
- **擦除后全部变 0xFF**
- **Page Program 最多 256 字节**，不能跨页
- 写/擦之前必须发 `Write Enable (0x06)` 命令

### SPI 通信原理

SPI 是全双工同步串行总线，**主机控制时钟，每个时钟周期同时收发 1 位**。

```
主机发送一个字节的过程：
  SCK:  ─┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌──
        └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘
  MOSI:  [D7]  [D6]  [D5]  [D4]  [D3]  [D2]  [D1]  [D0]
  MISO:  [D7]  [D6]  [D5]  [D4]  [D3]  [D2]  [D1]  [D0]
          ↑                                             ↑
       第一个边沿                                   最后一个边沿
       (采样数据)                                  (采样数据)
```

**发送 = 接收**：发 `0x9F` 的同时，MISO 上收到 W25Q64 返回的第一个字节。

**CS（片选）的作用**：`CS=H` → 芯片不响应，MISO 高阻态；`CS=L` → 芯片被选中，开始 SPI 通信。每次命令必须 CS=L 开始，CS=H 结束。

### 两套 SPI 实现（条件编译）

`w25q64.c` 通过 `BOOTLOADER_BUILD` 宏切换 SPI 实现：

```
                 ┌─────────────────────┐
                 │   w25q64.c          │
                 │   (共用接口)         │
                 └────────┬────────────┘
                          │
          BOOTLOADER_BUILD?
              ┌───────┴───────┐
              │               │
          是 (Bootloader)   否 (APP)
              │               │
    ┌─────────▼─────────┐ ┌──▼──────────────────┐
    │ boot_spi.c        │ │ spi.c + HAL_SPI     │
    │ 手写寄存器操作      │ │ STM32CubeMX 自动生成  │
    └────────────────────┘ └──────────────────────┘
```

**APP 版本（HAL SPI）**：
```c
static uint8_t SPI_SendByte(uint8_t tx) {
    uint8_t rx;
    HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1, 100);
    return rx;
}
#define W25Q64_HW_INIT()  do {} while(0)  // 由 MX_SPI1_Init 完成
```

**Bootloader 版本（手写寄存器）**：
```c
void Boot_SPI_Init(void) {
    __HAL_RCC_SPI1_CLK_ENABLE();           // 使能 SPI1 时钟
    __HAL_RCC_GPIOA_CLK_ENABLE();          // 使能 GPIOA 时钟

    // PA5=SCK, PA7=MOSI → AF Push-Pull
    // PA6=MISO → Input floating
    // PA4=CS → Output Push-Pull, 初始高
    HAL_GPIO_Init(GPIOA, &cfg);

    // SPI1: Master, Mode0, fPCLK/2=4MHz
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE;
}

uint8_t Boot_SPI_Transfer(uint8_t tx) {
    *(volatile uint8_t *)&SPI1->DR = tx;   // 写 DR 触发发送
    while (!(SPI1->SR & SPI_SR_RXNE));     // 等接收完毕
    return *(volatile uint8_t *)&SPI1->DR; // 读收到的数据
}
```

**为什么 Bootloader 不用 HAL SPI？** HAL SPI 库编译后约 4KB，Bootloader 只有 20KB 空间。

### W25Q64 命令集

```
命令         代码   功能                      耗时
Write Enable  0x06  写使能（写/擦前必发）      -
Read SR1      0x05  读状态寄存器（检查 BUSY）   -
Read Data     0x03  读数据（标准，≤50MHz）      -
Page Program  0x02  页编程（1~256B）           ≤3ms
Sector Erase  0x20  扇区擦除（4KB）            ≤400ms
Block Erase   0xD8  块擦除（64KB）             ≤2s
Chip Erase    0xC7  全片擦除（8MB）            ≤40s
JEDEC ID      0x9F  读芯片 ID（EF 40 17）      -
```

### 关键函数详解

#### 1. CS 控制

```c
// APP: 直接操作寄存器（快）
#define CS_LOW()    (GPIOA->BRR = GPIO_PIN_4)   // Bit Reset → 拉低
#define CS_HIGH()   (GPIOA->BSRR = GPIO_PIN_4)  // Bit Set → 拉高
```

#### 2. `W25Q64_Init` — 初始化 + JEDEC ID 验证

```c
bool W25Q64_Init(void) {
    W25Q64_HW_INIT();    // 初始化 SPI 硬件
    CS_HIGH();

    uint8_t id[3];
    W25Q64_ReadJEDEC_ID(id);
    // 制造商=0xEF(Winbond), 类型=0x40(W25Q64), 容量=0x17(8MB)
    if (id[0] != 0xEF || id[1] != 0x40 || id[2] != 0x17) return false;
    return true;
}
```

#### 3. `W25Q64_ReadJEDEC_ID` — 读芯片 ID

```
时序: CS=L → 发 0x9F → 收 0xEF → 收 0x40 → 收 0x17 → CS=H
```

```c
void W25Q64_ReadJEDEC_ID(uint8_t *pID) {
    CS_LOW();
    SPI_SendByte(CMD_JEDEC_ID);    // 发 0x9F
    pID[0] = SPI_SendByte(0xFF);   // 发 dummy，收 Manufacturer
    pID[1] = SPI_SendByte(0xFF);   // 发 dummy，收 Memory Type
    pID[2] = SPI_SendByte(0xFF);   // 发 dummy，收 Capacity
    CS_HIGH();
}
```

**为什么发 `0xFF`？** SPI 是全双工的，主机必须发数据才能产生时钟，从机才能在 MISO 上返回数据。`0xFF` 是 "dummy byte"。

#### 4. 状态寄存器与忙等待

```c
#define W25Q64_SR_BUSY  0x01   // SR 第 0 位 = BUSY

uint8_t W25Q64_ReadSR(void) {
    uint8_t sr;
    CS_LOW(); SPI_SendByte(CMD_READ_SR1); sr = SPI_SendByte(0xFF); CS_HIGH();
    return sr;
}

void W25Q64_WaitBusy(void) {
    while (W25Q64_ReadSR() & W25Q64_SR_BUSY);  // 轮询 BUSY 位
}
```

**WaitBusy 的作用**：写/擦命令发出后，W25Q64 内部执行需要时间（写 ≤3ms，擦 ≤400ms），BUSY 位为 1 期间不能发新命令。

#### 5. `W25Q64_WriteEnable` — 写使能

```
时序: CS=L → 发 0x06 → CS=H
```

**安全机制**：每次上电后 Write Enable Latch (WEL) 自动清零。写/擦之前必须先发 `0x06` 置位 WEL，操作完成后 WEL 自动清零，防止意外修改 Flash。

#### 6. `W25Q64_Read` — 读数据

```
时序: CS=L → 发 0x03 → 发 3 字节地址 → 收 Byte0 → 收 Byte1 → ... → CS=H
```

读没有页限制，可以连续读任意长度，W25Q64 收到地址后自动递增。

#### 7. `W25Q64_PageProgram` — 页编程（最核心）

```
完整流程:
  1. Write Enable (0x06)
  2. CS=L → 发 0x02 → 发 3 字节地址 → 发数据[0..N] → CS=H
  3. WaitBusy()  ← 等内部编程完成（≤3ms）
```

```c
void W25Q64_PageProgram(uint32_t addr, const uint8_t *pBuf, uint16_t len) {
    W25Q64_WriteEnable();             // 1. 开写使能
    CS_LOW();
    SPI_SendByte(CMD_PAGE_PROGRAM);   // 2. 发 0x02
    SPI_SendAddr(addr);               // 3. 发 3 字节地址
    for (i=0; i<len; i++) SPI_SendByte(pBuf[i]); // 4. 连续发数据
    CS_HIGH();                        // 5. CS 上升沿 → 触发内部编程
    W25Q64_WaitBusy();                // 6. 等编程完成
}
```

**为什么 CS 拉高才触发编程？** NOR Flash 的标准行为：CS 上升沿告诉芯片"数据发完了，开始写"。

#### 8. `W25Q64_Write` — 跨页写

```c
bool W25Q64_Write(uint32_t addr, const uint8_t *pBuf, uint32_t len) {
    while (offset < len) {
        uint32_t page_remain = 256 - (addr % 256);  // 当前页剩余
        uint32_t chunk = min(len - offset, page_remain);
        W25Q64_PageProgram(addr, pBuf + offset, chunk);
        offset += chunk; addr += chunk;
    }
}
```

#### 9. 擦除操作

```c
void W25Q64_SectorErase(uint32_t addr) {  // 擦 4KB, ~400ms
    W25Q64_WriteEnable();
    CS_LOW(); SPI_SendByte(0x20); SPI_SendAddr(addr); CS_HIGH();
    W25Q64_WaitBusy();
}
```

### CRC32 校验

```c
uint32_t W25Q64_CRC32(uint32_t addr, uint32_t len) {
    static const uint32_t crc32_table[256] = { ... };  // 预计算查找表
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[256];

    while (len > 0) {
        uint32_t chunk = min(len, 256);
        W25Q64_Read(addr, buf, chunk);   // 从 Flash 读一块
        for (i=0; i<chunk; i++)
            crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        addr += chunk; len -= chunk;
    }
    return crc ^ 0xFFFFFFFF;
}
```

多项式 `0xEDB88320`（标准 Ethernet/ZIP CRC32），用查找表法代替逐位计算，速度快 8 倍。

### 完整操作流程图

```
                    ┌─────────────┐
                    │  上电/复位   │
                    └──────┬──────┘
                           │
                    ┌──────▼──────┐
                    │ W25Q64_Init │
                    │  初始化 SPI  │
                    │  读 JEDEC ID │
                    │  EF 40 17?  │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌────▼────┐  ┌────▼────┐  ┌────▼────┐
         │  读数据  │  │  写数据  │  │  擦除   │
         └────┬────┘  └────┬────┘  └────┬────┘
              │            │            │
              │      ┌─────▼─────┐      │
              │      │ 擦除过吗？  │      │
              │      └─────┬─────┘      │
              │       N    │    Y       │
              │      ┌─────▼─────┐      │
              │      │ 返回错误   │      │
              │      └───────────┘      │
              │            │            │
              │      ┌─────▼─────┐ ┌────▼─────┐
              │      │WriteEnable│ │WriteEnable│
              │      │   (0x06)  │ │  (0x06)  │
              │      └─────┬─────┘ └────┬─────┘
              │            │            │
              │      ┌─────▼─────┐ ┌────▼─────┐
              │      │PageProgram│ │Sector/Block
              │      │  (0x02)   │ │Erase(0x20)
              │      │ CS↓→数据  │ │CS↓→地址  │
              │      │ →CS↑触发  │ │→CS↑触发  │
              │      └─────┬─────┘ └────┬─────┘
              │            │            │
              │      ┌─────▼─────┐ ┌────▼─────┐
              │      │ WaitBusy  │ │ WaitBusy  │
              │      │ (≤3ms)   │ │ (≤400ms)  │
              │      └─────┬─────┘ └────┬─────┘
              │            │            │
              │      ┌─────▼─────┐      │
              │      │跨页？继续写│      │
              │      │下一段     │      │
              │      └───────────┘      │
              └─────────────────────────┘
```

### OTA 中的 W25Q64 分区

```
0x000000 ┌──────────────────────┐
         │ OTA Slot A (60KB)    │  ← 下载新固件存这里
0x00F000 ├──────────────────────┤
         │ OTA Slot B (60KB)    │  ← 备用（双区升级）
0x01E000 ├──────────────────────┤
         │ Golden Image (60KB)  │  ← 出厂固件备份
0x02D000 ├──────────────────────┤
         │ 剩余空间 (约 7.8MB)   │
0x7FFFFF └──────────────────────┘
```

---

## 五、Bootloader 设计与实现

### 设计目标

```
1. 最小化体积 → 只链接必需的 HAL 库，不依赖 FreeRTOS
2. 只做三件事：读 EEPROM → 决定启动路径 → 跳 APP
3. 异常保护：擦除失败重试、CRC32 校验、Golden 恢复
4. 跳转前清理：关中断、关外设时钟，给 APP 干净环境
```

### Bootloader 编译链

```
源文件:
  boot_main.c       ← 主入口 + 启动状态机
  boot_flash.c/h    ← 内部 Flash 擦写读 + 向量表校验
  boot_spi.c        ← 手写 SPI1 寄存器驱动 (替代 HAL SPI)
  at24c02.c         ← EEPROM 驱动 (BOOTLOADER_BUILD 模式)
  w25q64.c          ← SPI Flash 驱动 (BOOTLOADER_BUILD 模式)
  bsp_i2c.c         ← 软 I2C (GPIO 模拟)
  system_stm32f1xx.c ← 时钟初始化 (SystemInit)
  startup_stm32f103xb.s ← 启动文件 (向量表)

链接脚本:
  STM32F103C8Tx_BOOT.ld  ← FLASH: 0x08000000, 20KB

编译选项:
  -DBOOTLOADER_BUILD  ← 条件编译宏
  -Os                 ← 体积优化
```

### 文件详解

#### 1. `boot_flash.h` — 地址定义和接口

```c
#define APP_START_ADDR   0x08005000   // APP 区起始 (Bootloader 占 20KB)
#define APP_END_ADDR     0x0800FFFF   // 64KB 末尾
#define APP_MAX_SIZE     (44 * 1024)  // 44KB

// 接口
bool Boot_EraseAppArea(void);
bool Boot_FlashWriteWord(uint32_t addr, uint32_t data);
uint32_t Boot_FlashReadWord(uint32_t addr);
bool Boot_FlashWriteBuf(uint32_t addr, const uint8_t *pBuf, uint32_t len);
bool Boot_CheckAppValid(void);
```

#### 2. `boot_flash.c` — 内部 Flash 操作

**STM32F103 Flash 特性**：
- 每页 1KB（1024 字节）
- 写之前必须擦除（擦除 = 全部变 `0xFFFFFFFF`）
- 写只能把 `1` 变 `0`，不能把 `0` 变 `1`
- 写/擦期间 CPU 暂停（Flash 被独占），必须关中断

**`Boot_FlashReadWord`** — Flash 可像 RAM 一样直接读：
```c
uint32_t Boot_FlashReadWord(uint32_t addr) {
    return *(volatile uint32_t *)addr;
}
```

**`Boot_FlashWriteWord`** — 写一个 32 位字 + 回读验证：
```c
bool Boot_FlashWriteWord(uint32_t addr, uint32_t data) {
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);  // CPU 暂停等待
    if (Boot_FlashReadWord(addr) != data) return false;      // 回读验证
    return true;
}
```

**`Boot_EraseAppArea`** — 擦除 APP 区（44 页，约 1 秒）：
```c
bool Boot_EraseAppArea(void) {
    HAL_FLASH_Unlock();        // 1. 解锁 Flash（默认上锁防误操作）
    __disable_irq();           // 2. 关中断（擦除期间 CPU 暂停）

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_START_ADDR;
    erase.NbPages     = 44;    // 44 页 = 44KB

    HAL_FLASHEx_Erase(&erase, &pageError);  // 3. 执行擦除

    __enable_irq();            // 4. 开中断
    HAL_FLASH_Lock();          // 5. 上锁

    // 6. 验证第一页确实擦除了
    if (Boot_FlashReadWord(APP_START_ADDR) != 0xFFFFFFFF) return false;
}
```

**`Boot_FlashWriteBuf`** — 批量写（自动按 32 位字对齐）：
```c
for (i = 0; i < len; i += 4) {
    // 拼成 32 位字（尾部不足 4 字节补 0xFF）
    word  = pBuf[i];
    word |= (i+1 < len) ? (pBuf[i+1] << 8)  : 0xFF000000;
    word |= (i+2 < len) ? (pBuf[i+2] << 16) : 0xFF000000;
    word |= (i+3 < len) ? (pBuf[i+3] << 24) : 0xFF000000;
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr+i, word);
    if (Boot_FlashReadWord(addr+i) != word) goto fail;  // 每字回读验证
}
```

**`Boot_CheckAppValid`** — 向量表合法性快速检查：
```c
bool Boot_CheckAppValid(void) {
    uint32_t sp = *(uint32_t *)APP_START_ADDR;       // [0] = 初始 SP
    uint32_t pc = *(uint32_t *)(APP_START_ADDR + 4); // [1] = Reset_Handler

    if (sp < 0x20000000 || sp > 0x20005000) return false;  // SP 不在 RAM
    if (pc < APP_START_ADDR || pc > APP_END_ADDR) return false;  // PC 不在 APP 区
    if ((pc & 0x01) == 0) return false;  // bit0 不为 1 (Cortex-M3 必须 Thumb)
    return true;
}
```

#### 3. `boot_spi.c` — 手写 SPI 寄存器驱动

**为什么不用 HAL SPI？** HAL SPI 库编译后约 4KB，Bootloader 只有 20KB 空间。

```c
void Boot_SPI_Init(void) {
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    // PA5=SCK, PA7=MOSI → AF Push-Pull
    // PA6=MISO → Input, PA4=CS → Output Push-Pull (初始高)

    SPI1->CR1 = SPI_CR1_MSTR      // Master 模式
              | SPI_CR1_SSM       // 软件 NSS
              | SPI_CR1_SSI       // 内部 NSS 高
              | SPI_CR1_SPE;      // 使能 SPI
    // BaudRate = fPCLK/2 = 4MHz
}

uint8_t Boot_SPI_Transfer(uint8_t tx) {
    *(volatile uint8_t *)&SPI1->DR = tx;   // 写 DR → 触发发送
    while (!(SPI1->SR & SPI_SR_RXNE));     // 等接收完毕
    return *(volatile uint8_t *)&SPI1->DR; // 读收到的数据
}
```

#### 4. `boot_main.c` — 主入口 + 启动状态机

```c
int main(void)
{
    // ===== 第 1 步：初始化硬件 =====
    MyI2C_Init();        // 软 I2C (PB6/PB7)
    AT24C02_Init();      // EEPROM (读 Magic，首次写默认值)

    // ===== 第 2 步：读取启动命令 =====
    uint8_t bootCmd = 0;
    AT24C02_ReadByte(EE_BOOT_CMD, &bootCmd);

    // ===== 第 3 步：状态机 =====
    switch (bootCmd) {
    case 0:  // 正常启动
        break;

    case 1:  // OTA 升级
        ① 读 EEPROM: FW_Slot, FW_Size, FW_CRC32
        ② fw_size 有效? 否则清 BootCmd → 跳 APP
        ③ W25Q64_CRC32() == FW_CRC32? 否则清 BootCmd → 跳 APP
        ④ Boot_EraseAppArea() (失败重试 3 次 → Golden 恢复)
        ⑤ 从 W25Q64 逐 256B 搬到内部 Flash
           (失败重试 3 次 → Golden 恢复)
        ⑥ 清 BootCmd/RetryCount → 跳新 APP
        break;

    case 2:  // Golden 恢复
        ① Boot_EraseAppArea()
        ② 从 W25Q64 Golden 区搬出厂固件到 APP 区
        break;
    }

    // ===== 第 4 步：跳转 APP =====
jump_to_app:
    if (Boot_CheckAppValid())
        Boot_JumpToApp(APP_START_ADDR);
    while (1);  // APP 不可用 → 死循环
}
```

#### 5. `Boot_JumpToApp` — 跳转函数（最关键）

```c
static void Boot_JumpToApp(uint32_t appAddr)
{
    uint32_t appSp = *(volatile uint32_t *)appAddr;        // 向量表[0] = SP
    uint32_t appPc = *(volatile uint32_t *)(appAddr + 4);  // 向量表[1] = PC

    __disable_irq();                // 关全局中断（APP 会在 main 里重开）

    // 关所有外设时钟 → 给 APP 干净初始状态
    __HAL_RCC_GPIOA_CLK_DISABLE();
    __HAL_RCC_GPIOB_CLK_DISABLE();
    __HAL_RCC_GPIOC_CLK_DISABLE();
    __HAL_RCC_SPI1_CLK_DISABLE();
    __HAL_RCC_USART1_CLK_DISABLE();
    __HAL_RCC_USART2_CLK_DISABLE();
    __HAL_RCC_I2C1_CLK_DISABLE();
    __HAL_RCC_TIM2_CLK_DISABLE();
    __HAL_RCC_DMA1_CLK_DISABLE();

    __set_MSP(appSp);               // 设 APP 的栈指针
    SCB->VTOR = appAddr;            // 向量表偏移

    ((void (*)(void))appPc)();      // 跳转：加载 appPc 到 PC 寄存器
}
```

**跳转前为什么关中断和外设时钟？** APP 不知道 Bootloader 用了哪些外设，必须把一切恢复到上电初始状态，让 APP 的 `HAL_Init()` + `MX_xxx_Init()` 从头配置。

**APP 端必须做的事**：
```c
// main.c 最开头
SCB->VTOR = 0x08005000;   // 向量表偏移（中断才能正确路由到 APP 的 ISR）
__enable_irq();             // 重新开中断（Bootloader 关的）
```

### 完整启动流程图

```
                         ┌──────────┐
                         │  上电    │
                         └────┬─────┘
                              │
                    ┌─────────▼─────────┐
                    │ startup .s         │
                    │ 设 MSP = _estack   │
                    │ 调 SystemInit()    │  ← 配置 HSI 8MHz
                    │ 调 __libc_init     │
                    │ 调 main()          │
                    └─────────┬─────────┘
                              │
                    ┌─────────▼─────────┐
                    │ MyI2C_Init()      │  软 I2C GPIO 初始化
                    │ AT24C02_Init()    │  EEPROM 初始化
                    └─────────┬─────────┘
                              │
                    ┌─────────▼─────────┐
                    │ 读 EEPROM[0x04]   │
                    │ → bootCmd         │
                    └─────────┬─────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
         bootCmd=0       bootCmd=1       bootCmd=2
              │               │               │
              │    ┌──────────▼──────────┐    │
              │    │ 读 FW_Slot/Size/CRC │    │
              │    └──────────┬──────────┘    │
              │               │               │
              │    ┌──────────▼──────────┐    │
              │    │ 安全检查 + CRC32    │    │
              │    │ 不通过 → 清标志跳APP│    │
              │    └──────────┬──────────┘    │
              │               │               │
              │    ┌──────────▼──────────┐    │
              │    │ Boot_EraseAppArea   │    │
              │    │ (失败 3 次→Golden)  │    │
              │    └──────────┬──────────┘    │
              │               │               │
              │    ┌──────────▼──────────┐    │
              │    │ W25Q64 → 内部Flash  │    │
              │    │ 逐 256B 搬移        │    │
              │    │ (失败 3 次→Golden)  │    │
              │    └──────────┬──────────┘    │
              │               │               │
              │    ┌──────────▼──────────┐    │
              │    │ 清 BootCmd/Retry    │    │
              │    │ 跳新 APP            │    │
              │    └─────────────────────┘    │
              │                               │
              │    ┌──────────────────────────▼──┐
              │    │ Golden 恢复:                  │
              │    │ 擦 APP → 搬 Golden 区到 APP   │
              │    └──────────────────────────────┘
              │               │
              └───────────────┼───────────────┘
                              │
                    ┌─────────▼─────────┐
                    │ Boot_CheckAppValid│
                    │ SP 合法? PC 合法? │
                    │ bit0=1?           │
                    └────┬────────┬─────┘
                      Y  │      N │
                ┌────────▼──┐ ┌─▼──────┐
                │Boot_JumpTo│ │死循环   │
                │App(0x5000)│ │        │
                └────────┬──┘ └────────┘
                         │
                ┌────────▼──────────┐
                │ __disable_irq()   │
                │ 关所有外设时钟     │
                │ __set_MSP(appSp)  │
                │ SCB->VTOR=appAddr │
                │ 跳 Reset_Handler  │
                └───────────────────┘
                         │
                         ▼
                    APP 开始运行
```

### Bootloader 使用的外设（精简列表）

| 外设 | 用途 | 初始化方式 |
|------|------|-----------|
| GPIO PB6/PB7 | 软 I2C (AT24C02) | `MyI2C_Init()` 直接操作寄存器 |
| GPIO PA4~PA7 | SPI1 + CS (W25Q64) | `Boot_SPI_Init()` 手写寄存器 |
| SPI1 | W25Q64 通信 | `SPI1->CR1 = ...` 手写 |
| Flash 控制器 | 内部 Flash 擦写 | HAL_FLASH_xxx (仅链接 hal_flash.c) |
| RCC | 时钟配置 | `system_stm32f1xx.c` (CubeMX 生成) |

**Bootloader 不使用的外设**（跳转前全部关闭）：
USART1, USART2, DMA1, TIM2, I2C1 硬件

---

## 七、Bootloader + APP 双固件分区

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

---

## 六、OTA 远程升级完整流程

### 整体架构

```
OneNET 平台 ←→ ESP8266 ←→ STM32 ←→ W25Q64 (暂存固件)
                             ↕
                          AT24C02 (EEPROM: 版本号、升级标志、CRC32)
                             ↕
                        内部 Flash (APP 区: 运行的固件)
```

### 完整时序

```
每次开机:
  │
  ├─ Bootloader @ 0x08000000
  │   ├─ 读 EEPROM[BootCmd]
  │   │   ├─ 0: 正常 → 检查 APP 向量表 → 合法 → 跳 APP
  │   │   ├─ 1: OTA 升级 → 从 W25Q64 搬固件到 APP 区 → 清标志 → 跳新 APP
  │   │   └─ 2: Golden 恢复
  │   └─ Boot_JumpToApp(0x08005000)
  │       ├─ __disable_irq()       ← 关中断
  │       ├─ 关所有外设时钟          ← 避免 APP 初始化冲突
  │       ├─ __set_MSP(appSp)      ← 设 APP 的栈指针
  │       ├─ SCB->VTOR = appAddr   ← 向量表偏移
  │       └─ ((void(*)(void))appPc)() ← 跳 Reset_Handler
  │
  ├─ APP @ 0x08005000
  │   ├─ SCB->VTOR = 0x08005000
  │   ├─ __enable_irq()            ← 重新开中断（Bootloader 关的）
  │   ├─ HAL_Init() → TIM2 时基启动
  │   ├─ 外设初始化 (GPIO/USART/SPI/DMA/I2C)
  │   ├─ OLED + MPU6050 + AT24C02 + W25Q64 初始化
  │   ├─ FreeRTOS 对象创建 (互斥锁/队列/任务)
  │   │
  │   ├─ ESP8266_Init()            ← 连 WiFi
  │   │
  │   ├─ OTA_CheckAndDownload()    ← ★ OTA 升级检查
  │   │   │
  │   │   ├─ ① POST /fuse-ota/{pid}/{dev}/version
  │   │   │   上报: {"s_version":"V1.0", "f_version":"V9.9"}
  │   │   │   服务器: iot-api.heclouds.com:80 (HTTP)
  │   │   │   → 回复 "msg":"succ" ✓
  │   │   │
  │   │   ├─ ② GET /fuse-ota/{pid}/{dev}/check?type=2&version=1.0
  │   │   │   服务器: iot-api.heclouds.com:80
  │   │   │   ├─ "msg":"not exist" → 无任务 → 返回 false → 继续 MQTT 启动
  │   │   │   └─ "msg":"succ" + {target, tid, size, md5} → 有任务！
  │   │   │       解析: ota_new_ver, ota_tid, ota_file_size, ota_server_md5
  │   │   │       安全检查: tid 为空 或 size=0 → 放弃，不改版本号
  │   │   │
  │   │   ├─ ③ 擦除 W25Q64 OTA Slot A (60KB, 15 个 4KB 扇区)
  │   │   │
  │   │   └─ ④ GET /fuse-ota/{pid}/{dev}/{tid}/download
  │   │        Range:bytes=0-255 (每片 256 字节)
  │   │        循环直到 size 全部收完:
  │   │          ├─ ESP8266 发 HTTP GET + Range 头
  │   │          ├─ 循环拼接多个 +IPD 片段直到 Content-Length 收满
  │   │          ├─ 校验 payload_len >= chunk_len
  │   │          ├─ 单片失败 → 重试 2 次
  │   │          └─ 成功 → 写 W25Q64
  │   │        → 全部收完
  │   │
  │   │   └─ ⑤ OTA_Finish()
  │   │        ├─ W25Q64_CRC32() → crc (校验 W25Q64 数据完整性)
  │   │        ├─ 写 EEPROM: FW_CRC32 = crc       ← Bootloader 搬移前校验
  │   │        ├─ 写 EEPROM: FW_Size = ota_file_size
  │   │        ├─ 写 EEPROM: Version_Str = ota_new_ver
  │   │        ├─ 写 EEPROM: BootCmd = 1           ← 触发升级
  │   │        └─ NVIC_SystemReset() → 重启
  │   │            ↓
  │   │        回到 Bootloader → BootCmd=1 → 搬 W25Q64 → 清标志 → 跳新 APP
  │   │            ↓
  │   │        新 APP 运行 → 版本号变成新版本 → 又回到 ① 查询
  │   │
  │   ├─ OneNET_RegisterDevice()  ← 设备注册
  │   ├─ OneNet_DevLink()         ← MQTT 连接 mqtts.heclouds.com:1883
  │   ├─ OneNET_Subscribe()       ← 订阅 Topic
  │   ├─ OneNet_SendData()        ← 首次数据上报
  │   │
  │   └─ vTaskStartScheduler()    ← FreeRTOS 启动
  │       ├─ MPU_Read (500ms 周期读传感器)
  │       ├─ DataProc (数据转换)
  │       ├─ OLED_Disp (OLED 显示)
  │       ├─ OneNET_Upl (5s 周期上报 OneNET)
  │       └─ UartTX/UartRX (串口 DMA 收发)
```

### Bootloader 搬移流程

```
BootCmd = 1:
  ├─ 读 EEPROM: FW_Size, FW_CRC32
  ├─ 安全检查: fw_size==0 或 > APP_MAX_SIZE → 清 BootCmd → 跳旧 APP
  ├─ CRC32 校验: W25Q64_CRC32(OTA_SLOT_A, fw_size) == FW_CRC32?
  │   └─ 不匹配 → W25Q64 数据损坏 → 清 BootCmd → 跳旧 APP
  ├─ 擦除 APP 区 (44 页, ~1 秒)
  │   └─ 擦除失败 → 重试 (最多 3 次) → 仍失败 → Golden 恢复
  ├─ 从 W25Q64 逐页搬到内部 Flash (256B/次)
  │   ├─ W25Q64_Read(fw_addr + offset, buf, chunk)
  │   ├─ Boot_FlashWriteBuf(APP_START + offset, buf, chunk)
  │   │   └─ 每字回读验证 (Boot_FlashReadWord)
  │   └─ 写失败 → 重试 (最多 3 次) → 仍失败 → Golden 恢复
  └─ 搬移成功 → 清 BootCmd/RetryCount/DownloadStatus → Boot_CheckAppValid() → 跳新 APP
```

### 三层数据完整性保护

```
┌─────────────────────────────────────────────────────────────────┐
│  第一层：下载阶段 (APP → W25Q64)                                  │
│  ├─ 每片 256B: payload_len >= chunk_len 才写入                   │
│  ├─ 单片失败: 重试 2 次                                          │
│  └─ 全部收完: W25Q64_CRC32() → 存 EEPROM[FW_CRC32]              │
├─────────────────────────────────────────────────────────────────┤
│  第二层：搬移前校验 (Bootloader)                                   │
│  ├─ EEPROM[FW_CRC32] vs W25Q64_CRC32()                          │
│  └─ 不匹配 → 放弃升级，跳旧 APP                                   │
├─────────────────────────────────────────────────────────────────┤
│  第三层：搬移中校验 (Bootloader)                                   │
│  ├─ 每字写入后回读验证 (Boot_FlashReadWord)                       │
│  └─ 失败 → 重试 → 仍失败 → Golden 恢复                            │
└─────────────────────────────────────────────────────────────────┘
```

### 各阶段断电/异常应对

| 阶段 | 发生了什么 | 断电后果 | 下次上电行为 |
|------|-----------|---------|------------|
| 下载固件到 W25Q64 时 | W25Q64 数据不完整 | W25Q64 部分为新数据 | BootCmd=0 → 正常启动旧 APP |
| 下载完成，写 EEPROM 时 | BootCmd 未写入或版本号不完整 | EEPROM 状态不确定 | BootCmd 可能为 0 → 正常启动；或为 1 → 走搬移流程（有 CRC32 保护） |
| 擦除 APP 区时 | APP 区被擦除（全 0xFF） | 旧 APP 已不可用 | BootCmd=1 → 重新搬移 W25Q64 数据 → 跳新 APP ✅ |
| 搬移新固件到 APP 区时 | APP 区部分为新数据 | 新旧混杂，不可运行 | BootCmd=1 → 重新搬移（W25Q64 数据完好）→ 跳新 APP ✅ |
| 搬移完成，清标志前 | APP 区完整，BootCmd 仍为 1 | 新 APP 已就位 | 重新进搬移流程 → 发现 APP 区已有数据 → 写覆盖（无影响）→ 跳新 APP |
| W25Q64 数据损坏 | 固件数据错误 | 无法恢复 | CRC32 校验失败 → 放弃升级 → 跳旧 APP ✅ |
| FW_Size 异常 (0 或超大) | 参数错误 | 无法搬移 | 大小检查拦截 → 清 BootCmd → 跳旧 APP ✅ |
| 擦除/搬移连续失败 3 次 | 硬件故障 | APP 区不可用 | 进入 Golden 恢复模式 → 从 W25Q64 Golden 区恢复出厂固件 |

### 升级成功判定

**串口日志特征**：
```
① 下载阶段：
  [OTA] Upgrade found: V1.0 -> V1.0.3, size=39020
  [OTA] Erasing W25Q64 slot A...
  [OTA] 256/39020 (0%)
  ... (进度递增)
  [OTA] 39020/39020 (100%)
  [OTA] Download complete, verifying...
  [OTA] CRC32 = 0xXXXXXXXX
  [OTA] Rebooting...

② Bootloader 搬移：
  (Bootloader 无串口输出)

③ 新 APP 启动：
  === FIRMWARE V1.0.3 (OTA TEST) ===
  === 如果看到这行，说明 V1.0.3 升级成功！ ===
```

**OneNET 控制台**：升级任务状态变为"升级成功"。

### 升级失败判定

```
① 无任务: [OTA] No upgrade task → 正常进入 MQTT 业务
② tid/size 无效: [OTA] Invalid tid or size, abort → 版本号不变
③ 下载超时/失败: [OTA] Download failed, version NOT changed → 版本号不变
④ CRC32 不匹配: (Bootloader 内部) → 清 BootCmd → 跳旧 APP
⑤ 搬移失败: (Bootloader 内部) → 重试 → Golden 恢复
```

---

## OTA 远程升级改进记录

这次 OneNET OTA 下载链路主要做了三类改进：

1. **修复 ESP8266 下载阶段丢包与超时**
  - 将 ESP8266 接收缓冲从 512 字节提升到 1KB，减少 HTTP 头部与分片数据同时到达时的覆盖风险。
  - 下载响应不再依赖单次 `strlen` 判断，而是按 `+IPD` 声明长度和 HTTP `Content-Length` 做完整性校验。
  - 支持多个 `+IPD` 片段拼接，避免 TCP 拆包导致的“前几片正常、后面突然 No data in response”。

2. **修复 OTA 完成后卡在 EEPROM 写入**
  - OTA 流程在调度器启动前执行，而 AT24C02 的延时原先使用 `vTaskDelay`，会在未启动调度器时卡死。
  - 已改为根据调度器状态自动选择延时方式：调度器运行时使用 `vTaskDelay`，调度器未启动时使用裸机忙等延时。

3. **在稳定性和速度之间重新取平衡**
  - 采用“单片 256B 分片、收满再写 W25Q64”的方式，保持你原来的高速下载思路。
  - 仅在单片失败时做有限重试，避免过度保守导致下载速度下降太多。
  - 连接和接收逻辑做了轻量兜底，但没有把整条链路改成过重的保守模式。

### 当前结论

现在的 OTA 下载流程已经可以：

- 正常识别任务、版本号、tid 和文件大小；
- 按 256 字节分片下载并逐片写入 W25Q64；
- 在下载完成后正确写入 EEPROM 并复位；
- 在速度上尽量接近原始方案，同时补上必要的稳定性保护。

### 后续如果还要继续优化

- 可以再做“动态分片大小”策略，例如网络好时提高到 384/512 字节，异常时回退到 256 字节；
- 也可以在单片失败时先关闭 TCP 再重连，进一步提升弱网下的成功率。
