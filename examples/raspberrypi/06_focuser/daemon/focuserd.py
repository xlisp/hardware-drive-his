#!/usr/bin/env python3
"""
focuserd.py - 树莓派调焦器守护进程：内核驱动（sysfs）⇄ Moonlite 协议

内核驱动（../kernel/pifocuser.c）只负责"准时地发脉冲"。本进程负责所有慢逻辑：

  * Moonlite 串口协议：KStars/Ekos（INDI "MoonLite" 驱动）、ASCOM、许多自动对焦软件都支持。
    进程创建一个伪终端（pty），软链接到 --pty（默认 /tmp/pifocuser），对上层软件来说
    它就是一个 USB 串口调焦器。同时在 TCP --port（默认 4044）上提供同一协议，便于网络访问和 focuserctl。
  * 回差补偿（backlash）：齿轮/皮带换向时有空程。向内运动时先多走 N 步再退回，
    保证最终总是从同一方向逼近目标。
  * 温度补偿：镜筒热胀冷缩会跑焦。读 DS18B20 温度，按"步/°C"系数自动修正。
  * 位置持久化：步进电机不知道自己的绝对位置，断电重启后从状态文件恢复。

后端：
  --backend sysfs   真机（默认），自动查找 /sys/bus/platform/drivers/pi-focuser/*/
  --backend sim     模拟电机，任何电脑上都能跑，用于开发和测试（test_focuserd.py）

  python3 focuserd.py --backend sim --state /tmp/foc.json      # 在电脑上试
  python3 focuserctl.py goto 12000 --wait                      # 另一个终端
"""
import argparse
import glob
import json
import logging
import os
import selectors
import signal
import socket
import time
import tty

log = logging.getLogger("focuserd")
MOONLITE_MAX = 0xFFFF            # Moonlite 位置是 4 位十六进制


# ------------------------------------------------------------------ 后端

class SysfsBackend:
    """内核驱动的 sysfs 接口。所有单位都是（微）步。"""

    def __init__(self, path=None):
        if path is None:
            found = glob.glob("/sys/bus/platform/drivers/pi-focuser/*/position")
            if not found:
                raise SystemExit("pi-focuser device not found: is the overlay loaded and the module installed?")
            path = os.path.dirname(os.path.realpath(found[0]))
        self.path = path
        log.info("sysfs backend at %s", path)

    def _r(self, name):
        with open(os.path.join(self.path, name)) as f:
            return int(f.read().strip())

    def _w(self, name, value):
        with open(os.path.join(self.path, name), "w") as f:
            f.write(str(int(value)))

    def position(self): return self._r("position")
    def target(self): return self._r("target")
    def moving(self): return bool(self._r("moving"))
    def max_position(self): return self._r("max_position")
    def max_speed(self): return self._r("max_speed")
    def sync(self, pos): self._w("position", pos)
    def goto(self, pos): self._w("target", pos)
    def halt(self): self._w("halt", 1)
    def set_max_speed(self, v): self._w("max_speed", v)


class SimBackend:
    """模拟电机：匀速运动，不考虑加减速。行为（sync 在运动中报错等）与内核驱动一致。"""

    def __init__(self, max_position=50000, speed=4000):
        self._pos = self._target = 0.0
        self._max = max_position
        self._speed = speed
        self._t = time.monotonic()

    def _update(self):
        now = time.monotonic()
        step = self._speed * (now - self._t)
        self._t = now
        d = self._target - self._pos
        self._pos = self._target if abs(d) <= step else self._pos + (step if d > 0 else -step)

    def position(self):
        self._update()
        return int(round(self._pos))

    def target(self): return int(self._target)
    def moving(self):
        self._update()
        return self._pos != self._target

    def max_position(self): return self._max
    def max_speed(self): return self._speed

    def sync(self, pos):
        if self.moving():
            raise OSError("EBUSY")
        self._pos = self._target = float(pos)

    def goto(self, pos):
        if not 0 <= pos <= self._max:
            raise OSError("ERANGE")
        self._update()
        self._target = float(pos)

    def halt(self):
        self._update()
        self._target = float(round(self._pos))

    def set_max_speed(self, v): self._speed = v


# ------------------------------------------------------------------ 温度

