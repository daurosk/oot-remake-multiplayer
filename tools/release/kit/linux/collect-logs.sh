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
# What identifies a player is masked, as the Windows kit's log-mask.ps1 does: a set password
# (password = ..., server_password = ..., --password x) and an invite code become (removed),
# MAC addresses <mac>, each IPv4 address its kind with a number (<lan-1>, <public-1>, ...; the
# same address gets the same tag in every file), a host name as the server <host>, the user
# name (in /home/<name> and on its own) <user> and the PC name <pc>. Crash dumps stay as they are.
mask_awk='
function kind(a,   o, n, i) {
    n = split(a, o, ".")
    for (i = 1; i <= 4; i++) if (length(o[i]) > 3 || o[i] + 0 > 255) return ""
    if (a == "0.0.0.0" || a == "255.255.255.255") return ""
    o[1] += 0; o[2] += 0
    if (o[1] == 127) return "loopback"
    if (o[1] == 10 || (o[1] == 172 && o[2] >= 16 && o[2] <= 31) || (o[1] == 192 && o[2] == 168) || (o[1] == 169 && o[2] == 254)) return "lan"
    if (o[1] == 100 && o[2] >= 64 && o[2] <= 127) return "tailscale"
    if (o[1] == 25) return "hamachi"
    if (o[1] == 26) return "radmin"
    return "public"
}
function tag(k, a) {
    if (!(a in tags)) { count[k]++; tags[a] = "<" k "-" count[k] ">" }
    return tags[a]
}
function words(t, w, r,   out, before, after, lw, i) {
    if (length(w) < 3) return t
    out = ""; lw = tolower(w)
    while ((i = index(tolower(t), lw)) > 0) {
        before = substr(t, i - 1, 1); after = substr(t, i + length(w), 1)
        if ((i == 1 || before !~ /[A-Za-z0-9_]/) && after !~ /[A-Za-z0-9_]/) out = out substr(t, 1, i - 1) r
        else out = out substr(t, 1, i - 1 + length(w))
        t = substr(t, i + length(w))
    }
    return out t
}
FNR == 1 { if (outfile != "") close(outfile); outfile = FILENAME ".masked" }
{
    t = $0
    if (match(t, /^[ \t]*[A-Za-z0-9_.]*([Pp][Aa][Ss][Ss][Ww][Oo][Rr][Dd]|[Ss][Ee][Cc][Rr][Ee][Tt]|[Tt][Oo][Kk][Ee][Nn])[A-Za-z0-9_]*[ \t]*[=:][ \t]*[^ \t]/))
        t = substr(t, 1, RLENGTH - 1) "(removed)"
    gsub(/--?[A-Za-z0-9-]*password[A-Za-z0-9-]*[ \t=]+[^ \t-][^ \t]*/, "--password (removed)", t)
    gsub(/[Ii]nvite code:?[ \t]*[A-Z2-9][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-]+/, "invite code: (removed)", t)
    gsub(/[0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f]/, "<mac>", t)
    out = ""
    while (match(t, /[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/)) {
        a = substr(t, RSTART, RLENGTH)
        before = substr(t, RSTART - 1, 1); after = substr(t, RSTART + RLENGTH, 2)
        k = (RSTART > 1 && before ~ /[0-9.vV]/) || after ~ /^\.[0-9]/ ? "" : kind(a)
        out = out substr(t, 1, RSTART - 1) (k == "" ? a : tag(k, a))
        t = substr(t, RSTART + RLENGTH)
    }
    t = out t
    if (match(t, /^[ \t]*server[ \t]*=[ \t]*[A-Za-z][A-Za-z0-9_.-]*/) && t !~ /=[ \t]*localhost/) {
        p = t; sub(/=[ \t]*[A-Za-z][A-Za-z0-9_.-]*/, "= <host>", p); t = p
    }
    gsub("/home/" user "(/|$)", "/home/<user>/", t)
    if (length(user) >= 5 && user !~ /^(admin|administrator|users?|owner|player|guest|link|zelda|navi|default|public|deck)$/) t = words(t, user, "<user>")
    t = words(t, pc, "<pc>")
    print t > outfile
}'
find "$stage" -type f ! -path '*/crash_dumps/*' | while IFS= read -r f; do
    awk -v user="$(id -un 2>/dev/null)" -v pc="$host" "$mask_awk" "$f" && mv -f "$f.masked" "$f"
done
tar -czf "$out" -C "$stage" .
rm -rf "$stage"
echo "Wrote $out ($((found + 1)) files). Send this file back."
echo 'IP addresses, MAC addresses, passwords, the invite code, your user name and PC name are masked in it.'
