# SPDX-License-Identifier: GPL-3.0-or-later
"""ENet and Monocypher sources for the ExeFS module's net code, fetched, never copied into the tree.

The pins are read from cmake/enet.cmake and cmake/monocypher.cmake (GIT_REPOSITORY + GIT_TAG),
so the module and the PC build always use the same commits. Fetched once into
build/switch-deps/<name> (git clone + checkout of that commit).
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

ENET_SOURCES = ("host.c", "list.c", "packet.c", "peer.c", "protocol.c")  # not unix.c/win32.c/callbacks.c/compress.c


def pin(name: str) -> tuple[str, str]:
    text = (REPO / "cmake" / f"{name}.cmake").read_text(encoding="utf-8")
    repo = re.search(r"GIT_REPOSITORY\s+(\S+)", text)
    tag = re.search(r"GIT_TAG\s+([0-9a-f]{40})", text)
    if not repo or not tag:
        sys.exit(f"deps: no GIT_REPOSITORY / 40-hex GIT_TAG in cmake/{name}.cmake")
    return repo.group(1), tag.group(1)


def fetch(name: str, root: Path) -> Path:
    url, commit = pin(name)
    dst = root / name
    if (dst / ".git").is_dir():
        head = subprocess.run(["git", "-C", str(dst), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if head == commit:
            return dst
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--quiet", url, str(dst)], check=True)
    subprocess.run(["git", "-C", str(dst), "fetch", "--quiet", "origin", commit], check=False)
    subprocess.run(["git", "-C", str(dst), "checkout", "--quiet", "--detach", commit], check=True)
    return dst


def fetch_all(root: Path | None = None) -> dict[str, Path]:
    root = root or REPO / "build" / "switch-deps"
    return {"enet": fetch("enet", root), "monocypher": fetch("monocypher", root)}


if __name__ == "__main__":
    for k, v in fetch_all().items():
        print(f"{k}: {v} at {pin(k)[1]}")
