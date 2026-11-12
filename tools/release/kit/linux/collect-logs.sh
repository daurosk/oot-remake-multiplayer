#!/usr/bin/env bash
# Packs what is needed to see what went wrong into coop-logs-<pc>-<time>.tar.gz: the emulator
# logs, coop.ini, the server logs, VERSION.txt, coop-options.ini, the puppet pack's manifest,
# the newest crash dumps and a short note about this PC. Send that file back.
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
user=$KIT/emulator/user
stamp=$(date +%Y%m%d-%H%M%S)
host=$(hostname 2>/dev/null || uname -n)
out=$KIT/coop-logs-$host-$stamp.tar.gz
stage=$(mktemp -d "${TMPDIR:-/tmp}/coop-logs-XXXXXX")
found=0
pick() {  # pick FROM TO
    if [[ -f $1 ]]; then
        mkdir -p "$stage/$(dirname "$2")"
        cp -- "$1" "$stage/$2" && found=$((found + 1))
    else
        echo "Missing: $1"
    fi
}
for f in "$user"/log/*; do [[ -f $f ]] && pick "$f" "emulator/log/${f##*/}"; done
pick "$user/config/coop.ini" emulator/coop.ini
for f in "$KIT"/logs/*.log; do [[ -f $f ]] && pick "$f" "server/${f##*/}"; done
pick "$KIT/VERSION.txt" VERSION.txt
pick "$KIT/coop-options.ini" coop-options.ini
pick "$user/load/0400486024DEC000/coop-puppet/coop_puppet.json" emulator/coop_puppet.json
if [[ -d $user/crash_dumps ]]; then
    ls -t "$user"/crash_dumps 2>/dev/null | head -n 2 | while IFS= read -r f; do
        mkdir -p "$stage/emulator/crash_dumps" && cp -- "$user/crash_dumps/$f" "$stage/emulator/crash_dumps/"
    done
fi
{
    echo "pc: $host"
    echo "collected: $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "system: $(uname -srm)"
    [[ -f /etc/os-release ]] && . /etc/os-release && echo "distribution: ${PRETTY_NAME:-$NAME}"
    command -v lspci >/dev/null && lspci 2>/dev/null | grep -Ei 'vga|3d|display' | sed 's/^/gpu: /'
    command -v ip >/dev/null && ip -4 -o addr show 2>/dev/null | awk '$4 !~ /^169\.254\./ { print "ip: " $4 " (" $2 ")" }'
    pgrep -af -- "$KIT/(emulator/lichii.AppImage|server/coop_server)" 2>/dev/null | sed 's/^/running: /'
} >"$stage/about-this-pc.txt"
tar -czf "$out" -C "$stage" .
rm -rf "$stage"
echo "Wrote $out ($((found + 1)) files). Send this file back."
