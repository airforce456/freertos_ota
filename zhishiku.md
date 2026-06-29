# FreeRTOS 系统学习方案（STM32F103 + CubeMX + GCC）

> 前置条件：已完成 FreeRTOS 手动移植到 STM32F103C8Tx，工程可编译、可运行。

## 学习路线总览

```
第1阶段：基础（2周）
  任务管理 → 调度机制 → 任务间通信基础
         ↓
第2阶段：同步与通信（2周）
  队列 → 信号量 → 互斥量 → 事件组 → 任务通知
         ↓
第3阶段：进阶（1周）
  软件定时器 → 内存管理 → 中断管理 → 低功耗
         ↓
第4阶段：实战（2周）
  综合项目 → 调试优化 → 代码审查
```

## 第1阶段：任务管理（核心基础）

### 1.1 任务创建与删除

**学习目标**：理解任务控制块（TCB）、任务栈、任务优先级。

**实践内容**：

```c
// 1. 动态创建任务
xTaskCreate(TaskFunc, "Task", 128, NULL, 1, &hTask);

// 2. 静态创建任务（使用预分配栈）
xTaskCreateStatic(TaskFunc, "Task", STACK_SIZE, NULL, 1, stackBuffer, &taskBuffer);

// 3. 删除任务（自己删除自己 / 其他任务删除）
vTaskDelete(NULL);  // 自杀
vTaskDelete(hTask); // 他杀

// 4. 获取任务句柄、优先级、状态
TaskHandle_t me = xTaskGetCurrentTaskHandle();
UBaseType_t prio = uxTaskPriorityGet(hTask);
eTaskState state = eTaskGetState(hTask);
```

**实验清单**：

- [ ] 创建 3 个不同优先级的 LED 闪烁任务，观察抢占
- [ ] 在任务中调用 `vTaskDelete(NULL)` 删除自己
- [ ] 用 `vTaskList()` 或调试器查看所有任务状态
- [ ] 对比动态创建 (`xTaskCreate`) 和静态创建 (`xTaskCreateStatic`) 的内存占用

**关键问题（回答并写注释）**：

- [ ] 任务栈大小怎么估算？不够会怎样？（栈溢出检测）
- [ ] `configMAX_PRIORITIES` 改大会有什么代价？
- [ ] Idle Task 是干什么的？优先级是多少？

### 1.2 延时函数对比

**学习目标**：理解相对延时和绝对延时的区别。

```c
// 相对延时：从现在起延时 N 个 tick
vTaskDelay(pdMS_TO_TICKS(500));     // 500ms 后恢复就绪

// 绝对延时：从上次唤醒起延时 N 个 tick（更精确的周期执行）
TickType_t xLastWakeTime = xTaskGetTickCount();
vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));  // 精确 100ms 周期
```

**实验清单**：

- [ ] 用 `vTaskDelay()` 实现 500ms 周期闪烁，长时间运行观察是否漂移
- [ ] 用 `vTaskDelayUntil()` 实现 500ms 周期闪烁，对比精度
- [ ] 在任务中调用 `vTaskDelay(1)`，观察实际最小延时是多少

### 1.3 任务抢占与时间片

**学习目标**：理解抢占式调度和时间片轮转。

```c
// FreeRTOSConfig.h 中配置
#define configUSE_PREEMPTION    1   // 抢占式
#define configUSE_TIME_SLICING  1   // 同优先级时间片轮转
#define configTICK_RATE_HZ      1000
```

**实验清单**：

- [ ] 创建优先级 2 和优先级 3 的任务，观察高优先级如何抢占低优先级
- [ ] 创建 3 个同优先级任务，每个打印自己的 ID，观察时间片轮转
- [ ] 把 `configUSE_PREEMPTION` 改为 0，对比行为差异

### 1.4 任务挂起与恢复

```c
vTaskSuspend(hTask);     // 挂起（无限期移出调度）
vTaskResume(hTask);      // 恢复
vTaskSuspendAll();       // 挂起调度器（临界区，慎用）
xTaskResumeAll();        // 恢复调度器
```

**实验清单**：

- [ ] 任务 A 挂起任务 B，5 秒后恢复
- [ ] 按键中断中挂起/恢复 LED 闪烁任务
- [ ] 对比 `vTaskSuspend()` 和 `vTaskDelay()` 的区别

