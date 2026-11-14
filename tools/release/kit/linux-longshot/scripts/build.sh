#!/usr/bin/env bash
# Builds the co-op game on this PC from your own copy of the game (start-oot-coop.sh runs it; the
# Linux twin of the Windows kit's scripts/hookshot-build.ps1). Longshot (Team MagicBean, GPL-3.0,
# git.magicbean.dev/MagicBeanDev/Longshot) is a static recompilation of the game: it is compiled
# here, from your files, and nothing of the game leaves this PC.
#
#   bash scripts/build.sh                       finds your .nup (or asks), builds once, puts co-op in
#   bash scripts/build.sh --game FILE.nup       no questions (or --game <romfs folder> --exefs <exefs folder>)
#   bash scripts/build.sh --rebuild             build again even when nothing changed
#   bash scripts/build.sh --check-only          get the tools and check your game files; build nothing
#   bash scripts/build.sh --jobs N              at most N compilers at once
#   bash scripts/build.sh --renderer-only DIR   (CI, no game) steps 1 to 3, then compile Longshot's patched
#                                               Linux renderer and our host files into DIR
#
# 1. Longshot's own Linux builder for the release in data/longshot/LINUX.txt (its AppImage, SHA-256
#    checked), unpacked into data/builder/image (no FUSE needed): its Python and its source.
# 2. That source copied to data/builder/source with our patches (data/longshot/patches) applied
#    (apply_patch.py --refresh: a later patch change puts back only what the old ones changed).
# 3. Longshot's build tools for Linux (its tools.lock: clang/lld, the C library to link against,
#    FFmpeg, SDL2, the Vulkan headers; about 170 MB, SHA-256 checked by Longshot's own
#    builder/linux/linux_tools.py) into data/builder/home.
# 4. Longshot's nx2/make_game.py on your game: 15 to 60 minutes and about 60 GB free while it runs.
#    Its renderer is compiled here from the patched source (not the AppImage's prebuilt one, which has
#    no co-op drawing). The work folder data/work is kept: an update rebuilds only what changed.
# 5. Co-op into data/game: hookshot_coop.so beside the game, coop.ini, and the puppet pack built from
#    your game files in data/game/mods.
# Steps 1 to 4 are skipped when nothing they depend on changed, so most starts only check.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game='' exefs='' rebuild=0 check_only=0 jobs='' renderer_out=''
while (($#)); do
    case $1 in
        --game) game=$2; shift ;;
        --exefs) exefs=$2; shift ;;
        --rebuild) rebuild=1 ;;
        --check-only) check_only=1 ;;
        --jobs) jobs=$2; shift ;;
        --renderer-only) renderer_out=$2; shift ;;
        *) fail "build.sh: unknown option $1" ;;
    esac
    shift
done

LS=$DATA/longshot
B=$DATA/builder
WORK=$DATA/work
mkdir -p "$B" "$LOGS"
[[ -f $LS/LINUX.txt && -f $LS/PIN ]] || fail "data/longshot/LINUX.txt or PIN is missing: unzip the kit again."

