# FreeRTOS 系统学习方案（STM32F103 + CubeMX + GCC）

> 前置条件：已完成 FreeRTOS 手动移植到 STM32F103C8Tx，工程可编译、可运行。
>
> **硬件清单**：3×LED (PC13/PC14/PC15) | MPU6050 (I2C) | 0.96" OLED (I2C) | W25Q64 (SPI) | AT24C02 (I2C EEPROM) | USART1 (UART CLI)

## 学习路线总览

```
第1阶段：任务管理 + LED/UART（2周）
  裸任务 → 延时对比 → 抢占/时间片 → 挂起恢复 → CLI 雏形
         ↓
第2阶段：I2C 外设 + 任务间通信（3周）
  队列 → 信号量 → 互斥量(I2C 总线) → 事件组 → 任务通知
  硬件：OLED 显示 + MPU6050 读数 + EEPROM 读写
         ↓
第3阶段：SPI 存储 + 进阶机制（2周）
  软件定时器 → 流缓冲 → 内存管理 → 中断管理
  硬件：W25Q64 数据记录 + DMA 传输
         ↓
第4阶段：综合项目（2周）
  CLI 控制台 + 传感器数据记录器 + OLED 仪表盘
```

---

## 第1阶段：任务管理（核心基础）

**本阶段硬件**：3×LED (PC13/PC14/PC15) + USART1 (CLI 输入)

### 1.1 任务创建与删除

**学习目标**：理解任务控制块（TCB）、任务栈、任务优先级。

**实践内容**：

```c
// 动态创建任务
xTaskCreate(TaskFunc, "Task", 128, NULL, 1, &hTask);

// 静态创建任务
xTaskCreateStatic(TaskFunc, "Task", STACK_SIZE, NULL, 1, stackBuf, &tcbBuf);

// 删除任务
vTaskDelete(NULL);   // 自杀
vTaskDelete(hTask);  // 他杀

// 查询信息
TaskHandle_t me = xTaskGetCurrentTaskHandle();
UBaseType_t prio = uxTaskPriorityGet(hTask);
```

**实验清单**：

- [ ] 3 个 LED 闪烁任务，不同优先级（PC13 Prio=3, PC14 Prio=2, PC15 Prio=1），观察抢占
- [ ] 在任务中 `vTaskDelete(NULL)` 删除自己
- [ ] 用 `vTaskList()` 打印所有任务状态到串口
- [ ] 对比 `xTaskCreate` 和 `xTaskCreateStatic` 的内存占用差异

**关键问题**：

- [ ] 任务栈大小怎么估算？不够会怎样？（栈溢出检测 `configCHECK_FOR_STACK_OVERFLOW`）
- [ ] `configMAX_PRIORITIES` 改大会有什么代价？
- [ ] Idle Task 是干什么的？优先级是多少？

### 1.2 延时函数对比

**学习目标**：理解相对延时和绝对延时的区别。

```c
// 相对延时：从现在起延时 N 个 tick（执行时间累加，会漂移）
vTaskDelay(pdMS_TO_TICKS(500));

// 绝对延时：从上次预定唤醒时间起延时（执行时间被吞掉，不漂移）
TickType_t xLastWakeTime = xTaskGetTickCount();
vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
```

**实验清单**：

- [ ] PC13 用 `vTaskDelay()` 做 500ms 周期闪烁，PC14 用 `vTaskDelayUntil()` 对比
- [ ] 在延时任务中加忙等模拟负载，观察 `vTaskDelay` 漂移而 `vTaskDelayUntil` 不漂
- [ ] 调 `vTaskDelay(1)` 观察实际最小延时（受 `configTICK_RATE_HZ` 限制）

### 1.3 任务抢占与时间片

**学习目标**：理解抢占式调度和时间片轮转的区别。

```c
#define configUSE_PREEMPTION    1   // 高优先级就绪立即抢占
#define configUSE_TIME_SLICING  1   // 同优先级轮流（每 tick 切换）
```

**实验清单**：

- [ ] Prio=3 和 Prio=2 任务各占一个 LED，观察高优先级如何抢占低优先级
- [ ] 3 个同优先级任务各自打印 ID，观察时间片轮转（A→B→C→A→B→C...）
- [ ] 设 `configUSE_TIME_SLICING = 0`，同优先级先到独占，对比行为差异
- [ ] 设 `configUSE_PREEMPTION = 0`，不再抢占，只能靠阻塞来切换

