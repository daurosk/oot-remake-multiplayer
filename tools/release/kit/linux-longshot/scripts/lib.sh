# Shared by the Linux kit's scripts (sourced, not run). The kit:
#   start-squadala.sh   the one file players start
#   README.txt
#   scripts/            these scripts (players never open it)
#   data/               the server, Longshot's pin and our patches, the co-op module, docs, licences;
#                       and what the first start makes: builder/ (Longshot's builder and its tools),
#                       work/ (the build's work folder, kept for quick rebuilds), game/ (the built
#                       game), logs/, game-files.txt, coop-options.ini (your settings)
# shellcheck shell=bash

KIT=${KIT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
DATA=$KIT/data
LOGS=$DATA/logs
GAME=$DATA/game
REPO=daurosk/oot-remake-multiplayer

# A terminal to talk to? (Steam's Game Mode and a desktop shortcut start the kit without one.)
kit_tty() { [[ -t 0 && -t 1 ]]; }

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
warn() { printf 'Note: %s\n' "$*" >&2; }

# A message for a player who has no terminal open: a desktop dialog when there is one.
kit_dialog() {
    kit_tty && return 0
    [[ -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]] || return 0
    if command -v zenity >/dev/null 2>&1; then
        zenity --info --title="OoT Co-op" --text="$1" >/dev/null 2>&1 || true
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --title "OoT Co-op" --msgbox "$1" >/dev/null 2>&1 || true
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send "OoT Co-op" "$1" >/dev/null 2>&1 || true
    fi
}

fail() {
    printf '\nOoT Co-op: %s\n' "$*" >&2
    kit_dialog "$*"
    exit 1
}

# pin_get FILE KEY: the value of 'KEY=value' in a pin file (hookshot/PIN, LINUX.txt).
pin_get() {
    local line
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        [[ $line == "$2="* ]] && { printf '%s\n' "${line#*=}"; return 0; }
    done <"$1"
    return 1
}

# fetch URL OUT: a download with curl or wget (OOT_COOP_FETCH_LOCAL=1, for tests: URL may be a local file).
fetch() {
    # (OOT_COOP_TEST_BLOCK_HOST, tests only: a host matching that regex fails as if blocked)
    if [[ -n ${OOT_COOP_TEST_BLOCK_HOST-} ]]; then
        local h=${1#*://}; h=${h%%/*}
        [[ $h =~ $OOT_COOP_TEST_BLOCK_HOST ]] && return 1
    fi
    case $1 in
        http://* | https://*)
            if command -v curl >/dev/null 2>&1; then
                curl -fL --retry 3 --connect-timeout 30 ${KIT_QUIET_FETCH:+-s} -o "$2" "$1"
            else
                wget ${KIT_QUIET_FETCH:+-q} -t 3 -T 60 -O "$2" "$1"
            fi ;;
        *) [[ -n ${OOT_COOP_FETCH_LOCAL-} ]] && cp -- "${1#file://}" "$2" ;;
    esac
}

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

# fetch_verified OUT SHA256 URL...: OUT from the first URL (Longshot's own, then the co-op mirror) whose file has
# that SHA-256 (kit-mirror, 0.11.1: players' downloads from git.magicbean.dev fail now and then). Each URL gets 3
# tries, waiting KIT_FETCH_WAIT (2) seconds and then twice as long; a file that differs is deleted, never used.
# On failure KIT_FETCH_WHY says, host by host, what went wrong.
fetch_verified() {
    local out=$1 want=$2 url h i ok wait
    local -a why=()
    shift 2
    KIT_FETCH_WHY=
    for url; do
        [[ -n $url ]] || continue
        h=${url#*://}; h=${h%%/*}; [[ -n $h ]] || h='this PC'
        ok= wait=${KIT_FETCH_WAIT:-2}
        for i in 1 2 3; do
            rm -f "$out.part"
            if fetch "$url" "$out.part"; then ok=1; break; fi
            if ((i < 3)); then say "  $h failed (try $i of 3), trying again in ${wait} s ..."; sleep "$wait"; wait=$((wait * 2)); fi
        done
        if [[ -z $ok ]]; then why+=("$h: the download failed"); continue; fi
        if [[ $(sha256_of "$out.part") != "$want" ]]; then
            rm -f "$out.part"; why+=("$h: the file is damaged (its SHA-256 differs from the one this kit pins)"); continue
        fi
        mv -f "$out.part" "$out"
        ((${#why[@]})) && say "  got it from $h"
        return 0
    done
    rm -f "$out.part"
    KIT_FETCH_WHY=$(IFS=';'; printf '%s' "${why[*]}")
    KIT_FETCH_WHY=${KIT_FETCH_WHY//;/; }
    return 1
}

# fetch_fail_text FILE TEST_URL HAD_MIRROR: what a player reads when fetch_verified failed everywhere.
fetch_fail_text() {
    local tried="Longshot's servers"
    [[ $3 == 1 ]] && tried="Longshot's servers and the co-op mirror on GitHub"
    printf '%s\n' "The download of $1 failed. The kit tried $tried, each 3 times: $KIT_FETCH_WHY." \
        'This is nearly always this PC or network: a VPN or proxy, a firewall, or a filtering DNS. Test: open this in a browser:' \
        "  $2" \
        'Start again to retry: what was downloaded is kept.'
}

# kit_offline [FORCE]: no internet here (github.com, where the kit's downloads are, cannot be reached in a few
# seconds). FORCE is OOT_COOP_OFFLINE as read before build.sh clears it: 1 says offline, 0 online, no probe (tests).
# Behind a proxy (curl and wget use https_proxy/all_proxy, a direct connect is often blocked there) and when it
# can't tell (no timeout tool: the connect could hang for minutes) it says online, and the download is tried.
kit_offline() {
    case ${1-} in 1) return 0 ;; 0) return 1 ;; esac
    [[ -n ${https_proxy-}${HTTPS_PROXY-}${all_proxy-}${ALL_PROXY-} ]] && return 1
    command -v timeout >/dev/null 2>&1 || return 1
    timeout 4 bash -c '</dev/tcp/github.com/443' 2>/dev/null && return 1
    return 0
}

# unzip_to ZIP DIR: unzip, bsdtar, or a Python (the system's, else the builder's).
unzip_to() {
    mkdir -p "$2"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$1" -d "$2"
    elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xf "$1" -C "$2"
    else
        local py
        py=$(command -v python3 || true)
        [[ -n $py ]] || py=$DATA/builder/image/usr/python/bin/python3
        [[ -x $py ]] || { warn 'no unzip, bsdtar or python3 to unpack a zip with'; return 1; }
        "$py" -I -m zipfile -e "$1" "$2"
    fi
}

# kit_version_compare A B: prints -1, 0 or 1. v0.1.0 > v0.1.0-beta2; beta2 < beta10.
# Something that is not a version counts as the oldest. (As tools/release/kit/linux/kit-lib.sh.)
kit_version_compare() {
    local a=$1 b=$2 re='^v?([0-9]+)\.([0-9]+)\.([0-9]+)(-([A-Za-z0-9.]+))?$'
    local ok_a=0 ok_b=0 ca=() cb=() pa='' pb=''
    if [[ $a =~ $re ]]; then ok_a=1; ca=("${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"); pa=${BASH_REMATCH[5]}; fi
    if [[ $b =~ $re ]]; then ok_b=1; cb=("${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"); pb=${BASH_REMATCH[5]}; fi
    if ((!ok_a || !ok_b)); then echo $((ok_a - ok_b)); return; fi
    local i
    for i in 0 1 2; do
        if ((10#${ca[$i]} != 10#${cb[$i]})); then
            ((10#${ca[$i]} < 10#${cb[$i]})) && echo -1 || echo 1
            return
        fi
    done
    [[ -z $pa && -z $pb ]] && { echo 0; return; }
    [[ -z $pa ]] && { echo 1; return; }
    [[ -z $pb ]] && { echo -1; return; }
    awk -v a="$pa" -v b="$pb" '
        function run(s, i,   c) { c = substr(s, i, 1); return c ~ /[0-9]/ }
        BEGIN {
            i = 1; j = 1
            while (i <= length(a) && j <= length(b)) {
                if (run(a, i) && run(b, j)) {
                    x = ""; while (i <= length(a) && run(a, i)) x = x substr(a, i++, 1)
                    y = ""; while (j <= length(b) && run(b, j)) y = y substr(b, j++, 1)
                    if (x + 0 != y + 0) { print (x + 0 < y + 0) ? -1 : 1; exit }
                } else {
                    x = substr(a, i++, 1); y = substr(b, j++, 1)
                    if (x != y) { print (x < y) ? -1 : 1; exit }
                }
            }
            if (i <= length(a)) print 1; else if (j <= length(b)) print -1; else print 0
        }'
}

# The release this kit was made from: VERSION.txt's first line "kit: ... (release <tag>)".
kit_version() {
    local first
    [[ -f $KIT/VERSION.txt ]] || return 0
    IFS= read -r first <"$KIT/VERSION.txt" || true
    [[ $first =~ \(release\ ([^\)]+)\) ]] && printf '%s\n' "${BASH_REMATCH[1]}"
}

# The kit's auto_update setting (data/coop-options.ini): ask (default), yes or no.
kit_auto_update() {
    local v
    v=$(sed -n 's/^[[:space:]]*auto_update[[:space:]]*=[[:space:]]*\([A-Za-z]*\).*/\1/p' "$DATA/coop-options.ini" 2>/dev/null | tail -n1)
    case ${v,,} in yes | no) printf '%s\n' "${v,,}" ;; *) echo ask ;; esac
}

# The built game: data/game/longshot. The kit builds without Longshot's packaged-builder layout (no
# bin/ and launcher script), so the game, its renderer (longshot_vk.so), SDL, our module, coop.ini and
# mods/ are in one folder that Longshot (host/hos_paths.h) and the module (its GameDir) both read.
kit_game_exe() { [[ -x $GAME/longshot && -f $GAME/image.bin ]] && printf '%s\n' "$GAME/longshot"; }

# Steam Input outside Steam (player report 202; the Windows kit's Get-KitSteamInputHint): with Steam open and the game
# not started by Steam, Steam gives a Steam Controller (or a pad Steam Input runs) its desktop layout, a mouse and
# keyboard. Steam sets SteamGameId, SteamAppId, SteamClientLaunch or SteamOverlayGameId for what it starts (a non-Steam
# game too). Says the warning, never stops. $1: the "Steam is running" check (tests), else kit_steam_running.
kit_steam_running() {
    local p c
    for p in /proc/[0-9]*/comm; do
        read -r c <"$p" 2>/dev/null && [[ $c == steam ]] && return 0
    done
    return 1
}
kit_steam_input_hint() {
    local n v
    for n in SteamGameId SteamAppId SteamClientLaunch SteamOverlayGameId; do
        v=${!n-}
        [[ -n ${v//[[:space:]]/} ]] && return 0
    done
    "${1:-kit_steam_running}" 2>/dev/null || return 0
    say 'Steam is open, but this game was not started from Steam. A Steam Controller (or a pad Steam Input runs) then works as a mouse and keyboard, not as a controller.'
    say '  To play with it: start start-squadala.sh from Steam (add it as a non-Steam game) with a gamepad controller layout, or turn Steam Input off for that pad.'
    return 0
}

# Is this kit's game running? (a process whose program is data/game/longshot)
kit_game_running() {
    local exe p
    [[ -e $GAME/longshot ]] || return 1
    exe=$(readlink -f "$GAME/longshot")
    for p in /proc/[0-9]*; do
        [[ $(readlink "$p/exe" 2>/dev/null) == "$exe" ]] && return 0
    done
    return 1
}
