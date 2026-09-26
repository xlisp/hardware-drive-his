#!/usr/bin/env python3
"""
i2c_regdump.py - 未知 I2C 芯片的寄存器逆向：扫描、整页 dump、差分找"活"寄存器

  i2c_regdump.py scan  [-b 1]                  扫描 0x08..0x77（和 i2cdetect -r 一样用读字节探测）
  i2c_regdump.py dump  0x76 [-b 1]             读 0x00..0xFF 全部寄存器，16 列显示
  i2c_regdump.py diff  0x76 [-n 5] [-t 0.5]    连续 dump N 次，只列出值会变化的寄存器
  i2c_regdump.py watch 0x76 0xFA 3             盯住某几个字节，边操作设备边看

思路：一颗没有数据手册的芯片（或者手册只给了一半），先 dump 全表：
  * 常量区：ID 寄存器（BMP280 在 0xD0 = 0x58）、出厂校准（0x88..0x9F 一片不规则常数）；
  * 活区：diff 下反复变化的就是 ADC 结果（BMP280 的 0xF7..0xFC）；
  * 再对照"写某个配置寄存器 → 哪里跟着变"推断控制位。
这和 hezi-hack 里对串口滤镜轮"发命令 → 看回应 → 计时"是同一套方法：可控的输入，观测输出。

只读不写，是安全的第一步。不过有些芯片"读即清除"（中断状态、FIFO），dump 会改变其状态，注意。
需要 Linux + i2c-dev（sudo modprobe i2c-dev；树莓派上启用 I2C 即可），不需要 smbus 库。
"""
import argparse
import fcntl
import os
import time

I2C_SLAVE = 0x0703          # <linux/i2c-dev.h>
I2C_SLAVE_FORCE = 0x0706    # 地址已被内核驱动占用时仍强制访问（只读 dump 时可用，写操作别用）


class Bus:
    def __init__(self, bus, force=False):
        self.fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
        self.req = I2C_SLAVE_FORCE if force else I2C_SLAVE

    def set_addr(self, addr):
        fcntl.ioctl(self.fd, self.req, addr)

    def read_reg(self, addr, reg, n=1):
        self.set_addr(addr)
        os.write(self.fd, bytes([reg]))      # 写寄存器指针，再读（两次独立传输，中间有 STOP）
        return os.read(self.fd, n)

    def probe(self, addr):
        try:
            self.set_addr(addr)
            os.read(self.fd, 1)
            return True
        except OSError:
            return False

    def dump(self, addr):
        # 分 16 字节一块读：多数芯片支持寄存器指针自增；不支持的会重复同一个值，这本身也是线索
        out = bytearray()
        for base in range(0, 256, 16):
            try:
                out += self.read_reg(addr, base, 16)
            except OSError:
                out += b"\xff" * 16          # 该区域 NACK，用 0xff 占位（下方显示为 --）
        return bytes(out)


def fmt_table(data, mark=None):
    lines = ["     " + " ".join(f"{i:2x}" for i in range(16))]
    for base in range(0, 256, 16):
        cells = []
        for i in range(16):
            v = data[base + i]
            s = f"{v:02x}"
            if mark and base + i in mark:
                s = f"\x1b[7m{s}\x1b[0m"      # 反色标出变化的寄存器
            cells.append(s)
        lines.append(f"{base:02x}:  " + " ".join(cells))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["scan", "dump", "diff", "watch"])
    ap.add_argument("addr", nargs="?", type=lambda s: int(s, 0))
    ap.add_argument("regs", nargs="*", type=lambda s: int(s, 0), help="watch: 起始寄存器 [字节数]")
    ap.add_argument("-b", "--bus", type=int, default=1)
    ap.add_argument("-n", type=int, default=5)
    ap.add_argument("-t", type=float, default=0.5)
    ap.add_argument("--force", action="store_true", help="I2C_SLAVE_FORCE（设备已被内核驱动绑定时）")
    a = ap.parse_args()
    bus = Bus(a.bus, a.force)

    if a.cmd == "scan":
        found = [x for x in range(0x08, 0x78) if bus.probe(x)]
        print("found:", " ".join(f"0x{x:02x}" for x in found) or "nothing")
        return
    if a.addr is None:
        ap.error("address required")

    if a.cmd == "dump":
        print(fmt_table(bus.dump(a.addr)))
    elif a.cmd == "diff":
        snaps = []
        for _ in range(a.n):
            snaps.append(bus.dump(a.addr))
            time.sleep(a.t)
        changed = {i for i in range(256) if len({s[i] for s in snaps}) > 1}
        print(fmt_table(snaps[-1], changed))
        print(f"\n{len(changed)} register(s) changed over {a.n} reads:",
              " ".join(f"0x{i:02x}" for i in sorted(changed)))
    elif a.cmd == "watch":
        reg = a.regs[0] if a.regs else 0
        n = a.regs[1] if len(a.regs) > 1 else 1
        while True:
            v = bus.read_reg(a.addr, reg, n)
            print(f"{time.strftime('%H:%M:%S')}  0x{reg:02x}: {v.hex(' ')}  (be={int.from_bytes(v, 'big')})")
            time.sleep(a.t)


if __name__ == "__main__":
    main()
