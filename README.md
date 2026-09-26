# 硬件与驱动开发：Arduino · STM32 · 树莓派

发展史、驱动分层、可运行的例子和驱动逆向工程。

同一颗传感器（BMP280）在三个平台上各写一遍：先写寄存器，再用厂商库，然后是 Linux 用户态和内核态。写完之后，"驱动"在每一层指的是什么就清楚了。逆向部分用真实项目 [hezi-hack](../hezi-hack)（给 ZWO ASIAIR 盒子加 QHY 相机和滤镜轮驱动）做案例。

---

## 目录

0. [仓库内容与例子索引](#0-仓库内容与例子索引)
1. [什么是"驱动"](#1-什么是驱动)
2. [发展史](#2-发展史)
3. [三个平台横向对比](#3-三个平台横向对比)
4. [驱动开发分层详解（配例子）](#4-驱动开发分层详解配例子)
5. [常用总线速查与坑](#5-常用总线速查与坑)
6. [驱动逆向工程](#6-驱动逆向工程)
7. [案例：hezi-hack，给 ASIAIR 加 QHY 驱动](#7-案例hezi-hack给-asiair-加-qhy-驱动)
8. [工具链与调试](#8-工具链与调试)
9. [学习路线](#9-学习路线)
10. [参考资料](#10-参考资料)

---

## 0. 仓库内容与例子索引

```
examples/
├── portable_driver/bmp280/        平台无关的 BMP280 驱动 + 主机单元测试（不需要硬件）
├── arduino/
│   ├── 01_blink_register/         digitalWrite vs PORTB vs PINB：抽象层有多大开销
│   └── 02_bmp280_wire/            可移植驱动接到 Wire 库
├── stm32/
│   ├── 01_baremetal_blink_uart/   不用 HAL、不用 CMSIS：启动代码、链接脚本、SysTick、中断串口
│   └── 02_hal_bmp280/             可移植驱动接到 HAL_I2C_Mem_Read
├── raspberrypi/
│   ├── 01_userspace_gpio/         libgpiod v2 / mmap 寄存器 / gpiozero 三种 GPIO 写法
│   ├── 02_i2c_dev_bmp280/         可移植驱动接到 /dev/i2c-1（用户态驱动）
│   ├── 03_kernel_misc_chardev/    最小内核模块：/dev/hello
│   ├── 04_platform_driver_dt/     platform 驱动 + 设备树 overlay + sysfs + 内核定时器
│   └── 05_i2c_hwmon_driver/       BMP280 内核 I2C 驱动，温度导出到 hwmon
├── usb/libusb_probe/              libusb 用户态 USB 驱动骨架：枚举、描述符、厂商控制请求
└── reverse_engineering/
    ├── shim_interpose/            同名替换库 + RTLD_DEEPBIND + 调用者识别 + 汇编跳板（可运行的 Linux 演示）
    ├── usb_spy_preload/           LD_PRELOAD 钩子：记录闭源 SDK 的每个 USB 传输
    └── tools/
        ├── elf_survey.sh          闭源 .so 的静态侦察：ABI、依赖、符号版本、导入导出、字符串
        ├── usbmon_parse.py        usbmon 文本抓包整理成"请求 → 应答"表
        ├── serial_probe.py        未知串口设备：监听、猜波特率、发候选命令并计时
        └── i2c_regdump.py         未知 I2C 芯片：扫描、整页 dump、差分找"活"寄存器
```

| 例子 | 平台 | 怎么跑 | 已验证 |
|---|---|---|---|
| `portable_driver/bmp280` | 任意电脑 | `make test` | ✅ 主机上跑通，结果等于数据手册示例值（25.08 °C / 100653 Pa） |
| `arduino/*` | Uno / Nano | Arduino IDE 打开 `.ino` | 代码审阅 |
| `stm32/01_baremetal_blink_uart` | STM32F103C8 Blue Pill | `make && make flash` | ✅ clang 交叉编译 Cortex-M3 通过 |
| `stm32/02_hal_bmp280` | 任意 STM32 + CubeMX 工程 | 加进工程 | 代码审阅 |
| `raspberrypi/01,02` | Pi 0～5（`gpio_mmap` 仅 Pi 0～4） | `make` | 代码审阅 |
| `raspberrypi/03,04,05` | Pi OS，内核 ≥ 6.3 | `make && sudo insmod *.ko` | 代码审阅 |
| `usb/libusb_probe` | Linux / macOS | `make` | 代码审阅 |
| `reverse_engineering/shim_interpose` | Linux x86-64 / arm64 / armhf | `make demo` | ✅ 跳板在三种架构上汇编通过 |
| `reverse_engineering/tools/usbmon_parse.py` | 任意 | 见文件头 | ✅ 用样例抓包测试 |
| `reverse_engineering/tools/serial_probe.py` | Linux / macOS | 见文件头 | ✅ 用伪终端模拟设备测试 |

---

## 1. 什么是"驱动"

**驱动 = 把硬件的寄存器、时序和协议，翻译成上层能用的接口。** 三个平台上"驱动"的形态差别很大：

```
             Arduino (AVR)             STM32 (Cortex-M)              树莓派 (Linux)
应用层       sketch: loop()            main() / RTOS 任务             进程（Python/C/…）
                 │                          │                          │ 系统调用 open/read/ioctl
设备驱动     Adafruit_BMP280 库        bmp280.c（自己写或厂商给）     ┌ 用户态: /dev/i2c-1 + 自己解析
                 │                          │                        └ 内核态: i2c_driver → hwmon/IIO
总线驱动     Wire (TWI)                HAL_I2C / LL_I2C              i2c-bcm2835 / i2c-designware（内核）
                 │                          │                          │
寄存器       TWBR/TWCR/TWDR            I2C1->CR1/SR1/DR              BSC 控制器寄存器（内核 ioremap）
                 │                          │                          │
硬件         ATmega328P TWI 外设        STM32 I2C 外设                 BCM2711 BSC / RP1 I2C
```

几个关键区别：

* **有没有操作系统。** Arduino 和 STM32 裸机上，驱动就是程序的一部分，想怎么访问寄存器都行，没有权限、没有隔离，一个野指针就能写坏外设。Linux 上驱动分成内核态和用户态，中间隔着系统调用、权限和设备模型。
* **谁决定"硬件在哪"。** MCU 上引脚和地址写死在代码或 CubeMX 生成的初始化里。Linux 用**设备树**描述硬件，驱动靠 `compatible` 匹配，换板子只改设备树。
* **"驱动"的层数。** 一个 I2C 传感器在 Linux 上至少涉及三层：I2C 控制器驱动（SoC 厂商写）、I2C 核心（内核）、传感器驱动（你写）。在 Arduino 上，这三层压缩成 Wire 库加一个传感器库。

---

## 2. 发展史

### 2.1 前史：单片机时代（1971–2004）

| 年份 | 事件 | 意义 |
|---|---|---|
| 1971 | Intel 4004 | 第一颗商用微处理器 |
| 1976 | Intel 8048（MCS-48） | 早期单片机：CPU、RAM、ROM、I/O 做在一颗芯片上 |
| 1980 | **Intel 8051（MCS-51）** | 统治嵌入式二十多年的架构，至今仍有大量兼容核（STC、Silicon Labs、CH55x） |
| 1993 | Microchip PIC16C84 | 片上 EEPROM，可以反复擦写，业余爱好者从此能自己烧程序 |
| 1996–97 | **Atmel AVR**（挪威理工学院学生 Alf-Egil Bogen、Vegard Wollan 设计） | 为 C 编译器设计的 RISC 架构，32 个通用寄存器，片上 Flash；后来成为 Arduino 的心脏 |
| 2001 | GCC 的 AVR 后端（avr-gcc）+ avr-libc 成熟 | 免费工具链，这是 Arduino 能出现的前提 |

这个阶段的"驱动开发"就是**对着数据手册写寄存器**，用汇编或 Keil C51、IAR 这类昂贵的商业编译器，下载程序靠编程器。门槛高，基本是电子工程师的专业活。

### 2.2 Arduino：把单片机交给所有人（2005–）

| 年份 | 事件 |
|---|---|
| 2003 | Hernando Barragán 在意大利 Ivrea 交互设计学院的硕士论文项目 **Wiring**：硬件板加上仿照 Processing 的 IDE 和 API |
| **2005** | Massimo Banzi、David Cuartielles、Tom Igoe、Gianluca Martino、David Mellis 在 Wiring 的基础上做出 **Arduino**（名字来自当地的 Bar di Re Arduino）。第一块板用 ATmega8 |
| 2007–09 | Diecimila（ATmega168）、Duemilanove（**ATmega328**）。bootloader 加 USB 串口免编程器下载，成为标准体验 |
| 2010 | **Arduino Uno**：用 ATmega8U2（R3 起为 16U2）做 USB 转串口，替掉 FTDI |
| 2012 | Due（Atmel SAM3X8E，**第一块 ARM Cortex-M3 的 Arduino**）、Leonardo（32U4，原生 USB） |
| 2014–15 | Espressif ESP8266 爆红，社区做出 ESP8266 的 Arduino core，**"Arduino" 从一种板子变成一套 API** |
| 2015–17 | Arduino LLC 与 Arduino S.r.l. 的商标之争，2016–17 年和解合并 |
| 2016 | Microchip 收购 Atmel；ESP32 发布 |
| 2017 | ST 官方的 **STM32duino** core：STM32 也能用 Arduino API |
| 2019–20 | Nano 33 BLE（nRF52840，基于 Mbed OS）、Portenta H7（**STM32H747** 双核）进入工业市场 |
| 2022 | Arduino IDE 2.0（基于 Theia，带调试器） |
| 2023 | UNO R4（Renesas RA4M1，Cortex-M4）：经典外形换成 32 位芯片 |
| 2025 | 高通宣布收购 Arduino，同时发布 **UNO Q**（Linux 应用处理器 QRB2210 加上 STM32U585 MCU）。Arduino、STM32、"跑 Linux 的板子"三条线汇到了同一块板上 |

**Arduino 对驱动开发的影响：**

* **HAL 思想平民化。** `digitalWrite()`、`Wire`、`SPI` 把寄存器藏起来，同一个传感器库能在 AVR、SAMD、ESP32、STM32 上编译。代价是性能（见 [例子 01](examples/arduino/01_blink_register/01_blink_register.ino)：`digitalWrite` 比直接写 `PINB` 慢一个数量级以上）和可控性。
* **库生态。** Adafruit、SparkFun 为几乎所有模块写了库。今天拿到一个新传感器，第一件事常常是读它的 Arduino 库源码，这些源码比数据手册更容易读懂。这本身也是一种"逆向"。
* **Arduino core 就是一套可移植的板级支持包（BSP）。** 移植一个新 core 等于实现 `pinMode/digitalWrite/millis/HardwareSerial/TwoWire` 这组接口，和 Linux 里给新 SoC 写 GPIO/I2C 控制器驱动是同一回事。

### 2.3 ARM Cortex-M 与 STM32（2004–）

| 年份 | 事件 |
|---|---|
| 2004 | ARM 发布 **Cortex-M3**（ARMv7-M）：只支持 Thumb-2，硬件压栈的 NVIC 中断，中断函数可以直接用 C 写。向量表是一张函数指针数组 |
| 2006 | Luminary Micro（后被 TI 收购）推出第一颗 Cortex-M3 MCU |
| **2007** | **ST 推出 STM32F1**（STM32F103 等），配套 **Standard Peripheral Library（SPL）** |
| 2008 | ARM 发布 **CMSIS**：统一的内核寄存器定义（`NVIC`、`SysTick`）和外设结构体风格 `GPIOA->ODR` |
| 2009–10 | Cortex-M0（小、便宜）、Cortex-M4（DSP 指令加可选 FPU） |
| 2011 | STM32F4（Cortex-M4F，168 MHz），嵌入式 DSP、飞控（PX4、Betaflight 前身）的主力 |
| 2013 | 兆易创新 **GD32F103**（与 STM32F103 引脚和寄存器兼容），国产兼容芯片开始出现 |
| 2014 | **STM32Cube**：**HAL** 库加图形化配置工具 **CubeMX** 生成初始化代码；之后又加入轻量的 **LL** 库。Cortex-M7 发布 |
| 2016 | ARMv8-M（Cortex-M23/M33）加入 TrustZone；STM32H7（M7，400 MHz 以上） |
| 2017 | ST 收购 Atollic（TrueSTUDIO），2019 年推出免费的 **STM32CubeIDE**（Eclipse + GCC + CubeMX） |
| 2019 | STM32MP1：Cortex-A7 跑 Linux，加 Cortex-M4 跑实时任务，**MPU 与 MCU 合在一颗芯片上** |
| 2020–22 | 全球缺芯，STM32 价格翻几十倍，GD32、APM32、AT32、CH32（沁恒，包括 RISC-V 的 CH32V）大量替代 |
| 2021–24 | STM32U5（低功耗 M33）、H5、MP2（Cortex-A35）、**STM32N6**（带 NPU，边缘 AI） |

**STM32 驱动库的四个层次**（例子里都有体现）：

```
写法                    代码样子                                    特点
直接寄存器（裸机）      REG(0x4001100C) |= 1<<13                    最快最小，要看手册，不可移植
CMSIS 设备头文件        GPIOC->BSRR = GPIO_BSRR_BS13                有名字的寄存器，仍然是寄存器级
LL（Low Layer）         LL_GPIO_SetOutputPin(GPIOC, LL_GPIO_PIN_13) 内联函数包装，几乎零开销
HAL                     HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, 1)    带状态机、超时、回调，跨系列可移植，体积大
```

[stm32/01_baremetal_blink_uart](examples/stm32/01_baremetal_blink_uart) 从零写出启动代码（向量表、`.data` 搬运、`.bss` 清零）、链接脚本、SysTick 和中断驱动的串口环形缓冲区。**一个 MCU 从上电到 `main()` 之间发生的全部事情**都在这两百行里。[stm32/02_hal_bmp280](examples/stm32/02_hal_bmp280) 则是 HAL 的写法。

### 2.4 树莓派：让 Linux 驱动触手可及（2012–）

| 年份 | 事件 |
|---|---|
| 2009 | Eben Upton 等人在剑桥成立树莓派基金会，初衷是让孩子学编程 |
| **2012.02** | **Raspberry Pi 1 Model B**：BCM2835（ARM1176JZF-S 700 MHz），35 美元。同年 Broadcom 公开《BCM2835 ARM Peripherals》，GPIO、I2C（BSC）、SPI、PWM 寄存器都能查到 |
| 2012–13 | 用户态 GPIO 库涌现：**WiringPi**（仿 Arduino API）、**RPi.GPIO**、pigpio、bcm2835 C 库。它们大多直接 **mmap `/dev/mem` 写寄存器**，快，但绕开了内核 |
| 2014 | Broadcom 公开 VideoCore IV 3D 文档，开源 GPU 驱动（vc4）随后开发 |
| **2015** | Pi 2（BCM2836，Cortex-A7 四核）。树莓派内核 3.18 起**默认启用设备树**，`config.txt` 里的 `dtoverlay=` / `dtparam=` 成了加外设的标准方式。gpiozero 发布 |
| 2016 | Pi 3（BCM2837，64 位 Cortex-A53，WiFi/BT）；Linux 4.8 引入 **GPIO 字符设备**（`/dev/gpiochipN`），sysfs GPIO 开始废弃 |
| 2019 | Pi 4（BCM2711，Cortex-A72，USB3，PCIe）。WiringPi 作者宣布停止维护（后由社区接手） |
| 2020 | 内核 5.10 的 GPIO uAPI v2；Pi 400；CM4 |
| **2021.01** | **Raspberry Pi Pico / RP2040**：基金会自研的 **MCU**（双 Cortex-M0+），带 **PIO**（可编程 I/O 状态机，能用软件"造"出 WS2812、DVI、SDIO 这类外设）。树莓派进入 MCU 领域，和 Arduino、STM32 直接竞争 |
| 2022 | libgpiod 2.0 |
| **2023.10** | **Pi 5**（BCM2712，Cortex-A76）加自研南桥 **RP1**（通过 PCIe 连接）。40 针 GPIO 改由 RP1 提供，**所有直接写 BCM 寄存器的老库（RPi.GPIO、WiringPi 老版、pigpio）都失效了**，官方转向 libgpiod、lgpio、gpiozero。同期 Pi OS Bookworm 把 `/boot` 改到 `/boot/firmware` |
| 2024 | Pi 基金会的商业公司在伦敦上市；**RP2350 / Pico 2**（Cortex-M33 与 RISC-V Hazard3 双架构可选，带安全启动）；CM5；Pi 500 |

**树莓派对驱动开发的意义：** 它是很多人**第一次接触 Linux 设备驱动**的平台。一块 35 美元的板子加上公开的外设手册、设备树 overlay 机制、庞大的社区，内核驱动开发不再只是 SoC 厂商和嵌入式公司的专业活。

Pi 5 的变动是一堂很好的反面教材（[gpio_mmap.c](examples/raspberrypi/01_userspace_gpio/gpio_mmap.c)）：**绕过内核直接写寄存器，快，但硬件一换就全部失效；走内核标准接口（libgpiod）慢一点，但十年都能用。**

### 2.5 Linux 驱动模型的演进（树莓派驱动开发的背景）

| 年份 / 内核 | 变化 |
|---|---|
| 1990s | 字符设备和块设备，`register_chrdev` 加主次设备号；i2c-dev 随 lm_sensors 出现 |
| 2003 / 2.6 | **统一设备模型**（bus / device / driver / class）加 **sysfs**；udev 在用户态动态创建 `/dev` 节点 |
| 2005 / 2.6.11 | **usbmon**：内核自带的 USB 抓包点，USB 逆向的基础设施 |
| 2007 / 2.6.22–23 | spidev（用户态 SPI）、**UIO**（用户态 I/O：内核只做中断和映射，驱动逻辑在用户态） |
| 2011 | Linus 公开抱怨 ARM 平台的板级代码混乱（每块板一个 `board-xxx.c`），ARM 全面转向**设备树（Device Tree）**；**regmap** 抽象寄存器访问 |
| 2012 / 3.5 | **IIO**（Industrial I/O）从 staging 转正，ADC、IMU、气压计等传感器有了统一框架 |
| 2014–16 | devm_*（设备资源自动释放）普及；4.8 引入 GPIO 字符设备 |
| 2020 / 5.10 | GPIO uAPI v2 |
| 2022 / 6.1 | **Rust for Linux** 合入主线，此后逐步出现 Rust 写的驱动（NVMe、网络 PHY、Apple AGX GPU 等） |
| 2023 / 6.3 | `i2c_driver.probe` 统一为单参数形式（[例子 05](examples/raspberrypi/05_i2c_hwmon_driver/bmp280_hwmon.c)） |

### 2.6 总时间线

```
1980 8051 ─ 1993 PIC16C84 ─ 1996 AVR ─────────────── 2005 Arduino ─ 2010 Uno ─ 2012 Due(ARM) ─ 2023 R4 ─ 2025 UNO Q
                                                          │
2004 Cortex-M3 ─ 2007 STM32F1+SPL ─ 2008 CMSIS ─ 2011 F4 ─ 2014 Cube/HAL ─ 2019 CubeIDE/MP1 ─ 2021 缺芯/国产替代 ─ 2024 N6
                                                          │
2003 Linux设备模型 ─ 2011 ARM设备树 ─ 2012 Pi 1 ─ 2015 Pi上默认DT ─ 2016 gpiochip ─ 2019 Pi 4 ─ 2021 Pico ─ 2023 Pi 5/RP1 ─ 2024 RP2350
                                                          │
逆向：2005 nouveau/usbmon ─ 2008 Bus Pirate/Saleae ─ 2010 Kinect/sigrok ─ 2012 rtl-sdr ─ 2019 Ghidra开源 ─ 2021 Asahi Linux
```

**大趋势：**

1. **抽象层越来越厚。** 寄存器、厂商库、HAL，再到 Arduino API 和 RTOS 设备模型（Zephyr、RT-Thread），最后是 MicroPython/CircuitPython。
2. **MCU 和 MPU 的边界模糊。** STM32MP1、Pi 5 的 RP1、UNO Q 都是"Linux 加实时 MCU"的组合。Pico 让树莓派做 MCU，Portenta 和 UNO Q 让 Arduino 带上 Linux。
3. **标准接口胜过私有捷径。** sysfs GPIO 和 `/dev/mem` 让位给 gpiochip 和 libgpiod；板级 C 文件让位给设备树；私有传感器驱动让位给 IIO/hwmon。
4. **开源逆向推动硬件开放。** 很多今天"官方支持"的硬件，最初的 Linux 驱动都是逆向出来的（第 6.8 节）。

---

## 3. 三个平台横向对比

| | Arduino（Uno） | STM32（F103 / F4） | 树莓派（4 / 5） |
|---|---|---|---|
| 核心 | 8 位 AVR 16 MHz | 32 位 Cortex-M 72–480 MHz | 64 位 Cortex-A 1.5–2.4 GHz |
| 内存 | 2 KB RAM / 32 KB Flash | 20 KB–1 MB RAM | 1–16 GB DDR |
| 系统 | 无（`setup/loop`） | 裸机 / FreeRTOS / Zephyr / RT-Thread | Linux |
| 实时性 | 确定（µs 级） | 确定（ns–µs 级，中断优先级可控） | 不确定（ms 级抖动；PREEMPT_RT 能改善） |
| 驱动写在哪 | sketch 或库里 | 工程里（HAL/LL/寄存器） | 内核模块或用户态程序 |
| 硬件描述 | 写死在代码 | CubeMX `.ioc` 生成的初始化代码 | 设备树（`.dts` / overlay） |
| 调试 | `Serial.print`（新板有 SWD） | **SWD + GDB**，断点、单步、看外设寄存器 | printk/dmesg、ftrace、gdb（用户态）、kgdb |
| 典型驱动工作 | 用库，偶尔写寄存器提速 | 外设初始化、DMA、中断、低功耗 | 设备树、内核驱动、用户态驱动、子系统对接 |
| 崩溃代价 | 复位 | HardFault，复位 | 用户态：进程崩溃；内核态：**整机 oops/panic** |
| 适合 | 原型、教学、简单控制 | 产品、电机控制、实时、低功耗 | 网络、图像、AI、复杂应用、网关 |

实际项目经常**组合使用**：树莓派做大脑，跑 Linux、网络、界面；STM32 或 Arduino 通过 UART/I2C/SPI/USB 挂在它下面，负责实时控制。ASIAIR 盒子（第 7 节）就是一个 Linux 主机管着多台 USB 设备。

---

## 4. 驱动开发分层详解（配例子）

### 4.1 第 0 层：读数据手册、写寄存器

无论哪个平台，最底层都一样：**外设是一组映射到地址空间的寄存器，驱动按手册规定的顺序读写它们。**

| 平台 | 例子 | 关键点 |
|---|---|---|
| AVR | [01_blink_register.ino](examples/arduino/01_blink_register/01_blink_register.ino) | I/O 寄存器有专用指令（`SBI/CBI/OUT`）；往 `PINx` 写 1 能翻转引脚 |
| STM32 | [main.c](examples/stm32/01_baremetal_blink_uart/main.c) | **先开时钟**（`RCC_APB2ENR`），否则写外设寄存器无效；`BSRR` 原子置位/复位，不用读-改-写 |
| BCM2711 | [gpio_mmap.c](examples/raspberrypi/01_userspace_gpio/gpio_mmap.c) | 用户态通过 `/dev/gpiomem` mmap 同一组寄存器，效果和 MCU 一样，但绕过了内核 |

寄存器驱动的必备常识：

* **`volatile`**：告诉编译器每次都要真的去读写这个地址，不能缓存、不能合并、不能删掉。
* **读-改-写的竞态**：`REG |= bit` 是三条指令，中间被中断打断、中断里也改了同一个寄存器，就会丢更新。解决办法是用原子寄存器（STM32 `BSRR`、BCM `GPSET/GPCLR`）、位带（Cortex-M3/M4），或者临时关中断。
* **"写 1 清零"（W1C）、"读即清除"（RC）**：状态寄存器常见，dump 寄存器时可能改变芯片状态（[i2c_regdump.py](examples/reverse_engineering/tools/i2c_regdump.py) 的提示）。
* **时钟、复位、引脚复用**：外设能用要同时满足三个条件：时钟开了、复位释放了、引脚切到了该外设功能。STM32 的 CubeMX 和 Linux 的设备树（`pinctrl`、`clocks`、`resets` 属性）都在处理这件事。

### 4.2 第 1 层：启动代码与中断

MCU 没有操作系统，**从上电到 `main()` 的路要自己铺好**（[startup.c](examples/stm32/01_baremetal_blink_uart/startup.c)、[linker.ld](examples/stm32/01_baremetal_blink_uart/linker.ld)）：

```
上电 → 硬件从 0x08000000 读 [0]=初始 SP、[1]=Reset_Handler
     → Reset_Handler: 把 .data 初值从 Flash 拷到 RAM，把 .bss 清零
     → （可选）SystemInit：配置 PLL、Flash 等待周期
     → main()
中断 → NVIC 按向量表下标跳转（USART1 = IRQ37 → 下标 53）
     → Cortex-M 硬件自动压栈 r0–r3、r12、lr、pc、xPSR，所以中断函数就是普通 C 函数
```

中断驱动的设备驱动一般是**中断里只搬数据，主循环处理**。例子里用单生产者单消费者的环形缓冲区，不需要关中断。

### 4.3 第 2 层：厂商库 / HAL

| 平台 | 库 | 例子 |
|---|---|---|
| Arduino | Arduino core（`Wire`、`SPI`、`HardwareSerial`） | [02_bmp280_wire.ino](examples/arduino/02_bmp280_wire/02_bmp280_wire.ino) |
| STM32 | HAL / LL / CMSIS | [bmp280_stm32_hal.c](examples/stm32/02_hal_bmp280/bmp280_stm32_hal.c) |
| 树莓派 | 内核的 I2C/SPI 控制器驱动 + i2c-dev/spidev | [bmp280_linux.c](examples/raspberrypi/02_i2c_dev_bmp280/bmp280_linux.c) |

三个端口在总线上产生的波形**完全一样**：`START, addr+W, reg, repeated START, addr+R, data..., STOP`。只是 API 名字不同：

```c
/* Arduino */ Wire.beginTransmission(a); Wire.write(reg); Wire.endTransmission(false); Wire.requestFrom(a, n);
/* STM32   */ HAL_I2C_Mem_Read(&hi2c1, a << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, n, 100);
/* Linux   */ struct i2c_msg m[2] = {{a, 0, 1, &reg}, {a, I2C_M_RD, n, buf}}; ioctl(fd, I2C_RDWR, ...);
```

### 4.4 第 3 层：平台无关的设备驱动

[portable_driver/bmp280](examples/portable_driver/bmp280) 是本仓库的核心例子。驱动只依赖一个"总线操作"结构体：

```c
struct bmp280_bus {
    int  (*read)(void *ctx, uint8_t reg, uint8_t *buf, size_t len);
    int  (*write)(void *ctx, uint8_t reg, uint8_t val);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
};
```

* 同一份 `bmp280.c` 在 Arduino、STM32、Linux 上**一行不改**。
* 可以接一个**假总线在电脑上做单元测试**（[test_host.c](examples/portable_driver/bmp280/test_host.c)），用数据手册的示例校准值验证补偿公式，不用等硬件到货：

  ```
  $ make test
  T = 25.08 C, P = 100653.25 Pa
  all tests passed
  ```

* Bosch 官方的 BMP2-Sensor-API、Linux 的 **regmap**、Zephyr 的设备 API 都是这个思路：**把"芯片逻辑"和"总线访问"分开。**

### 4.5 第 4 层：Linux 用户态驱动

内核只提供通用通道，芯片协议在用户进程里实现：

| 接口 | 设备节点 | 用途 | 例子 |
|---|---|---|---|
| GPIO 字符设备 | `/dev/gpiochipN` | GPIO 读写、边沿事件 | [gpio_libgpiod.c](examples/raspberrypi/01_userspace_gpio/gpio_libgpiod.c)、[gpio_gpiozero.py](examples/raspberrypi/01_userspace_gpio/gpio_gpiozero.py) |
| i2c-dev | `/dev/i2c-N` | I2C 传输 | [bmp280_linux.c](examples/raspberrypi/02_i2c_dev_bmp280/bmp280_linux.c) |
| spidev | `/dev/spidevB.C` | SPI 全双工传输 | （结构同 i2c-dev，`SPI_IOC_MESSAGE`） |
| tty | `/dev/ttyUSB0`、`/dev/ttyAMA0` | 串口设备 | [serial_probe.py](examples/reverse_engineering/tools/serial_probe.py) |
| libusb（usbfs） | `/dev/bus/usb/BBB/DDD` | USB 设备 | [usb_probe.c](examples/usb/libusb_probe/usb_probe.c) |
| UIO / VFIO | `/dev/uioN` | 寄存器映射加中断，FPGA、工业卡 | — |
| `/dev/mem`、`/dev/gpiomem` | — | 直接映射物理地址（不推荐） | [gpio_mmap.c](examples/raspberrypi/01_userspace_gpio/gpio_mmap.c) |

**什么时候用户态就够了：** 原型开发；低速设备；闭源 SDK（QHY、ZWO 的相机 SDK 全是 libusb 用户态）；不需要被其他程序以标准方式使用。
**什么时候该进内核：** 需要中断或低延迟；需要 DMA；要接入标准子系统（网卡、声卡、输入设备、hwmon/IIO、块设备）；开机就要可用（比如根文件系统所在的存储）。

### 4.6 第 5 层：Linux 内核驱动

三个递进的例子：

**(a) 最小字符设备** [hello_misc.c](examples/raspberrypi/03_kernel_misc_chardev/hello_misc.c)：`module_init`、`file_operations`、`copy_to_user`、mutex。

```bash
make && sudo insmod hello_misc.ko
echo "hi kernel" | sudo tee /dev/hello ; sudo cat /dev/hello ; dmesg | tail -2
```

**(b) platform 驱动 + 设备树** [demo_gpio_led.c](examples/raspberrypi/04_platform_driver_dt/demo_gpio_led.c) 和 [demo-led-overlay.dts](examples/raspberrypi/04_platform_driver_dt/demo-led-overlay.dts)：

```
设备树 overlay                       驱动
demo-led {                           of_device_id: { .compatible = "demo,gpio-led" }
  compatible = "demo,gpio-led"; ───► probe(): devm_gpiod_get(dev, "led", ...)
  led-gpios = <&gpio 17 0>;   ─────────────────────┘   ("led" + "-gpios")
}                                    dev_groups → /sys/devices/platform/demo-led/{state,blink_ms}
```

```bash
dtc -@ -I dts -O dtb -o demo-led.dtbo demo-led-overlay.dts
sudo dtoverlay demo-led.dtbo            # 运行时加载 overlay
make && sudo insmod demo_gpio_led.ko    # compatible 匹配 → probe()
echo 200 | sudo tee /sys/devices/platform/demo-led/blink_ms
```

这里用到的都是现代内核驱动的惯用法：`devm_*` 资源自动释放、`dev_err_probe`、`sysfs_emit`、`dev_groups`、`MODULE_DEVICE_TABLE`（让 udev 按 compatible 自动加载模块）。

**(c) I2C 客户端驱动 + hwmon** [bmp280_hwmon.c](examples/raspberrypi/05_i2c_hwmon_driver/bmp280_hwmon.c)：同一颗 BMP280，这次温度出现在 `/sys/class/hwmon/hwmonN/temp1_input`，`sensors` 命令直接能读，**不需要任何人知道 BMP280 的寄存器**。这就是"驱动子系统"的价值：把设备特有的协议翻译成**所有程序都懂的标准接口**。

```bash
echo demo_bmp280 0x76 | sudo tee /sys/bus/i2c/devices/i2c-1/new_device
sudo insmod bmp280_hwmon.ko && sensors
```

### 4.7 RTOS 层（STM32 的另一条路）

MCU 上功能多了（网络、文件系统、多个传感器、低功耗管理），裸机的大循环就不够用了：

* **FreeRTOS**（2003，2017 年起由 AWS 维护）：只有调度器和同步原语，驱动仍用 HAL 自己写。
* **Zephyr**（Linux 基金会，2016）：**借鉴 Linux**，有设备树、Kconfig、统一的设备驱动 API（`sensor_sample_fetch`、`i2c_write_read`），同一个传感器驱动能在 STM32、nRF、ESP32、RP2040 上用。
* **RT-Thread**（国产，2006）：设备框架加软件包生态。
* **Arduino core** 本身也可以跑在 Mbed OS（Nano 33 BLE、Portenta）或 FreeRTOS（ESP32）上。

学到这里会发现，**Linux 的设备模型、Zephyr 的设备模型、平台无关驱动的 bus ops 是同一个思想在不同规模上的实现。**

---

## 5. 常用总线速查与坑

| 总线 | 线 | 速率 | 常见坑 |
|---|---|---|---|
| GPIO | 1 | — | 电平：3.3 V 的 STM32、Pi 接 5 V 信号会烧（部分 STM32 引脚 FT 容忍 5 V）；上拉/下拉；按键要去抖 |
| UART | TX/RX（+RTS/CTS） | 9600–数 Mbps | TX 要接 RX；波特率误差超过约 2% 就乱码；**打开 USB 串口时 DTR/RTS 翻转可能复位设备**（Arduino 自动下载和 QHY CFW3 复位都是这个机制） |
| I2C | SDA/SCL | 100 k / 400 k / 1 M | 必须有**上拉电阻**；7 位地址和 8 位地址的混淆（HAL 要 `addr << 1`）；设备卡死时 SDA 被一直拉低，要发 9 个 SCL 时钟来恢复；树莓派 BCM2835 的 I2C **不支持时钟延展**（clock stretching） |
| SPI | SCK/MOSI/MISO/CS | 1–100 MHz | 四种模式（CPOL/CPHA）要对上；CS 时序；全双工，读的同时一定在写 |
| USB | D+/D-（+SS） | 1.5 M–10 G | 枚举、描述符、端点类型；Linux 下权限要写 udev 规则；**固件下载型设备**（FX2/FX3）插上后要先上传固件再重新枚举 |
| CAN | CANH/CANL | 125 k–1 M（FD 5 M+） | 两端 120 Ω 终端电阻；Linux 下用 SocketCAN（`can0` 网络接口） |
| 1-Wire | DQ | 16 kbps | 时序严格；Linux 下有 `w1-gpio` overlay |

**调试总线的第一件工具是逻辑分析仪**（几十元的 FX2 克隆加 sigrok/PulseView 就能用），它能让你看到线上实际发生了什么，而不是代码"以为"发生了什么。

---

## 6. 驱动逆向工程

逆向的对象：**没有文档、文档不全，或者只有闭源驱动的硬件**。目标通常是**互操作**：让设备在不支持的系统上工作，比如 Linux 驱动、新平台移植、替换停止维护的 SDK，或者修复厂商不修的 bug。

### 6.1 法律与伦理（先读）

* 多数司法辖区为**互操作性**留有逆向空间：美国 DMCA §1201(f)，欧盟软件指令 2009/24/EC 第 6 条（为互操作而反编译）。中国《计算机软件保护条例》第十七条允许"为了学习和研究软件内含的设计思想和原理"而使用软件；最高法关于不正当竞争案件的司法解释把"通过反向工程获得"商业秘密排除在侵权之外。
* 仍需注意：许可协议（EULA）里的禁止条款、规避加密或技术保护措施的限制、**不要分发厂商的二进制和固件**（hezi-hack 通过 `fetch_sdk.sh` 从官方渠道下载，仓库里不放 SDK）、不要用于绕过付费功能或攻击他人设备。
* 只在**自己拥有的设备**上实验。随意发送厂商命令可能改写 EEPROM 或固件，把设备变砖。
* 本节不是法律意见。涉及商业产品时请咨询律师。

### 6.2 方法论

```
      ┌───────────────── 1. 侦察（不碰设备状态）─────────────────┐
      │ 看板子/芯片丝印 · lsusb/dmesg · 设备树 · readelf/nm/strings │
      └──────────────────────────┬─────────────────────────────────┘
                                 ▼
      ┌─────────────── 2. 观察（让原厂软件跑，旁路记录）────────────┐
      │ usbmon/Wireshark · LD_PRELOAD 钩子 · 逻辑分析仪 · 串口监听  │
      │ strace/ltrace · 厂商程序自己的日志                          │
      └──────────────────────────┬─────────────────────────────────┘
                                 ▼
      ┌─────────────── 3. 假设（读反汇编/抓包，猜协议）─────────────┐
      │ Ghidra/objdump 看调用链 · 对比多次抓包找变化字段             │
      └──────────────────────────┬─────────────────────────────────┘
                                 ▼
      ┌─────────────── 4. 验证（最小实验，一次只改一个变量）─────────┐
      │ 重放抓到的请求 · 改一个参数看反应 · 计时 · 对照实验           │
      └──────────────────────────┬─────────────────────────────────┘
                                 ▼
      ┌─────────────── 5. 实现（驱动 / shim / 兼容层）──────────────┐
      │ 先做命令行工具，再接入目标系统 · 保留回退路径 · 写下证据     │
      └────────────────────────────────────────────────────────────┘
```

**铁律：先只读，后写入；先重放，后创造；每个结论都要有日志或抓包作证据。** hezi-hack 的 README 在每个结论旁边写明了"获取方式"和"证据"列，值得学习。

### 6.3 硬件层

| 目标 | 方法 / 工具 |
|---|---|
| 认芯片 | 看丝印查数据手册；看 PCB 走线判断哪些引脚接到了哪里；USB 设备看 VID:PID（[usb.ids](http://www.linux-usb.org/usb.ids)）|
| 找调试串口 | 板上成排的 3–4 个焊盘：GND、TX、RX（、VCC）。用万用表或逻辑分析仪找持续跳变的 TX，`serial_probe.py baudscan` 猜波特率。很多 Linux 设备的 U-Boot 和内核日志、甚至 root shell 都在这个口上 |
| 找 SWD/JTAG | STM32 的 SWDIO（PA13）、SWCLK（PA14）；用 ST-Link 或 J-Link 加 OpenOCD 连接。能连上就能读 Flash、下断点 |
| 读固件 | SWD 直读（没开读保护时）；外置 SPI Flash 用 CH341A 编程器夹读；Linux 设备从 `/dev/mtd*`、升级包里提取；`binwalk` 解包 |
| 读保护 | STM32 RDP：Level 0 不保护；Level 1 禁止调试器读 Flash，降回 Level 0 会整片擦除；Level 2 永久禁用调试口。学术界有针对部分系列 RDP1 的研究（如 Obermaier 等人关于 STM32 固件保护的论文），属于安全研究范畴，本仓库不展开 |
| 总线嗅探 | 逻辑分析仪加 sigrok/PulseView 协议解码（I2C/SPI/UART/1-Wire/CAN/USB 低速）；Bus Pirate；USB 高速需要专用分析仪，或者改用软件抓包 |

### 6.4 协议层：USB

USB 是外设逆向中最常见的对象：相机、SDR、示波器、烧录器、游戏外设。

1. **侦察**：`lsusb -v` 和 [usb_probe.c](examples/usb/libusb_probe/usb_probe.c) 看描述符。厂商类（class `ff`）意味着私有协议；VID/PID 插上后变了，说明设备需要主机下载固件（Cypress FX2/FX3 的 `04b4:xxxx` 或厂商自定义 bootloader PID）。
2. **抓包**，三种手段：
   * **usbmon**（内核）加 Wireshark，或者在无界面的板子上抓文本，用 [usbmon_parse.py](examples/reverse_engineering/tools/usbmon_parse.py) 整理：
     ```
     sudo cat /sys/kernel/debug/usb/usbmon/1u > cap.txt
     usbmon_parse.py cap.txt --dev 7 --ctrl --unique
       0.000000 dev7  CTRL IN  type=c0 req=12 val=0000 idx=0000 len=4   st=0  0.32ms  01 02 03 04
       0.000847 dev7  CTRL OUT type=40 req=b5 val=0010 idx=0000 len=2   st=0  0.30ms  ab cd
     ```
   * **LD_PRELOAD 钩子** [usbspy.c](examples/reverse_engineering/usb_spy_preload/usbspy.c)：拦截 `libusb_control_transfer` 和 `libusb_bulk_transfer`，还能记录**是 SDK 里哪个函数发的**，方便把 USB 请求和 SDK API 对应起来。注意它只能看到同步 API；用 `libusb_submit_transfer` 异步读图像的 SDK 需要再钩那个函数，或者直接用 usbmon。
   * Windows 上用 USBPcap 加 Wireshark 抓厂商官方软件。**厂商的 Windows 软件通常功能最全，是最好的"协议老师"。**
3. **找规律**：一次只改一个参数（比如增益从 10 改到 11），对比前后两次抓包，找出变化的字段。
4. **重放**：`usb_probe VID:PID ctl-in 0x12 0 0 4`，先只重放读请求。
5. **固件下载型设备**：把抓到的"下载固件"流量和固件文件格式对上。hezi-hack 的 [fx3load.c](../hezi-hack/fx3load/fx3load.c) 就是按 Cypress 公开的 FX3 格式（`CY` 头、分段地址、校验和，厂商请求 `0xA0`）实现的，由 udev 在设备插入时自动调用。

### 6.5 协议层：串口、I2C、SPI

* **串口**：先 `listen`，很多设备上电或打开端口时会主动发数据（版本号、复位完成标志、NMEA 语句）；再 `baudscan`；然后用 `send` 试探常见命令格式（ASCII 命令、`AT`、Modbus、`0x55 0xAA` 帧头）。[serial_probe.py](examples/reverse_engineering/tools/serial_probe.py) 会给每条命令计时，**时间本身就是信息**：滤镜轮转一格要 0.9 s，转到对侧要 5 s，由此能判断"回显发生在到位之后"。
* **I2C**：`i2c_regdump.py scan`，然后 `dump` 全表，再用 `diff` 找变化的寄存器（ADC 结果），常量区里找 ID 寄存器和校准数据。对比同一家公司的相似芯片的手册，往往能猜对大部分寄存器。
* **SPI Flash、EEPROM**：通常遵守 JEDEC 标准命令（`0x9F` 读 ID、`0x03` 读数据），`flashrom` 直接支持。

### 6.6 二进制层：闭源驱动与 SDK

| 手段 | 工具 | 用途 |
|---|---|---|
| 静态侦察 | [elf_survey.sh](examples/reverse_engineering/tools/elf_survey.sh)（readelf、nm、objdump、strings、c++filt） | ABI、依赖、符号版本、导入导出、是否 strip |
| 反汇编/反编译 | objdump -d、**Ghidra**（NSA，2019 开源）、IDA、radare2/Cutter、Binary Ninja | 看调用链、常量比较（比如 `cmp r2, #0x3c3` 就是在检查 VID） |
| 调用追踪 | `strace`（系统调用）、`ltrace`（库调用）、`LD_DEBUG=bindings`（符号绑定到哪个库） | 看程序实际打开了什么、调用了什么 |
| 动态插桩 | gdb 断点、**LD_PRELOAD**、Frida | 在运行中改参数、记录调用 |
| 内核驱动 | `modinfo`（alias 表列出支持的设备）、`/sys/module/*/parameters`、ftrace、kprobes | 闭源 `.ko` 的行为 |

**没有 strip 的二进制是逆向者的礼物**：hezi-hack 里 `zwoair_imager` 保留了 `img_cam_zwo::ScanZWOCam`、`imager_hotplug_callback` 这样的函数名，节省了大量分析时间。

### 6.7 兼容层技术：不改原程序，让它支持新硬件

当目标程序是闭源的（比如 ASIAIR 的 `zwoair_imager`），最有力的手段是**替换它依赖的动态库**。[shim_interpose](examples/reverse_engineering/shim_interpose) 把 hezi-hack 用到的技术缩小成一个可运行的演示：

```
app（闭源，只认 VID 0x1234） ──► libvendor.so（shim）
                                    ├─ 下标 0：外来设备 ─► 自己实现（真实场景：调用 QHY SDK）
                                    └─ 下标 1..：原厂设备 ─► libvendor_orig.so（dlopen RTLD_DEEPBIND）
```

| 技术 | 解决的问题 | 演示文件 |
|---|---|---|
| **同名替换**，导出符号完全一致 | 程序按 SONAME 加载库，换掉文件就接管了所有调用 | `make demo` 里用 `nm -D` 做 diff 检查 |
| **dlopen + RTLD_DEEPBIND** 加载原库 | 原库内部调用自己的导出函数时，不要绕回 shim（shim 的编号体系不同） | `SHIM_NO_DEEPBIND=1` 对照：原库会打印错误的设备名 |
| **按调用者撒谎**：`dladdr1(__builtin_return_address(0))` | 只对应用本体伪装 VID，其他库看到的仍是真实值 | `dev_get_vid()` |
| **汇编跳板**转发原型未知的函数 | 头文件里没有的内部函数，不知道参数也能原样转发 | [trampolines.S](examples/reverse_engineering/shim_interpose/trampolines.S)（x86-64 / AArch64 / ARM32） |
| **符号版本改写**（DT_VERNEED） | SDK 是用新 glibc 编的，目标系统的 glibc 太旧 | hezi-hack [tools/patch_verneed.py](../hezi-hack/tools/patch_verneed.py) |
| **补缺失符号** | 旧 libstdc++ 缺少某个构造函数 | hezi-hack [compat/qhy_glibc_compat.cpp](../hezi-hack/compat/qhy_glibc_compat.cpp) |
| **LD_PRELOAD 拦截** | 在不替换文件的情况下观察或修改调用 | [usbspy.c](examples/reverse_engineering/usb_spy_preload/usbspy.c) |

预期输出（按代码逐行推导；本仓库在 macOS 上编写，这个演示需要 Linux 才能运行）：

```
$ make demo            # Linux 上
===== 原厂：app + 原库 =====
[app] 2 device(s)
[app] #0 VendorCam A    capture=4 first byte 0x10
...
===== 装上 shim（DEEPBIND）=====
[shim] reporting foreign device #0 as VID 1234 to the app
[app] #0 OtherBrand X1  capture=4 first byte 0xee
[vendor] capturing on VendorCam A
[app] #1 VendorCam A    capture=4 first byte 0x10
...
===== 不用 DEEPBIND：原库内部的 dev_count() 被 shim 截走 =====
[vendor] capturing on OtherBrand X1      ← 原库拿自己的下标 0 问 dev_name()，被 shim 按 shim 的编号回答了
```

### 6.8 开源逆向驱动简史

| 年份 | 项目 | 逆向对象 | 结果 |
|---|---|---|---|
| 1990s– | lm_sensors | 主板上的各种硬件监控芯片 | 今天的 hwmon 子系统 |
| 2004– | gspca | 大量廉价 USB 摄像头 | 进入主线，之后并入 V4L2 |
| 2005– | **nouveau** | NVIDIA GPU（通过 MMIO 跟踪 `mmiotrace` 抓闭源驱动的寄存器访问） | 2.6.33 进主线；多年后 NVIDIA 开源了内核模块 |
| 2005 | OpenOCD | 各种 JTAG 适配器和片上调试模块 | 开源嵌入式调试的事实标准 |
| 2008 | Bus Pirate、Saleae Logic | — | 平价的总线嗅探和逻辑分析 |
| 2010 | **libfreenect** | 微软 Kinect（Adafruit 悬赏） | 发布几天内被破解，催生了一批开源深度相机应用 |
| 2010 | **sigrok** | 各家逻辑分析仪、示波器、万用表的私有协议 | 统一的开源测量软件 |
| 2011 | stlink（texane） | ST-Link 调试器协议 | Linux、macOS 上的 STM32 烧录 |
| 2012 | **rtl-sdr** | 电视棒 RTL2832U 的调试模式（能输出原始 IQ 数据） | 20 美元的软件无线电，带动了业余 SDR 的爆发 |
| 2012– | Freedreno、Lima、Panfrost、etnaviv | 高通 Adreno、ARM Mali、Vivante 这些移动 GPU | 陆续进入主线 Mesa 和内核 |
| 2021– | **Asahi Linux** | Apple M1/M2（GPU、DCP、音频……） | 大量驱动进入主线，其中 GPU 驱动用 Rust 编写 |
| 2026 | hezi-hack（第 7 节） | ZWO ASIAIR 的相机和滤镜轮接口 | 原厂盒子支持 QHY 相机和滤镜轮 |

---

## 7. 案例：hezi-hack，给 ASIAIR 加 QHY 驱动

> 完整文档见 [`../hezi-hack/README.md`](../hezi-hack/README.md)（本机路径 `/Users/xlisp/PyPro/hezi-hack`）。这里按第 6 节的方法论把它重新梳理一遍。

### 7.1 问题

ZWO ASIAIR Mini 是一台天文摄影控制盒（Rockchip RV1126，ARMv7 armhf，Raspbian 10，glibc 2.28），原厂只支持 ZWO 的相机和滤镜轮。目标是让它在**原厂 App 和原厂程序**里直接使用：

| 设备 | USB | 角色 |
|---|---|---|
| QHY183M | `1618:c183` → 下载固件后变成 `1618:c184` | 主相机 |
| QHY5III715C | `1618:0715` → `0716` | 导星相机 |
| QHY CFW3 滤镜轮 | `10c4:ea60`（CP2102 USB 串口） | 滤镜轮 |

### 7.2 侦察

* `/proc/device-tree/model`、`ldd --version`、`lsusb -t`、`df`：确认硬件、glibc 版本、**只有 USB 2.0**、可写分区。
* `ldd zwoair_imager | grep ASI`：相机 SDK `libASICamera2.so` 是**动态链接**的，从 `LD_LIBRARY_PATH` 加载，因此**可以被替换**。
* `nm -D --undefined-only`：imager 只用到 27 个 `ASI*` 函数，shim 只需实现这个子集的语义（但要导出全部 54 个符号）。
* 二进制**没有 strip**，函数名都在。
* App 与 imager 之间是**行分隔的 JSON-RPC over TCP**（端口 4700）。不用手机也能完整测试（`scripts/re/asiair_rpc.py`）。

### 7.3 三个障碍与对应技术

| 障碍 | 现象 | 解决 | 对应本仓库 |
|---|---|---|---|
| ① SDK 太新 | QHY SDK 需要 `GLIBC_2.29`、`GLIBCXX_3.4.26`，盒子上只有 2.28 和 3.4.25 | 分析发现真正缺的只有 4 个符号：`log/exp/pow` 改写 DT_VERNEED 指向旧版本；缺失的 `stringstream` 构造函数用一个小的兼容库补上 | 6.7 节"符号版本改写" |
| ② 固件下载 | 相机插上后是 `Cypress WestBridge` bootloader，QHY SDK 在 Linux 上不负责下载固件 | 自己写 `fx3load`（libusb、厂商请求 `0xA0`、分块重试），udev 规则在插入时自动调用 | 6.4 节第 5 条；[usb_probe.c](examples/usb/libusb_probe/usb_probe.c) |
| ③ 接入闭源 imager | imager 只认 ZWO 相机 | 用同名替换库 `libASICamera2.so` 把 ASI API 翻译成 QHY SDK 调用，原厂库改名后用 DEEPBIND 加载，ZWO 相机照常可用 | [shim_interpose](examples/reverse_engineering/shim_interpose) |

### 7.4 逆向中的三个 bug（反汇编 + 日志定位）

1. **imager 看不到 QHY 相机。** 反汇编 `imager_hotplug_callback`，发现 `cmp r2, #0x3c3`：只有 VID 等于 ZWO 的设备才会触发扫描。解决办法是 shim 拦截 `libusb_get_device_descriptor`，**并且只对 imager 可执行文件本身**把 1618 报成 03c3。libusb、QHY SDK、gphoto 看到的仍是真实 VID。用 `LD_DEBUG=bindings` 验证 shim 在符号搜索顺序中排在 libusb 前面。
2. **imager 崩溃循环。** 反汇编 `ScanZWOCam`，发现调用 `ASIGetCameraLocalPath` 之后会 `operator delete[]` 返回的指针，也就是说**调用方负责释放**。shim 原来返回的是静态缓冲区，改成 `new char[]` 后解决。这类"内存所有权"约定头文件里不会写，只能从调用方的汇编里读出来。
   shim 还加了**崩溃循环保护**：90 秒内重启 3 次，就自动退回纯转发模式，保证盒子原有功能不受影响。
3. **构造函数里段错误。** `__attribute__((constructor))` 比同一编译单元里的 C++ 全局对象构造得更早，改成函数内静态变量后解决。

### 7.5 滤镜轮：实验驱动的设计

CFW3 走 CP2102 串口，协议参考 INDI 的 `qhycfw3.cpp`：9600 8N1，发 `VRS`、`MXP`、`NOW` 查询，发 `'0'..'6'` 换孔，到位后回显同一个字符。

写驱动之前先做实验（`cfw_probe.py`、`cfw_reset_test.py`，泛化后就是本仓库的 [serial_probe.py](examples/reverse_engineering/tools/serial_probe.py)），得到一个关键发现：**打开串口会让轮子复位**，15 秒后才发出第一个字节；复位过程中发命令会把轮子搞乱。

这个发现直接决定了架构：

```
udev ─► qhycfw@ttyUSB0.service（长期持有串口，TIOCEXCL 独占）─► /tmp/qhycfw/ttyUSB0.sock
zwoair_imager ─► libEFWFilter.so（shim）─┬─ ZWO EFW ─► 原厂库（DEEPBIND）
                                          └─ QHY ─► socket: STATUS / GOTO n / CAL
```

* 不能让 imager 每次需要时才打开串口，否则 imager 重启就会让轮子在拍摄途中复位。
* 串口用 `TIOCEXCL` 独占：同一台盒子上赤道仪驱动也配置成 `/dev/ttyUSB0`，而赤道仪命令里含有数字，轮子会把它当成换孔命令执行。
* 探测失败就写 `.none` 并释放端口：这个 CP2102 可能其实是赤道仪线。

### 7.6 可以带走的经验

1. **先证明能替换，再写代码**：`ldd`、`nm -D` 确认动态链接和符号集合。
2. **找到"门卫"**：闭源程序里决定是否接纳设备的那一两条比较指令（VID 检查、`EFWCheck`），用最小的伪装通过它，其余一律保持真实。
3. **只对需要的调用者撒谎**，用调用者地址区分。
4. **原库照常工作**：DEEPBIND 加纯转发，外加崩溃自动回退。
5. **所有结论写下证据**：日志、抓包、反汇编地址。
6. **实验先于设计**：串口复位的对照实验决定了守护进程架构。

---

## 8. 工具链与调试

| 平台 | 编译 | 下载/烧录 | 调试 |
|---|---|---|---|
| Arduino | avr-gcc（IDE 自带）、`arduino-cli`、PlatformIO | USB bootloader（avrdude）、ISP | `Serial.print`；UNO R4 / Nano 33 等有 SWD |
| STM32 | `arm-none-eabi-gcc`、CubeIDE、Keil MDK、IAR、PlatformIO | ST-Link（`st-flash`、STM32CubeProgrammer）、OpenOCD、J-Link、USB DFU、串口 ISP（BOOT0） | **OpenOCD + GDB**，SWO/ITM 打印，SEGGER RTT，CubeIDE 的 SFR 视图 |
| 树莓派 | 板上直接 `gcc`；交叉编译 `aarch64-linux-gnu-gcc`；内核模块要装 `linux-headers-rpi-*` | `scp` / NFS；内核和设备树放在 `/boot/firmware` | `dmesg -w`、`/sys`、`/proc/device-tree`、`dtc -I fs /proc/device-tree`（反编译运行中的设备树）、ftrace、`gpioinfo`、`i2cdetect` |

硬件调试必备：**万用表、逻辑分析仪（sigrok/PulseView）、USB 转串口（CP2102/CH340/FTDI）、ST-Link 或 J-Link**，有条件再加一台示波器。

---

## 9. 学习路线

```
阶段 1  Arduino：GPIO、串口、I2C 传感器，用现成的库              → examples/arduino
阶段 2  换成寄存器写同样的功能，读数据手册                        → 01_blink_register
阶段 3  STM32 裸机：启动代码、时钟、中断、DMA                     → stm32/01
阶段 4  STM32 HAL/LL + FreeRTOS 或 Zephyr，写可移植驱动            → portable_driver、stm32/02
阶段 5  树莓派用户态：libgpiod、i2c-dev、spidev、libusb            → raspberrypi/01-02、usb
阶段 6  Linux 内核：模块、字符设备、platform + DT、I2C/SPI 子系统  → raspberrypi/03-05
阶段 7  子系统：IIO、hwmon、input、V4L2、regmap；向主线提交补丁
阶段 8  逆向：抓包、插桩、反汇编、兼容层                          → reverse_engineering、hezi-hack
```

建议每一阶段都**用同一颗传感器**（比如 BMP280，或者带 I2C 和 SPI 两种接口的 BME280），这样注意力可以集中在"层"的差异上。

---

## 10. 参考资料

**数据手册与官方文档**
* Bosch BMP280 Datasheet（BST-BMP280-DS001），§3.11 补偿公式
* ATmega328P Datasheet，§14 I/O Ports、§22 TWI
* ST RM0008（STM32F10x 参考手册）、PM0056（Cortex-M3 编程手册）
* ARMv7-M Architecture Reference Manual
* BCM2835 ARM Peripherals、BCM2711 ARM Peripherals、RP1 Peripherals（Raspberry Pi 官方文档站）
* Raspberry Pi Documentation：Device Trees, overlays and parameters
* Cypress AN76405（FX3 启动选项与固件映像格式）

**Linux**
* 内核文档 `Documentation/driver-api/`（driver-model、gpio、i2c、hwmon、iio）、`Documentation/usb/usbmon.rst`
* *Linux Device Drivers, 3rd Ed.*（Corbet、Rubini、Kroah-Hartman，免费在线；API 较旧，概念仍然适用）
* Bootlin 的嵌入式 Linux 和内核驱动培训讲义（免费 PDF，持续更新）
* *Linux Driver Development for Embedded Processors*（Alberto Liberal de los Ríos）

**MCU**
* *Making Embedded Systems*（Elecia White）
* *The Definitive Guide to ARM Cortex-M3/M4*（Joseph Yiu）
* Zephyr Project Documentation：Device Driver Model

**逆向**
* *Hardware Hacking Handbook*（Jasper van Woudenberg、Colin O'Flynn）
* *The Hardware Hacker*（Andrew "bunnie" Huang）
* *Practical Reverse Engineering*、Ghidra 官方课程
* nouveau 的 `envytools` / `mmiotrace` 文档、Asahi Linux 的博客（M1 GPU 逆向系列）
* 本机案例：[`/Users/xlisp/PyPro/hezi-hack`](../hezi-hack)