**阶段测验**：用 3 个任务实现一个简单的"任务看门狗"——Task A 监控 Task B 是否还在运行，若卡死则重启它。

---

## 第2阶段：任务间通信与同步

### 2.1 队列（Queue）

**学习目标**：掌握 FreeRTOS 最核心的通信机制。

```c
// 创建队列（长度 10，每项 uint32_t）
QueueHandle_t xQueue = xQueueCreate(10, sizeof(uint32_t));

// 发送（队尾入队）
uint32_t data = 42;
xQueueSend(xQueue, &data, portMAX_DELAY);        // 阻塞等待
xQueueSendToBack(xQueue, &data, 0);               // 非阻塞
xQueueSendToFront(xQueue, &data, 0);              // 队首入队（插队）

// 接收（队首出队）
xQueueReceive(xQueue, &data, pdMS_TO_TICKS(100)); // 超时 100ms

// 覆写（队列满时覆盖最旧数据，不阻塞）
xQueueOverwrite(xQueue, &data);

// 查看队首但不出队
xQueuePeek(xQueue, &data, 0);

// 查询队列状态
UBaseType_t count = uxQueueMessagesWaiting(xQueue);  // 当前消息数
UBaseType_t spaces = uxQueueSpacesAvailable(xQueue); // 剩余空间
```

**实验清单**：

- [ ] Task A 每秒发送递增计数器到队列，Task B 接收并打印（生产者-消费者模型）
- [ ] 当队列满时，Task A 用不同超时值观察阻塞行为
- [ ] 用 `xQueueOverwrite()` 实现"只关心最新值"的模式（如传感器读数）
- [ ] 用队列传递结构体（如 `{timestamp, sensor_id, value}`）

**理解要点**：

- [ ] `xQueueSend` 和 `xQueueSendToBack` 是完全相同的
- [ ] 队列满时 `xQueueSend` 会阻塞，`xQueueOverwrite` 不会
- [ ] 队列是 **拷贝式传递**（不是指针传递），数据被复制到队列内部

### 2.2 信号量（Semaphore）

**学习目标**：区分二值信号量、计数信号量、互斥量的使用场景。

#### 二值信号量（Binary Semaphore）

```c
SemaphoreHandle_t xBinarySem = xSemaphoreCreateBinary();

// 中断中"给出"信号量（通知任务有事件发生）
xSemaphoreGiveFromISR(xBinarySem, &pxHigherPriorityTaskWoken);

// 任务中"获取"信号量（等待事件）
xSemaphoreTake(xBinarySem, portMAX_DELAY);
```

**经典场景**：中断通知任务处理数据（UART 收完一帧、按键按下、ADC 转换完成）。

#### 计数信号量（Counting Semaphore）

```c
SemaphoreHandle_t xCountSem = xSemaphoreCreateCounting(10, 0);  // 最大值 10，初值 0

xSemaphoreGive(xCountSem);    // 计数 +1
xSemaphoreTake(xCountSem, 0); // 计数 -1
```

**经典场景**：资源池管理（如 5 个 UART 发送缓冲区，最多 5 个任务同时使用）。

**实验清单**：

- [ ] 用二值信号量实现：按键中断 → 通知任务 → LED 翻转
- [ ] 用计数信号量管理固定大小的资源池（模拟打印机队列）
- [ ] 对比 `xSemaphoreGive()` 和 `xSemaphoreGiveFromISR()` 的用法差异

### 2.3 互斥量（Mutex）

**学习目标**：理解优先级反转问题和互斥量的优先级继承机制。

```c
SemaphoreHandle_t xMutex = xSemaphoreCreateMutex();

xSemaphoreTake(xMutex, portMAX_DELAY);   // 上锁
/* 临界区操作（访问共享资源） */
xSemaphoreGive(xMutex);                  // 解锁

// 递归互斥量（同一任务可多次获取）
xSemaphoreCreateRecursiveMutex();
xSemaphoreTakeRecursive(xMutex, portMAX_DELAY);  // 可嵌套调用
xSemaphoreGiveRecursive(xMutex);
```

**经典场景**：保护 I2C 总线、SPI Flash、全局变量等共享资源。

**实验清单**：

- [ ] 两个任务交替向同一 UART 发送数据，用互斥量保护，观察输出不乱序
- [ ] 构造优先级反转场景：低优先级持有锁 → 中优先级抢占 → 高优先级阻塞
- [ ] 用互斥量（有优先级继承）和信号量（无优先级继承）对比上述场景的行为差异

