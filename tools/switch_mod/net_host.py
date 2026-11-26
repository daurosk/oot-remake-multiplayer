"""Build and run the PC test of the ExeFS module's net code (switch/net).

    net_host.py [--llvm <LLVM bin>] [--out <folder>]                 build + selftest
    net_host.py ... --server <coop_server> --client <coop_test>       + end-to-end join

The net sources are compiled exactly as for the module (freestanding, the shim
headers in switch/net/shim, no C library headers), only for the PC; the test's
nn::socket runs on the PC's sockets (net_host_test.cpp).

End to end: starts coop_server (password, free port), a real coop_net client
(coop_test --connect, the client Longshot games use) and the freestanding client;
passes when the freestanding client opens WELCOME and sees the coop_test player,
and coop_test (exit 0) heard from the server the whole time.
"""
from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
NET = REPO / "switch" / "net"
sys.path.insert(0, str(HERE))
import deps  # noqa: E402
from build import find_tool  # noqa: E402

PROTOCOL_HPP = REPO / "coop" / "protocol" / "include" / "coop" / "protocol" / "messages.hpp"


def protocol_version() -> int:
    import re
    m = re.search(r"kProtocolVersion\s*=\s*(\d+)", PROTOCOL_HPP.read_text(encoding="utf-8"))
    if not m:
        sys.exit("net_host: kProtocolVersion not found in messages.hpp")
    return int(m.group(1))


def freestanding_flags(d: dict[str, Path]) -> list[str]:
    flags = ["-O2", "-ffreestanding", "-nostdlibinc", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
             "-I", str(NET / "shim"), "-I", str(d["enet"] / "include"), "-I", str(d["monocypher"] / "src")]
    if os.name == "nt":
        # ENet's enet.h picks win32.h on _WIN32; the module's platform is ours (shim/enet/unix.h).
        flags += ["-U_WIN32", "-U_WIN64"]
    return flags


def build(clang: str, out: Path, d: dict[str, Path]) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    objs = []
    ff = freestanding_flags(d)
    units = [(d["enet"] / s, ["-w"]) for s in deps.ENET_SOURCES]
    units.append((d["monocypher"] / "src" / "monocypher.c", ["-w"]))
    units.append((NET / "coop_join.cpp", ["-std=c++20", "-Wall", "-Wextra", "-Werror"]))
    units.append((NET / "enet_nx.cpp", ["-std=c++20", "-Wall", "-Wextra", "-Werror",
                                        "-Dsockaddr=sockaddr_bsd", "-Dpollfd=pollfd_nx"]))
    for src, extra in units:
        obj = out / (src.parent.name + "-" + src.name + ".o")
        lang = ["-x", "c"] if src.suffix == ".c" else []
        r = subprocess.run([clang, *lang, *ff, *extra, "-c", str(src), "-o", str(obj)])
        if r.returncode:
            sys.exit(f"net_host: compile failed: {src}")
        objs.append(str(obj))
    exe = out / ("net_host_test" + (".exe" if os.name == "nt" else ""))
    clangxx = clang.replace("clang.exe", "clang++.exe") if clang.endswith("clang.exe") else clang + "++"
    cmd = [clangxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(HERE / "net_host_test.cpp"), *objs,
           "-o", str(exe)]
    if os.name == "nt":
        cmd += ["-lws2_32", "-D_CRT_SECURE_NO_WARNINGS"]
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit("net_host: link failed")
    return exe


def free_udp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def end_to_end(exe: Path, server: str, client: str, out: Path) -> int:
    port = free_udp_port()
    pw = "exefs-crossplay-test"
    proto = protocol_version()
    logs = {k: open(out / f"e2e-{k}.log", "w", encoding="utf-8") for k in ("server", "coop_test", "freestanding")}
    srv = subprocess.Popen([server, "--port", str(port), "--bind", "127.0.0.1", "--password", pw,
                            "--run-for-ms", "40000"], stdout=logs["server"], stderr=subprocess.STDOUT)
    time.sleep(1.0)
    ct = subprocess.Popen([client, "--connect", f"127.0.0.1:{port}", "--password", pw, "--clients", "1",
                           "--seconds", "25", "--name", "longshot"], stdout=logs["coop_test"], stderr=subprocess.STDOUT)
    time.sleep(3.0)
    fs = subprocess.run([str(exe), "join", "127.0.0.1", str(port), pw, "emulator", str(proto), "15", "1"],
                        stdout=logs["freestanding"], stderr=subprocess.STDOUT, timeout=60)
    ct_rc = ct.wait(timeout=60)
    srv.terminate()
    srv.wait(timeout=20)
    for f in logs.values():
        f.close()
    text = {k: (out / f"e2e-{k}.log").read_text(encoding="utf-8", errors="replace") for k in logs}
    print("--- freestanding client ---\n" + text["freestanding"])
    print("--- server (join lines) ---")
    for line in text["server"].splitlines():
        if any(w in line.lower() for w in ("join", "welcome", "refus", "player", "left")):
            print(line)
    print(f"--- coop_test exit {ct_rc} (tail) ---\n" + "\n".join(text["coop_test"].splitlines()[-6:]))
    ok = fs.returncode == 0 and ct_rc == 0
    print(f"e2e: {'PASS' if ok else 'FAIL'} (freestanding exit {fs.returncode}, coop_test exit {ct_rc}, protocol {proto})")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--llvm")
    ap.add_argument("--out", default=str(REPO / "build" / "switch-net-host"))
    ap.add_argument("--server")
    ap.add_argument("--client")
    a = ap.parse_args()
    d = deps.fetch_all()
    out = Path(a.out)
    exe = build(find_tool("clang", a.llvm), out, d)
    r = subprocess.run([str(exe), "selftest"])
    if r.returncode:
        return 1
    if a.server and a.client:
        return end_to_end(exe, a.server, a.client, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