# --- 1: Longshot's builder ----------------------------------------------------------------------
tag=$(pin_get "$LS/LINUX.txt" tag) || fail 'data/longshot/LINUX.txt has no tag.'
name=$(pin_get "$LS/LINUX.txt" appimage) || fail 'data/longshot/LINUX.txt has no appimage.'
url=$(pin_get "$LS/LINUX.txt" url) || fail 'data/longshot/LINUX.txt has no url.'
want=$(pin_get "$LS/LINUX.txt" sha256) || fail 'data/longshot/LINUX.txt has no sha256.'
[[ $(pin_get "$LS/PIN" tag) == "$tag" ]] || fail "data/longshot/LINUX.txt ($tag) and PIN ($(pin_get "$LS/PIN" tag)) name different Longshot releases."
[[ $name =~ ^[A-Za-z0-9._-]+$ && $url == https://git.magicbean.dev/MagicBeanDev/Longshot/releases/download/* ]] ||
    [[ -n ${OOT_COOP_FETCH_LOCAL-} ]] || fail "data/longshot/LINUX.txt: not a Longshot release download ($url)."
IMG=$B/image
PY=$IMG/usr/python/bin/python3
if [[ $(cat "$B/image.stamp" 2>/dev/null) != "$want" || ! -x $PY ]]; then
    step "Getting Longshot $tag's builder (about 95 MB, once)"
    rm -rf "$IMG" "$B/image.stamp" "$B/squashfs-root"
    if [[ ! -f $B/$name || $(sha256_of "$B/$name") != "$want" ]]; then
        rm -f "$B/$name.part"
        fetch "$url" "$B/$name.part" || fail "The download of $name failed (no network?). Start again to retry."
        [[ $(sha256_of "$B/$name.part") == "$want" ]] || { rm -f "$B/$name.part"; fail "$name is damaged (its SHA-256 does not match). Start again to retry."; }
        mv -f "$B/$name.part" "$B/$name"
    fi
    chmod +x "$B/$name"
    # (--appimage-extract is the AppImage runtime's own unpacker: no FUSE, no root)
    (cd "$B" && "./$name" --appimage-extract >/dev/null) || fail "$name could not be unpacked (is this an x86-64 PC?)."
    mv "$B/squashfs-root" "$IMG"
    [[ -x $PY && -f $IMG/usr/share/longshot/nx2/make_game.py ]] || fail "$name has no Python or no nx2/make_game.py: not the builder we expect."
    printf '%s' "$want" >"$B/image.stamp"
    rm -f "$B/$name"   # (unpacked: the 95 MB file is not needed any more)
fi

# --- 2: the patched source ----------------------------------------------------------------------
SRC=$B/source
patches=("$LS"/patches/*.patch)
[[ -f ${patches[0]} ]] || fail 'data/longshot/patches has no patches: unzip the kit again.'
src_stamp=$want
for p in "${patches[@]}"; do src_stamp+=" ${p##*/}:$(sha256_of "$p")"; done
if [[ $(cat "$SRC/.coop-stamp" 2>/dev/null) != "$src_stamp" ]]; then
    step "Longshot's source with co-op's ${#patches[@]} patches"
    if [[ $(cat "$SRC/.coop-image" 2>/dev/null) != "$want" ]]; then
        rm -rf "$SRC" "$B/source-orig"
        cp -a "$IMG/usr/share/longshot" "$SRC"
        printf '%s' "$want" >"$SRC/.coop-image"
    fi
    "$PY" -I -B "$KIT/scripts/apply_patch.py" --refresh "$B/source-orig" "$SRC" "${patches[@]}" ||
        fail "Co-op's patches do not apply to Longshot $tag (said why above). This kit is damaged: unzip it again."
    printf '%s' "$src_stamp" >"$SRC/.coop-stamp"
fi

# --- 3: Longshot's build tools ------------------------------------------------------------------
# (linux_tools.py prints 'export NAME=value' lines: EMSDK, LONGSHOT_SYSROOT, LONGSHOT_FFMPEG,
# LONGSHOT_SDL2, RX_CPM. Its downloads go to LONGSHOT_BUILDER_HOME: kept inside the kit.)
export LONGSHOT_BUILDER_HOME=$B/home
step "Longshot's build tools (about 170 MB, once)"
tools_env=$(env -u LONGSHOT_TOOLS "$PY" -I "$SRC/builder/linux/linux_tools.py" env --install) ||
    fail "Longshot's build tools could not be fetched (said why above). Start again to retry."
eval "$tools_env"
# Longshot's own variables for its build, and nothing of a launcher or test run: LONGSHOT_TOOLS
# unset keeps the game in one folder (no bin/), LONGSHOT_VK_PREBUILT unset compiles the patched
# renderer. Its lift key hashes every A64_/OOT_/RX_ variable: co-op's OOT_COOP_* ones go.
language=${OOT_COOP_LANGUAGE:-en-US}   # (read before the OOT_COOP_* variables go below)
fetch_local=${OOT_COOP_FETCH_LOCAL-}   # (tests: a local OOTRR in step 5, after they go)
offline=${OOT_COOP_OFFLINE-}           # (tests: 1 = no internet, for step 5)
unset LONGSHOT_TOOLS LONGSHOT_VK_PREBUILT LONGSHOT_BUILDER_EXE LONGSHOT_NGX_SDK LONGSHOT_FFX_SDK
while IFS='=' read -r v _; do [[ $v == OOT_COOP_* ]] && unset "$v"; done < <(env)
export PYTHONIOENCODING=utf-8

if [[ -n $renderer_out ]]; then
    # (CI: no game. Longshot's renderer from the patched source, as its scripts/linux/appimage.sh builds it)
    step 'The patched renderer (longshot_vk.so), no game'
    mkdir -p "$renderer_out"
    (cd "$SRC" && OOT_WORK=$renderer_out RX_VK_ONLY=1 RX_RELEASE=1 "$PY" -I nx2/build.py --standalone --jobs "${jobs:-$(nproc)}")
    [[ -f $renderer_out/sa/build/longshot_vk.so ]] || fail 'longshot_vk.so was not built.'
    say "built $renderer_out/sa/build/longshot_vk.so"
    exit 0
fi

# --- 4: the game --------------------------------------------------------------------------------
make_game() {
    # (its "@@<word> ..." lines are for Longshot's launcher window: stages and errors shown, the rest left out)
    (cd "$SRC" && "$PY" -I nx2/make_game.py "$@") 2>&1 | while IFS= read -r line; do
        case $line in
            '@@stage '*) line=${line#@@stage }; printf '  -- %s\n' "${line#* }" ;;
            '@@error '*) printf '  %s\n' "${line#@@error }" ;;
            '@@'*) ;;
            *) printf '  %s\n' "$line" ;;
        esac
    done
    return "${PIPESTATUS[0]}"
}

