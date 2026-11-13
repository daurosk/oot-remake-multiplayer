#!/usr/bin/env python3
"""A bad-connection relay for testing co-op on one PC.

Sits between one game (or coop_test client) and the co-op server and passes the
UDP packets on, late, lost, doubled, out of order, squeezed through a slow link
or frozen for a moment, so sync can be watched under a bad connection without a
second PC. Nothing in the game or server changes: the game's coop.ini names the
relay's port instead of the server's. See docs/testing.md, "Bad connections".

    python tools/netsim/netsim.py --listen 27561 --server 127.0.0.1:27560 \
        --lag 150 --jitter 20 --loss 2 --spike 500/10

--lag is added round trip: each direction gets half. --jitter, --loss, --dup,
--reorder and --rate apply to each direction as given. --up / --down set one
direction on its own (lag there is one way), for example --down lag=300,loss=5.

Standard library only (Python 3.8 or newer).
"""

from __future__ import annotations

import argparse
import heapq
import math
import random
import selectors
import socket
import sys
import time
from dataclasses import dataclass, field, replace
from typing import Dict, List, Optional, Tuple

MAX_DATAGRAM = 65535


@dataclass
class Conditions:
    """One direction of the link. Times in ms, percentages 0 to 100."""

    lag: float = 0.0  # one-way delay
    jitter: float = 0.0  # each packet +- up to this, uniformly
    loss: float = 0.0  # packets dropped
    dup: float = 0.0  # packets sent twice
    reorder: float = 0.0  # packets held back past later ones
    reorder_gap: float = 0.0  # how far back (0: twice the jitter, at least 10 ms)
    rate: float = 0.0  # link speed in kbit/s, 0 for no limit
    queue: float = 500.0  # with a rate: most ms a packet may wait in the queue, else dropped
    spike: float = 0.0  # stall length
    every: float = 0.0  # seconds from one stall to the next

    def quiet(self) -> bool:
        return not (self.lag or self.jitter or self.loss or self.dup or self.reorder or self.rate
                    or (self.spike and self.every))

    def text(self) -> str:
        parts = [f"lag {self.lag:g} ms"]
        if self.jitter:
            parts.append(f"jitter +-{self.jitter:g} ms")
        if self.loss:
            parts.append(f"loss {self.loss:g}%")
        if self.dup:
            parts.append(f"dup {self.dup:g}%")
        if self.reorder:
            parts.append(f"reorder {self.reorder:g}% by {self.gap_ms():g} ms")
        if self.rate:
            parts.append(f"rate {self.rate:g} kbit/s (queue {self.queue:g} ms)")
        if self.spike and self.every:
            parts.append(f"stall {self.spike:g} ms every {self.every:g} s")
        return ", ".join(parts)

    def gap_ms(self) -> float:
        return self.reorder_gap if self.reorder_gap > 0 else max(2 * self.jitter, 10.0)


_KEYS = {f.name for f in Conditions.__dataclass_fields__.values()}  # type: ignore[attr-defined]


def parse_spike(text: str) -> Tuple[float, float]:
    """'500/10' -> (500 ms, 10 s)."""
    try:
        ms, every = text.split("/")
        ms_v, every_v = float(ms), float(every)
    except ValueError:
        raise ValueError(f"spike must be <ms>/<every s>, as 500/10, not {text!r}") from None
    if ms_v < 0 or every_v <= 0 or ms_v >= every_v * 1000:
        raise ValueError(f"spike {text!r}: the stall must be shorter than the time between stalls")
    return ms_v, every_v


def parse_direction(text: str, base: Conditions) -> Conditions:
    """'lag=300,loss=5,spike=500/10' on top of base."""
    values = {}
    for item in filter(None, (p.strip() for p in text.split(","))):
        if "=" not in item:
            raise ValueError(f"{item!r}: want name=value")
        key, value = (s.strip() for s in item.split("=", 1))
        if key == "spike":
            values["spike"], values["every"] = parse_spike(value)
            continue
        if key not in _KEYS:
            raise ValueError(f"{key!r}: not one of {', '.join(sorted(_KEYS | {'spike'}))}")
        values[key] = float(value)
    out = replace(base, **values)
    check(out)
    return out


