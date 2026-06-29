"""
生成 compile_commands.json，用于 VSCode C/C++ IntelliSense。
每次在 Makefile 中添加/删除 .c 文件后运行一次即可。
"""
import json, os

ws = os.path.dirname(os.path.abspath(__file__))

sources = [
    "Core/Src/main.c",
    "Core/Src/stm32f1xx_it.c",
    "Core/Src/stm32f1xx_hal_msp.c",
    "Core/Src/system_stm32f1xx.c",
    "Core/Src/stm32f1xx_hal_timebase_tim.c",
    "Core/Src/gpio.c",
    "Core/Src/usart.c",
    "Core/Src/debug_uart.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c",
    "Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c",
    "Middlewares/FreeRTOS/Source/tasks.c",
    "Middlewares/FreeRTOS/Source/list.c",
    "Middlewares/FreeRTOS/Source/queue.c",
    "Middlewares/FreeRTOS/Source/timers.c",
    "Middlewares/FreeRTOS/Source/event_groups.c",
    "Middlewares/FreeRTOS/Source/stream_buffer.c",
    "Middlewares/FreeRTOS/Source/portable/MemMang/heap_4.c",
    "Middlewares/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c",
]

includes = [
    "-ICore/Inc",
    "-IDrivers/STM32F1xx_HAL_Driver/Inc",
    "-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy",
    "-IDrivers/CMSIS/Device/ST/STM32F1xx/Include",
    "-IDrivers/CMSIS/Include",
    "-IMiddlewares/FreeRTOS/Source/include",
    "-IMiddlewares/FreeRTOS/Source/portable/GCC/ARM_CM3",
]

cc = "D:/stm32tools/arm-none-eabi-gcc/14.2.1-1.1.1/.content/bin/arm-none-eabi-gcc.exe"
common_flags = f"-mcpu=cortex-m3 -mthumb -DUSE_HAL_DRIVER -DSTM32F103xB {' '.join(includes)} -Og -Wall -fdata-sections -ffunction-sections -g -gdwarf-2"

entries = []
for src in sources:
    entries.append({
        "directory": ws,
        "command": f"{cc} -c {common_flags} {src} -o build/{os.path.basename(src).replace('.c', '.o')}",
        "file": src,
    })

out = os.path.join(ws, "compile_commands.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(entries, f, indent=2, ensure_ascii=False)

print(f"OK: {len(entries)} source files → {out}")
