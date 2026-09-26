#!/usr/bin/env python3
"""
focuserctl.py - 命令行控制调焦器（通过 focuserd 的 TCP 端口，说的也是 Moonlite 协议）

  focuserctl.py status
  focuserctl.py goto 12000 [--wait]
  focuserctl.py in 200 / out 200 [--wait]     相对移动（in = 位置减小）
  focuserctl.py halt
  focuserctl.py sync 25000                    把当前位置定义为 25000（不转电机）
  focuserctl.py coef -12                      温度系数：步/°C
  focuserctl.py comp on|off                   温度补偿开关
  --host 192.168.1.50 --port 4044             远程控制
"""
import argparse
import socket
import sys
import time


class Client:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=3)
        self.buf = b""

    def cmd(self, c, reply=True):
        self.s.sendall(f":{c}#".encode())
        if not reply:
            return None
        while b"#" not in self.buf:
            chunk = self.s.recv(64)
            if not chunk:
                raise ConnectionError("focuserd closed the connection")
            self.buf += chunk
        r, self.buf = self.buf.split(b"#", 1)
        return r.decode()

    def position(self): return int(self.cmd("GP"), 16)
    def moving(self): return self.cmd("GI") == "01"

    def temperature(self):
        v = int(self.cmd("GT"), 16)
        return (v - 0x10000 if v & 0x8000 else v) / 2.0

    def goto(self, pos):
        self.cmd("SN%04X" % pos, reply=False)
        self.cmd("FG", reply=False)

    def wait(self):
        time.sleep(0.2)                      # 给守护进程一个 tick 开始运动（含回差第二段）
        while self.moving():
            print(f"\r  position {self.position():6d}", end="", flush=True)
            time.sleep(0.2)
        print(f"\r  position {self.position():6d}  done")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["status", "goto", "in", "out", "halt", "sync", "coef", "comp"])
    ap.add_argument("value", nargs="?")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4044)
    ap.add_argument("--wait", action="store_true")
    a = ap.parse_args()
    c = Client(a.host, a.port)

    if a.action == "status":
        coef = int(c.cmd("GC"), 16)
        print(f"position    {c.position()}")
        print(f"moving      {c.moving()}")
        print(f"temperature {c.temperature():.1f} C")
        print(f"temp coef   {coef - 256 if coef > 127 else coef} steps/C")
        return
    if a.action in ("goto", "in", "out", "sync", "coef", "comp") and a.value is None:
        sys.exit(f"{a.action} needs a value")
    if a.action == "goto":
        c.goto(int(a.value))
    elif a.action in ("in", "out"):
        d = int(a.value) * (-1 if a.action == "in" else 1)
        c.goto(max(0, min(0xFFFF, c.position() + d)))
    elif a.action == "halt":
        c.cmd("FQ", reply=False)
    elif a.action == "sync":
        c.cmd("SP%04X" % int(a.value), reply=False)
    elif a.action == "coef":
        c.cmd("SC%02X" % (int(a.value) & 0xFF), reply=False)
    elif a.action == "comp":
        c.cmd("+" if a.value == "on" else "-", reply=False)
    if a.wait:
        c.wait()


if __name__ == "__main__":
    main()
