#!/usr/bin/env python3
"""
usbmon_parse.py - 把 Linux usbmon 的文本抓包整理成"请求 → 应答"的表格

usbmon 是内核自带的 USB 抓包点（2.6.11 起），不需要改任何程序，所有 USB 流量都能看到。
Wireshark 抓 USB 在 Linux 上用的就是它。文本接口适合在没有图形界面的板子上（ASIAIR、树莓派）用：

  sudo modprobe usbmon
  lsusb                                   # 找到设备在 Bus 001 Device 007
  sudo cat /sys/kernel/debug/usb/usbmon/1u > cap.txt     # 1 = bus 号；操作设备后 Ctrl-C
  usbmon_parse.py cap.txt --dev 7         # 只看 7 号设备
  usbmon_parse.py cap.txt --dev 7 --ctrl  # 只看控制传输（厂商协议大多在这里）
  usbmon_parse.py cap.txt --dev 7 --unique  # 去重：每种 (bRequest, wValue, wIndex) 只显示一次

文本格式（Documentation/usb/usbmon.rst）：
  URB标签 时间戳(µs) 事件(S提交/C完成/E错误) 地址(类型方向:总线:设备:端点)
  控制传输提交时：s bmRequestType bRequest wValue wIndex wLength
  其他：状态  长度  = 数据字（大端分组的十六进制）
例：
  ffff8a0b 3576190153 S Ci:1:007:0 s c0 12 0000 0000 0004 4 <
  ffff8a0b 3576190472 C Ci:1:007:0 0 4 = 01020304
"""
import argparse
import sys

TYPES = {"C": "CTRL", "B": "BULK", "I": "INTR", "Z": "ISOC"}


def parse_line(line):
    f = line.split()
    if len(f) < 4:
        return None
    tag, ts, ev, addr = f[0], int(f[1]), f[2], f[3]
    try:
        xfer_dir, bus, dev, ep = addr[:2], *addr[3:].split(":")
    except ValueError:
        return None
    rec = {"tag": tag, "ts": ts, "ev": ev, "type": TYPES.get(xfer_dir[0], "?"),
           "dir": "IN" if xfer_dir[1] == "i" else "OUT",
           "bus": int(bus), "dev": int(dev), "ep": int(ep), "setup": None, "data": b"", "status": None}
    rest = f[4:]
    if rest and rest[0] == "s":                      # 控制传输的 setup 包
        rec["setup"] = tuple(int(x, 16) for x in rest[1:6])
        rest = rest[6:]
    else:
        rec["status"] = rest[0] if rest else None    # 可能是 "0"、"-115" 或 "-115:8"（带间隔）
        rest = rest[1:]
        if rec["type"] == "ISOC" and rest and ":" in rest[0]:
            rest = rest[1:]
    if rest:
        rec["len"] = int(rest[0]) if rest[0].lstrip("-").isdigit() else 0
        if len(rest) > 2 and rest[1] == "=":
            rec["data"] = bytes.fromhex("".join(rest[2:]))
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", default="-")
    ap.add_argument("--dev", type=int, help="只看该设备号")
    ap.add_argument("--ctrl", action="store_true", help="只看控制传输")
    ap.add_argument("--unique", action="store_true", help="相同请求只显示第一次")
    ap.add_argument("--max", type=int, default=24, help="每条数据最多显示多少字节")
    a = ap.parse_args()

    src = sys.stdin if a.file == "-" else open(a.file, errors="replace")
    pending, seen, t0 = {}, set(), None
    for line in src:
        r = parse_line(line)
        if not r or (a.dev is not None and r["dev"] != a.dev):
            continue
        if a.ctrl and r["type"] != "CTRL":
            continue
        t0 = r["ts"] if t0 is None else t0
        if r["ev"] == "S":
            pending[r["tag"]] = r
            continue
        s = pending.pop(r["tag"], None)
        if s is None:
            continue
        if s["setup"]:
            rt, req, val, idx, ln = s["setup"]
            key = (rt, req, val, idx)
            desc = f"type={rt:02x} req={req:02x} val={val:04x} idx={idx:04x} len={ln}"
        else:
            key = (r["type"], r["ep"], r["dir"])
            desc = f"ep={r['ep']} len={s.get('len', 0)}"
        if a.unique and key in seen:
            continue
        seen.add(key)
        payload = s["data"] if r["dir"] == "OUT" else r["data"]   # OUT 数据在提交时，IN 数据在完成时
        hexs = payload[:a.max].hex(" ") + (" ..." if len(payload) > a.max else "")
        dt = (s["ts"] - t0) / 1e6
        lat = (r["ts"] - s["ts"]) / 1e3
        print(f"{dt:10.6f} dev{r['dev']:<3} {r['type']} {r['dir']:3} {desc:48} st={r['status']:>5} "
              f"{lat:7.2f}ms  {hexs}")


if __name__ == "__main__":
    main()
