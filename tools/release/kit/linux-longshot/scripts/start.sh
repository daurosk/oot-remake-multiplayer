#!/usr/bin/env bash
# What start-squadala.sh does (the Linux twin of the Windows kit's scripts/start.ps1):
#   1. checks this PC and the kit folder (each problem said in plain words, with what to do)
#   2. offers a new co-op version when there is one (data/coop-options.ini auto_update: ask, yes, no)
#   3. builds the game once from your game file (scripts/build.sh), or brings it up to date
#   4. starts the game; Host and Join are in its menu (the Multiplayer tab)
#
#   bash start-squadala.sh                     all of the above
#   bash start-squadala.sh --game FILE.nup     the game file, no question (also --exefs, --rebuild, --jobs N)
#   bash start-squadala.sh --dlss              NVIDIA DLSS: asks for NVIDIA's license once, builds the game with it
#                                              (--no-dlss: without it again)
#   bash start-squadala.sh --no-update         no update check this time
#   bash start-squadala.sh --logs              collect the logs for a bug report, start nothing
#   bash start-squadala.sh --pack              make the puppet pack again (how you see the others), start nothing
#   bash start-squadala.sh --problem-reports send|nosend|on|off   send problem reports after games (or not); local
#                                              reports at each start (or not); off turns both off (docs: problem-reports)
# Started without a terminal (Steam's Game Mode, a desktop shortcut) it asks nothing: an update is
# left for the next start from a terminal, and a game that is not built yet is said in a dialog.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

no_update=0 pack_only=0 build_args=()
while (($#)); do
    case $1 in
        --no-update) no_update=1 ;;
        --logs) exec bash "$KIT/scripts/collect-logs.sh" ;;
        --pack) pack_only=1 ;;
        --game | --exefs | --jobs) build_args+=("$1" "${2-}"); shift ;;
        --rebuild | --check-only | --dlss | --no-dlss) build_args+=("$1") ;;
        --problem-reports)
            pr_py=$DATA/builder/image/usr/python/bin/python3
            [[ -x $pr_py ]] || pr_py=$(command -v python3 || true)
            [[ -n $pr_py ]] || fail 'Problem reports need Python 3 (python3), which this PC does not have.'
            case ${2-} in send | nosend | on | off) ;; *) fail 'Use: --problem-reports send, nosend, on or off' ;; esac
            exec "$pr_py" -I -B "$KIT/scripts/problem_report.py" --kit "$KIT" "$2" ;;
        -h | --help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "Unknown option: $1 (bash start-squadala.sh --help lists them)" ;;
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
    fail 'Files of the kit are missing. Unzip the whole zip again (not only start-squadala.sh).'
