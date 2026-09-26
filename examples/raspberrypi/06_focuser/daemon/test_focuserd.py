#!/usr/bin/env python3
"""
test_focuserd.py - 不接硬件测试守护进程：模拟电机 + 真实的 pty/TCP 通信

  python3 test_focuserd.py -v
"""
import os
import socket
import tempfile
import threading
import time
import unittest

import focuserd as fd


class FakeTemp:
    def __init__(self, t): self.t = t
    def read(self): return self.t


def make(backlash=0, temp=15.0, state=None, speed=20000):
    hw = fd.SimBackend(max_position=50000, speed=speed)
    return fd.Focuser(hw, FakeTemp(temp), state, backlash, comp_interval=0, comp_threshold=1)


def run_until_idle(f, timeout=5):
    end = time.monotonic() + timeout
    f.tick()
    while f.moving() and time.monotonic() < end:
        time.sleep(0.01)
        f.tick()
    f.tick()


class ProtocolTest(unittest.TestCase):
    def test_frames(self):
        p = fd.FrameParser()
        self.assertEqual(p.feed(b"noise:GP#:G"), ["GP"])      # 噪声和半帧
        self.assertEqual(p.feed(b"I#"), ["GI"])

    def test_sync_goto(self):
        f = make()
        m = fd.Moonlite(f)
        m.handle("SP1000")                                    # 同步到 0x1000
        self.assertEqual(m.handle("GP"), "1000#")
        m.handle("SN2000")
        self.assertEqual(m.handle("GN"), "2000#")
        m.handle("FG")
        self.assertEqual(m.handle("GI"), "01#")
        run_until_idle(f)
        self.assertEqual(m.handle("GP"), "2000#")
        self.assertEqual(m.handle("GI"), "00#")

    def test_temperature_encoding(self):
        m = fd.Moonlite(make(temp=-3.5))
        self.assertEqual(m.handle("GT"), "FFF9#")             # -3.5 °C = -7 个 0.5 °C
        m = fd.Moonlite(make(temp=21.0))
        self.assertEqual(m.handle("GT"), "002A#")

    def test_signed_coef(self):
        f = make()
        m = fd.Moonlite(f)
        m.handle("SCF4")                                      # -12
        self.assertEqual(f.temp_coef, -12)
        self.assertEqual(m.handle("GC"), "F4#")

    def test_halt(self):
        f = make(speed=1000)
        f.goto(40000)
        time.sleep(0.1)
        f.halt()
        run_until_idle(f)
        self.assertLess(f.hw.position(), 1000)


class BacklashTest(unittest.TestCase):
    def test_inward_overshoots_then_returns(self):
        f = make(backlash=300)
        f.hw.sync(10000)
        seen_min = [10000]
        f.goto(8000)
        end = time.monotonic() + 5
        while f.moving() and time.monotonic() < end:
            f.tick()
            seen_min[0] = min(seen_min[0], f.hw.position())
            time.sleep(0.002)
        self.assertEqual(f.hw.position(), 8000)
        self.assertLessEqual(seen_min[0], 7700 + 50)          # 确实冲过了头

    def test_outward_is_direct(self):
        f = make(backlash=300)
        f.goto(9000)
        self.assertIsNone(f.final_target)


class TempCompTest(unittest.TestCase):
    def test_follows_temperature(self):
        f = make(temp=10.0)
        f.hw.sync(20000)
        f.temp_coef = -20                                     # 降温 1 °C 向外 20 步
        f.comp_enabled = True
        f.tick()                                              # 记录参考点 (10 °C, 20000)
        f.temp.t = 7.0
        run_until_idle(f)
        self.assertEqual(f.hw.position(), 20060)


class PersistenceTest(unittest.TestCase):
    def test_position_survives_restart(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "state.json")
            f = make(state=path)
            f.goto(12345)
            run_until_idle(f)                                 # 停止时自动保存
            f2 = make(state=path)
            self.assertEqual(f2.hw.position(), 12345)


class TransportTest(unittest.TestCase):
    """起一个真正的 Server（pty + TCP），像 INDI 和 focuserctl 那样连上去。"""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.link = os.path.join(self.dir.name, "focuser")
        self.foc = make()
        self.srv = fd.Server(self.foc, pty_link=self.link, port=0, host="127.0.0.1")
        self.stop = False
        self.th = threading.Thread(target=self.srv.serve, args=(lambda: self.stop,), daemon=True)
        self.th.start()

    def tearDown(self):
        self.stop = True
        self.th.join(2)
        self.srv.close()
        self.dir.cleanup()

    def test_pty(self):
        fdesc = os.open(self.link, os.O_RDWR | os.O_NOCTTY)   # INDI 打开"串口"时就是这样
        try:
            os.write(fdesc, b":SP0100#:GP#")
            time.sleep(0.2)
            self.assertEqual(os.read(fdesc, 64), b"0100#")
        finally:
            os.close(fdesc)

    def test_tcp_with_focuserctl_client(self):
        import focuserctl
        c = focuserctl.Client("127.0.0.1", self.srv.port)
        c.goto(3000)
        end = time.monotonic() + 5
        time.sleep(0.2)
        while c.moving() and time.monotonic() < end:
            time.sleep(0.05)
        self.assertEqual(c.position(), 3000)
        self.assertAlmostEqual(c.temperature(), 15.0)
        c.s.close()


if __name__ == "__main__":
    unittest.main()
