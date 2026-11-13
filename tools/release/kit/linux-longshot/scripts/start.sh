#!/usr/bin/env bash
# What start-oot-coop.sh does (the Linux twin of the Windows kit's scripts/start.ps1):
#   1. checks this PC and the kit folder (each problem said in plain words, with what to do)
#   2. offers a new co-op version when there is one (data/coop-options.ini auto_update: ask, yes, no)
#   3. builds the game once from your game file (scripts/build.sh), or brings it up to date
#   4. starts the game; Host and Join are in its menu (the Multiplayer tab)
#
#   bash start-oot-coop.sh                     all of the above
#   bash start-oot-coop.sh --game FILE.nup     the game file, no question (also --exefs, --rebuild, --jobs N)
#   bash start-oot-coop.sh --no-update         no update check this time
#   bash start-oot-coop.sh --logs              collect the logs for a bug report, start nothing
# Started without a terminal (Steam's Game Mode, a desktop shortcut) it asks nothing: an update is
# left for the next start from a terminal, and a game that is not built yet is said in a dialog.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

no_update=0 build_args=()
while (($#)); do
    case $1 in
        --no-update) no_update=1 ;;
        --logs) exec bash "$KIT/scripts/collect-logs.sh" ;;
        --game | --exefs | --jobs) build_args+=("$1" "${2-}"); shift ;;
        --rebuild | --check-only) build_args+=("$1") ;;
        -h | --help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "Unknown option: $1 (bash start-oot-coop.sh --help lists them)" ;;
    esac
    shift
done

# Without a terminal, what is said goes to data/logs/start.log too.
mkdir -p "$LOGS" 2>/dev/null || true
if ! kit_tty && [[ -w $LOGS ]]; then
    exec > >(tee -a "$LOGS/start.log") 2>&1
    say "--- $(date '+%Y-%m-%d %H:%M:%S') start without a terminal"
fi

# --- 1: this PC and this folder -----------------------------------------------------------------
[[ $(id -u) != 0 ]] || fail 'Do not start this as root (no sudo): start it as yourself.'
[[ $(uname -s) == Linux ]] || fail 'This is the Linux kit. On Windows, take the Windows zip.'
[[ $(uname -m) == x86_64 ]] || fail "This kit is for 64-bit x86 PCs (this one is $(uname -m))."
glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{ print $2 }') || glibc=''
if [[ -n $glibc && $(kit_version_compare "v$glibc.0" v2.35.0) == -1 ]]; then
    fail "This Linux is too old (glibc $glibc; the game needs 2.35 or newer: Ubuntu 22.04, Debian 12, Fedora 36, SteamOS 3 or newer)."
fi
[[ -f $DATA/longshot/PIN && -f $KIT/scripts/build.sh ]] ||
    fail 'Files of the kit are missing. Unzip the whole zip again (not only start-oot-coop.sh).'