class TempSensor:
    """优先 DS18B20（w1_therm），其次任意 hwmon 温度（例如 05_i2c_hwmon_driver 的 BMP280）。"""

    def __init__(self, path=None, fixed=None):
        self.fixed = fixed
        self.path = path
        if path is None and fixed is None:
            cands = glob.glob("/sys/bus/w1/devices/28-*/temperature") + \
                    glob.glob("/sys/class/hwmon/hwmon*/temp1_input")
            self.path = cands[0] if cands else None
        log.info("temperature source: %s", self.path or (fixed if fixed is not None else "none"))

    def read(self):
        """°C，读不到返回 None。"""
        if self.fixed is not None:
            return self.fixed
        if not self.path:
            return None
        try:
            with open(self.path) as f:
                return int(f.read().strip()) / 1000.0     # 两种接口都是毫摄氏度
        except (OSError, ValueError):
            return None


# ------------------------------------------------------------------ 调焦逻辑

class Focuser:
    def __init__(self, backend, temp, state_file=None, backlash=0,
                 comp_interval=30.0, comp_threshold=5):
        self.hw = backend
        self.temp = temp
        self.state_file = state_file
        self.backlash = backlash
        self.comp_interval = comp_interval
        self.comp_threshold = comp_threshold
        self.new_target = 0                # Moonlite 的 SN 先设目标，FG 才开始走
        self.final_target = None           # 回差补偿的第二段目标
        self.temp_coef = 0                 # 步/°C（有符号）
        self.temp_offset = 0.0             # 温度读数校正 °C
        self.comp_enabled = False
        self.comp_ref = None               # (温度, 位置)：温度补偿的参考点
        self.last_comp = 0.0
        self.half_step = False
        self.step_delay = 2                # Moonlite SD：02 最快 .. 20 最慢
        self.base_speed = backend.max_speed()
        self._was_moving = False
        self._load()

    # ---- 持久化 ----
    def _load(self):
        if not self.state_file or not os.path.exists(self.state_file):
            return
        try:
            with open(self.state_file) as f:
                st = json.load(f)
        except (OSError, ValueError) as e:
            log.warning("state file unreadable (%s), starting at 0", e)
            return
        pos = min(max(int(st.get("position", 0)), 0), self.hw.max_position())
        self.hw.sync(pos)
        self.new_target = pos
        self.temp_coef = int(st.get("temp_coef", 0))
        self.comp_enabled = bool(st.get("comp_enabled", False))
        self.backlash = int(st.get("backlash", self.backlash))
        log.info("restored position %d from %s", pos, self.state_file)

    def save(self):
        if not self.state_file:
            return
        st = {"position": self.hw.position(), "temp_coef": self.temp_coef,
              "comp_enabled": self.comp_enabled, "backlash": self.backlash, "saved": time.time()}
        tmp = self.state_file + ".tmp"
        with open(tmp, "w") as f:           # 先写临时文件再 rename：断电也不会留下半个 JSON
            json.dump(st, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.state_file)

    # ---- 运动 ----
    def temperature(self):
        t = self.temp.read()
        return None if t is None else t + self.temp_offset

    def goto(self, target, user=True):
        target = min(max(int(target), 0), self.hw.max_position())
        pos = self.hw.position()
        if self.backlash and target < pos:
            # 向内：先冲过头 backlash 步，再向外回到目标 —— 最终总是"向外"逼近
            self.final_target = target
            self.hw.goto(max(target - self.backlash, 0))
        else:
            self.final_target = None
            self.hw.goto(target)
        if user:                            # 人为调焦后，以当前温度为新的补偿参考
            self.comp_ref = None
        log.info("goto %d (from %d%s)", target, pos, ", backlash" if self.final_target is not None else "")

    def halt(self):
        self.final_target = None
        self.hw.halt()

    def target(self):
        return self.final_target if self.final_target is not None else self.hw.target()

    def moving(self):
        return self.hw.moving() or self.final_target is not None

    def set_speed_delay(self, delay):
        self.step_delay = max(1, delay)
        self.hw.set_max_speed(max(1, int(self.base_speed * 2 / self.step_delay)))

    def tick(self):
        """主循环每 0.1 s 调用一次。"""
        hw_moving = self.hw.moving()
        if not hw_moving and self.final_target is not None:
            t, self.final_target = self.final_target, None
            self.hw.goto(t)                 # 回差补偿第二段
            hw_moving = True
        if self._was_moving and not hw_moving:
            log.info("stopped at %d", self.hw.position())
            self.save()
        self._was_moving = hw_moving
        if not hw_moving:
            self._temp_comp()

    def _temp_comp(self):
        if not self.comp_enabled or not self.temp_coef:
            return
        t = self.temperature()
        if t is None:
            return
        pos = self.hw.position()
        if self.comp_ref is None:
            self.comp_ref = (t, pos)
            return
        now = time.monotonic()
        if now - self.last_comp < self.comp_interval:
            return
        t0, p0 = self.comp_ref
        want = int(round(p0 + (t - t0) * self.temp_coef))
        if abs(want - pos) >= self.comp_threshold:
            self.last_comp = now
            log.info("temp comp: %.2f C (ref %.2f) -> %d", t, t0, want)
            self.goto(want, user=False)


