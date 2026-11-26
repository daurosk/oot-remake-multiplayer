#!/usr/bin/env bash
# The Python inside Longshot's Linux builder (hookshot/LINUX.txt: the AppImage the Linux kit unpacks and runs its tools with)
# must import what Character Select v0.2.2's tool needs (numpy, zstandard, astc_encoder, texture2ddecoder): the kit runs
# build-from-your-game/player.py with it. Downloads the pinned AppImage from the co-op mirror (SHA-256 checked), unpacks it
# as the kit does (--appimage-extract, no FUSE) and imports the four packages. Linux only; ~100 MB download.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
pin=$repo/hookshot/LINUX.txt
get() { sed -n "s/^$1=//p" "$pin" | tr -d '\r' | head -n1; }
name=$(get appimage) sha=$(get sha256) mirror=$(get mirror) url=$(get url)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
curl -fsSL --retry 3 -o "$name" "$mirror/$name" || curl -fsSL --retry 3 -o "$name" "$url"
[[ $(sha256sum "$name" | cut -d' ' -f1) == "$sha" ]] || { echo "FAIL the AppImage's SHA-256 differs from hookshot/LINUX.txt"; exit 1; }
chmod +x "$name"
./"$name" --appimage-extract >/dev/null
py=squashfs-root/usr/python/bin/python3
[[ -x $py ]] || { echo "FAIL no $py in the AppImage"; exit 1; }
if out=$("$py" -I -B -c 'import numpy, zstandard, astc_encoder, texture2ddecoder; print("numpy", numpy.__version__)' 2>&1); then
    echo "ok   the builder's Python imports numpy, zstandard, astc_encoder, texture2ddecoder ($out)"
else
    echo "FAIL the builder's Python lacks a package Character Select's tool needs: $(printf '%s\n' "$out" | tail -n1)"; exit 1
fi