### 1.4 任务挂起与恢复 + UART CLI 雏形

**学习目标**：掌握 `vTaskSuspend`/`vTaskResume`，初步建立串口命令控制能力。

```c
vTaskSuspend(hTask);   // 无限期移出调度
vTaskResume(hTask);    // 恢复
```

**实验清单**：

- [ ] Task A 挂起 Task B，5 秒后恢复（观察 B 的 LED 暂停/恢复）
- [ ] 通过 UART 发送命令控制 LED 闪烁任务的挂起/恢复（1.4 已实现的基础上升级）
  - `suspend LED_H` → 挂起 PC13 任务
  - `resume LED_H` → 恢复
  - `delete LED_H` → 删除任务
- [ ] 对比 `vTaskSuspend()` 和 `vTaskDelay()` 的区别

**阶段测验**：实现一个"任务看门狗"——Task A 监控 Task B 是否还在输出，若卡死则 `vTaskDelete` + 重建。
/*手写注释请勿删除
我不会直接使用 eTaskGetState() 判断，因为任务即使处于 Running 状态也可能陷入死循环。通常会设计一个心跳机制，由 Task B 周期性更新心跳计数，Task A 定时检查该计数是否变化。如果长时间没有变化，则认为 Task B 已失去响应，可以先调用 vTaskDelete(TaskBHandle) 删除任务，再使用 xTaskCreate() 重建任务，实现软件级任务看门狗。
*/
---

## 第2阶段：I2C 外设 + 任务间通信

**本阶段硬件**：MPU6050 (I2C) + 0.96" OLED (I2C) + AT24C02 (I2C)

> **CubeMX 准备**：启用 I2C1 (PB6=SCL, PB7=SDA)，频率 400kHz。三个外设共用一条 I2C 总线。

```
          ┌──────────────────────────────────────┐
          │           I2C1 总线                   │
          │    PB6(SCL)  PB7(SDA)                │
          └────┬────────────┬──────────┬─────────┘
               │            │          │
           MPU6050      OLED(0.96")  AT24C02
           (0x68)        (0x3C)      (0x50)
```

### 2.1 队列（Queue）— 传感器数据管道

**学习目标**：掌握生产者-消费者模型，队列作为任务间的数据管道。

**硬件映射**：MPU6050 读数任务（生产者）→ 队列 → OLED 显示任务（消费者）

```
MPU6050_ReadTask             OLED_DisplayTask
  (Prio=2)                     (Prio=1)
      │                            │
      ├─ 读加速度计                │
      ├─ xQueueSend(q, &data) ────→├─ xQueueReceive(q, &data)
      └─ vTaskDelay(100ms)         ├─ OLED 显示 ax/ay/az
                                   └─ 循环
```

```c
// 数据结构体（拷贝式传递，不传指针）
typedef struct {
    int16_t ax, ay, az;   // 加速度
    int16_t gx, gy, gz;   // 角速度
    TickType_t timestamp;
} SensorData_t;

QueueHandle_t xSensorQueue = xQueueCreate(8, sizeof(SensorData_t));
```

**实验清单**：

- [ ] MPU6050 任务每 100ms 读一次，通过队列发给 OLED 任务显示
- [ ] 队列满时，生产者用不同超时值观察阻塞行为（0 / 100ms / portMAX_DELAY）
- [ ] 用 `xQueueOverwrite()` 只保留最新数据，适合"只关心当前值"的场景
- [ ] 用 `uxQueueMessagesWaiting()` / `uxQueueSpacesAvailable()` 监控队列水位

### 2.1 附录：MPU6050 驱动编写过程与踩坑记录

#### 硬件接线

| MPU6050 | STM32F103 |
|---------|-----------|
| VCC | 3.3V |
| GND | GND |
| SCL | PB6 (I2C1_SCL) |
| SDA | PB7 (I2C1_SDA) |
| AD0 | GND（I2C 地址 = 0x68） |

> 如果 AD0 接 VCC 或悬空，I2C 地址可能变成 0x69，导致扫描到设备但寄存器读失败。

