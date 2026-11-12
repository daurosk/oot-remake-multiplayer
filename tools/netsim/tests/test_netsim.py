"""Tests for the bad-connection relay (tools/netsim/netsim.py): the scheduler
with made-up times, and one real relay over loopback UDP."""

import argparse
import os
import random
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import netsim  # noqa: E402
from netsim import Conditions, Link  # noqa: E402


def link(seed=1, **kw):
    return Link(Conditions(**kw), random.Random(seed), start=0.0)


class SchedulerTest(unittest.TestCase):
    def test_quiet_link_passes_at_once(self):
        lk = link()
        self.assertEqual(lk.plan(1.0, 100), [1.0])
        self.assertTrue(Conditions().quiet())

    def test_fixed_lag(self):
        lk = link(lag=75)
        for i in range(10):
            (out,) = lk.plan(i * 0.016, 100)
            self.assertAlmostEqual(out - i * 0.016, 0.075)

    def test_jitter_within_bounds_and_in_order(self):
        lk = link(lag=100, jitter=30)
        last = 0.0
        for i in range(2000):
            now = i * 0.001
            (out,) = lk.plan(now, 100)
            self.assertGreaterEqual(out, last)  # never out of order
            self.assertGreaterEqual(out - now, 0.070 - 1e-9)
            last = out
        # Packets 1 ms apart with +-30 ms: held behind earlier ones, so the
        # delay never exceeds lag + jitter by more than the spacing.
        self.assertLessEqual(lk.stats.delay_max, 130 + 1e-6)

    def test_loss_rate(self):
        lk = link(seed=7, loss=10)
        n = 20000
        sent = sum(len(lk.plan(i * 0.001, 50)) for i in range(n))
        self.assertAlmostEqual(lk.stats.lost / n, 0.10, delta=0.01)
        self.assertEqual(sent + lk.stats.lost, n)

    def test_full_loss_and_none(self):
        self.assertEqual(link(loss=100).plan(0, 10), [])
        self.assertEqual(link(loss=0).plan(0, 10), [0])

    def test_duplicates(self):
        lk = link(seed=3, dup=100, lag=10)
        out = lk.plan(0.0, 10)
        self.assertEqual(len(out), 2)
        self.assertEqual(lk.stats.dups, 1)
        self.assertEqual(lk.stats.sent, 2)

    def test_reorder_comes_after_later_packets(self):
        lk = link(seed=5, lag=20, reorder=100, reorder_gap=50)
        (a,) = lk.plan(0.0, 10)
        lk.c = Conditions(lag=20)  # the next one is not held back
        (b,) = lk.plan(0.010, 10)
        self.assertGreater(a, b)
        self.assertEqual(lk.stats.reordered, 1)

    def test_rate_spaces_packets(self):
        # 80 kbit/s: a 1000-byte packet takes 100 ms on the wire.
        lk = link(rate=80, queue=1000)
        outs = [lk.plan(0.0, 1000)[0] for _ in range(3)]
        for got, want in zip(outs, (0.1, 0.2, 0.3)):
            self.assertAlmostEqual(got, want)

    def test_rate_queue_drops(self):
        lk = link(rate=80, queue=150)
        results = [lk.plan(0.0, 1000) for _ in range(4)]
        # 0 and 100 ms of waiting fit in 150; 200 does not.
        self.assertEqual([len(r) for r in results], [1, 1, 0, 0])
        self.assertEqual(lk.stats.queue_drops, 2)

    def test_stall_windows(self):
        lk = link(spike=500, every=10)
        self.assertIsNone(lk.stall_end(5.0))  # none in the first period
        self.assertIsNone(lk.stall_end(9.99))
        self.assertAlmostEqual(lk.stall_end(10.0), 10.5)
        self.assertAlmostEqual(lk.stall_end(20.2), 20.5)
        self.assertIsNone(lk.stall_end(20.5))

    def test_stall_holds_then_releases_together(self):
        lk = link(lag=20, spike=500, every=10)
        outs = [lk.plan(10.0 + i * 0.05, 100)[0] for i in range(10)]  # 10.00 .. 10.45
        # Sent during the stall: all come out at its end plus the lag, in order.
        for o in outs:
            self.assertAlmostEqual(o, 10.52)
        self.assertEqual(lk.stats.stalled, 10)
        # In flight when the stall starts: held to its end.
        lk2 = link(lag=50, spike=500, every=10)
        (o,) = lk2.plan(9.99, 100)
        self.assertAlmostEqual(o, 10.5)
        # After it: normal again.
        self.assertAlmostEqual(lk.plan(11.0, 100)[0], 11.02)

    def test_same_seed_same_plan(self):
        def run(seed):
            lk = link(seed=seed, lag=50, jitter=20, loss=5, dup=2, reorder=3)
            return [tuple(lk.plan(i * 0.016, 200)) for i in range(500)]
        self.assertEqual(run(11), run(11))
        self.assertNotEqual(run(11), run(12))


