#!/usr/bin/env python3
"""Print the UDP datagrams the ExeFS prototype sends (default 127.0.0.1:27520, LANE1's port).

    listen.py [--port 27520] [--seconds 600]
"""
from __future__ import annotations

import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=27520)
    ap.add_argument("--seconds", type=float, default=600)
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", a.port))
    s.settimeout(1.0)
    print(f"listen: waiting on 127.0.0.1:{a.port}", flush=True)
    end = time.monotonic() + a.seconds
    got = 0
    while time.monotonic() < end:
        try:
            data, peer = s.recvfrom(2048)
        except socket.timeout:
            continue
        got += 1
        print(f"listen: {peer[0]}:{peer[1]} {data!r}", flush=True)
    print(f"listen: {got} datagram(s)")
    return 0 if got else 1


if __name__ == "__main__":
    sys.exit(main())
