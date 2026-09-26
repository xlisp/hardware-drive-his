#!/usr/bin/env python3
"""
serial_probe.py - 未知串口设备的协议探测（只用标准库 termios，无需 pyserial）

从 hezi-hack/scripts/re/cfw_probe.py 泛化而来。探测一个 USB 转串口（CP2102/CH340/FTDI）
后面到底是什么设备，是写驱动前的第一步 —— 同一个 CP2102 可能是滤镜轮，也可能是赤道仪线，
乱发命令可能让设备动作。

  serial_probe.py /dev/ttyUSB0 listen [--baud 9600] [--secs 30]
        只听不发：很多设备上电/打开端口时会主动吐东西（版本号、复位完成字节、NMEA 语句）
  serial_probe.py /dev/ttyUSB0 baudscan
        每个常见波特率听 2 秒 + 发一个 \\r，按"可打印字符比例"打分，猜波特率
  serial_probe.py /dev/ttyUSB0 send VRS MXP NOW [--baud 9600] [--wait 3]
        逐条发送候选命令，打印回复的 hex/ASCII 和耗时（\\r \\n \\x06 等转义可用）
  serial_probe.py /dev/ttyUSB0 send --hex "55aa0100" ...
        发送二进制帧

注意（hezi-hack 14.2 节的教训）：
  * 很多 USB 串口设备在打开端口时因 DTR/RTS 变化而复位（Arduino 就是靠这个进 bootloader）。
    --no-dtr 会在打开后立刻拉低 DTR/RTS，但有的设备在打开瞬间就已经复位了。
  * 复位过程中发命令可能把设备状态搞乱：listen 先等到"开机字节"再发命令。
"""
import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import time

BAUDS = [9600, 19200, 38400, 57600, 115200, 4800, 2400, 230400, 460800, 921600]


def baud_const(b):
    c = getattr(termios, f"B{b}", None)
    if c is None:
        sys.exit(f"baud {b} not supported by termios on this OS")
    return c


def open_port(dev, baud, no_dtr=False):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    set_baud(fd, baud)
    if no_dtr:
        bits = termios.TIOCM_DTR | termios.TIOCM_RTS
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", bits))
    return fd


def set_baud(fd, baud):
    a = termios.tcgetattr(fd)
    a[0] = 0                                                   # iflag：不做 CR/LF 转换、不做软件流控
    a[1] = 0                                                   # oflag
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL        # 8N1，忽略调制解调器线
    a[3] = 0                                                   # lflag：raw 模式，无回显、无行缓冲
    a[4] = a[5] = baud_const(baud)
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)


def read_for(fd, secs, idle=None):
    """读 secs 秒；idle 秒内没有新字节就提前结束。"""
    buf, end, last = b"", time.time() + secs, time.time()
    while time.time() < end:
        left = min(end - time.time(), 0.05)
        if select.select([fd], [], [], max(left, 0))[0]:
            chunk = os.read(fd, 4096)
            if chunk:
                buf += chunk
                last = time.time()
        elif idle and buf and time.time() - last > idle:
            break
    return buf


def show(data):
    hexs = data.hex(" ")
    text = "".join(chr(c) if 32 <= c < 127 else "." for c in data)
    return f"{len(data):4d}B  {hexs[:96]}{' ...' if len(hexs) > 96 else ''}  |{text[:48]}|"


def printable_score(data):
    if not data:
        return 0.0
    ok = sum(1 for c in data if 32 <= c < 127 or c in (9, 10, 13))
    return ok / len(data)


def unescape(s):
    return s.encode().decode("unicode_escape").encode("latin-1")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dev")
    ap.add_argument("mode", choices=["listen", "baudscan", "send"])
    ap.add_argument("cmds", nargs="*")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--wait", type=float, default=3, help="每条命令最多等多久回复")
    ap.add_argument("--hex", action="store_true", help="cmds 是十六进制字节串")
    ap.add_argument("--no-dtr", action="store_true")
    a = ap.parse_args()

    if a.mode == "baudscan":
        fd = open_port(a.dev, BAUDS[0], a.no_dtr)
        results = []
        for b in BAUDS:
            set_baud(fd, b)
            data = read_for(fd, 2)
            os.write(fd, b"\r")
            data += read_for(fd, 1)
            results.append((printable_score(data), b, data))
            print(f"{b:7d}  score {printable_score(data):.2f}  {show(data)}")
        best = max(results)
        print(f"best guess: {best[1]} baud" if best[0] > 0.8 else "no clear winner (binary protocol? try send)")
        return

    fd = open_port(a.dev, a.baud, a.no_dtr)
    t0 = time.time()
    if a.mode == "listen":
        print(f"{a.dev} @ {a.baud}, listening {a.secs:.0f}s ...")
        while time.time() - t0 < a.secs:
            data = read_for(fd, a.secs - (time.time() - t0), idle=0.2)
            if data:
                print(f"+{time.time() - t0:7.2f}s  {show(data)}")
        return

    for c in a.cmds:
        payload = bytes.fromhex(c) if a.hex else unescape(c)
        termios.tcflush(fd, termios.TCIFLUSH)                  # 丢掉之前残留的字节，免得对错回复
        t = time.time()
        os.write(fd, payload)
        reply = read_for(fd, a.wait, idle=0.3)
        print(f"{c!r:14} -> {show(reply)}  ({time.time() - t:.2f}s)")


if __name__ == "__main__":
    main()