case $KIT in
    /tmp/* | /var/tmp/* | */.cache/*) [[ ! -d $DATA/game ]] &&
        fail "This kit is in a temporary folder ($KIT): it would be lost. Unzip it to a folder of your own (for example ~/OoT-Coop) and start it there." ;;
esac
touch "$DATA/.write-test" 2>/dev/null && rm -f "$DATA/.write-test" ||
    fail "This folder is read-only for you ($KIT). Unzip the kit to a folder of your own (for example ~/OoT-Coop)."
missing=()
command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || missing+=('curl')
for c in sha256sum find sed awk df; do command -v "$c" >/dev/null 2>&1 || missing+=("$c"); done
((${#missing[@]} == 0)) || fail "Missing on this PC: ${missing[*]}. Install it with your system's package manager (for example: sudo apt install ${missing[*]}), then start again."
if ! { ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; } && ! ls /usr/lib*/libvulkan.so.1 /usr/lib/*/libvulkan.so.1 >/dev/null 2>&1; then
    warn 'no Vulkan driver found (libvulkan.so.1). The game needs Vulkan 1.3: install your graphics driver (Mesa or NVIDIA) and the Vulkan loader (Ubuntu: sudo apt install libvulkan1 mesa-vulkan-drivers).'
fi

# One start at a time for this kit.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$DATA/.start.lock"
    flock -n 9 || fail 'This kit is starting already (another window). Wait for it, or close it first.'
fi
kit_game_running && fail 'The game of this kit is running already. Switch to its window (or close it first).'

# --- 2: a new co-op version ---------------------------------------------------------------------
mode=$(kit_auto_update)
if ((!no_update)) && [[ $mode != no ]] && [[ -z ${OOT_COOP_NO_UPDATE-} ]]; then
    status=$(KIT_QUIET_FETCH=1 bash "$KIT/scripts/update.sh" --status 2>/dev/null || true)
    case $status in
        'newer '* | unfinished)
            go=0
            if [[ $status == unfinished ]]; then go=1
            elif [[ $mode == yes ]]; then go=1
            elif kit_tty; then
                say "Co-op ${status#newer } is out (this kit: $(kit_version)). Every player needs the same version."
                read -r -t 60 -p 'Update now? Your saves and settings are kept. [Y/n] ' a || a=y
                [[ $a =~ ^[nN] ]] || go=1
            else
                say "Co-op ${status#newer } is out: start start-oot-coop.sh from a terminal to update."
            fi
            if ((go)); then
                set +e
                bash "$KIT/scripts/update.sh" --launch
                code=$?
                set -e
                ((code == 3)) && exec bash "$KIT/start-oot-coop.sh" --no-update "${build_args[@]}"
                ((code == 0)) || warn 'the update did not finish (said why above); starting this version.'
            fi ;;
        'failed '*) warn "no update check this time (${status#failed })." ;;
    esac
fi

# --- 3: the game, built once --------------------------------------------------------------------
if [[ -z $(kit_game_exe) ]] && kit_tty; then
    say ''
    say 'First start: co-op builds the game on this PC from your own game file. This happens once,'
    say 'takes 15 to 60 minutes and about 60 GB free space, and needs the internet for Longshot'"'"'s tools.'
fi
bash "$KIT/scripts/build.sh" "${build_args[@]}"
for a in "${build_args[@]}"; do [[ $a == --check-only ]] && exit 0; done
exe=$(kit_game_exe) || fail 'The game is not built (said why above).'

# A menu entry once (Steam Deck: Steam's "Add a Non-Steam Game" lists it), for this kit.
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
entry=$apps/oot-coop.desktop
if [[ -d $(dirname "$apps") && -z ${OOT_COOP_NO_SHORTCUT-} ]] && ! grep -qF -- "$KIT/start-oot-coop.sh" "$entry" 2>/dev/null; then
    mkdir -p "$apps"
    # (the desktop entry spec: inside "...", \ " ` $ are escaped with a backslash)
    q=${KIT//\\/\\\\}; q=${q//\"/\\\"}; q=${q//\`/\\\`}; q=${q//\$/\\\$}
    {
        echo '[Desktop Entry]'
        echo 'Type=Application'
        echo 'Name=OoT Co-op'
        echo 'Comment=Ocarina of Time co-op on Longshot: host or join in the game'
        echo "Exec=bash \"$q/start-oot-coop.sh\""
        echo "Path=$KIT"
        [[ -f $GAME/longshot.png ]] && echo "Icon=$GAME/longshot.png"
        echo 'Terminal=false'
        echo 'Categories=Game;'
    } >"$entry"
    say "A menu entry 'OoT Co-op' was added (Steam Deck: add it to Steam with 'Add a Non-Steam Game')."
fi

# --- 4: the game --------------------------------------------------------------------------------
step 'Starting the game: Host or Join in its menu (Multiplayer)'
# (as Longshot's own builder/linux/longshot-launch: nothing of a launcher's environment goes in; its
# own update check is off, the kit's updater brings Longshot up to date with co-op)
while IFS='=' read -r v _; do
    case $v in LONGSHOT_* | PYTHON* | APPDIR | APPIMAGE | ARGV0 | OWD | LD_LIBRARY_PATH) unset "$v" ;; esac
done < <(env)
export LONGSHOT_NO_UPDATE_CHECK=1 OOT_COOP_KIT=$DATA
cd "$GAME"
if kit_tty; then
    exec ./longshot
fi
exec ./longshot >>"$LOGS/game-output.log" 2>&1