**理解要点**：

- [ ] 信号量 vs 互斥量：信号量用于**同步**（通知），互斥量用于**互斥**（保护资源）
- [ ] 互斥量不能用于 ISR 中（`xSemaphoreGiveFromISR` 对互斥量无效）
- [ ] 互斥量是谁 Take 谁 Give，不能跨任务 Give

### 2.4 事件组（Event Group）

**学习目标**：掌握多事件组合等待。

```c
#define BIT_TEMP_READY  (1 << 0)
#define BIT_PRESS_READY (1 << 1)
#define BIT_HUM_READY   (1 << 2)

EventGroupHandle_t xEventGroup = xEventGroupCreate();

// 设置事件位
xEventGroupSetBits(xEventGroup, BIT_TEMP_READY);
xEventGroupSetBitsFromISR(xEventGroup, BIT_PRESS_READY, &pxHigherPriorityTaskWoken);

// 等待事件
EventBits_t bits = xEventGroupWaitBits(
    xEventGroup,
    BIT_TEMP_READY | BIT_PRESS_READY,  // 等待这些位
    pdTRUE,         // 等待后清除
    pdTRUE,         // 等待 ALL 位（pdFALSE = 等待任一）
    pdMS_TO_TICKS(5000)
);

// 同步点（多任务同时到达某点后才继续）
xEventGroupSync(xEventGroup, BIT_TASK1, ALL_TASKS_BITS, portMAX_DELAY);
```

**实验清单**：

- [ ] 一个"数据聚合"任务等待温度、压力、湿度三个传感器数据都就绪后统一处理
- [ ] 用事件组实现：按键1按下 + 按键2按下 → LED 点亮（AND 逻辑）
- [ ] 用事件组实现：任一按键按下 → LED 点亮（OR 逻辑）
- [ ] 用 `xEventGroupSync()` 实现 3 个任务的栅栏同步

### 2.5 任务通知（Task Notification）

**学习目标**：理解任务通知是"轻量级替代方案"，在简单场景下比队列/信号量/事件组更高效。

```c
// 通知任务（替代二值信号量）
xTaskNotifyGive(hTask);                          // 通知
xTaskNotifyGiveFromISR(hTask, &pxHigherWoken);   // ISR 中通知
ulTaskNotifyTake(pdTRUE, portMAX_DELAY);          // 等待通知（类似 Take）

// 发送值（替代队列，传 32 位值）
xTaskNotify(hTask, value, eSetValueWithOverwrite);
xTaskNotifyWait(0, 0xFFFFFFFF, &notifiedValue, portMAX_DELAY);

// 用作事件位（替代轻量级事件组）
xTaskNotify(hTask, (1 << 2), eSetBits);
xTaskNotifyWait(0, 0xFFFFFFFF, &bits, portMAX_DELAY);
```

**实验清单**：

- [ ] 用任务通知替代二值信号量，实现中断→任务通知模式，对比代码量
- [ ] 用任务通知传递传感器值（32 位内），与队列方案对比 RAM 占用
- [ ] 测量：发送 1000 次通知 vs 发送 1000 次队列的时间差异

**理解要点**：

- [ ] 任务通知只能 1 对 1（一个发送者 → 一个接收者），队列可以多对多
- [ ] 任务通知速度比队列快约 45%，RAM 占用更少
- [ ] 任务通知不能用在"等待多个事件源"的场景

**阶段测验**：实现一个数据采集系统——ADC 中断采样 100 点后通知任务计算平均值，计算结果通过队列发送给 UART 打印任务。

---

## 第3阶段：进阶主题

### 3.1 软件定时器（Software Timer）

**学习目标**：理解软件定时器的回调机制和 Daemon Task。

```c
// 单次定时器
TimerHandle_t xOnceTimer = xTimerCreate(
    "Once", pdMS_TO_TICKS(3000), pdFALSE, 0, TimerCallback
);
xTimerStart(xOnceTimer, 0);

// 周期定时器
TimerHandle_t xPeriodicTimer = xTimerCreate(
    "Periodic", pdMS_TO_TICKS(1000), pdTRUE, 0, TimerCallback
);

// 修改周期、停止、获取状态
xTimerChangePeriod(xTimer, pdMS_TO_TICKS(500), 0);
xTimerStop(xTimer, 0);
xTimerIsTimerActive(xTimer);

void TimerCallback(TimerHandle_t xTimer) {
    // ⚠️ 回调在 Daemon Task 中执行，不能阻塞！
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}
```

