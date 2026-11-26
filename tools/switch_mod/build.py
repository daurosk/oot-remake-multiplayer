#!/usr/bin/env python3
"""Build the ExeFS prototype (switch/proto) into an NSO code module with LLVM.

    build.py [--llvm <LLVM bin folder>] [--out <folder>] [--protocol-from <sha>]

Needs clang and ld.lld with the AArch64 target (any stock LLVM build). Writes
<out>/module.elf, <out>/subsdk1 and <out>/subsdk1.entries (default out:
build/switch-proto), after lift_check.py has passed (the module stays liftable by a
recomp). Install by copying subsdk1 into the emulator's
<user>/load/0400486024DEC000/<mod>/exefs/.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SRC = REPO / "switch" / "proto"
sys.path.insert(0, str(HERE))
import deps  # noqa: E402
import elf2nso  # noqa: E402
import lift_check  # noqa: E402

NET = REPO / "switch" / "net"
PROTOCOL_HPP = REPO / "coop" / "protocol" / "include" / "coop" / "protocol" / "messages.hpp"


def protocol_version(sha: str | None = None) -> int:
    """kProtocolVersion from messages.hpp: this checkout, or the file at commit <sha> (git show),
    so a run builds the module for the protocol of the game build it plays with, never a typed number."""
    import re
    if sha:
        rel = PROTOCOL_HPP.relative_to(REPO).as_posix()
        r = subprocess.run(["git", "-C", str(REPO), "show", f"{sha}:{rel}"], capture_output=True, text=True,
                           encoding="utf-8")
        if r.returncode != 0:
            sys.exit(f"build: --protocol-from {sha}: git show failed: {r.stderr.strip()}")
        text = r.stdout
    else:
        text = PROTOCOL_HPP.read_text(encoding="utf-8")
    m = re.search(r"kProtocolVersion\s*=\s*(\d+)", text)
    if not m:
        sys.exit("build: kProtocolVersion not found in messages.hpp")
    return int(m.group(1))


def c_string(text: str) -> str:
    if '"' in text or "\\" in text or not text.isprintable():
        sys.exit(f"build: {text!r}: no quotes, backslashes or control characters")
    return '"' + text + '"'


def write_join_config(out: Path, join: str, password: str, name: str, protocol: int) -> None:
    host, _, port = join.rpartition(":")
    parts = host.split(".")
    if len(parts) != 4 or not all(p.isdigit() and int(p) < 256 for p in parts) or not port.isdigit():
        sys.exit("build: --join must be IPv4:port, for example 127.0.0.1:27520")
    if not 1 <= len(name) <= 15:
        sys.exit("build: --name must be 1 to 15 characters")
    if not password:
        sys.exit("build: --password must not be empty (coop_server needs one)")
    lines = [
        "// Written by tools/switch_mod/build.py: the server this module joins.",
        "#pragma once",
        f"#define COOP_JOIN_IP {{{', '.join(parts)}}}",
        f"#define COOP_JOIN_PORT {int(port)}",
        f"#define COOP_JOIN_PASSWORD {c_string(password)}",
        f"#define COOP_JOIN_NAME {c_string(name)}",
        f"#define COOP_PROTOCOL_VERSION {protocol}  // coop/protocol messages.hpp kProtocolVersion",
    ]
    (out / "coop_join_config.h").write_text("\n".join(lines) + "\n", encoding="utf-8")


# ENet calls a few optional hooks through pointers: host->compressor (compress, decompress,
# destroy), host->checksum, host->intercept and packet->freeCallback. switch/net never sets
# any of them, and ENet calls each only when it is not null, so these branches never run.
# They are the only indirect calls allowed outside the slot/PLT shapes; a lifting recomp
# needs an entries line or a trap for them (docs/exefs.md, "Lifting"). Any other function
# with an unknown indirect call still fails the build.
ENET_NULL_GUARDED = {"enet_host_destroy", "enet_host_compress", "enet_packet_destroy",
                     "enet_protocol_send_outgoing_commands", "enet_host_service",
                     "enet_protocol_receive_incoming_commands", "enet_protocol_handle_incoming_commands"}

CFLAGS = [
    "--target=aarch64-none-elf", "-march=armv8-a", "-O2", "-std=c++20",
    "-fPIC", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
    "-funwind-tables", "-fno-jump-tables", "-fvisibility=hidden", "-ffixed-x18",
    "-nostdlib", "-Wall", "-Wextra", "-Werror",
]
LDFLAGS = [
    "-shared", "-z", "now", "--hash-style=sysv", "--build-id=sha1", "-z", "noexecstack",
    "-e", "__module_start", "--eh-frame-hdr", "--apply-dynamic-relocs",
]


def find_tool(name: str, llvm: str | None) -> str:
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = [Path(llvm) / exe] if llvm else []
    if os.name == "nt":
        candidates.append(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / exe)
    for c in candidates:
        if c.is_file():
            return str(c)
    found = shutil.which(name)
    if not found:
        sys.exit(f"build: {name} not found (pass --llvm <LLVM bin folder>)")
    return found


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"build: failed: {' '.join(cmd)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--llvm")
    ap.add_argument("--out", default=str(REPO / "build" / "switch-proto"))
    ap.add_argument("--join", default="127.0.0.1:27520", help="server the module joins (IPv4:port)")
    ap.add_argument("--password", default="exefs-crossplay-test", help="the server's join password")
    ap.add_argument("--name", default="emulator", help="nickname, 1 to 15 characters")
    ap.add_argument("--protocol-from", metavar="SHA",
                    help="take kProtocolVersion from messages.hpp at this commit (the game build played with); "
                         "the handshake the module speaks must not differ between the two")
    a = ap.parse_args()
    clang = find_tool("clang", a.llvm)
    lld = find_tool("ld.lld", a.llvm)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    protocol = protocol_version(a.protocol_from)
    print(f"build: protocol {protocol}" + (f" (from {a.protocol_from})" if a.protocol_from else ""))
    write_join_config(out, a.join, a.password, a.name, protocol)
    d = deps.fetch_all()
    inc = ["-nostdlibinc", "-I", str(NET / "shim"), "-I", str(d["enet"] / "include"),
           "-I", str(d["monocypher"] / "src"), "-I", str(out)]
    cflags_c = [f for f in CFLAGS if f not in ("-std=c++20", "-fno-rtti", "-Wall", "-Wextra", "-Werror")]
    units = [(SRC / "start.s", ["--target=aarch64-none-elf", "-march=armv8-a"]),
             (SRC / "main.cpp", [*CFLAGS, *inc]),
             (NET / "coop_join.cpp", [*CFLAGS, *inc]),
             (NET / "enet_nx.cpp", [*CFLAGS, *inc])]
    units += [(d["enet"] / s, ["-x", "c", *cflags_c, *inc, "-w"]) for s in deps.ENET_SOURCES]
    units.append((d["monocypher"] / "src" / "monocypher.c", ["-x", "c", *cflags_c, *inc, "-w"]))
    objs = []
    for src, flags in units:
        obj = out / (src.parent.name + "-" + src.name + ".o")
        run([clang, *flags, "-c", str(src), "-o", str(obj)])
        objs.append(str(obj))
    elf = out / "module.elf"
    run([lld, *LDFLAGS, "-T", str(SRC / "module.ld"), "--version-script", str(SRC / "exports.map"),
         *objs, "-o", str(elf)])
    try:
        problems, entries = lift_check.check(lift_check.Elf(elf.read_bytes()))
    except lift_check.LiftError as e:
        problems, entries = [str(e)], []
    allowed = [p for p in problems if any(f"(in {f})" in p for f in ENET_NULL_GUARDED)]
    problems = [p for p in problems if p not in allowed]
    for p in allowed:
        print(f"build: allowed (ENet optional hook, never set): {p}")
    if problems:
        sys.exit("build: the module would not lift:\n  " + "\n  ".join(problems))
    lift_check.write_entries(str(out / "subsdk1.entries"), entries)
    nso = elf2nso.build_nso(elf.read_bytes())
    (out / "subsdk1").write_bytes(nso)
    print(f"build: {out / 'subsdk1'} ({len(nso)} bytes), {len(entries)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