def check(c: Conditions) -> None:
    for name in ("lag", "jitter", "rate", "queue", "spike", "every", "reorder_gap"):
        if getattr(c, name) < 0:
            raise ValueError(f"{name} must not be negative")
    for name in ("loss", "dup", "reorder"):
        if not 0 <= getattr(c, name) <= 100:
            raise ValueError(f"{name} must be 0 to 100 (%)")


@dataclass
class LinkStats:
    packets: int = 0  # came in
    sent: int = 0  # went out (a duplicate counts twice)
    lost: int = 0  # dropped by --loss
    queue_drops: int = 0  # dropped by a full --rate queue
    dups: int = 0
    reordered: int = 0
    stalled: int = 0  # held by a stall
    delay_sum: float = 0.0  # ms, over the packets sent
    delay_max: float = 0.0

    def line(self) -> str:
        avg = self.delay_sum / self.sent if self.sent else 0.0
        return (f"in {self.packets}, out {self.sent}, lost {self.lost}, queue drops {self.queue_drops}, "
                f"dup {self.dups}, reordered {self.reordered}, stalled {self.stalled}, "
                f"delay avg {avg:.1f} max {self.delay_max:.1f} ms")


class Link:
    """Decides when each packet of one direction comes out, in seconds on the
    caller's clock. Pure: no sockets, no clock of its own, so it is tested
    with made-up times."""

    def __init__(self, cond: Conditions, rng: random.Random, start: float):
        check(cond)
        self.c = cond
        self.rng = rng
        self.start = start  # stalls are counted from here
        self.last_out = -math.inf  # latest in-order release, so jitter keeps order
        self.free_at = -math.inf  # with a rate: when the link has sent all it holds
        self.stats = LinkStats()

    def stall_end(self, t: float) -> Optional[float]:
        """If t is inside a stall, when it ends. Stalls are [start + k*every,
        start + k*every + spike) for k = 1, 2, ..."""
        c = self.c
        if not (c.spike and c.every) or t < self.start:
            return None
        k = math.floor((t - self.start) / c.every)
        if k < 1:
            return None
        begin = self.start + k * c.every
        end = begin + c.spike / 1000.0
        return end if begin <= t < end else None

    def plan(self, now: float, size: int) -> List[float]:
        """Release times for one packet that came in at `now`: [] lost, two
        times for a duplicate."""
        c, st = self.c, self.stats
        st.packets += 1
        if c.loss and self.rng.random() * 100 < c.loss:
            st.lost += 1
            return []
        t = now
        held = self.stall_end(t)
        if held is not None:
            t = held
        if c.rate:
            start = max(t, self.free_at)
            if (start - t) * 1000 > c.queue:
                st.queue_drops += 1
                return []
            self.free_at = start + size * 8 / (c.rate * 1000.0)
            t = self.free_at
        out = t + c.lag / 1000.0
        if c.jitter:
            out += self.rng.uniform(-c.jitter, c.jitter) / 1000.0
        out = max(out, t)
        reordered = bool(c.reorder) and self.rng.random() * 100 < c.reorder
        if reordered:
            # Comes out after packets sent later than it.
            out = max(out, self.last_out) + c.gap_ms() / 1000.0
            st.reordered += 1
        else:
            out = max(out, self.last_out)  # jitter alone keeps the order, as on one route
        held2 = self.stall_end(out)
        if held2 is not None:
            out = held2  # arrives in a stall: comes out when it ends, with the rest
        if not reordered:
            self.last_out = out
        if held is not None or held2 is not None:
            st.stalled += 1
        times = [out]
        if c.dup and self.rng.random() * 100 < c.dup:
            st.dups += 1
            times.append(out + self.rng.uniform(0, max(c.jitter, 1.0)) / 1000.0)
        for x in times:
            d = (x - now) * 1000.0
            st.sent += 1
            st.delay_sum += d
            st.delay_max = max(st.delay_max, d)
        return times