**实验清单**：

- [ ] 创建 3 个软件定时器，分别以 100ms、500ms、1s 周期闪烁不同 LED
- [ ] 单次定时器：按键按下后 3 秒关闭 LED
- [ ] 对比软件定时器和 `vTaskDelay()` 实现周期任务的区别
- [ ] 查看 Daemon Task 的栈使用情况（`uxTaskGetStackHighWaterMark()`）

**理解要点**：

- [ ] 软件定时器回调在 **Timer Service Task (Daemon)** 中执行，不是独立线程
- [ ] 回调函数**绝对不能阻塞**（不能调用 `vTaskDelay`、带超时的 `xQueueReceive` 等）
- [ ] `configUSE_TIMERS` 必须为 1，同时 `configTIMER_TASK_STACK_DEPTH` 和 `configTIMER_TASK_PRIORITY` 需要合理配置

### 3.2 内存管理

**学习目标**：理解 FreeRTOS 的堆管理方案，选择并配置合适的 heap。

| heap_x.c | 特点 | 适用场景 |
|---|---|---|
| heap_1 | 只分配不释放，简单 | 只创建不删除任务的项目 |
| heap_2 | 可释放但不合并碎片 | 已过时，不推荐 |
| heap_3 | 封装标准 malloc/free | 需要线程安全 malloc |
| **heap_4** | **可释放 + 相邻空闲块合并** | **推荐，大多数项目首选** |
| heap_5 | heap_4 + 多块不连续内存 | 有外部 SRAM 等场景 |

**实验清单**：

- [ ] 用 `xPortGetFreeHeapSize()` 追踪创建/删除任务前后的堆空间变化
- [ ] 打印 Idle Task 的栈水位线：`uxTaskGetStackHighWaterMark(xIdleHandle)`
- [ ] 在 main.c 开头打印 `configTOTAL_HEAP_SIZE` 和启动时的空闲堆大小
- [ ] 创建 100 个小任务然后全部删除，观察是否有内存泄漏（堆空间是否恢复）

### 3.3 中断管理

**学习目标**：掌握中断优先级规则和 FreeRTOS API 在 ISR 中的使用。

#### 中断优先级硬规则

```
中断优先级 0 ~ 4  (数值)：不能调用任何 FreeRTOS API
中断优先级 5 ~ 15 (数值)：可以调用 FreeRTOS API（FromISR 版本）
FreeRTOS 内核中断  (15)：  最低优先级
```

#### FromISR 系列 API

| 任务中调用 | ISR 中调用 |
|---|---|
| `xSemaphoreGive()` | `xSemaphoreGiveFromISR()` |
| `xQueueSend()` | `xQueueSendFromISR()` |
| `xTaskNotifyGive()` | `xTaskNotifyGiveFromISR()` |
| `xEventGroupSetBits()` | `xEventGroupSetBitsFromISR()` |

**关键：`pxHigherPriorityTaskWoken` 参数**

```c
BaseType_t xHigherPriorityTaskWoken = pdFALSE;
xSemaphoreGiveFromISR(xSem, &xHigherPriorityTaskWoken);
portYIELD_FROM_ISR(xHigherPriorityTaskWoken);  // 在 ISR 末尾调用
```

**实验清单**：

- [ ] STM32 定时器中断（优先级 6）中发送信号量，通知任务处理
- [ ] 提高外设中断优先级到 4，尝试调用 `xQueueSendFromISR()` —— 观察 assert 失败
- [ ] 用 `portYIELD_FROM_ISR()` 实现 ISR 返回时立即切换到高优先级任务
- [ ] 测量从中断触发到任务开始执行的时间（中断延迟）

### 3.4 调试与诊断

**实验清单**：

- [ ] 使用栈溢出检测：
  ```c
  #define configCHECK_FOR_STACK_OVERFLOW  2
  // 实现 vApplicationStackOverflowHook()
  void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
      // 在这里设置断点
      while(1);
  }
  ```
- [ ] 内存分配失败钩子：
  ```c
  void vApplicationMallocFailedHook(void) {
      while(1);  // 设断点
  }
  ```
