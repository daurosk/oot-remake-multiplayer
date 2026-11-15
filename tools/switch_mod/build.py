#!/usr/bin/env python3
"""Build the ExeFS prototype (switch/proto) into an NSO code module with LLVM.

    build.py [--llvm <LLVM bin folder>] [--out <folder>]

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
import elf2nso  # noqa: E402
import lift_check  # noqa: E402

CFLAGS = [
    "--target=aarch64-none-elf", "-march=armv8-a", "-O2", "-std=c++20",
    "-fPIC", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
    "-funwind-tables", "-fno-jump-tables", "-fvisibility=hidden",
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
    a = ap.parse_args()
    clang = find_tool("clang", a.llvm)
    lld = find_tool("ld.lld", a.llvm)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in ("start.s", "main.cpp"):
        obj = out / (src + ".o")
        flags = CFLAGS if src.endswith(".cpp") else ["--target=aarch64-none-elf", "-march=armv8-a"]
        run([clang, *flags, "-c", str(SRC / src), "-o", str(obj)])
        objs.append(str(obj))
    elf = out / "module.elf"
    run([lld, *LDFLAGS, "-T", str(SRC / "module.ld"), "--version-script", str(SRC / "exports.map"),
         *objs, "-o", str(elf)])
    try:
        problems, entries = lift_check.check(lift_check.Elf(elf.read_bytes()))
    except lift_check.LiftError as e:
        problems, entries = [str(e)], []
    if problems:
        sys.exit("build: the module would not lift:\n  " + "\n  ".join(problems))
    lift_check.write_entries(str(out / "subsdk1.entries"), entries)
    nso = elf2nso.build_nso(elf.read_bytes())
    (out / "subsdk1").write_bytes(nso)
    print(f"build: {out / 'subsdk1'} ({len(nso)} bytes), {len(entries)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