#### CubeMX 配置

- I2C1：PB6=SCL, PB7=SDA, Fast Mode 400kHz（生成代码为 100kHz）
- GPIO 自动配置为开漏输出（`GPIO_MODE_AF_OD`）
- MPU6050 模块**必须有板载上拉电阻**（大多数模块自带 10kΩ）

#### 踩坑 1：`HAL_I2C_Mem_Read` 在 STM32F1 上失败（rc=1 HAL_ERROR）

**现象**：

```
I2C Scan: 0x68 (1 device(s))       ← HAL_I2C_IsDeviceReady 成功
[MPU] Raw WHO_AM_I: rc=1 val=0x08  ← HAL_I2C_Mem_Read 失败
```

**根因**：STM32F1 的 I2C 外设存在重复起始（Repeated Start）硬件 bug。`HAL_I2C_Mem_Read` 内部流程是：

```
START → 设备地址+W → 寄存器地址 → RESTART → 设备地址+R → 读数据 → STOP
                                        ↑
                                   F1 这里容易挂
```

`HAL_I2C_IsDeviceReady` 只发 `START → 地址+W → ACK → STOP`，不涉及 REPEATED START，所以能通。

**解决方案**：将 `HAL_I2C_Mem_Read/Write` 替换为两步法，用 `HAL_I2C_Master_Transmit` + `HAL_I2C_Master_Receive` 拆分操作，避免重复起始：

```c
// ❌ 原写法（F1 上不稳定）
HAL_I2C_Mem_Read(&hi2c1, devAddr, regAddr, I2C_MEMADD_SIZE_8BIT, buf, len, timeout);

// ✅ F1 安全写法：先发寄存器号，再读数据
uint8_t reg = 0x75;
HAL_I2C_Master_Transmit(&hi2c1, devAddr, &reg, 1, timeout);   // 写寄存器号
HAL_I2C_Master_Receive(&hi2c1, devAddr, buf, len, timeout);    // 读数据
```

```c
// ❌ 原写法（F1 上不稳定）
HAL_I2C_Mem_Write(&hi2c1, devAddr, regAddr, I2C_MEMADD_SIZE_8BIT, &val, 1, timeout);

// ✅ F1 安全写法：寄存器号+数据合并为一次传输
uint8_t buf[2] = { regAddr, val };
HAL_I2C_Master_Transmit(&hi2c1, devAddr, buf, 2, timeout);
```

**封装后的驱动 API**（`mpu6050.c`）：

```c
// 写单个寄存器
static HAL_StatusTypeDef I2C_WriteReg(uint8_t devAddr, uint8_t regAddr, uint8_t val)
{
    uint8_t buf[2] = { regAddr, val };
    return HAL_I2C_Master_Transmit(&hi2c1, devAddr, buf, 2, I2C_TIMEOUT);
}

// 读单个寄存器
static HAL_StatusTypeDef I2C_ReadReg(uint8_t devAddr, uint8_t regAddr, uint8_t *val)
{
    HAL_I2C_Master_Transmit(&hi2c1, devAddr, &regAddr, 1, I2C_TIMEOUT);
    return HAL_I2C_Master_Receive(&hi2c1, devAddr, val, 1, I2C_TIMEOUT);
}

// 连续读多个寄存器（如加速度计 6 字节）
static HAL_StatusTypeDef I2C_ReadRegs(uint8_t devAddr, uint8_t regAddr,
                                       uint8_t *buf, uint8_t len)
{
    HAL_I2C_Master_Transmit(&hi2c1, devAddr, &regAddr, 1, I2C_TIMEOUT);
    return HAL_I2C_Master_Receive(&hi2c1, devAddr, buf, len, I2C_TIMEOUT);
}
```

#### 踩坑 2：I2C 扫描后状态机变脏

**现象**：在 `MPU6050_Init` 之前调用 `HAL_I2C_IsDeviceReady` 循环扫描后，后续 `HAL_I2C_Mem_Read` 可能失败。

**解决**：扫描后做一次 `HAL_I2C_DeInit` + `MX_I2C1_Init` 复位 I2C 外设状态机。

#### 踩坑 3：I2C 超时值的选择

