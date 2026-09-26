# 树莓派电动天文调焦器：从硬件设计到驱动

目标：用一块树莓派、一个步进电机和一块 TMC2209 驱动板，做一个能被 **KStars/Ekos（INDI）** 等天文软件直接识别的电动调焦器，支持自动对焦、回差补偿、温度补偿，断电后记得位置。

这个例子是主 README 第 4 节"驱动分层"的一次完整实践，同一个项目里用到了：

* **硬件层**：选型、接线、电源、机械安装，以及分辨率计算
* **内核驱动**：platform 驱动 + 设备树 overlay、hrtimer 实时发脉冲、梯形加减速、自旋锁、sysfs ABI
* **用户态驱动**：守护进程把内核接口翻译成行业标准协议（Moonlite），用伪终端冒充串口设备
* **测试**：没有硬件也能跑的模拟后端和单元测试

```
06_focuser/
├── README.md                      本文
├── kernel/
│   ├── pifocuser.c                内核驱动：STEP/DIR + hrtimer + 梯形加减速 + sysfs
│   ├── pifocuser-overlay.dts      设备树 overlay：引脚和运动参数
│   ├── 99-pifocuser.rules         udev：让普通用户能写 sysfs
│   └── Makefile                   make / make install
└── daemon/
    ├── focuserd.py                守护进程：Moonlite 协议（pty + TCP）、回差、温补、持久化
    ├── focuserctl.py              命令行客户端
    ├── test_focuserd.py           11 个测试（模拟电机 + 真实 pty/TCP 通信）
    └── pifocuser.service          systemd 服务
```

**验证情况**：守护进程和客户端已在电脑上用模拟后端测试通过，包括 11 个单元测试和一次端到端运行（移动、回差、重启后恢复位置、SIGTERM 时减速停车并保存位置）。内核驱动和 overlay **还没有在真机上编译运行**，第 5 节给出了逐步验证的方法。

---

## 目录

