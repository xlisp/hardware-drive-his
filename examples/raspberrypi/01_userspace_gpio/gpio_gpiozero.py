#!/usr/bin/env python3
"""
gpio_gpiozero.py - Python 用户态 GPIO：gpiozero（树莓派官方推荐，Pi 1～5 通用）

gpiozero 是"设备级"抽象（LED、Button、DistanceSensor……），底层可切换 pin factory：
  lgpio（Pi 5 默认，走 /dev/gpiochip 字符设备）/ RPi.GPIO（老，Pi 5 不支持）/ pigpio / native
这是 Python 世界里"驱动分层"的缩影：应用 → 设备类 → pin factory → 内核 GPIO 字符设备 → 硬件。

  sudo apt install python3-gpiozero python3-lgpio
  python3 gpio_gpiozero.py            # LED 接 GPIO17，按钮接 GPIO27 到 GND
"""
from signal import pause

from gpiozero import LED, Button

led = LED(17)
button = Button(27, pull_up=True, bounce_time=0.05)   # 内部上拉 + 软件去抖

button.when_pressed = led.toggle                      # 回调在后台线程里执行（边沿事件）
led.blink(on_time=0.1, off_time=0.9, n=3, background=True)
print("press the button on GPIO27, Ctrl-C to quit")
pause()
