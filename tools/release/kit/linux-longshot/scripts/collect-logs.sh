#!/usr/bin/env bash
# Packs what is needed to see what went wrong into coop-logs-<time>.tar.gz beside start-oot-coop.sh:
# the game's longshot_log.txt files, coop.ini, the kit's logs (start, build, game output),
# VERSION.txt, coop-options.ini, the puppet pack's manifest and a short note about this PC.
# bash start-oot-coop.sh --logs runs it. Send that file back.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
stamp=$(date +%Y%m%d-%H%M%S)
host=$(hostname 2>/dev/null || uname -n)
out=$KIT/coop-logs-$stamp.tar.gz  # no PC name: it is masked inside too
stage=$(mktemp -d "${TMPDIR:-/tmp}/coop-logs-XXXXXX")
found=0
pick() {  # pick FROM TO
    if [[ -f $1 ]]; then
        mkdir -p "$stage/$(dirname "$2")"
        cp -- "$1" "$stage/$2" && found=$((found + 1))
    else
        echo "Missing: ${1#"$KIT"/}"
    fi
}
for f in "$GAME"/longshot_log*.txt; do [[ -f $f ]] && pick "$f" "game/${f##*/}"; done
pick "$GAME/coop.ini" game/coop.ini
for f in "$LOGS"/*.log; do [[ -f $f ]] && pick "$f" "logs/${f##*/}"; done
pick "$KIT/VERSION.txt" VERSION.txt
pick "$DATA/coop-options.ini" coop-options.ini
pick "$GAME/mods/contents/0400486024DEC000/coop-puppet/coop_puppet.json" game/coop_puppet.json
{
    echo "pc: $host"
    echo "collected: $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "system: $(uname -srm)"
    [[ -f /etc/os-release ]] && . /etc/os-release && echo "distribution: ${PRETTY_NAME:-$NAME}"
    echo "glibc: $(getconf GNU_LIBC_VERSION 2>/dev/null)"
    echo "session: ${XDG_SESSION_TYPE:-?} ${XDG_CURRENT_DESKTOP:-} ${SteamDeck:+SteamDeck=$SteamDeck}"
    command -v lspci >/dev/null && lspci 2>/dev/null | grep -Ei 'vga|3d|display' | sed 's/^/gpu: /'
    command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo|apiVersion' | sed 's/^[[:space:]]*/vulkan: /'
    command -v ip >/dev/null && ip -4 -o addr show 2>/dev/null | awk '$4 !~ /^169\.254\./ { print "ip: " $4 " (" $2 ")" }'
    df -h "$DATA" 2>/dev/null | tail -n1 | sed 's/^/disk: /'
    kit_game_running && echo 'running: the game'
} >"$stage/about-this-pc.txt"
# What identifies a player is masked, as the Windows kit's log-mask.ps1 does: a set password
# (password = ..., server_password = ..., relay_key = ..., --password x) and an invite code become (removed),
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
    if (match(t, /^[ \t]*[A-Za-z0-9_.]*([Pp][Aa][Ss][Ss][Ww][Oo][Rr][Dd]|[Ss][Ee][Cc][Rr][Ee][Tt]|[Tt][Oo][Kk][Ee][Nn]|[Kk][Ee][Yy])[A-Za-z0-9_]*[ \t]*[=:][ \t]*[^ \t]/))
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