# ------------------------------------------------------------------ Moonlite 协议

class Moonlite:
    """
    帧格式 ":<命令><参数>#"，数值为大写十六进制。有应答的命令回 "<值>#"。
    参考 INDI drivers/focuser/moonlite.cpp 与 Moonlite 官方文档。
    """

    def __init__(self, focuser):
        self.f = focuser

    def handle(self, cmd):
        f = self.f
        c, arg = cmd[:2], cmd[2:]
        try:
            if c == "GP":
                return "%04X#" % min(f.hw.position(), MOONLITE_MAX)
            if c == "GN":
                return "%04X#" % min(f.target() if f.moving() else f.new_target, MOONLITE_MAX)
            if c == "GT":                      # 有符号 16 位，单位 0.5 °C
                t = f.temperature()
                return "%04X#" % (int(round((t or 0.0) * 2)) & 0xFFFF)
            if c == "GI":
                return "01#" if f.moving() else "00#"
            if c == "GD":
                return "%02X#" % f.step_delay
            if c == "GH":
                return "FF#" if f.half_step else "00#"
            if c == "GV":
                return "10#"
            if c == "GC":
                return "%02X#" % (f.temp_coef & 0xFF)
            if c == "GB":                      # 背光（原厂手柄），固定值
                return "00#"
            if c == "SP":                      # 同步：把当前位置定义为该值，不转电机
                f.hw.sync(int(arg, 16))
                f.new_target = int(arg, 16)
                f.save()
                return None
            if c == "SN":
                f.new_target = int(arg, 16)
                return None
            if c == "FG":
                f.goto(f.new_target)
                return None
            if c == "FQ":
                f.halt()
                return None
            if c == "SF":
                f.half_step = False            # 细分由驱动器 MS 引脚硬件决定，这里只记录
                return None
            if c == "SH":
                f.half_step = True
                return None
            if c == "SD":
                f.set_speed_delay(int(arg, 16))
                return None
            if c == "SC":
                v = int(arg, 16)
                f.temp_coef = v - 256 if v > 127 else v
                f.save()
                return None
            if c == "PO":
                v = int(arg, 16)
                f.temp_offset = (v - 256 if v > 127 else v) / 2.0
                return None
            if cmd == "C":                     # 开始温度转换：w1_therm 读时自动转换
                return None
            if cmd == "+":
                f.comp_enabled, f.comp_ref = True, None
                f.save()
                return None
            if cmd == "-":
                f.comp_enabled = False
                f.save()
                return None
        except (ValueError, OSError) as e:
            log.warning("command %r failed: %s", cmd, e)
            return None
        log.debug("unknown command %r", cmd)
        return None


class FrameParser:
    """从字节流里切出 ":...#" 帧；串口上的噪声、半帧都能容忍。"""

    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            start = self.buf.find(b":")
            if start < 0:
                self.buf = b""
                break
            end = self.buf.find(b"#", start)
            if end < 0:
                self.buf = self.buf[start:][-64:]
                break
            out.append(self.buf[start + 1:end].decode("ascii", "replace"))
            self.buf = self.buf[end + 1:]
        return out


# ------------------------------------------------------------------ 服务器（pty + TCP）