**现象**：使用 `HAL_MAX_DELAY` 作为超时可能导致异常行为。

**解决**：在 FreeRTOS 环境下，I2C 操作（特别是调度器启动前）使用固定毫秒超时（如 `100`），避免 HAL 内部依赖 SysTick 在中断关闭时出现死等。

#### 调试技巧：I2C 设备扫描

用于确认设备物理连接正确、地址正确：

```c
printf("I2C Scan: ");
for (uint8_t addr = 1; addr < 127; addr++) {
    if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1), 2, 10) == HAL_OK) {
        printf("0x%02X ", addr);
    }
}
```

#### MPU6050 寄存器速查

| 寄存器 | 地址 | 说明 |
|--------|------|------|
| WHO_AM_I | 0x75 | 应返回 0x68 |
| PWR_MGMT_1 | 0x6B | bit6=SLEEP，写 0x00 唤醒 |
| SMPRT_DIV | 0x19 | 采样率 = 1kHz/(1+N) |
| ACCEL_CONFIG | 0x1C | 量程：0=±2g, 1=±4g, 2=±8g, 3=±16g |
| GYRO_CONFIG | 0x1B | 量程：0=±250°/s, 1=±500°/s, 2=±1000°/s, 3=±2000°/s |
| ACCEL_XOUT_H | 0x3B | 加速度 X 高字节（大端，连续 6 字节） |
| GYRO_XOUT_H | 0x43 | 角速度 X 高字节（连续 6 字节） |

#### 文件结构

```
Core/Inc/mpu6050.h    — 寄存器宏、MPU6050_Data_t 结构体、函数原型
Core/Src/mpu6050.c    — I2C 读写封装 + MPU6050_Init/ReadAll/ReadWhoAmI
Core/Src/main.c       — 初始化调用 + 测试任务
Core/Src/i2c.c        — CubeMX 生成的 I2C1 初始化
```

### 2.2 信号量（Semaphore）— ISR 通知任务

**学习目标**：区分二值信号量和计数信号量，掌握 ISR→任务通知模式。

**硬件映射**：MPU6050 的 INT 引脚产生数据就绪中断 → 信号量通知任务读取

```
MPU6050_INT 引脚 (PA0)
      │
      ▼
EXTI0_IRQHandler
      │
      ├─ xSemaphoreGiveFromISR(xDataReadySem, &xWoken)
      └─ portYIELD_FROM_ISR(xWoken)
           │
           ▼
MPU6050_ReadTask
      │
      ├─ xSemaphoreTake(xDataReadySem, portMAX_DELAY)
      ├─ 读取传感器寄存器
      ├─ 发送到队列
      └─ 循环
```

**实验清单**：

- [ ] 用二值信号量实现 MPU6050 数据就绪中断 → 任务读取（替代轮询）
- [ ] 用计数信号量管理固定大小的"传输缓冲区池"（如最多 3 个任务同时请求 I2C）
- [ ] 对比轮询 vs 信号量通知的 CPU 占用差异
- [ ] 验证 `xSemaphoreGiveFromISR` 的 `pxHigherPriorityTaskWoken` 机制

### 2.3 互斥量（Mutex）— I2C 总线共享

**学习目标**：理解 I2C 总线作为共享资源需要互斥保护，优先级反转与继承。

**硬件映射**：OLED、MPU6050、AT24C02 共用 I2C1，**同时只有一个外设能用总线**。

```
三个任务都想访问 I2C1：
  OLED_Task     ──→ xSemaphoreTake(xI2CMutex) ──→ 写 OLED  ──→ Give
  MPU6050_Task  ──→ xSemaphoreTake(xI2CMutex) ──→ 读 MPU──→ Give
  EEPROM_Task   ──→ xSemaphoreTake(xI2CMutex) ──→ 读写 EEP ──→ Give

没有互斥量 → OLED 显示花屏、MPU 读数错乱、EEPROM 写入失败
有互斥量   → 排队使用 I2C，各自完整完成
```

```c
SemaphoreHandle_t xI2CMutex = xSemaphoreCreateMutex();

// 每个访问 I2C 的任务：
if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    HAL_I2C_Mem_Read(&hi2c1, MPU6050_ADDR, ...);  // 临界区
    xSemaphoreGive(xI2CMutex);
}
```