game_stamp=$GAME/coop-built-from.txt
ready=0
[[ -n $(kit_game_exe) && $(cat "$game_stamp" 2>/dev/null) == "$src_stamp" ]] && ready=1
remembered=$DATA/game-files.txt

if [[ -z $game ]] && ((!ready || rebuild || check_only)); then
    if [[ -f $remembered ]]; then
        IFS= read -r line <"$remembered" || true
        [[ -n $line && -e $line ]] && { game=$line; say "Your game (from the last time): $game"; }
    fi
fi
if [[ -z $game ]] && ((!ready || rebuild || check_only)); then
    # As Longshot's launcher: a .nup in Downloads, Desktop, Documents, the home folder or beside the kit.
    mapfile -t found < <(find "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents" "$HOME" "$(dirname "$KIT")" \
        -maxdepth 2 -iname '*.nup' -type f 2>/dev/null | sort -u)
    if ((${#found[@]} == 1)); then
        game=${found[0]}
        say "Found your game: $game"
    elif ((${#found[@]} > 1)) && kit_tty; then
        say 'Several game files found:'
        for i in "${!found[@]}"; do say "  $((i + 1))) ${found[$i]}"; done
        read -r -p 'Which one is the game (number)? ' n || n=''
        [[ $n =~ ^[0-9]+$ && $n -ge 1 && $n -le ${#found[@]} ]] || fail 'No game chosen. Start again.'
        game=${found[$((n - 1))]}
    fi
fi
if [[ -z $game ]] && ((!ready || rebuild || check_only)); then
    kit_tty || fail 'The game is not built yet and needs your game file. Start start-oot-coop.sh from a terminal once (Steam Deck: in Desktop Mode, right-click it, "Run in Konsole"), then it starts from anywhere.'
    say ''
    say "Drag your game's .nup file (or its romfs folder) into this window and press Enter:"
    read -r game || game=''
    game=${game#\'} game=${game%\'} game=${game#\"} game=${game%\"}
    game=${game#file://}
    [[ -n $game ]] || fail 'No game file given. Start again.'
fi

game_args=()
if [[ -n $game ]]; then
    [[ -e $game ]] || fail "Not found: $game"
    game=$(cd "$(dirname "$game")" && printf '%s/%s' "$(pwd)" "$(basename "$game")")
    if [[ -d $game ]]; then
        if [[ -z $exefs ]]; then
            if [[ -f $(dirname "$game")/exefs/main ]]; then exefs=$(dirname "$game")/exefs
            elif kit_tty; then
                read -r -p 'A romfs folder needs its exefs folder too. Drag the exefs folder in and press Enter: ' exefs || exefs=''
                exefs=${exefs#\'} exefs=${exefs%\'}
            fi
        fi
        [[ -f $exefs/main ]] || fail "$exefs is not an exefs folder (no main in it)."
        game_args=(--romfs "$game" --exefs "$(cd "$exefs" && pwd)")
    else
        game_args=(--nup "$game")
    fi
fi

if ((ready && !rebuild && !check_only)); then
    step 'The game: built already'
else
    ((${#game_args[@]})) || fail 'No game files given.'
    step 'Checking your game files'
    make_game "${game_args[@]}" --check ||
        fail 'Longshot cannot build from these files (it said why above). Co-op needs the game program 0400486024DEC000, update v131072, build e0f3ccfa: dump that version of your game and give its .nup.'
    if ((check_only)); then say ''; say 'Your game files are right. Start again without --check-only to build.'; exit 0; fi
    printf '%s\n' "$game" >"$remembered"
    free_kb=$(df -Pk "$DATA" | awk 'NR == 2 { print $4 }')
    if [[ -n $free_kb && $free_kb -lt $((60 * 1024 * 1024)) && ! -d $WORK/sa ]]; then
        warn "only $((free_kb / 1024 / 1024)) GB free here; the first build wants about 60 GB."
    fi
    step 'Building the game (15 to 60 minutes, once; the PC stays usable)'
    build_args=("${game_args[@]}" --out "$GAME" --work "$WORK" --language "$language" --log "$LOGS/build.log")
    [[ -n $jobs ]] && build_args+=(--jobs "$jobs")
    mkdir -p "$GAME"
    make_game "${build_args[@]}" || fail "The build stopped. Its log: $LOGS/build.log"
    [[ -n $(kit_game_exe) ]] || fail "The build finished but there is no game in $GAME. Its log: $LOGS/build.log"
    printf '%s' "$src_stamp" >"$game_stamp"
fi

# --- 5: co-op into the game ---------------------------------------------------------------------
step 'Co-op into the game'
[[ -f $LS/hookshot_coop.so ]] || fail 'data/longshot/hookshot_coop.so is missing: unzip the kit again.'
cp -f "$LS/hookshot_coop.so" "$GAME/hookshot_coop.so"
say "  hookshot_coop.so -> $GAME"
# The randomizer, OOTRR (data/longshot/OOTRR.txt: its release and SHA-256), into data/game/tools/ootrr where the
# module finds it with no setting; it reads the game's own files beside it (data/game/romfs) and never changes
# them. A failed download only warns: co-op plays without it, and the next start tries again.
ootrr_pin=$LS/OOTRR.txt
if [[ -f $ootrr_pin ]]; then
    o_rel=$(pin_get "$ootrr_pin" release || true) o_url=$(pin_get "$ootrr_pin" url || true)
    o_file=$(pin_get "$ootrr_pin" linux || true) o_sha=$(pin_get "$ootrr_pin" linux_sha256 || true)
    o_dir=$GAME/tools/ootrr o_want="$o_rel $o_sha"
    if [[ -x $o_dir/ootrr && $(cat "$o_dir/coop-ootrr.txt" 2>/dev/null) == "$o_want" ]]; then
        say "  randomizer: OOTRR $o_rel -> $o_dir (there already)"
    elif [[ ! $o_file =~ ^[A-Za-z0-9._-]+$ || ! $o_sha =~ ^[0-9a-f]{64}$ || -z $o_url ]]; then
        warn 'data/longshot/OOTRR.txt is incomplete: the randomizer is left out.'
    else
        mkdir -p "$B"
        o_tgz=$B/$o_file o_why=''
        o_get=0
        [[ ! -f $o_tgz || $(sha256_of "$o_tgz") != "$o_sha" ]] && o_get=1
        if ((o_get)) && { [[ $offline == 1 || $o_url =~ ^https?:// ]] && kit_offline "$offline"; }; then
            # (no internet: no download try, which would only wait for its timeout, on every start)
            o_why=offline
        elif ((o_get)); then
            say "  downloading $o_file ..."
            if ! OOT_COOP_FETCH_LOCAL=$fetch_local fetch "$o_url/$o_file" "$o_tgz.part"; then o_why='the download failed (no network?)'
            elif [[ $(sha256_of "$o_tgz.part") != "$o_sha" ]]; then o_why='its SHA-256 differs from the one this kit was made with'
            else mv -f "$o_tgz.part" "$o_tgz"; fi
            rm -f "$o_tgz.part"
        fi
        if [[ -z $o_why ]]; then
            rm -rf "$o_dir" && mkdir -p "$o_dir"
            # (one folder, ootrr-<version>-linux-x86_64/: ootrr, LICENSE, NOTICE, README.md, install.sh, ootrr.desktop)
            if tar -xzf "$o_tgz" -C "$o_dir" --strip-components=1 && [[ -f $o_dir/ootrr ]]; then
                chmod +x "$o_dir/ootrr"
                printf '%s\n' "$o_want" >"$o_dir/coop-ootrr.txt"
            else
                o_why="unpacking $o_file failed"
            fi
        fi
        if [[ $o_why == offline ]]; then
            say "  offline: using the tools already here (the randomizer, OOTRR $o_rel, is set up at the next start with internet)"
        elif [[ -n $o_why ]]; then
            warn "the randomizer (OOTRR $o_rel) was not set up: $o_why. Co-op works without it; start again to retry."
        else
            say "  randomizer: OOTRR $o_rel -> $o_dir"
        fi
    fi
fi
# coop.ini beside the game (the module reads it there): the Multiplayer tab fills in the rest.
if [[ ! -f $GAME/coop.ini ]]; then
    sed -e 's/^\(server\|name\|password\) = @[A-Z]*@$/# \1 =/' -e 's/@PORT@/27510/' "$KIT/scripts/coop.ini.template" >"$GAME/coop.ini"
fi
# The puppet pack (the other players' Link, and the title menu's rows), from the same game files,
# where the module and Longshot's mods loader both look: data/game/mods/contents/<title>/coop-puppet.
pack_from=$game
[[ -n $pack_from ]] || { [[ -f $remembered ]] && IFS= read -r pack_from <"$remembered" || true; }
mod=$GAME/mods/contents/0400486024DEC000/coop-puppet
if [[ -f $mod/coop_puppet.json && $(cat "$mod/.coop-stamp" 2>/dev/null) == "$src_stamp" ]]; then
    say '  puppet pack: built already'
elif [[ -n $pack_from && -e $pack_from ]]; then
    say '  building the puppet pack ...'
    if "$PY" -I -B "$DATA/tools/puppet_pack/build.py" --romfs "$pack_from" --mod-dir "$mod" --title-rows 2>&1 | sed 's/^/    /'; then
        printf '%s' "$src_stamp" >"$mod/.coop-stamp"
    else
        warn 'the puppet pack was not built (said why above): other players show as stand-ins.'
    fi
else
    warn 'no game files to build the puppet pack from: start again with --game <your .nup>.'
fi
say ''
say 'The game is ready.'