class ParseTest(unittest.TestCase):
    def test_spike(self):
        self.assertEqual(netsim.parse_spike("500/10"), (500.0, 10.0))
        for bad in ("500", "x/1", "2000/1", "5/0"):
            with self.assertRaises(ValueError):
                netsim.parse_spike(bad)

    def test_direction(self):
        base = Conditions(lag=50, loss=1)
        d = netsim.parse_direction("lag=300, loss=5,spike=200/4", base)
        self.assertEqual((d.lag, d.loss, d.spike, d.every, d.jitter), (300, 5, 200, 4, 0))
        self.assertEqual(base.lag, 50)  # base untouched
        for bad in ("lag", "speed=1", "loss=120", "lag=-1"):
            with self.assertRaises(ValueError):
                netsim.parse_direction(bad, base)

    def test_lag_is_round_trip(self):
        a = argparse.Namespace(
            lag=200, jitter=10, loss=2, dup=0, reorder=0, rate=0, queue=500, spike="500/10",
            up="", down="loss=0")
        up, down = netsim.build_conditions(a)
        self.assertEqual((up.lag, up.loss, up.spike), (100, 2, 500))
        self.assertEqual((down.lag, down.loss, down.jitter), (100, 0, 10))


class RelayTest(unittest.TestCase):
    """A real relay on loopback: an echo server behind it, a client in front."""

    def setUp(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.settimeout(0.2)
        self.stop = False
        self.echo = threading.Thread(target=self._echo, daemon=True)
        self.echo.start()
        self.lines = []

    def tearDown(self):
        self.stop = True
        self.echo.join(2)
        self.server.close()

    def _echo(self):
        while not self.stop:
            try:
                data, addr = self.server.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            self.server.sendto(b"echo:" + data, addr)

    def _relay(self, up, down):
        r = netsim.Relay(("127.0.0.1", 0), self.server.getsockname(), up, down, 1, self.lines.append)
        t = threading.Thread(target=r.run, kwargs={"seconds": 3.0, "report_every": 0}, daemon=True)
        t.start()
        return r, t

    def test_round_trip_lag(self):
        r, t = self._relay(Conditions(lag=60), Conditions(lag=40))
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.settimeout(2)
        try:
            t0 = time.monotonic()
            c.sendto(b"hi", ("127.0.0.1", r.port()))
            data, _ = c.recvfrom(100)
            rtt = (time.monotonic() - t0) * 1000
            self.assertEqual(data, b"echo:hi")
            self.assertGreaterEqual(rtt, 95)  # 60 up + 40 down
            self.assertLess(rtt, 400)
            # A second client gets its own path to the server.
            c2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            c2.settimeout(2)
            c2.sendto(b"two", ("127.0.0.1", r.port()))
            self.assertEqual(c2.recvfrom(100)[0], b"echo:two")
            c2.close()
            self.assertEqual(len(r.clients), 2)
        finally:
            c.close()
            t.join(5)
            r.close()

    def test_total_loss_down(self):
        r, t = self._relay(Conditions(), Conditions(loss=100))
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.settimeout(0.5)
        try:
            c.sendto(b"x", ("127.0.0.1", r.port()))
            with self.assertRaises(socket.timeout):
                c.recvfrom(100)
            (_, up, down, _), = r.clients.values()
            self.assertEqual((up.stats.sent, down.stats.lost), (1, 1))
        finally:
            c.close()
            t.join(5)
            r.close()


if __name__ == "__main__":
    unittest.main()