**实验清单**：

- [ ] 构造优先级反转场景：低优先任务持有 I2C 锁 → 中优先任务抢占 → 高优先任务阻塞在锁上
- [ ] 用互斥量（有优先级继承）和普通信号量（无继承）对比上述场景
- [ ] 互斥量递归版本：在同一个任务中嵌套获取（如 OLED 写操作内部再调 I2C）

**理解要点**：

- [ ] 信号量 vs 互斥量：前者用于**同步**（通知事件），后者用于**互斥**（保护资源）
- [ ] 互斥量**不能**在 ISR 中使用（`xSemaphoreGiveFromISR` 对互斥量无效）
- [ ] 互斥量是"谁 Take 谁 Give"，不能跨任务

### 2.4 事件组（Event Group）— 多传感器同步

**学习目标**：多事件组合等待（AND/OR 逻辑）。

**硬件映射**：MPU6050 加速度 + 角速度数据都就绪后，OLED 才刷新显示。

```c
#define BIT_ACCEL_READY  (1 << 0)
#define BIT_GYRO_READY   (1 << 1)

EventGroupHandle_t xSensorEventGroup = xEventGroupCreate();

// 等待两组数据都就绪后才显示
EventBits_t bits = xEventGroupWaitBits(
    xSensorEventGroup,
    BIT_ACCEL_READY | BIT_GYRO_READY,  // 等待这些位
    pdTRUE,   // 等待后清除
    pdTRUE,   // AND 逻辑（全部就绪）
    pdMS_TO_TICKS(100)
);
```

**实验清单**：

- [ ] 加速度/角速度各自就绪后设事件位，OLED 任务等待全部就绪后刷新
- [ ] 用事件组实现：命令 1 收到 + 命令 2 收到 → 执行特殊操作（AND）
- [ ] 用事件组实现：任一 LED 任务完成一个周期 → 计数器 +1（OR）
- [ ] 用 `xEventGroupSync()` 实现 3 个任务的栅栏同步点

### 2.5 任务通知（Task Notification）— 轻量级

**学习目标**：在 1 对 1 场景用任务通知替代信号量/队列，更省 RAM、更快。

```c
// ISR → 任务（替代二值信号量）
xTaskNotifyGiveFromISR(hTask, &xWoken);   // ISR 通知
ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // 任务等待

// 任务 → 任务（替代队列，传 32 位值）
xTaskNotify(hTask, value, eSetValueWithOverwrite);
xTaskNotifyWait(0, 0xFFFFFFFF, &value, portMAX_DELAY);
```

**实验清单**：

- [ ] 用任务通知替代二值信号量，重写 MPU6050 中断通知（对比代码量）
- [ ] 用任务通知直接传一个 `uint32_t` 传感器值，替代单元素队列
- [ ] 对比：发送 1000 次通知 vs 发送 1000 次队列的时间差异

**阶段测验**：用 MPU6050 中断 + 队列 + 互斥量（I2C）+ OLED 显示，实现一个完整的"运动数据实时显示"系统。

---

## 第3阶段：SPI 存储 + 进阶机制

**本阶段硬件**：W25Q64 (SPI) + AT24C02 (EEPROM) + 已有 I2C 外设

> **CubeMX 准备**：启用 SPI1 (PA5=SCK, PA6=MISO, PA7=MOSI, PA4=NSS)，Mode=Full-Duplex Master，NSS 软件管理。

### 3.1 软件定时器（Software Timer）

**学习目标**：理解定时器回调机制和 Daemon Task，区分"周期任务"和"定时回调"。

**硬件映射**：软件定时器触发周期传感器采样，结果写入 W25Q64。

```c
// 周期定时器：每 1 秒触发一次
TimerHandle_t xSampleTimer = xTimerCreate(
    "Sample", pdMS_TO_TICKS(1000), pdTRUE,  // pdTRUE=自动重载
    (void*)0, vSampleTimerCallback
);
xTimerStart(xSampleTimer, 0);

// ⚠️ 回调在 Daemon Task 中执行，绝对不能阻塞
void vSampleTimerCallback(TimerHandle_t xTimer) {
    // 给采样任务发通知，让它在自己上下文里做 I2C 读取
    xTaskNotifyGive(hSampleTask);
}
```

