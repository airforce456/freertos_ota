# 嵌入式基础知识库

## 一、链接脚本（.ld 文件）

### 作用

链接脚本定义了**程序在内存中的布局**。两个关键定义：

```ld
FLASH (rx) : ORIGIN = 起始地址, LENGTH = 大小
RAM   (rw) : ORIGIN = 0x20000000,  LENGTH = 20K
```

- `ORIGIN`：代码的起始地址（`.text` 段、中断向量表放这里）
- `LENGTH`：可用的最大空间（超了就报错）

### 为什么 Bootloader 和 APP 需要不同的链接脚本

**中断向量表的位置决定了 CPU 上电后从哪里取第一条指令**。

```
CPU 上电 → 读 0x08000000 → 取 SP 值
        → 读 0x08000004 → 取 PC 值 (Reset_Handler 地址)
        → 跳到 PC 执行
```

- **Bootloader 的向量表必须放在 `0x08000000`**，因为 CPU 上电只从这里读。
- **APP 的向量表必须放在 `0x08005000`**，因为 `0x08000000~0x08004FFF` 已被 Bootloader 占用。

如果 APP 也用 `FLASH.ld`（ORIGIN=0x08000000），编译出的函数地址全部错位 → Bootloader 跳到 `0x08005000` 读到错误代码 → HardFault。

### Flash 和 RAM 的分配

```
Flash (64KB)              RAM (20KB)
┌──────────────┐ 0x08000000 ┌──────────────┐ 0x20000000
│ Bootloader   │            │              │
│ (20KB)       │            │  共用 RAM    │
├──────────────┤ 0x08005000 │  Bootloader  │
│              │            │  先跑，用完  │
│ APP (44KB)   │            │  就还给 APP  │
│              │            │              │
└──────────────┘ 0x0800FFFF └──────────────┘ 0x20005000
```

- **Flash 是静态分割的**：Bootloader 和 APP 的代码永远不重叠。
- **RAM 是动态共享的**：Bootloader 先跑用完，`__set_MSP(appSp)` 切换栈后 APP 从头初始化覆盖。

### 项目中的三个链接脚本

| 文件 | FLASH ORIGIN | LENGTH | 用途 |
|------|-------------|--------|------|
| `STM32F103C8Tx_FLASH.ld` | `0x08000000` | 64K | `make` 默认编译，单一完整固件 |
| `STM32F103C8Tx_BOOT.ld` | `0x08000000` | 20K | Bootloader |
| `STM32F103C8Tx_APP.ld` | `0x08005000` | 44K | APP |

---

## 二、.data 段 和 .bss 段

### .data 段 — 已初始化的全局/静态变量

**有非零初始值**的全局变量和静态变量。

```c
int count = 100;           // → .data 段，初始值 100
static int flag = 1;       // → .data 段，初始值 1
char name[] = "hello";     // → .data 段，初始值 "hello"
```

**存储方式**：
```
Flash 中：保存初始值（100, 1, "hello"）
上电后：startup.s 把初始值从 Flash 复制到 RAM
```

**为什么存两份？** Flash 是只读的，但全局变量必须可读写。Flash 存初始值，RAM 存运行时的值。

### .bss 段 — 未初始化或初始值为 0 的全局/静态变量

```c
int counter;               // → .bss 段，初始值 0（C 标准规定）
static int temp;           // → .bss 段
char buffer[256] = {0};    // → .bss 段，全部为 0
```

**存储方式**：
```
Flash 中：不存任何数据（全 0 没必要占 Flash 空间）
上电后：startup.s 把 .bss 段在 RAM 中的区域全部清零
```

### 对比

| | .data | .bss | 栈 |
|---|-------|------|-----|
| 存储内容 | 有非零初始值的全局/静态变量 | 未初始化/零初始值的全局/静态变量 | 局部变量、函数参数、返回地址 |
| 占用 Flash | ✅ 存初始值 | ❌ 不占 Flash | ❌ |
| 占用 RAM | ✅ | ✅ | ✅ |
| 上电初始化 | Flash → RAM 复制 | 全部填 0 | 不需要（SP 指向栈顶即可） |
| 生命周期 | 程序全周期 | 程序全周期 | 函数调用期间 |

### 举例

```c
int a = 42;          // .data: Flash 存 42, RAM 存运行时值
int b;               // .bss:  Flash 无,    RAM 上电清零
static int c = 7;    // .data
static int d;        // .bss

void func() {
    int e = 5;       // 栈（不是 .data 也不是 .bss）
    static int f = 9;// .data（静态局部变量 = 全局生命周期）
}
```

### Bootloader 中的变量分布

Bootloader 不使用 `.data` 段（没有需要非零初始值的全局变量）。所有变量要么在栈上，要么在 `.bss` 段：

| 来源文件 | 变量 | 类型 | 位置 |
|---------|------|------|------|
| `boot_main.c` | `bootCmd`, `slot`, `fw_size`, `fw_crc`, `pageBuf[256]` | 局部变量 | **栈** |
| `boot_main.c` | `appSp`, `appPc` | 局部变量 | **栈** |
| `boot_flash.c` | `erase`, `pageError` | 局部变量 | **栈** |
| `at24c02.c` | `Boot_DelayMs` 循环计数 `i`, `j` | 局部变量 | **栈** |

跳转到 APP 后，APP 的 `__libc_init` 会清零 `.bss`、复制 `.data`，Bootloader 用过的 RAM 被完全覆盖。

---

## 三、volatile 关键字

### 作用

告诉编译器：**每次访问这个变量都必须从内存读写，不能优化到寄存器里**。它只改变编译器生成的指令，不改变变量的存储位置。

### 为什么需要

```c
// 不加 volatile
int flag;
while (flag == 0) { }
// 编译器优化：把 flag 读到 R0，死循环读 R0，不再访问内存
// → 即使中断改了 flag，这里永远看不到！

// 加 volatile
volatile int flag;
while (flag == 0) { }
// 编译器：每次循环都从内存读 flag
// → 中断改了 flag，这里立即看到
```

### 不改变存储位置

```c
volatile int a = 5;      // → 还是在 .data 段
volatile int b;           // → 还是在 .bss 段
volatile int c;           // 局部变量 → 还是在栈上
```

### 项目中的实际用途

```c
// esp8266.c — 中断和主循环共享的变量
volatile unsigned short esp8266_cnt = 0;
// ISR 里 esp8266_cnt++
// main 循环里读 esp8266_cnt
// 不加 volatile → 编译器缓存到寄存器，读不到 ISR 的更新

// boot_flash.c — 硬件寄存器访问
uint32_t sp = *(volatile uint32_t *)0x08005000;
// 强制每次从 0x08005000 读，不优化掉
```

### 使用场景

| 场景 | 说明 |
|------|------|
| 中断和主循环共享的变量 | ISR 修改，主循环读取 |
| 多任务共享的变量（无锁时） | 不同任务可能修改 |
| 硬件寄存器映射 | 寄存器值由硬件改变 |
| 内存映射 I/O | 外设寄存器 |

### 三者关系总结

| 关键字/段 | 决定什么 |
|----------|---------|
| `.data` / `.bss` / 栈 | 变量**存在哪里**（RAM 哪个区域） |
| `volatile` | 变量**怎么访问**（每次都读内存 vs 缓存到寄存器） |
| `static` | 变量的**作用域和生命周期**（文件内可见 / 全局生命周期） |

三者互不冲突，可以组合使用：`static volatile int flag;` → 文件内可见、每次读内存、在 `.bss` 段。