@dataclass(order=True)
class _Due:
    at: float
    seq: int
    sock: socket.socket = field(compare=False)
    dest: Optional[Tuple[str, int]] = field(compare=False)  # None: the socket is connected
    data: bytes = field(compare=False)


def _quiet_resets(sock: socket.socket) -> None:
    """Windows reports an ICMP 'port unreachable' as an error on the next
    receive (the server or game went away); turn that off where possible."""
    if sys.platform == "win32":
        try:
            sock.ioctl(0x9800000C, False)  # SIO_UDP_CONNRESET
        except (AttributeError, OSError, ValueError):
            pass


class Relay:
    def __init__(self, listen: Tuple[str, int], server: Tuple[str, int], up: Conditions,
                 down: Conditions, seed: Optional[int], log, idle: float = 30.0):
        self.server = server
        self.up_c, self.down_c = up, down
        self.rng = random.Random(seed)
        self.log = log
        self.idle = idle
        self.sel = selectors.DefaultSelector()
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        _quiet_resets(self.front)
        self.front.bind(listen)
        self.front.setblocking(False)
        self.sel.register(self.front, selectors.EVENT_READ, None)
        self.start = time.monotonic()
        # One upstream socket per client address, so the server sees each client apart.
        self.clients: Dict[Tuple[str, int], Tuple[socket.socket, Link, Link, List[float]]] = {}
        self.due: List[_Due] = []
        self.seq = 0

    def port(self) -> int:
        return self.front.getsockname()[1]

    def _client(self, addr):
        entry = self.clients.get(addr)
        if entry is None:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            _quiet_resets(s)
            s.bind((self.front.getsockname()[0], 0))
            s.connect(self.server)
            s.setblocking(False)
            self.sel.register(s, selectors.EVENT_READ, addr)
            # Both directions stall at the same moments: one frozen connection.
            entry = (s, Link(self.up_c, self.rng, self.start), Link(self.down_c, self.rng, self.start),
                     [time.monotonic()])
            self.clients[addr] = entry
            self.log(f"client {addr[0]}:{addr[1]} -> server via local port {s.getsockname()[1]}")
        return entry

    def _queue(self, times, sock, dest, data):
        for at in times:
            self.seq += 1
            heapq.heappush(self.due, _Due(at, self.seq, sock, dest, data))

    def _read(self, key, now):
        sock = key.fileobj
        for _ in range(256):
            try:
                if key.data is None:
                    data, addr = sock.recvfrom(MAX_DATAGRAM)
                else:
                    data = sock.recv(MAX_DATAGRAM)
            except (BlockingIOError, InterruptedError):
                return
            except ConnectionResetError:
                continue
            if key.data is None:
                s, up, _, seen = self._client(addr)
                seen[0] = now
                self._queue(up.plan(now, len(data)), s, None, data)
            else:
                entry = self.clients.get(key.data)
                if entry is None:
                    return
                entry[3][0] = now
                self._queue(entry[2].plan(now, len(data)), self.front, key.data, data)

    def _flush(self, now):
        while self.due and self.due[0].at <= now:
            d = heapq.heappop(self.due)
            try:
                if d.dest is None:
                    d.sock.send(d.data)
                else:
                    d.sock.sendto(d.data, d.dest)
            except OSError:
                pass  # closed or unreachable: the packet is lost, as on a real link

    def _forget_idle(self, now):
        for addr, (s, up, down, seen) in list(self.clients.items()):
            if now - seen[0] > self.idle:
                self.log(f"client {addr[0]}:{addr[1]} idle {self.idle:g} s: forgotten")
                self.log(f"  up:   {up.stats.line()}")
                self.log(f"  down: {down.stats.line()}")
                self.sel.unregister(s)
                s.close()
                del self.clients[addr]

    def report(self):
        if not self.clients:
            self.log("no clients")
        for addr, (_, up, down, _) in self.clients.items():
            self.log(f"client {addr[0]}:{addr[1]}")
            self.log(f"  up:   {up.stats.line()}")
            self.log(f"  down: {down.stats.line()}")

    def run(self, seconds: float = 0.0, report_every: float = 10.0):
        end = self.start + seconds if seconds > 0 else math.inf
        next_report = self.start + report_every if report_every > 0 else math.inf
        next_idle = self.start + 1.0
        while True:
            now = time.monotonic()
            if now >= end:
                break
            wait = min(end, next_report, next_idle) - now
            if self.due:
                wait = min(wait, self.due[0].at - now)
            for key, _ in self.sel.select(max(0.0, min(wait, 0.5))):
                self._read(key, time.monotonic())
            now = time.monotonic()
            self._flush(now)
            if now >= next_report:
                self.report()
                next_report += report_every
            if now >= next_idle:
                self._forget_idle(now)
                next_idle = now + 1.0

    def close(self):
        for s, *_ in self.clients.values():
            s.close()
        self.front.close()
        self.sel.close()