**实验清单**：

- [ ] 创建 3 个软件定时器，以不同周期采样 MPU6050，OLED 显示采样频率
- [ ] 单次定时器：串口发 "log 5s" → 5 秒内持续记录传感器数据到 W25Q64
- [ ] 对比软件定时器和 `vTaskDelayUntil()` 的适用场景
- [ ] 查看 Daemon Task 栈水位：`uxTaskGetStackHighWaterMark(xTimerTask)`

**配置要点**：

```c
#define configUSE_TIMERS              1
#define configTIMER_TASK_PRIORITY     2
#define configTIMER_QUEUE_LENGTH      10
#define configTIMER_TASK_STACK_DEPTH  256
```

### 3.2 内存管理 — W25Q64 缓冲区池

**学习目标**：掌握 heap_4 特性，设计固定大小的缓冲区池供 Flash 页写入。

**硬件映射**：W25Q64 按 256 字节 Page 写入，需要对齐的缓冲区。

```c
// W25Q64 Page Write 需要 256 字节对齐缓冲区
#define FLASH_PAGE_SIZE  256
#define BUFFER_POOL_SIZE 4

typedef struct {
    uint8_t data[FLASH_PAGE_SIZE];
    SemaphoreHandle_t sem;  // 缓冲区空闲信号量
} PageBuffer_t;

static PageBuffer_t pageBuffers[BUFFER_POOL_SIZE];
static SemaphoreHandle_t xBufferPoolSem;  // 计数信号量，初值 = BUFFER_POOL_SIZE
```

**实验清单**：

- [ ] 用 `xPortGetFreeHeapSize()` 追踪各阶段堆变化
- [ ] 创建/销毁 50 个小任务，观察 heap_4 是否能回收碎片
- [ ] 对比：堆上动态分配 W25Q64 页缓冲区 vs 静态 BSS 分配
- [ ] 打印 Idle Task 栈水位线

### 3.3 中断管理 — SPI DMA 传输

**学习目标**：掌握 SPI DMA 中断 + FromISR API，实现高速 Flash 读写。

**硬件映射**：W25Q64 的页读取/写入通过 SPI DMA 实现，CPU 不参与逐字节搬运。

```
W25Q64_WriteTask
      │
      ├─ SPI 发 Write Enable 命令（轮询）
      ├─ SPI 发 Page Program 命令（轮询）
      ├─ SPI DMA 发 256 字节页数据
      │     │
      │     └─ DMA 完成中断 → xSemaphoreGiveFromISR → 任务继续
      ├─ 等 W25Q64 BUSY 位清除
      └─ 完成
```

**实验清单**：

- [ ] W25Q64 整页读取（SPI DMA，256 字节一次性读完）
- [ ] W25Q64 页写入（SPI DMA，256 字节一次性写完）
- [ ] 验证 W25Q64 写入前后数据一致性（读回比对）
- [ ] 测量 SPI DMA vs SPI 轮询 传输 4KB 的时间差异

### 3.4 流缓冲区（Stream Buffer）— 传感器流式记录

**学习目标**：掌握 Stream Buffer 处理连续数据流。

**硬件映射**：MPU6050 连续采样 → Stream Buffer → W25Q64 批量写入。

```c
// 创建流缓冲区（1KB）
StreamBufferHandle_t xSensorStream = xStreamBufferCreate(1024, 16);

// 传感器任务：写入流
xStreamBufferSend(xSensorStream, &sensorData, sizeof(SensorData_t), 0);

// 存储任务：攒够 256 字节就写 Flash
size_t len = xStreamBufferReceive(xSensorStream, buf, 256, pdMS_TO_TICKS(1000));
if (len >= 256) {
    W25Q64_WritePage(buf, len);
}
```

**实验清单**：

- [ ] MPU6050 100Hz 采样 → Stream Buffer → 每攒 256 字节写 W25Q64 一页
- [ ] 对比 Stream Buffer 和 Queue 的区别（Stream Buffer 任意长度，Queue 定长）
- [ ] 用 `xStreamBufferBytesAvailable()` 监控缓冲区水位

---

## 第4阶段：综合项目