1. [硬件设计](#1-硬件设计)
2. [软件架构：为什么分成内核和用户态两半](#2-软件架构为什么分成内核和用户态两半)
3. [内核驱动详解](#3-内核驱动详解)
4. [守护进程详解](#4-守护进程详解)
5. [一步步搭起来](#5-一步步搭起来)
6. [标定：行程、回差、温度系数](#6-标定行程回差温度系数)
7. [排错](#7-排错)
8. [扩展方向](#8-扩展方向)

---

## 1. 硬件设计

### 1.1 先想清楚需求

调焦器要解决的问题：**把调焦座的位置控制在"临界焦点区"（CFZ）以内，并且能重复到达同一位置。**

| 需求 | 数字 | 决定了什么 |
|---|---|---|
| 分辨率 | 每一步远小于 CFZ（至少 CFZ/5） | 步距角、细分、减速比 |
| 行程 | 调焦座全行程，常见 30–80 mm | 最大步数（Moonlite 协议上限 65535） |
| 力矩 | 能推动相机、滤镜轮（1–3 kg）并在竖直指向时保持住 | 电机尺寸、减速比、保持电流 |
| 重复性 | 回到同一个数字，焦点就回到同一处 | 开环步进不能丢步，还要做回差补偿 |
| 环境 | 户外 –20 °C 到 30 °C，结露，长线缆 | 电机和驱动器余量、线材、电源 |
| 噪声和振动 | 曝光中也可能温补微调 | 静音驱动（StealthChop） |

**临界焦点区（CFZ）**的近似公式：

```
CFZ ≈ 4.88 · λ · F²        （λ = 0.55 µm，F = 焦比）
f/5  : 4.88 × 0.55 × 25  ≈  67 µm
f/7  : 4.88 × 0.55 × 49  ≈ 131 µm
f/10 : 4.88 × 0.55 × 100 ≈ 268 µm
```

快焦比的镜子（f/4、f/5）对调焦精度要求最高，下面按 f/5 设计。

### 1.2 选型

| 部件 | 推荐 | 备选 | 理由 |
|---|---|---|---|
| 主控 | 树莓派 4 / 5 / Zero 2 W | 任何带 40 针排针的 Pi | 通常这台 Pi 已经在跑 KStars/Ekos 或 INDI 服务器了，再多一个设备而已 |
| 电机 | **NEMA17 短机身**（如 17HS4023，1.8°，约 0.7 A、0.13 N·m）或 NEMA14 | 28BYJ-48（5 V 减速电机，便宜，但有回差、慢） | 力矩够用、发热小、便宜；1.8° 即 200 步/圈 |
| 驱动器 | **TMC2209 模块**（STEP/DIR 独立模式） | A4988、DRV8825（有噪声）；28BYJ-48 配 ULN2003 | 3.3 V 逻辑可直接接 Pi；StealthChop 静音、几乎无振动；内部 256 细分插值；带过热和短路保护 |
| 传动 | GT2 同步带 16T（电机）→ 60T（调焦粗调旋钮），减速比 3.75 | 直接用联轴器连到微调旋钮 | 皮带能消除联轴器的同轴度问题，同时增加力矩和分辨率 |
| 温度传感器 | **DS18B20 防水探头**（1-Wire）贴在镜筒上 | 主仓库例子 05 的 BMP280（hwmon） | Linux 自带 `w1-gpio` 和 `w1_therm` 驱动，零代码 |
| 电源 | 12 V（与赤道仪、除露带共用的电源） | 24 V（TMC2209 最高 29 V） | 电压越高，高速时力矩越好；调焦速度低，12 V 足够 |
| 杂项 | 100 µF/35 V 电解电容、4.7 kΩ 电阻、杜邦线或端子、3D 打印支架 | | |

**为什么不用 28BYJ-48？** 它便宜，也确实常见于 DIY 调焦器。但它自带的齿轮箱有明显回差（约 1–3°），是 5 V 单极电机，需要 4 路相序驱动（ULN2003）。要用的话，内核驱动得改成输出 4 相序列，而不是 STEP/DIR。本例选 STEP/DIR 接口，驱动代码对 A4988、DRV8825、TMC 系列通用。

### 1.3 接线

```
                树莓派 40 针排针                        TMC2209 模块（独立模式）
          ┌──────────────────────────┐              ┌────────────────────────┐
  3.3V  1 │ ●──────────────┬─────────┼──────────────│ VIO            VM      │── +12V ─┬── 电源 +
          │                │         │              │                        │         │
  GPIO4 7 │ ●──────┬───────┼── 4.7k ─┘              │                GND     │── GND ──┴── 电源 −
          │        │       │                        │                        │   (VM 与 GND 之间并 100µF 电容，
  GND   9 │ ●──────┼───────┼──────────┬─────────────│ GND            A1/A2 ──│── 电机线圈 A    紧贴模块)
          │        │       │          │             │                B1/B2 ──│── 电机线圈 B
 GPIO17 11│ ●──────┼───────┼──────────┼─────────────│ STEP                   │
 GPIO27 13│ ●──────┼───────┼──────────┼─────────────│ DIR                    │
 GPIO22 15│ ●──────┼───────┼──────────┼─────────────│ EN（低有效）           │
          └────────┼───────┼──────────┘             │ MS1、MS2 悬空/接地     │── 1/8 细分（默认）
                   │       │                        └────────────────────────┘
              DS18B20：DQ(黄)──┘  VDD(红)──3.3V   GND(黑)──GND
```

| 信号 | Pi GPIO（BCM） | 排针脚号 | 接到 | 说明 |
|---|---|---|---|---|
| STEP | 17 | 11 | TMC2209 STEP | 每个上升沿走一个微步 |
| DIR | 27 | 13 | TMC2209 DIR | 方向；方向反了在设备树里改极性，不用动线 |
| EN | 22 | 15 | TMC2209 EN | 低电平使能；不接的话模块默认一直使能 |
| 1-Wire | 4 | 7 | DS18B20 DQ | 系统 `w1-gpio` overlay 的默认引脚；DQ 与 3.3 V 之间接 4.7 kΩ 上拉 |
| 3.3 V | — | 1 | TMC2209 VIO、DS18B20 VDD、上拉电阻 | TMC2209 的逻辑电源，决定 STEP/DIR 的电平 |
| GND | — | 9 | TMC2209 GND、DS18B20 GND、**12 V 电源地** | **必须共地**，否则 STEP 信号没有参考电平 |

引脚都能在设备树参数里改（`dtoverlay=pifocuser,step=5,dir=6,en=13`），驱动代码不写死任何引脚。

### 1.4 分辨率、行程、速度的计算

```
每圈微步数 = 200 步 × 8 细分 × 3.75（16T→60T）          = 6000 微步 / 粗调旋钮一圈
粗调旋钮一圈的行程（实测；齿条式调焦座常见 ~20 mm）      ≈ 20 mm
每微步行程 = 20 mm / 6000                                 ≈ 3.3 µm
f/5 的 CFZ 67 µm 内有                                      ≈ 20 步    ✔（每步 < CFZ/5）
50 mm 全行程                                              ≈ 15000 步 ✔（< 65535，Moonlite 协议能表示）
```

速度：`max-speed = 800` 微步/秒，约 0.13 圈/秒，也就是 2.7 mm/s，全行程约 19 秒。自动对焦每次只移动几十到几百步，速度够用。加速度 2000 微步/s²：0.4 秒加到全速，加速段约 160 步（v²/2a）。

力矩：17HS4023 约 0.13 N·m，经 3.75 倍减速后，旋钮上约 0.45 N·m（扣掉皮带损耗）。这足够推动一般的齿条调焦座加 2 kg 负载。力矩不够的表现是**丢步**：数字到了，焦点没到。这时加大电流、换更长的电机，或者加大减速比。

> 细分的误区：1/16、1/32 细分能让位置"数字"更细，但单个微步的实际力矩和精度会下降，负载下并不能真正停在每个微步上。由减速比提供分辨率，比由细分提供更可靠。所以这里用 1/8 细分加 3.75 倍减速。

### 1.5 电源与可靠性

* **TMC2209 的电流**用模块上的电位器设置 VREF。常见模块（Rsense = 0.11 Ω）上约 `I_rms ≈ 0.71 × VREF`，**以模块手册为准**。调焦器从 0.4–0.6 A 起步，够推就行，电流越小越凉、越安静。
* **绝对不要在通电时插拔电机线。** 线圈断开时产生的反电动势会击穿驱动芯片。
* 在 VM 与 GND 之间、靠近模块的位置并一个 100 µF 电解电容，吸收电机回灌的电压尖峰。
* Pi 用自己的 5 V 电源（或者质量好的 12 V→5 V 降压模块），不要从驱动器的 12 V 线路取电。**但所有地线必须连在一起。**
* STEP/DIR 线超过约 50 cm 时，用双绞线（信号线和地线绞在一起），或者把 TMC2209 放在 Pi 旁边、只把电机线拉长。电机线可以长，逻辑线越短越好。
* 户外结露：驱动板装盒；DS18B20 用防水探头，贴在镜筒上而不是空气中，测的是镜筒温度。

### 1.6 机械安装

* 支架（3D 打印或铝板）固定在调焦座上，电机轴与调焦旋钮平行，用 GT2 皮带连接。60T 同步轮用顶丝固定在**粗调**旋钮轴上。
* 大多数双速调焦座的粗调轴贯穿两侧，另一侧装皮带轮最方便。
* 皮带张紧到"拨动有弹性、不打滑"即可。太紧会增加轴承负担，太松会产生回差。
* **Crayford（摩擦式）调焦座**：先把摩擦力（张力螺丝）调到竖直指向加上相机负载时不打滑。打滑的话，电机转了镜筒却没动，驱动无法察觉（开环），这是自动对焦失败的头号原因。
* 装好后，手动拧一圈旋钮，数电机转了几圈，核对 1.4 节的减速比。

---

## 2. 软件架构：为什么分成内核和用户态两半

```
  KStars/Ekos (INDI "MoonLite" 驱动)     focuserctl.py        其他 Moonlite 客户端
             │  打开 /tmp/pifocuser（伪串口）   │ TCP :4044          │
             └───────────────┬──────────────────┴────────────────────┘
                             ▼
   ┌──────────────── focuserd.py（用户态，慢逻辑）────────────────┐
   │ Moonlite 协议解析 · 回差补偿 · 温度补偿 · 位置持久化          │
   │ 温度：/sys/bus/w1/devices/28-*/temperature（DS18B20）         │
   └─────────────────────────────┬─────────────────────────────────┘
                                 │ sysfs: target / position / moving / halt ...
   ┌─────────────────────────────▼──────────────────────────────────┐
   │ pifocuser.ko（内核，快逻辑）：hrtimer 每步触发一次              │
   │ 梯形加减速 · 平滑改道 · 行程限位 · 电机使能                     │
   └─────────────────────────────┬──────────────────────────────────┘
                                 │ gpiod_set_value()（设备树给出引脚）
                      GPIO17 STEP / GPIO27 DIR / GPIO22 EN
                                 ▼
                       TMC2209 ──► NEMA17 ──► 皮带 ──► 调焦座
```

**分工原则：对时间敏感的放内核，对时间不敏感的放用户态。**

| 任务 | 时间要求 | 放在哪 | 理由 |
|---|---|---|---|
| 发 STEP 脉冲 | 每步间隔几百微秒，抖动要在微秒级 | 内核 hrtimer | 用户态进程可能被调度走几毫秒，这会让电机抖动、噪声变大、极端时丢步 |
| 加减速规划 | 每一步都要算 | 内核（和发脉冲在一起） | 它决定下一个脉冲的时刻 |
| Moonlite 协议 | 客户端每 0.5–1 s 轮询一次 | 用户态 | 字符串处理，放内核毫无必要，出了 bug 还会让整机崩溃 |
| 回差、温补、持久化 | 秒级 | 用户态 | 策略类代码，经常要改；需要读写文件 |

**为什么不全放在用户态？** 可以。用 libgpiod 在一个线程里 `sleep` 然后翻转引脚，慢速时也能用。但 Linux 默认内核下，用户态定时的抖动常有 0.1–1 ms，偶尔会到几毫秒。在 800 步/秒（间隔 1.25 ms）时这已经是 10–100% 的抖动，电机声音难听、振动大。这正是主 README 4.5 节说的"需要低延迟时该进内核"。

**为什么不全放在内核？** 协议、策略、文件读写不属于内核。用户态代码可以用 Python 写、随时重启、单独测试（`test_focuserd.py` 在 Mac 上就能跑）。

**另一种常见方案**是加一片 MCU（Arduino Nano、Pico），由它发脉冲，Pi 通过 USB 串口和它说 Moonlite 协议（比如开源项目 myFocuserPro2）。这样实时性最好，但多一块板子、多一份固件。本例的思路是：**Pi 自己就是那片 MCU，内核驱动扮演固件的角色。**

---

## 3. 内核驱动详解

源码：[kernel/pifocuser.c](kernel/pifocuser.c)

### 3.1 设备树绑定：驱动不知道引脚在哪

```dts
pi-focuser {
    compatible   = "demo,pi-focuser";      // 驱动靠这个字符串匹配
    step-gpios   = <&gpio 17 0>;           // devm_gpiod_get(dev, "step", ...)
    dir-gpios    = <&gpio 27 0>;           // 方向反了：0 改成 1（GPIO_ACTIVE_LOW）
    enable-gpios = <&gpio 22 1>;           // 低有效：驱动里写逻辑值 1 = "使能"
    max-position = <15000>;                // device_property_read_u32()
    max-speed = <800>; start-speed = <100>; acceleration = <2000>;
    hold-current;                          // 布尔属性
};
```

**极性由设备树决定**：TMC2209 的 EN 低有效，设备树里写 `1`（ACTIVE_LOW），驱动代码里就可以统一写 `gpiod_set_value(enable, 1)` 表示"使能"。换成 EN 高有效的驱动板，只改设备树。这就是 gpiod "逻辑值"API 相对于老的 `gpio_set_value(引脚号, 电平)` 的好处。

### 3.2 probe：拿资源、查约束、注册

```
pifoc_probe()
 ├─ devm_gpiod_get("step"/"dir")、devm_gpiod_get_optional("enable")   引脚来自设备树
 ├─ gpiod_cansleep() 检查                     ← 关键约束，见下
 ├─ 读 max-position / max-speed / ...（带默认值和范围检查）
 ├─ hrtimer 初始化（6.13 起用 hrtimer_setup，版本宏兼容）
 └─ devm_add_action_or_reset(teardown)        卸载时：取消定时器、等工作队列、断电
sysfs 属性通过 driver.dev_groups 注册，随设备一起出现和消失，没有"设备有了、属性还没创建"的竞态
```

**`gpiod_cansleep` 检查为什么重要：** 定时器回调运行在**硬中断上下文**，不能睡眠。SoC 直连的 GPIO（BCM2711、RP1）写寄存器即可，不会睡眠。但如果有人在设备树里把 STEP 指向一片 I2C 扩展芯片（比如 MCP23017），设置引脚要走 I2C 传输，就必须睡眠。在中断里调用它会触发 "scheduling while atomic" 甚至死机。probe 时直接拒绝这种配置，把错误挡在最早的地方。

### 3.3 发脉冲：hrtimer 回调里的梯形加减速

每走一步，定时器回调执行一次：

```
         速度 v
max_speed ┤        ┌──────────────────┐
          │       ╱                    ╲
          │      ╱   加速：v += a/v      ╲   减速：剩余步数 ≤ v²/(2a) 时 v -= a/v
start_spd ┤─────╱                        ╲─────
          └────┴───────────────────────────┴────► 步数
           起点                              目标
```

```c
stop_dist = v * v / (2 * accel);          /* 以当前速度刹车需要多少步 */
dv = max(accel / v, 1);                   /* dv = a·dt，这一步的时长 dt = 1/v */
if (left <= stop_dist) v = max(v - dv, min_speed);   /* 该刹车了 */
else                   v = min(v + dv, max_speed);   /* 还能加速 */
STEP = 1; udelay(2); STEP = 0;            /* 2 µs 脉冲，TMC2209 只要求 >100 ns */
position += dir;
hrtimer_forward_now(t, NSEC_PER_SEC / v); /* 下一步的时刻 */
```

* **起跳速度**（`start-speed`）：步进电机能在不加速的情况下直接起停的最高速度，一般每秒几百步。从这个速度开始加速、减速到这个速度停止，既避免失步，也不浪费时间在极低速上。
* 算法只用整数，因为内核里不用浮点。
* 为什么不用 PWM 外设发脉冲？PWM 能产生准确的频率，但**数不了脉冲个数**，而调焦器最关心的恰恰是"走了几步"。有些 SoC 可以用 PWM 加计数器或 DMA 做到，树莓派的 pigpio 就是用 DMA 生成波形。hrtimer 方案最简单、可移植，800 步/秒下 CPU 占用可以忽略。

### 3.4 平滑改道与停车

运动中收到新目标（自动对焦经常这样），**不能立刻反向**，否则等于在高速下猛然掉头，电机会失步：

```
want = sign(target - position)
if want != 当前方向:
    if 速度 > 起跳速度:  沿原方向继续走并减速（left = 0 ⇒ 一定进入减速分支）
    else:                翻转 DIR，等 50 µs 让驱动器锁存方向，再开始新方向的加速
```

`halt` 也是同样的思路：不是直接停，而是把目标改成"当前位置 + 刹车距离"，让定时器自然减速停下。行程端点也在这里保护：减速过程中下一步会越过 0 或 `max_position` 时，立即换向。

### 3.5 并发：一个自旋锁

`position/target/speed/...` 同时被两类代码访问：

* **定时器回调**（硬中断上下文，可能在任意 CPU 上）
* **sysfs 写入**（进程上下文，用户随时 `echo`）

所以用 `spinlock` 加 `spin_lock_irqsave` 保护。不能用 mutex，因为中断上下文不能睡眠。几个细节：

* 在锁内调用 `hrtimer_start` 是安全的；但 `hrtimer_cancel` 会等待回调结束，**不能**在持有回调也要拿的锁时调用，所以它只出现在 teardown 里。
* `sysfs_notify`（唤醒 poll 等待者）可能睡眠，不能在中断里调，所以交给工作队列（`schedule_work`）。
* 运动中写 `position`（同步）返回 `-EBUSY`；目标超出行程返回 `-ERANGE`。**用标准错误码表达约束**，用户态 `echo` 时会直接看到 "Device or resource busy"。

### 3.6 sysfs ABI

| 文件 | 读 | 写 |
|---|---|---|
| `position` | 当前位置 | 同步（只改计数；运动中 `EBUSY`） |
| `target` | 目标 | 开始或改道运动（越界 `ERANGE`） |
| `moving` | 0/1（支持 `poll()`） | — |
| `halt` | — | 1 = 减速停车 |
| `max_position` / `max_speed` / `accel` | 参数 | 修改（带范围检查） |
| `hold` | 0/1 | 停止后是否保持通电 |

为什么选 sysfs，而不是字符设备加 ioctl？调焦器的状态就是几个整数，sysfs 的"一个文件一个值"最直观，`cat`、`echo` 就能调试，Python 读写也方便。如果要高频流式数据（比如编码器反馈），才考虑字符设备或 IIO。

---

## 4. 守护进程详解

源码：[daemon/focuserd.py](daemon/focuserd.py)

### 4.1 Moonlite 协议：借用一个行业标准

与其写一个 INDI 驱动（需要 libindi 开发环境、C++），不如**说一种 INDI 已经懂的语言**。Moonlite 是一家调焦器厂商的串口协议，简单公开，INDI、ASCOM、许多 DIY 调焦器（如 myFocuserPro2 的兼容模式）都支持。这和 hezi-hack 的思路一样：**让新硬件去适配现有软件的接口，而不是改软件。**

帧格式：`:命令[参数]#`，数值是大写十六进制，有应答的回复 `值#`。

| 命令 | 含义 | 应答 / 实现 |
|---|---|---|
| `:GP#` | 当前位置 | `1F40#`（4 位十六进制） |
| `:GN#` | 目标位置 | 同上 |
| `:GI#` | 是否运动中 | `01#` / `00#`（回差补偿的两段合起来算一次运动） |
| `:GT#` | 温度 | 有符号 16 位，单位 0.5 °C：–3.5 °C → `FFF9#` |
| `:GC#` / `:SCxx#` | 温度系数（有符号 8 位） | 本实现的单位是"步/°C" |
| `:+#` / `:-#` | 温补开 / 关 | |
| `:SPxxxx#` | 同步当前位置 | 写 sysfs `position` |
| `:SNxxxx#` + `:FG#` | 设定目标 + 出发 | 写 sysfs `target`（经过回差逻辑） |
| `:FQ#` | 停止 | 写 sysfs `halt` |
| `:SDxx#` / `:GD#` | 步进延时 02..20 | 映射为 `max_speed = 基准 × 2 / xx` |
| `:SF#` / `:SH#` / `:GH#` | 全步 / 半步 | 只记录：细分由 TMC2209 的 MS 引脚硬件决定 |
| `:GV#` | 固件版本 | `10#` |

[test_focuserd.py](daemon/test_focuserd.py) 对这些编码都有测试，包括负温度和负系数的补码。

### 4.2 伪终端：冒充一个 USB 串口

INDI 的 MoonLite 驱动要打开一个串口。守护进程用 `os.openpty()` 创建一对伪终端，把 slave 端软链接到 `/tmp/pifocuser`。INDI 打开这个路径，就像打开 `/dev/ttyUSB0` 一样：

* slave 端设为 raw 模式，否则终端回显会把客户端发的命令原样送回去，客户端就会读到错误的应答。
* 守护进程**自己也保留一个 slave fd**：否则客户端断开时（所有 slave 都关了），读 master 在 Linux 上会一直返回 EIO。
* 同一套协议也在 TCP 4044 端口上提供，给 `focuserctl.py` 和远程客户端用。INDI 在另一台机器上时，可以用 socat 把 TCP 转成本地伪串口：`socat pty,link=/tmp/pifocuser,raw tcp:树莓派IP:4044`。

### 4.3 回差补偿

齿轮、皮带、调焦座齿条换向时都有一段空程：电机转了，镜筒还没动。解决办法是**总从同一个方向逼近目标**：

```
向外（位置增大）：直接走到目标
向内（位置减小）：先走到 目标 − backlash，再向外走回目标
         目标        当前
   ──────┼────────────┼────►
   ◄─────────────────── 1. 冲过头
   ─►                   2. 回到目标（最后一段总是向外）
```

两段运动对客户端来说是一次：`GI` 在第二段结束前一直回 `01`。回差值用 `--backlash` 设置，测法见第 6 节。

### 4.4 温度补偿

镜筒随温度伸缩，焦点随之移动。铝镜筒的膨胀系数约 23 ppm/°C，500 mm 镜筒每 °C 变化约 11.5 µm，约 3.5 步。一夜降温 10 °C 就会跑出 f/5 的 CFZ。

```
开启温补时记录参考点 (T0, P0)；之后每 30 s 检查一次（只在静止时）：
    期望位置 = P0 + (T − T0) × 系数
    |期望 − 当前| ≥ 阈值 → 移动
用户（或自动对焦）手动调焦后，参考点重置为新的 (T, P)
```

温度来源自动查找：DS18B20（`/sys/bus/w1/devices/28-*/temperature`），其次是任意 hwmon 温度（比如主仓库例子 05 的 BMP280 驱动）。**两者都是内核驱动提供的标准接口，守护进程一行 1-Wire 协议都不用写。**

### 4.5 位置持久化

开环步进电机不知道自己在哪。位置在以下时机写入状态文件：每次停止、同步、修改设置、退出。

* 写入用"先写临时文件再 `os.replace`"，断电时不会留下半个 JSON。
* 收到 SIGTERM（`systemctl stop`、关机）时，**先减速停车，等电机停稳，再保存位置**。否则存下的是半路上的值，下次开机位置就错了。这个问题是端到端测试时发现的，已经修复并验证。
* 前提是断电前电机已停，并且断电期间没人动过调焦旋钮。不放心时，可以先把调焦座摇到最内端再"同步为 0"（或者加限位开关，见第 8 节）。

---

## 5. 一步步搭起来

**原则：先在电脑上跑软件，再在桌上跑电机，最后上望远镜。**

### 第 0 步：在任何电脑上试守护进程（不需要硬件）

```bash
cd daemon
python3 test_focuserd.py -v                         # 11 个测试
python3 focuserd.py --backend sim --pty /tmp/pifocuser --backlash 200 &
python3 focuserctl.py goto 12000 --wait
python3 focuserctl.py in 3000 --wait                # 会看到先冲过头 200 步再回来
python3 focuserctl.py status
```

### 第 1 步：接线前的检查

1. **先不接电机**，只给 TMC2209 接 12 V，用万用表测 VREF，调到目标电流对应的值（1.5 节）。
2. 断电，接电机线。用万用表找线圈：两根线之间有几欧姆电阻的是同一个线圈。
3. 按 1.3 节接逻辑线，**确认 Pi 的 GND 与 12 V 电源 GND 相连**。

### 第 2 步：编译安装驱动和 overlay（在 Pi 上）

```bash
sudo apt install linux-headers-rpi-v8 device-tree-compiler   # Pi 5 用 linux-headers-rpi-2712
cd kernel && make && make install
sudo nano /boot/firmware/config.txt
#   dtoverlay=pifocuser
#   dtoverlay=w1-gpio,gpiopin=4
sudo reboot
```

Pi 5 上请先把 overlay 里的 `compatible` 改成 `"brcm,bcm2712"`。

### 第 3 步：直接用 sysfs 驱动电机（不经过守护进程）

```bash
dmesg | grep focuser              # "focuser ready: max 15000 steps, 100..800 steps/s ..."
cd /sys/devices/platform/pi-focuser
cat position moving               # 0 0
echo 2000 > target                # 电机转动；在另一个终端 watch -n0.2 cat position
echo 0 > target                   # 回来
echo 20000 > target               # -bash: echo: write error: Numerical result out of range
gpioinfo | grep -i focus          # 三根引脚被 "pi-focuser" 占用
```

检查方向：`target` 增大时，调焦座应该向**外**走（远离主镜）。反了就把 overlay 里 `dir-gpios` 的 flag 改成 1，重新编译 overlay 并重启。

### 第 4 步：启动守护进程

```bash
sudo cp daemon/focuserd.py daemon/focuserctl.py /usr/local/bin/
sudo cp daemon/pifocuser.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now pifocuser
journalctl -u pifocuser -f
focuserctl.py status              # 温度应该是 DS18B20 的读数
```

### 第 5 步：接入 KStars/Ekos

1. Ekos 的 Profile Editor 里，Focuser 选 **MoonLite**。
2. 启动后，在 INDI 控制面板 MoonLite 的 Connection 页，Port 填 `/tmp/pifocuser`，Baud 9600，然后 Connect。
3. Main Control 页能看到位置和温度。Ekos Focus 模块里设置步长（从 CFZ 算：f/5 时每步 3.3 µm，自动对焦步长取 20–50 步），就可以跑自动对焦了。

INDI 服务器不在这台 Pi 上时，在 INDI 所在机器上运行第 4.2 节的 socat 命令即可。

---

## 6. 标定：行程、回差、温度系数

**行程 `max-position`**：把调焦座摇到最内端，执行 `focuserctl.py sync 0`。然后分几次 `out` 小步前进，直到接近最外端（留 1–2 mm 余量），读出位置，写进 overlay 的 `max-position`（也可以临时 `echo` 到 sysfs）。

**回差**：在调焦旋钮上贴一面小纸旗，对着它看：

```bash
focuserctl.py out 500 --wait        # 先向外，消除一个方向的间隙
focuserctl.py in 10 --wait          # 每次向内 10 步，直到纸旗开始动
...                                 # 累计的步数 ≈ 回差
```

更准的方法是看星点：向外对焦到最佳后，向内走 N 步再向外走 N 步，星点 HFR 变差说明有回差。设置 `--backlash` 为测得值的 1.5 倍左右（冲过头多一点没有坏处）。

**温度系数**：黄昏时对焦一次，记下 (T1, P1)。深夜降温后再对焦，记下 (T2, P2)。系数 = (P2 − P1) / (T2 − T1)。多测几个晚上取平均，然后：

```bash
focuserctl.py coef -4               # 例：降温时焦点向外 4 步/°C（符号由你的方向约定决定）
focuserctl.py comp on
```

碳纤维镜筒的系数接近 0；铝镜筒常见每 °C 几步到十几步（取决于焦距和每步行程）。

---

## 7. 排错

| 现象 | 可能原因 | 排查 |
|---|---|---|
| `dmesg` 里没有 "focuser ready" | overlay 没加载，或模块没装上 | `ls /proc/device-tree/pi-focuser`、`lsmod \| grep pifocuser`、`modinfo pifocuser` |
| probe 失败 "step-gpios" | 引脚被别的驱动占用 | `gpioinfo`；检查 config.txt 里有没有冲突的 overlay（比如把 GPIO17 用作别的功能） |
| 电机嗡嗡响但不转 | 电流太小、线圈接错、起跳速度太高 | 核对线圈对；调大 VREF；降低 `start-speed` 和 `max_speed` |
| 电机抖动、来回摆 | 一个线圈接反或断线 | 万用表量线圈电阻 |
| 方向反了 | — | overlay 里 `dir-gpios` 的 flag 改成 1 |
| 数字对了，焦点不对 | 丢步，或者 Crayford 打滑 | 降低速度或加速度、加大电流；调紧 Crayford；在旋钮上做记号，走 +N 再走 −N，看是否回到原位 |
| 停止后调焦座下滑 | 没有保持电流 | overlay 保留 `hold-current`，或者 `echo 1 > hold` |
| 电机很烫 | 电流太大 + 一直保持 | 调小 VREF；负载轻时关掉 hold |
| INDI 连不上 `/tmp/pifocuser` | 守护进程没跑；INDI 以别的用户运行，没有权限；systemd PrivateTmp | `ls -l /tmp/pifocuser`；`journalctl -u pifocuser` |
| `echo` 到 sysfs 报 Permission denied | 普通用户没有写权限 | 安装 `99-pifocuser.rules`，或者用 sudo |
| 温度一直是 0.0 | 没找到 DS18B20 | `ls /sys/bus/w1/devices/`（应有 `28-xxxx`）；检查 4.7 kΩ 上拉电阻 |

---

## 8. 扩展方向

* **限位 / 归零开关**：在最内端装一个微动开关，接一根 GPIO（设备树加 `home-gpios`），驱动里在开关触发时停车并置 0。这样上电后可以自动找零，不再依赖位置文件。
* **TMC2209 UART 模式**：PDN_UART 接 Pi 的串口，软件设置电流、细分、静止电流减半，还能用 **StallGuard** 检测堵转，实现无开关归零。这相当于给驱动器写一个串口配置驱动，协议在 TMC2209 数据手册里公开。
* **手控盒**：两个按钮接 GPIO，用 `gpio-keys` overlay 变成输入设备，守护进程读 `/dev/input/event*`，按住就连续走。这又是一个"用现成内核子系统"的例子。
* **原生 INDI 驱动**：继承 `INDI::Focuser`，直接读写 sysfs，省掉 Moonlite 这一层，还能暴露温度、回差等所有参数。
* **让 ASIAIR 使用它**：ASIAIR 只认 ZWO EAF。hezi-hack 逆向时已经在 `imager_hotplug_callback` 里看到 `EnumEAFFocuser()`。按主 README 第 7 节的方法，可以做一个 `libEAFFocuser.so` 替换库，把 EAF API 转发到本项目的 TCP 端口。这就是"兼容层"的又一个实例。
* **把脉冲交给 Pico**：需要更高速度或多轴（比如同时控制旋转器）时，由 RP2040 的 PIO 发脉冲，Pi 通过 USB CDC 串口发 Moonlite 命令。守护进程只需要加一个 `SerialBackend`，协议层和上层完全不变。**这正是把后端抽象出来的好处。**