class Server:
    def __init__(self, focuser, pty_link=None, port=None, host="0.0.0.0"):
        self.proto = Moonlite(focuser)
        self.focuser = focuser
        self.sel = selectors.DefaultSelector()
        self.pty_link = pty_link
        self.port = None
        if pty_link:
            self._open_pty(pty_link)
        if port is not None:
            ls = socket.socket()
            ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            ls.bind((host, port))
            ls.listen(4)
            ls.setblocking(False)
            self.port = ls.getsockname()[1]
            self.sel.register(ls, selectors.EVENT_READ, ("listen", None))
            log.info("Moonlite over TCP on %s:%d", host, self.port)

    def _open_pty(self, link):
        master, slave = os.openpty()
        tty.setraw(slave)                   # 关掉回显和行缓冲，否则客户端会收到自己发的命令
        # 自己保留一个 slave fd：客户端全部关闭后 master 读取也不会返回 EIO
        self._slave = slave
        os.set_blocking(master, False)
        name = os.ttyname(slave)
        try:
            os.unlink(link)
        except FileNotFoundError:
            pass
        os.symlink(name, link)
        self.sel.register(master, selectors.EVENT_READ, ("pty", FrameParser()))
        log.info("Moonlite serial on %s -> %s", link, name)

    def _reply(self, kind, obj, text):
        data = text.encode()
        if kind == "pty":
            os.write(obj, data)
        else:
            obj.sendall(data)

    def _handle(self, kind, obj, parser, data):
        for cmd in parser.feed(data):
            resp = self.proto.handle(cmd)
            log.debug("%s: %r -> %r", kind, cmd, resp)
            if resp is not None:
                self._reply(kind, obj, resp)

    def serve(self, stop=lambda: False):
        next_tick = 0.0
        while not stop():
            for key, _ in self.sel.select(timeout=0.05):
                kind, parser = key.data
                if kind == "listen":
                    conn, addr = key.fileobj.accept()
                    conn.setblocking(False)
                    self.sel.register(conn, selectors.EVENT_READ, ("tcp", FrameParser()))
                    log.info("client %s connected", addr)
                elif kind == "pty":
                    try:
                        data = os.read(key.fd, 1024)
                    except (BlockingIOError, InterruptedError):
                        continue
                    self._handle("pty", key.fd, parser, data)
                else:
                    try:
                        data = key.fileobj.recv(1024)
                    except (BlockingIOError, InterruptedError):
                        continue
                    except OSError:
                        data = b""
                    if not data:
                        self.sel.unregister(key.fileobj)
                        key.fileobj.close()
                        continue
                    self._handle("tcp", key.fileobj, parser, data)
            if time.monotonic() >= next_tick:
                next_tick = time.monotonic() + 0.1
                try:
                    self.focuser.tick()
                except OSError as e:
                    log.error("tick: %s", e)

    def close(self):
        for key in list(self.sel.get_map().values()):
            self.sel.unregister(key.fileobj)
            if isinstance(key.fileobj, int):
                os.close(key.fileobj)
            else:
                key.fileobj.close()
        if self.pty_link:
            os.close(self._slave)
            if os.path.islink(self.pty_link):
                os.unlink(self.pty_link)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=["sysfs", "sim"], default="sysfs")
    ap.add_argument("--sysfs", help="设备目录（默认自动查找）")
    ap.add_argument("--pty", default="/tmp/pifocuser", help="伪串口软链接路径，空字符串 = 不创建")
    ap.add_argument("--port", type=int, default=4044, help="TCP 端口，-1 = 不监听")
    ap.add_argument("--state", default=os.path.expanduser("~/.pifocuser.json"))
    ap.add_argument("--backlash", type=int, default=0, help="回差补偿步数")
    ap.add_argument("--temp-file", help="温度文件（毫摄氏度），默认自动找 DS18B20/hwmon")
    ap.add_argument("--sim-temp", type=float, help="sim：固定温度 °C")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    if a.backend == "sim":
        hw = SimBackend()
        temp = TempSensor(a.temp_file, a.sim_temp if a.sim_temp is not None else (None if a.temp_file else 15.0))
    else:
        hw = SysfsBackend(a.sysfs)
        temp = TempSensor(a.temp_file)
    foc = Focuser(hw, temp, a.state, a.backlash)
    srv = Server(foc, a.pty or None, None if a.port < 0 else a.port)
    # systemd 停止服务发 SIGTERM：同样走下面的"减速停车 + 保存位置"流程
    stopping = []
    signal.signal(signal.SIGTERM, lambda *_: stopping.append(1))
    try:
        srv.serve(stop=lambda: bool(stopping))
    except KeyboardInterrupt:
        pass
    finally:
        if foc.moving():                   # 先减速停稳再存位置，否则存下的是半路上的值
            foc.halt()
            deadline = time.monotonic() + 10
            while hw.moving() and time.monotonic() < deadline:
                time.sleep(0.05)
        foc.save()
        srv.close()


if __name__ == "__main__":
    main()