### 4.1 CLI 命令控制台

**目标**：通过串口交互控制所有外设。涉及队列、互斥量、任务挂起/恢复。

```
架构：
  UART_RX_ISR → xUartRxQueue → vUartRxTask (命令解析)
                                      │
                    ┌─────────────────┼─────────────────┐
                    ▼                 ▼                  ▼
              led on/off        sensor read         flash erase
              led blink <ms>    sensor log <n>      flash dump <addr>
              task suspend <name>   heap              eeprom write/read
              task list             oled on/off
```

**命令列表**：
| 命令 | 功能 |
|------|------|
| `help` | 列出所有命令 |
| `led <n> on/off/blink <ms>` | 控制 LED |
| `task list` | 打印所有任务状态 |
| `task suspend <name>` | 挂起指定任务 |
| `heap` | 打印剩余堆空间 |
| `sensor` | 读取一次 MPU6050 |
| `sensor log <n>` | 记录 n 次数据到 W25Q64 |
| `oled on/off` | 开关 OLED 显示 |
| `flash info` | 读取 W25Q64 ID |
| `flash dump <addr> <len>` | 十六进制 dump Flash 内容 |
| `eeprom write <addr> <byte>` | 写 EEPROM |
| `eeprom read <addr>` | 读 EEPROM |

### 4.2 运动数据记录器

**目标**：MPU6050 连续采样 → 环形缓冲 → W25Q64 存储 → CLI 导出。

```
MPU6050_ReadTask (Prio=3, vTaskDelayUntil 10ms)
      │
      ▼
  Stream Buffer (4KB 环形)
      │
      ▼
  LogWriterTask (Prio=2)
      │  攒够 256 字节 → W25Q64_WritePage
      ▼
  W25Q64 (8MB, 可存 ~200 万条记录)
      │
      ▼
  CLI `flash dump` 命令导出 .csv
```

**涉及知识点**：`vTaskDelayUntil` 精确周期、Stream Buffer、SPI DMA、互斥量保护 Flash 写。

### 4.3 OLED 仪表盘

**目标**：OLED 实时显示系统状态。

```
OLED 四行布局：
┌──────────────────┐
│ MPU: ax  ay  az  │  ← MPU6050 实时数据
│ Gyro:gx  gy  gz  │  ← 角速度数据
│ Heap: xxxx free   │  ← 堆剩余
│ Tasks: 8 running  │  ← 任务数量
└──────────────────┘
```

**涉及知识点**：互斥量保护 I2C、队列传递传感器数据、`vTaskList` 获取任务信息。

---

## 推荐学习资源

### 官方文档（必读）

| 资源 | 说明 |
|---|---|
| [FreeRTOS 官方文档](https://www.freertos.org/Documentation/RTOS_book.html) | 最权威的参考 |
| [Mastering the FreeRTOS Real Time Kernel](https://www.freertos.org/Documentation/02-Kernel/00-Overview) | 官方 handbook，160+ 页 |
| `Middlewares/FreeRTOS/Source/*.c` 源码 | 代码即文档，特别是 `tasks.c` 的头部注释 |

### 调试工具

| 工具 | 用途 |
|---|---|
| OpenOCD + arm-none-eabi-gdb | 命令行调试 |
| Segger SystemView | 免费、实时事件追踪 |
| FreeRTOS+Trace / Tracealyzer | 商业级可视化分析（30 天试用） |

---

## 项目目录学习索引

| 文件 | 学习时重点关注 |
|---|---|
| `Core/Inc/FreeRTOSConfig.h` | 修改调度、定时器、堆、溢出检测配置 |
| `Core/Src/main.c` | 任务创建和实验入口 |
| `Core/Src/stm32f1xx_it.c` | 添加外设 ISR，调用 FromISR API |
| `Core/Src/debug_uart.c` | UART 收发框架，队列 + DMA 实战参考 |
| `Middlewares/FreeRTOS/Source/tasks.c` | `vTaskDelay`、`vTaskSuspend` 源码 |
| `Middlewares/FreeRTOS/Source/queue.c` | 队列、信号量、互斥量源码 |
| `Middlewares/FreeRTOS/Source/portable/MemMang/heap_4.c` | 内存分配源码 |
