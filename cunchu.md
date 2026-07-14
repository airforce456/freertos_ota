# AT24C02 & W25Q64 驱动设计文档

## 硬件拓扑

```
STM32F103C8T6
├── I2C1 (软I2C, PB6=SCL, PB7=SDA)
│   ├── OLED     (0x3C)  — 软I2C直接操作, 无需锁
│   ├── MPU6050  (0x68)  — 软I2C直接操作, 无需锁
│   └── AT24C02  (0x50)  — 通过 xI2CMutex 保护, 共享总线
│
└── SPI1 (硬件SPI, PA5=SCK, PA6=MISO, PA7=MOSI, PA4=CS)
    └── W25Q64  (8MB NOR Flash) — 独占SPI1, 无需锁
```

**关键设计决策**：
- OLED 和 MPU6050 在调度器启动前（main函数内）完成初始化，此时单线程运行，不需要锁
- AT24C02 的 Init 被推迟到 xI2CMutex 创建之后，之后所有 AT24C02 操作都通过互斥锁保护
- W25Q64 独占 SPI1，没有其他设备共享，不需要锁

---

## AT24C02 驱动设计

### 芯片特性

| 属性 | 值 |
|------|-----|
| 容量 | 256 bytes (2Kbit) |
| 页大小 | 8 bytes |
| I2C 地址 | 0x50 (A0=A1=A2=GND) |
| 写周期 | ≤ 5ms (内部EEPROM编程时间) |
| 通信速率 | 100kHz (标准模式, 我们的软I2C实际约50kHz) |

### I2C 读写时序

#### 单字节写
```
S → DevAddr+W(0xA0) → ACK → MemAddr → ACK → Data → ACK → P
                                                ↑
                                          写周期开始(≤5ms)
```
- Start 后发送 `0xA0`（设备地址+写位）
- 芯片回 ACK 后发送存储器地址（0~255）
- 芯片回 ACK 后发送数据字节
- Stop 后芯片进入内部写周期，此期间不响应任何 I2C 操作

#### 单字节读（Dummy Write + Repeated Start）
```
S → DevAddr+W(0xA0) → ACK → MemAddr → ACK → Sr → DevAddr+R(0xA1) → ACK → Data(NACK) → P
```
- 先发"假写"来设置内部地址指针
- 不发送 Stop，而是发 Repeated Start
- 再发 `0xA1`（设备地址+读位）
- 读最后一个字节时主设备发 NACK（表示不再读）

#### 多字节页写（处理页边界）
```
AT24C02 页=8字节，地址 0~7 是一页，8~15 是下一页...

例：从地址 6 开始写 5 字节
  第1页(6,7):    2字节 → Stop → 等5ms
  第2页(8,9,10):  3字节 → Stop → 等5ms
```
每写完一页必须 Stop + 等 5ms，因为：
- 芯片内部编程只在一个页内进行
- 跨页写会"回绕"到同一页的起始地址（地址 8 写 3 字节 → 写到 8,9,0 而不是 8,9,10）

#### 多字节顺序读
```
S → DevAddr+W → ACK → StartAddr → ACK → Sr → DevAddr+R → ACK → Data0 → ACK → Data1 → ... → DataN(NACK) → P
```
- 设置起始地址后，每读一个字节芯片内部地址自动 +1
- 前 N-1 个字节回 ACK（继续读），最后一个回 NACK（停止读）

### 互斥锁策略

```c
bool AT24C02_ReadByte(uint8_t addr, uint8_t *pData)
{
    // 1. 拿锁（最多等100ms）
    xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100));

    // 2. 完整的 I2C 事务（Start→...→Stop）
    MyI2C_Start();
    MyI2C_SendByte(...);
    // ...
    MyI2C_Stop();

    // 3. 还锁
    xSemaphoreGive(xI2CMutex);
}
```

**为什么要在锁内完成整个事务？** 因为 OLED、MPU6050、AT24C02 共享同一条 I2C 总线。如果 AT24C02 发了 Start 后释放锁，OLED 可能抢到锁并发送数据 → I2C 状态机错乱。

### EEPROM OTA 元数据布局