case $KIT in
    /tmp/* | /var/tmp/* | */.cache/*) [[ ! -d $DATA/game ]] &&
        fail "This kit is in a temporary folder ($KIT): it would be lost. Unzip it to a folder of your own (for example ~/Squadala) and start it there." ;;
esac
touch "$DATA/.write-test" 2>/dev/null && rm -f "$DATA/.write-test" ||
    fail "This folder is read-only for you ($KIT). Unzip the kit to a folder of your own (for example ~/Squadala)."
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

# --pack (boredhermit: "it can't build the puppet independently anymore"): the pack made again, nothing else. Its stamp
# goes, so build.sh makes it whatever it was; the game's own build steps are skipped as on any start (nothing changed).
if ((pack_only)); then
    [[ -n $(kit_game_exe) ]] || fail 'The game is not built yet: start start-squadala.sh (it builds the game and makes the puppet pack).'
    rm -f "$GAME/mods/contents/0400486024DEC000/coop-puppet/.coop-stamp" "$DATA/pack-asked.txt"   # (and asks again)
    OOT_COOP_PACK_FRESH=1 bash "$KIT/scripts/build.sh" "${build_args[@]}"   # (made, never put back from data/pack-variants)
    [[ -f $GAME/mods/contents/0400486024DEC000/coop-puppet/.coop-stamp ]] || fail 'The puppet pack was not made (said why above).'
    say 'Done: the puppet pack is made. Start start-squadala.sh to play.'
    exit 0
fi

# --- 2: a new co-op version ---------------------------------------------------------------------
# The menu and Steam entries follow the kit's start file (start-oot-coop.sh, the name up to v0.11.3, is gone from
# v0.11.4 on; an updater up to v0.11.3 removes it without moving the entries): every start, the update check or not.
bash "$KIT/scripts/update.sh" --tidy || true
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
                say "Co-op ${status#newer } is out: start start-squadala.sh from a terminal to update."
            fi
            if ((go)); then
                set +e
                bash "$KIT/scripts/update.sh" --launch
                code=$?
                set -e
                ((code == 3)) && exec bash "$KIT/start-squadala.sh" --no-update "${build_args[@]}"
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
# (written again when it starts another kit or still has its name up to v0.11.3, "OoT Co-op")
if [[ -d $(dirname "$apps") && -z ${OOT_COOP_NO_SHORTCUT-} ]] &&
    { ! grep -qF -- "$KIT/start-squadala.sh" "$entry" 2>/dev/null || grep -qx 'Name=OoT Co-op' "$entry" 2>/dev/null; }; then
    mkdir -p "$apps"
    # (the desktop entry spec: inside "...", \ " ` $ are escaped with a backslash)
    q=${KIT//\\/\\\\}; q=${q//\"/\\\"}; q=${q//\`/\\\`}; q=${q//\$/\\\$}
    {
        echo '[Desktop Entry]'
        echo 'Type=Application'
        echo 'Name=Squadala'
        echo 'Comment=Ocarina of Time co-op on Longshot: host or join in the game'
        echo "Exec=bash \"$q/start-squadala.sh\""
        echo "Path=$KIT"
        [[ -f $GAME/longshot.png ]] && echo "Icon=$GAME/longshot.png"
        echo "StartupWMClass=squadala"
        echo 'Terminal=false'
        echo 'Categories=Game;'
    } >"$entry"
    say "A menu entry 'Squadala' was added (Steam Deck: add it to Steam with 'Add a Non-Steam Game')."
fi

# --- 4: the game --------------------------------------------------------------------------------
# Your own Link's tunic files on every start, as on Windows (kit-paths.ps1 Set-KitSelfTunic -Painted;
# colour-agree): tools/puppet_pack/self_tunic.py puts green's in when the pack has coop_look/green.bin (the
# module paints your exact colour on them), else your colour's, read as the module reads it.
pack=$GAME/mods/contents/0400486024DEC000/coop-puppet
py=$DATA/builder/image/usr/python/bin/python3
[[ -x $py ]] || py=$(command -v python3 || true)
if [[ -f $pack/coop_puppet.json && -n $py && -f $DATA/tools/puppet_pack/self_tunic.py ]]; then
    own=$("$py" -I -B "$DATA/tools/puppet_pack/self_tunic.py" --pack "$pack" --options "$DATA/coop-options.ini" \
        --coop-ini "$GAME/coop.ini" 2>/dev/null) || own=''
    [[ $own == green ]] && say "Your Link: the green tunic files; your coop-options.ini color is painted on in the game."
    [[ -n $own && $own != green ]] && say "Your Link: the $own tunic (coop-options.ini color)."
fi
# #48: one size table for the game (a seed from OOTRR's website has its own, without the puppet pack's entries):
# tools/puppet_pack/merge_tables.py, as the Windows kit's Invoke-KitSizeTableMerge. Never stops the start.
if [[ -f $DATA/tools/puppet_pack/merge_tables.py && -d $GAME/mods ]]; then
    if [[ -n $py ]]; then
        "$py" -I -B "$DATA/tools/puppet_pack/merge_tables.py" --mods "$GAME/mods" --romfs "$GAME/romfs" 2>&1 | sed 's/^/  /' || true
    else
        say "  Size tables: not merged (no Python): with a mod that has its own size table, the other players can't be shown."
    fi
fi
# "Playing Squadala!" on Discord: the Squadala! application and this kit's release in longshot_options.ini
# (scripts/discord_status.py, as the Windows kit's Invoke-KitDiscordStatus). Never stops the start.
if [[ -n $py && -f $KIT/scripts/discord_status.py ]]; then
    "$py" -I -B "$KIT/scripts/discord_status.py" --options "$GAME/longshot_options.ini" --version-file "$KIT/VERSION.txt" 2>&1 |
        sed 's/^/  /' || true
fi
# Problem reports (scripts/problem_report.py, docs/problem-reports.md): asked once (No in 20 s; only with a terminal):
# send the masked report of a game that had a co-op problem or a crash to the Squadala! team after it closes. With
# yes, the sessions since the last report are queued and sent in the background. Never stops the start.
if [[ -n $py && -f $KIT/scripts/problem_report.py ]]; then
    "$py" -I -B "$KIT/scripts/problem_report.py" --kit "$KIT" at-start || true
fi
step 'Starting the game: Host or Join in its menu (Multiplayer)'
# (as Longshot's own builder/linux/longshot-launch: nothing of a launcher's environment goes in; its
# own update check is off, the kit's updater brings Longshot up to date with co-op)
while IFS='=' read -r v _; do
    case $v in LONGSHOT_* | PYTHON* | APPDIR | APPIMAGE | ARGV0 | OWD | LD_LIBRARY_PATH) unset "$v" ;; esac
done < <(env)
export LONGSHOT_NO_UPDATE_CHECK=1 OOT_COOP_KIT=$DATA
# the window: title "Squadala! v<release>" (VERSION.txt's release, as the Windows module's
# coop_window_brand.cpp; letters, digits and . - + _ only, else "Squadala!" alone), its class (the menu
# entry's StartupWMClass) and our logo as the icon longshot.png (Longshot reads these)
title='Squadala!'
release=$(kit_version)
if [[ -n $release && ${#release} -le 32 && $release =~ ^[A-Za-z0-9.+_-]+$ ]]; then
    [[ $release == [0-9]* ]] && release="v$release"
    title="Squadala! $release"
fi
export HOS_WINDOW_TITLE=$title HOS_WM_CLASS=squadala
[[ -f $DATA/squadala-icon.png ]] && cp -f -- "$DATA/squadala-icon.png" "$GAME/longshot.png" 2>/dev/null || true
kit_steam_input_hint   # (player report 202: a Steam Controller outside Steam; a warning only)
cd "$GAME"
# Not exec'd: once the game closes (a crash too), its problems are sent when the player said yes (problem_report.py
# after-game-bg starts it on its own and returns at once; nothing waits for the upload).
code=0
if kit_tty; then
    ./longshot || code=$?
else
    ./longshot >>"$LOGS/game-output.log" 2>&1 || code=$?
fi
if [[ -n $py && -f $KIT/scripts/problem_report.py ]] && grep -qs '"send": "on"' "$LOGS/problem-send.json"; then
    "$py" -I -B "$KIT/scripts/problem_report.py" --kit "$KIT" after-game-bg >/dev/null 2>&1 || true
fi
exit "$code"