def _addr(text: str, default_host: str) -> Tuple[str, int]:
    host, _, port = text.rpartition(":")
    return (host or default_host, int(port))


def _fine_timer():
    """Windows sleeps in 15.6 ms steps unless asked for 1 ms ones."""
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.winmm.timeBeginPeriod(1)
        except (OSError, AttributeError):
            pass


def build_conditions(a) -> Tuple[Conditions, Conditions]:
    base = Conditions(lag=a.lag / 2.0, jitter=a.jitter, loss=a.loss, dup=a.dup, reorder=a.reorder,
                      rate=a.rate, queue=a.queue)
    if a.spike:
        base.spike, base.every = parse_spike(a.spike)
    check(base)
    up = parse_direction(a.up, base) if a.up else base
    down = parse_direction(a.down, base) if a.down else base
    return up, down


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--listen", required=True, help="[host:]port the game connects to (host 127.0.0.1)")
    p.add_argument("--server", required=True, help="host:port of the co-op server")
    p.add_argument("--lag", type=float, default=0, help="added round trip in ms (half each way)")
    p.add_argument("--jitter", type=float, default=0, help="+- ms on each packet, each way")
    p.add_argument("--loss", type=float, default=0, help="%% of packets dropped, each way")
    p.add_argument("--dup", type=float, default=0, help="%% of packets sent twice")
    p.add_argument("--reorder", type=float, default=0, help="%% of packets held back past later ones")
    p.add_argument("--rate", type=float, default=0, help="link speed in kbit/s each way (0: none)")
    p.add_argument("--queue", type=float, default=500, help="with --rate: most ms a packet waits")
    p.add_argument("--spike", default="", help="stall: <ms>/<every s>, as 500/10")
    p.add_argument("--up", default="", help="client to server only: name=value,... (lag one way)")
    p.add_argument("--down", default="", help="server to client only: name=value,...")
    p.add_argument("--seed", type=int, default=None, help="random seed, for a repeatable run")
    p.add_argument("--seconds", type=float, default=0, help="stop after this long (0: Ctrl+C)")
    p.add_argument("--report-every", type=float, default=10, help="counts every N s (0: at the end)")
    p.add_argument("--log", default="", help="also append the lines to this file")
    p.add_argument("--name", default="", help="label for the lines, as A or B")
    a = p.parse_args(argv)
    try:
        up, down = build_conditions(a)
    except ValueError as e:
        p.error(str(e))

    logf = open(a.log, "a", encoding="utf-8") if a.log else None
    tag = f"netsim {a.name}: " if a.name else "netsim: "

    def log(line: str):
        text = time.strftime("%H:%M:%S ") + tag + line
        print(text, flush=True)
        if logf:
            logf.write(text + "\n")
            logf.flush()

    _fine_timer()
    relay = Relay(_addr(a.listen, "127.0.0.1"), _addr(a.server, "127.0.0.1"), up, down, a.seed, log)
    log(f"listening on {relay.front.getsockname()[0]}:{relay.port()}, server {a.server}, seed {a.seed}")
    log(f"up (to server):   {up.text()}")
    log(f"down (to client): {down.text()}")
    try:
        relay.run(a.seconds, a.report_every)
    except KeyboardInterrupt:
        pass
    finally:
        relay.report()
        relay.close()
        if logf:
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