- [ ] 用 OpenOCD + GDB 查看任务状态：`info threads`、`pxCurrentTCB`、各任务栈
- [ ] 运行时打印任务信息：`vTaskList()` / `uxTaskGetSystemState()`

---

## 第4阶段：综合实战项目

### 4.1 多级菜单系统（UART 控制台）

**目标**：通过串口命令交互，综合运用队列、任务、软件定时器。

```
架构：
  UART_RX_ISR → Queue → ConsoleParserTask → CommandHandlerTask
         ↑                                       ↓
  UART_TX_Task ← Queue ← 各功能任务返回结果

功能：
  - help       : 列出命令
  - task list  : 打印所有任务状态
  - led on/off : 控制 LED
  - led blink <ms> : 设置闪烁周期
  - heap       : 打印剩余堆空间
```

**涉及知识点**：任务、队列、UART 中断、互斥量（保护 UART TX）、CLI 解析状态机。

### 4.2 数据采集与记录器

**目标**：定时采集传感器数据并存储。

```
架构：
  SensorTask(1s周期) → Queue → DataLoggerTask → Queue → UART_UploadTask
                                ↓
                          EventGroup(触发存储)
```

**涉及知识点**：`vTaskDelayUntil` 精确周期、队列流控、事件组触发、多优先级任务协作。

### 4.3 固件 OTA 升级框架

**目标**：通过 UART/YModem 协议接收固件包并写入 Flash。

```
架构：
  UART_RX_ISR → StreamBuffer → YModemTask
                                  ↓
                           FlashWriteTask(DMA)
                                  ↓
                           CRCVerifyTask(Deferred Interrupt)
                                  ↓
                           BootFlagSet → 软件复位

FreeRTOS 特性：
  - Stream Buffer (流式数据接收)
  - Deferred Interrupt Processing (DMA 完成中断 → 任务)
  - Software Timer (接收超时)
```

**涉及知识点**：StreamBuffer、DMA+中断、软件定时器、Flash HAL、复位管理。

---

## 推荐学习资源

### 官方文档（必读）

| 资源 | 说明 |
|---|---|
| [FreeRTOS 官方文档](https://www.freertos.org/Documentation/RTOS_book.html) | 最权威的参考，英文 |
| [Mastering the FreeRTOS Real Time Kernel](https://www.freertos.org/Documentation/02-Kernel/00-Overview) | 官方 handbook，160+ 页 |
| `Middlewares/FreeRTOS/Source/*.c` 源码 | 代码即文档，特别是 `tasks.c` 的头部注释 |

### 调试工具

| 工具 | 用途 |
|---|---|
| STM32CubeIDE FreeRTOS 调试视图 | 可视化任务列表、栈使用、队列状态 |
| OpenOCD + arm-none-eabi-gdb | 命令行调试 |
| FreeRTOS+Trace / Tracealyzer | 商业级可视化分析（30 天试用） |
| Segger SystemView | 免费、实时事件追踪 |

### 进阶方向

- [ ] **TrustZone + FreeRTOS**：在 STM32L5/U5 上使用 TrustZone 隔离安全任务和非安全任务
- [ ] **MPU 保护**：用 Cortex-M3 的 MPU 限制任务能访问的内存区域
- [ ] **SMP 支持**：在多核 MCU（如 STM32H7）上使用 FreeRTOS SMP 版本
- [ ] **POSIX 兼容层**：使用 FreeRTOS-Plus-POSIX 在嵌入式上写类 Linux 代码

---

## 项目目录学习索引

你当前项目的文件对应关系：

| 文件 | 学习时重点关注 |
|---|---|
| `Core/Inc/FreeRTOSConfig.h` | 第3阶段 3.1-3.3：修改定时器、堆、溢出检测配置 |
| `Core/Src/main.c` | 第1阶段：在此创建/修改任务实验 |
| `Core/Src/stm32f1xx_it.c` | 第3阶段 3.3：在此添加 FromISR API 调用 |
| `Middlewares/FreeRTOS/Source/tasks.c` | 第1阶段：`vTaskDelay`、`vTaskSuspend` 源码 |
| `Middlewares/FreeRTOS/Source/queue.c` | 第2阶段：队列、信号量、互斥量源码 |
| `Middlewares/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c` | 第3阶段 3.3：PendSV、SysTick 上下文切换源码 |
| `Middlewares/FreeRTOS/Source/portable/MemMang/heap_4.c` | 第3阶段 3.2：内存分配源码 |