```
AT24C02 256字节分配:
+--------+------+------------------------------------+
| 地址   | 大小 | 用途                               |
+--------+------+------------------------------------+
| 0x00   | 4B   | Magic: 0xA5A5A5A5 (标记已初始化)   |
| 0x04   | 1B   | BootCmd: 0=正常 1=升级 2=Golden恢复 |
| 0x05   | 4B   | FW_Size: 固件大小                    |
| 0x09   | 4B   | FW_CRC32: 固件CRC校验值              |
| 0x0D   | 4B   | FW_Version: 固件版本号               |
| 0x11   | 1B   | Download_Status: 0=空闲 1=下载中     |
|        |      |   2=完成 3=校验失败                  |
| 0x12   | 1B   | Retry_Count: 重试次数(>3回退Gold)    |
| 0x13   | 1B   | FW_Slot: 0=SlotA 1=SlotB            |
| 0x20   | 16B  | Version_Str: "v1.0.0"               |
+--------+------+------------------------------------+
```

---

## W25Q64 驱动设计

### 芯片特性

| 属性 | 值 |
|------|-----|
| 容量 | 8MB (64Mbit) |
| 扇区 | 4KB (最小擦除单位) |
| 块 | 64KB (16个扇区) |
| 页 | 256 bytes (编程单位) |
| JEDEC ID | 0xEF 0x40 0x17 |
| SPI 模式 | Mode 0 (CPOL=0, CPHA=0) |
| 最高时钟 | 80MHz (我们用 4MHz, Prescaler=2) |

### SPI 通信机制详解

#### SPI 硬件工作原理

STM32 的 SPI 外设是一个**移位寄存器环**：

```
 Master (STM32)                    Slave (W25Q64)
┌──────────────┐                ┌──────────────┐
│   TX Shift   │──MOSI─────────►│   RX Shift   │
│   Register   │                │   Register   │
│              │◄──MISO────────│              │
│   RX Buffer  │                │   TX Buffer  │
└──────────────┘                └──────────────┘
      SCK ──────────────────────────►  (由Master产生时钟)
      CS  ──────────────────────────►  (低电平选中芯片)
```

**核心机制**：每次 SPI 传输同时收发。Master 发送一个字节时，同时也收到一个字节（可能是有效数据，也可能是垃圾 0xFF）。这就是为什么 `HAL_SPI_TransmitReceive` 是 SPI 最基础的原子操作。

#### HAL 库 SPI 操作

```c
// 发一个字节，同时收一个字节
uint8_t SPI_SendByte(uint8_t tx)
{
    uint8_t rx;
    // 参数: hspi, &tx, &rx, 个数, 超时ms
    HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1, 100);
    return rx;
}
```

**为什么不用 `HAL_SPI_Transmit` + `HAL_SPI_Receive` 分开调？**
- SPI 是全双工的，分开调意味着中间需要重新拉 CS → 两次独立事务 → 对 Flash 芯片来说就是两次不同的命令
- `TransmitReceive` 保证同一个 CS 周期内完成收发

#### CS（片选）软件控制

```c
#define W25Q64_CS_PORT   GPIOA
#define W25Q64_CS_PIN    GPIO_PIN_4

#define CS_LOW()   (W25Q64_CS_PORT->BRR = W25Q64_CS_PIN)   // 选中
#define CS_HIGH()  (W25Q64_CS_PORT->BSRR = W25Q64_CS_PIN)  // 释放
```

用寄存器直接操作（`BRR`/`BSRR`）而不是 `HAL_GPIO_WritePin`，因为：
- `BRR`/`BSRR` 是单周期原子操作，不会被中断打断
- HAL 库的 GPIO 写需要读-修改-写，在中断中可能被抢占导致电平错误
- SPI Flash 对 CS 时序要求严格，必须快速切换

#### W25Q64 命令集

| 命令 | 代码 | 说明 |
|------|------|------|
| Write Enable | 0x06 | 写/擦除前必须发，设置 WEL 位 |
| Read Status1 | 0x05 | 读状态寄存器，bit0=BUSY |
| Read Data | 0x03 | 读数据，24位地址，无限长度 |
| Page Program | 0x02 | 页编程，1~256字节，不能跨页 |
| Sector Erase | 0x20 | 擦除4KB扇区，~400ms |
| Block Erase | 0xD8 | 擦除64KB块，~2s |
| Chip Erase | 0xC7 | 全片擦除，~40s |
| JEDEC ID | 0x9F | 读厂商ID+设备ID，3字节 |

#### 典型 SPI 交互流程（以 Page Program 为例）

```
时序:
CS:  ‾‾‾‾\___________________________/‾‾‾‾‾
MOSI:      \02\ADDR2\ADDR1\ADDR0\D0\D1\...\Dn\
MISO:      \FF\ FF  \ FF  \ FF  \FF\FF\...\FF\
SCK:       ‾|‾|‾‾|‾‾|‾‾|‾‾|‾‾|‾‾|‾‾|‾‾|‾‾|‾

1. CS 拉低 (选中芯片)
2. 发送 0x02 (Page Program 命令)
3. 发送 3 字节地址 (MSB first: A23~A16, A15~A8, A7~A0)
4. 发送数据字节 (1~256 个)
5. CS 拉高 (芯片开始内部编程)
6. 轮询 Status Register 的 BUSY 位直到清0
```

**注意**：MISO 在发送命令阶段返回的全是 `0xFF`（无效数据），只有在 Read Data 命令时 MISO 才返回有效数据。

#### 写使能 (Write Enable) 的必要性

W25Q64 的写保护机制：
```
上电默认: WEL=0 (写禁止)
编程/擦除前必须: 发 0x06 → WEL=1
编程/擦除完成后: WEL 自动清0
```

**这是硬件级别的防误写保护**。如果忘记发 `Write Enable`，芯片会忽略后续的 Program/Erase 命令，不会报错但数据不变。

#### 页编程的跨页处理

```c
bool W25Q64_Write(uint32_t addr, const uint8_t *pBuf, uint32_t len)
{
    while (offset < len) {
        // 计算当前页还能写多少
        page_remain = 256 - (addr % 256);
        chunk = min(len - offset, page_remain);

        // 写一页
        W25Q64_PageProgram(addr, pBuf + offset, chunk);

        offset += chunk;
        addr   += chunk;
    }
}
```

**为什么不能跨页？** W25Q64 内部有一个 256 字节的页缓冲区。Page Program 命令先把数据写入这个缓冲区，CS 拉高后芯片把缓冲区内容编程到 Flash 阵列。如果跨页，超出的字节会**回绕覆盖同一页的前面部分**，而不是写到下一页。

#### CRC32 校验

```
多项式: 0xEDB88320 (标准 Ethernet/zip CRC32)
初始值: 0xFFFFFFFF
最终异或: 0xFFFFFFFF

查表法: 预计算 256 个 32 位表项，每字节一次查表+异或
速度: 约 200KB/s @ 4MHz SPI
```

用于 OTA 固件完整性校验：
1. 固件下载到 W25Q64 的 OTA Slot
2. 对下载的数据计算 CRC32
3. 与服务器下发的 CRC32 比对
4. 匹配才允许写入内部 Flash

---

## 与 RTOS 的集成

### AT24C02: 互斥锁保护

```
任务A (OneNET)                任务B (OLED)
    │                            │
    │ AT24C02_ReadBuf()          │
    │ xSemTake(xI2CMutex) ◄──────┼──── 拿锁成功
    │                            │
    │ ... I2C 事务中 ...          │ OLED_ShowNum() 也要拿锁
    │                            │ xSemTake(xI2CMutex) → 阻塞等待
    │                            │
    │ MyI2C_Stop()               │
    │ xSemGive(xI2CMutex) ──────►│ 锁释放, OLED 拿到锁
    │                            │ ... OLED I2C 事务中 ...
```

### W25Q64: 无需锁

SPI1 只连了 W25Q64 一个设备，没有共享冲突。但在 FreeRTOS 中仍需注意：
- `W25Q64_SectorErase` 会阻塞 ~400ms → 不要在高优先级任务中调用
- 建议在 OneNET/OTA 任务（优先级1）中执行 Flash 操作

---

## 踩坑记录

1. **AT24C02 写后必须等 5ms**：不等直接读会收到 NACK，因为芯片还在内部编程
2. **AT24C02 Init 必须在 xI2CMutex 创建之后**：否则 `xSemaphoreTake(NULL)` → 死锁
3. **W25Q64 的 CS 必须用寄存器操作**：`BRR`/`BSRR` 是原子的，HAL GPIO 写不是
4. **SPI 收发是一体的**：用 `HAL_SPI_TransmitReceive` 而不是分开调 `Transmit` + `Receive`
5. **Page Program 不能跨页**：必须按 256 字节边界拆分，否则数据回绕
6. **Write Enable 是一次性的**：每执行一个 Program/Erase 命令后 WEL 自动清0，下次要重新发
