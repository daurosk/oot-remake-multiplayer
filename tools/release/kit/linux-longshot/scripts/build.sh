#!/usr/bin/env bash
# Builds the co-op game on this PC from your own copy of the game (start-squadala.sh runs it; the
# Linux twin of the Windows kit's scripts/hookshot-build.ps1). Longshot (Team MagicBean, GPL-3.0,
# git.magicbean.dev/MagicBeanDev/Longshot) is a static recompilation of the game: it is compiled
# here, from your files, and nothing of the game leaves this PC.
#
#   bash scripts/build.sh                       finds your .nup (or asks), builds once, puts co-op in
#   bash scripts/build.sh --game FILE.nup       no questions (or --game <romfs folder> --exefs <exefs folder>)
#   bash scripts/build.sh --rebuild             build again even when nothing changed
#   bash scripts/build.sh --check-only          get the tools and check your game files; build nothing
#   bash scripts/build.sh --jobs N              at most N compilers at once
#   bash scripts/build.sh --dlss                with NVIDIA DLSS: asks you to accept NVIDIA's license once, then every
#                                               build fetches NVIDIA's DLSS SDK and builds with it (data/dlss.txt)
#   bash scripts/build.sh --no-dlss             without DLSS again (the game is built again without it)
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

game='' exefs='' rebuild=0 check_only=0 jobs='' renderer_out='' dlss=0 no_dlss=0
while (($#)); do
    case $1 in
        --game) game=$2; shift ;;
        --exefs) exefs=$2; shift ;;
        --rebuild) rebuild=1 ;;
        --check-only) check_only=1 ;;
        --dlss) dlss=1 ;;
        --no-dlss) no_dlss=1 ;;
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
# The co-op mirror (kit-mirror): our GitHub release with the same AppImage, tried after Longshot's server (the SHA-256
# above decides either way). OOT_COOP_MIRROR (tests) points it elsewhere.
mirror=${OOT_COOP_MIRROR:-$(pin_get "$LS/LINUX.txt" mirror || true)}
mirror=${mirror%/}
[[ -z $mirror || $mirror == https://* || -n ${OOT_COOP_FETCH_LOCAL-} ]] || fail "data/longshot/LINUX.txt: mirror=$mirror is not an https:// address."
IMG=$B/image
PY=$IMG/usr/python/bin/python3
if [[ $(cat "$B/image.stamp" 2>/dev/null) != "$want" || ! -x $PY ]]; then
    step "Getting Longshot $tag's builder (about 95 MB, once)"
    rm -rf "$IMG" "$B/image.stamp" "$B/squashfs-root"
    if [[ ! -f $B/$name || $(sha256_of "$B/$name") != "$want" ]]; then
        say "  downloading $name ..."
        fetch_verified "$B/$name" "$want" "$url" ${mirror:+"$mirror/$name"} ||
            fail "$(fetch_fail_text "$name" "$url" "$([[ -n $mirror ]] && echo 1)")"
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
# renderer, LONGSHOT_ROMFS_FROM unset (a launcher's, Longshot rc.6 #360: game files hard-linked from
# another folder) unpacks the game files the kit names. Co-op's OOT_COOP_* ones go too (Longshot's lift
# key hashed every A64_/OOT_/RX_ variable before rc.6, and they are no business of the build's).
language=${OOT_COOP_LANGUAGE:-en-US}   # (read before the OOT_COOP_* variables go below)
fetch_local=${OOT_COOP_FETCH_LOCAL-}   # (tests: a local OOTRR in step 5, after they go)
offline=${OOT_COOP_OFFLINE-}           # (tests: 1 = no internet, for step 5)
unset LONGSHOT_TOOLS LONGSHOT_VK_PREBUILT LONGSHOT_BUILDER_EXE LONGSHOT_NGX_SDK LONGSHOT_FFX_SDK LONGSHOT_ROMFS_FROM
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

# OoT Character Select by Snail (data/longshot/CHARACTER-SELECT.txt; owner, 0.11.8 and 0.11.9), as kit-paths.ps1's
# Invoke-KitCharacterSelectStep: every kit gets his Characters page (the default part); his NPC characters (about 1 GB)
# only on the player's yes (asked once: y is yes; Enter, n, 15 s or no terminal: no; data/coop-options.ini
# character_select = yes|no|ask wins, data/character-select-answer.txt keeps the answer; every later start in a terminal
# shows one line and n within 2 s changes it). Snail's own release and his characters.json are downloaded from his GitHub
# pages onto this PC, SHA-256 checked, his customizer left out of his code patch (co-op's own Customize fills his
# screen), and the mod's folder goes into data/game/mods, on (the Mods tab switches
# it off; the list is never written here). The kit carries none of its files. Before the game's stamp
# below: the build takes in its code patch (patches 0034/0035), the pack its characters.
CS_HOME=$GAME/mods   # (where 0.11.8 puts it too; C #56: not under contents, where only the pack goes)
# Before the kit changes <home>/<folder>: what the game was last built with is kept as <home>/_<folder>.prev (the
# folder) or <home>/_<folder>.none (nothing was there), both skipped by the game and the build ('_'); kept once, so a
# start that stopped in its build keeps the older state, the one the game has (kit-paths.ps1 Save-KitCharacterSelectPrevious).
charsel_save_previous() {
    local dir=$CS_HOME/$1 prev=$CS_HOME/_$1.prev none=$CS_HOME/_$1.none
    mkdir -p "$CS_HOME"
    if [[ ! -e $prev && ! -e $none ]]; then
        if [[ -e $dir ]]; then mv "$dir" "$prev"; else printf 'no %s before the kit'"'"'s change
' "$1" >"$none"; fi
    elif [[ -e $dir ]]; then
        rm -rf "$dir"
    fi
}
charsel_folder() { [[ -f $LS/CHARACTER-SELECT.txt ]] && pin_get "$LS/CHARACTER-SELECT.txt" folder; }
# The game could not be built with the change (review-geno-0118): the mods folder goes back to what the game was built
# with, so the game starts as it was. 0 when something was put back. The player's answer stays: the next start tries again.
charsel_undo() {
    local folder dir prev none
    folder=$(charsel_folder) || return 1
    [[ -n $folder ]] || return 1
    dir=$CS_HOME/$folder prev=$CS_HOME/_$folder.prev none=$CS_HOME/_$folder.none
    [[ -e $prev || -e $none ]] || return 1
    rm -rf "$dir"
    [[ -e $prev ]] && mv "$prev" "$dir"
    rm -f "$none"
    return 0
}
# The game was built (or needed no build) with the change: the kept state goes.
charsel_confirm() {
    local folder
    folder=$(charsel_folder) || return 0
    [[ -n $folder ]] && rm -rf "$CS_HOME/_$folder.prev" "$CS_HOME/_$folder.none"
    return 0
}
# The folder work in Python (kit-paths.ps1's Test-KitCharacterSelectNpcFile, Get-KitCharselStrippedIps,
# Copy-KitTreeLinked and Complete-KitCharacterSelectFolder, the same rules and regions):
#   derive <copy there> <new> <part>   the copy's files as hard links (a copy where a link cannot be made), the NPC part left out for ui
#   complete <new> <part> <characters.json> <newer .ips or ''> <stamp> <release>
#   strip <his .ips> <out .ips>        (tests) the stripped patch; one line per record left out
# His customizer (his MIT source, mods/customize.py): cz_head and cz_frame (the places that call it) and the bodies
# it fills; a record across a region's edge or a region with no record: refused (exit 1, the reason on stderr).
CHARSEL_PY='
import os, re, shutil, sys
CZ = [("cz_head (HEAD_CALL)", 0x804D756C, 0x804D7570), ("cz_frame (FRAME_SITE)", 0x8289D600, 0x8289D604),
      ("COLOUR_CODE", 0x832E1620, 0x832E19F0), ("COLOUR_DATA", 0x832E39D0, 0x832E3C00),
      ("COLOUR_MORE", 0x8103AE80, 0x8103B160), ("COLOUR_KEEP", 0x83BCDC10, 0x83BCDDE0),
      ("COLOUR_TEX", 0x83BCCE70, 0x83BCD000)]
SHIFT = 0x8001FF00
def npc(rel):
    r = rel.replace("\\", "/")
    return bool(re.match(r"romfs/(Tex|Phive|Sound)/", r, re.I) or re.match(r"romfs/Model/Player_(Adult|Child)_C\d{3}\.", r, re.I)
                or re.match(r"romfs/Model/Player_Dark_", r, re.I) or r.lower() == "romfs/pack/actor/enemy_middleboss_darklink.pack.zs")
def records(d):
    if len(d) < 9 or d[:5] != b"IPS32":
        raise ValueError("not an IPS32 file")
    o, out = 5, []
    while True:
        if o + 4 > len(d):
            raise ValueError("no EEOF at its end")
        if d[o:o + 4] == b"EEOF":
            break
        if o + 6 > len(d):
            raise ValueError("a record is cut short")
        at, n, f = int.from_bytes(d[o:o + 4], "big"), int.from_bytes(d[o + 4:o + 6], "big"), o
        if n == 0:
            if o + 9 > len(d):
                raise ValueError("a run record is cut short")
            size, o = int.from_bytes(d[o + 6:o + 8], "big"), o + 9
        else:
            if o + 6 + n > len(d):
                raise ValueError("a record is cut short")
            size, o = n, o + 6 + n
        out.append((at, size, f, o))
    if o + 4 != len(d):
        raise ValueError("bytes after EEOF")
    return out
def strip(d):
    try:
        recs = records(d)
    except ValueError as e:
        sys.exit(f"his code patch cannot be read ({e})")
    hit, out, dropped = set(), bytearray(d[:5]), []
    for at, size, f, t in recs:
        lo, hi, name = at, at + size, None
        for g, a, b in CZ:
            glo, ghi = a - SHIFT, b - SHIFT
            if lo >= glo and hi <= ghi:
                name = g
                break
            if lo < ghi and glo < hi:
                sys.exit(f"his code patch changes {lo:08x}-{hi:08x} across the edge of {g}: not the layout his source names")
        if name:
            hit.add(name)
            dropped.append(f"{name} {lo:08x} {size}")
        else:
            out += d[f:t]
    for g, _, _ in CZ:
        if g not in hit:
            sys.exit(f"his code patch has nothing at {g}: not the layout his source names")
    return bytes(out + b"EEOF"), dropped
def rd(p):
    with open(p, "rb") as f:
        return f.read()
def fresh(p, b):
    if os.path.lexists(p):
        os.remove(p)   # (a hard link to the kept copy is never written through)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(b)
cmd = sys.argv[1]
if cmd == "derive":
    src, dst, part = sys.argv[2:5]
    for r, _, fs in os.walk(src):
        for n in fs:
            a = os.path.join(r, n)
            rel = os.path.relpath(a, src)
            if part == "ui" and npc(rel):
                continue
            b = os.path.join(dst, rel)
            os.makedirs(os.path.dirname(b), exist_ok=True)
            try:
                os.link(a, b)
            except OSError:
                shutil.copy2(a, b)
elif cmd == "complete":
    new, part, js, ips, want, rel = sys.argv[2:8]
    romfs = os.path.join(new, "romfs")
    if part == "ui":
        for r, _, fs in os.walk(romfs):
            for n in fs:
                p = os.path.join(r, n)
                if npc(os.path.relpath(p, new)):
                    os.remove(p)
        for d in ("Tex", "Phive", "Sound"):
            shutil.rmtree(os.path.join(romfs, d), ignore_errors=True)
    ex = os.path.join(new, "exefs")
    code = sorted(n for n in os.listdir(ex) if n.lower().endswith(".ips") and os.path.isfile(os.path.join(ex, n))) if os.path.isdir(ex) else []
    if len(code) != 1:
        sys.exit(f"the release has {len(code)} code patches, not one")
    mine = os.path.join(new, "coop-charsel-code.ips")
    his = rd(ips) if ips else rd(mine) if os.path.isfile(mine) else rd(os.path.join(ex, code[0]))
    data, dropped = strip(his)
    fresh(mine, his)
    fresh(os.path.join(ex, code[0]), data)
    fresh(os.path.join(new, "characters.json"), rd(js))
    fresh(os.path.join(new, "coop-charsel.txt"), (want + "\n").encode())
    npcs = "with its NPC characters" if part == "full" else "without its NPC characters (character_select = yes in data/coop-options.ini gets them)"
    fresh(os.path.join(new, "CREDITS-coop.txt"), "\n".join([
        f"OoT Character Select {rel} by snaildev99 (Snail): https://github.com/snaildev99/OoT-Character-Select",
        f"Downloaded by the Squadala co-op kit from his own release page onto this PC, {npcs}; the kit carries none of its files.",
        "His Customize code is left out of its code patch (co-op'"'"'s own Customize fills his Customize screen); the patch as he made it is coop-charsel-code.ips.",
        "Its code is under the MIT licence (Copyright (c) 2026 snaildev99; LICENSE.txt here when it could be fetched).",
        "The Legend of Zelda: Ocarina of Time and its characters belong to Nintendo.",
        "Switch it off in the game'"'"'s Mods tab.", ""]).encode())
elif cmd == "strip":
    data, dropped = strip(rd(sys.argv[2]))
    fresh(os.path.abspath(sys.argv[3]), data)
    print("\n".join(dropped))
'
charsel_py() { "$PY" -I -B -c "$CHARSEL_PY" "$@"; }

charsel_step() {
    local pin=$LS/CHARACTER-SELECT.txt choice='' line rel url zip zsha zfolder folder ourl osha iurl isha want dir new dl why='' part have derive=0 json ips a=''
    [[ -f $pin ]] || return 0
    rel=$(pin_get "$pin" release || true) url=$(pin_get "$pin" url || true) zip=$(pin_get "$pin" zip || true)
    zsha=$(pin_get "$pin" zip_sha256 || true) zfolder=$(pin_get "$pin" zip_folder || true) folder=$(pin_get "$pin" folder || true)
    ourl=$(pin_get "$pin" overlay_url || true) osha=$(pin_get "$pin" overlay_sha256 || true)
    iurl=$(pin_get "$pin" ips_url || true) isha=$(pin_get "$pin" ips_sha256 || true)
    if [[ ! $rel =~ ^[A-Za-z0-9._-]+$ || ! $url =~ ^https:// || ! $zip =~ ^[A-Za-z0-9._-]+$ || ! $zsha =~ ^[0-9a-fA-F]{64}$ ||
          -z $zfolder || $zfolder == *..* || ! $folder =~ ^[A-Za-z0-9][^/\\:*?\"\<\>\|]*$ || ! $ourl =~ ^https:// || ! $osha =~ ^[0-9a-fA-F]{64}$ ]]; then
        warn 'data/longshot/CHARACTER-SELECT.txt is incomplete: Character Select is left out.'; return 0
    fi
    [[ -n $iurl && ( ! $iurl =~ ^https:// || ! $isha =~ ^[0-9a-fA-F]{64}$ ) ]] && iurl=''
    zsha=${zsha,,} osha=${osha,,} isha=${isha,,}
    dir=$CS_HOME/$folder
    line=$(sed -n 's/^[[:space:]]*character_select[[:space:]]*=[[:space:]]*\(yes\|no\|ask\)[[:space:]]*\([#;].*\)\{0,1\}$/\1/p' "$DATA/coop-options.ini" 2>/dev/null | head -n1 || true)
    if [[ -n $line ]]; then choice=$line; [[ $choice == ask ]] && rm -f "$DATA/character-select-answer.txt"
    else choice=$(head -n1 "$DATA/character-select-answer.txt" 2>/dev/null | tr -d '\r[:space:]' || true); [[ $choice == yes || $choice == no ]] || choice=ask
    fi
    if [[ $choice == ask ]]; then
        say ''
        say 'The Characters page (in the - menu) comes with the kit: Character Select by Snail (snaildev99).'
        say "  Its NPC characters (Saria, Sheik, Darunia and over a hundred more) are an extra download from Snail's own"
        say '  GitHub page (about 1 GB, once). Without them you play as Link, and the page tells you when a character is not'
        say '  downloaded. The Legend of Zelda: Ocarina of Time and its characters belong to Nintendo.'
        local q='Download the NPC characters for the Characters page? [y/N]' s
        choice=no
        if kit_tty; then
            for ((s = ${OOT_COOP_CHARSEL_WAIT:-15}; s > 0; s--)); do
                printf '\r%s (no in %d s)  ' "$q" "$s"
                if IFS= read -r -s -n 1 -t 1 a; then break; fi
                a=timeout
            done
            [[ $a == [yY] ]] && choice=yes
            printf '%s\n' "${choice:0:1}"
        else
            say "$q n (no terminal: no)"
        fi
        printf '%s\n' "$choice" >"$DATA/character-select-answer.txt"
        [[ $choice == no ]] && say '  Not downloaded. To get them later: press n at the next start, while the kit shows the NPC line.'
    elif kit_tty; then
        # (row 206: on or off from the kit itself, no file to edit; coop-options.ini's line follows the change)
        if [[ $choice == yes ]]; then say 'Characters page, NPC characters: on (press n to take them out)'
        else say 'Characters page, NPC characters: off (press n to download them, about 1 GB)'; fi
        if IFS= read -r -s -n 1 -t 2 a && [[ $a == [nN] ]]; then
            if [[ $choice == yes ]]; then choice=no; else choice=yes; fi
            say '  changed.'
            printf '%s\n' "$choice" >"$DATA/character-select-answer.txt"
            [[ -f $DATA/coop-options.ini ]] && sed -i "s/^[[:space:]]*character_select[[:space:]]*=.*/character_select = $choice/" "$DATA/coop-options.ini"
        fi
    fi
    part=ui
    [[ $choice == yes ]] && part=full
    want="$rel $zsha $osha ${isha:--} $part cz1"
    local what='the Characters page (NPC characters not downloaded)'
    [[ $part == full ]] && what='with its NPC characters'
    if [[ -d $dir && ! -f $dir/coop-charsel.txt ]]; then say "Character Select: your own copy in $dir is used as it is."; return 0; fi
    have=$(head -n1 "$dir/coop-charsel.txt" 2>/dev/null | tr -d '\r' || true)
    if [[ -d $dir/romfs && $have == "$want" ]]; then say "Character Select $rel -> $dir, $what (there already)"; return 0; fi
    # The copy there, of this release (a 0.11.8 stamp, four fields: full): its part decides whether it can give the one wanted.
    local hf wf
    read -r -a hf <<<"$have"; read -r -a wf <<<"$want"
    if [[ -d $dir/romfs && ${#hf[@]} -ge 4 && "${hf[*]:0:4}" == "${wf[*]:0:4}" && ( $part == ui || ${hf[4]:-full} == full ) ]]; then derive=1; fi
    dl=$B/charsel new=$CS_HOME/_$folder.new json=$dl/characters.json ips=''
    mkdir -p "$dl" "$CS_HOME"
    rm -rf "$new"
    if ((derive)); then
        mkdir -p "$new"
        charsel_py derive "$dir" "$new" "$part" || why='the copy there could not be linked'
        json=$dir/characters.json
    else
        local free_kb
        free_kb=$(df -Pk "$CS_HOME" | awk 'NR == 2 { print $4 }')
        if [[ -n $free_kb && $free_kb -lt $((3 * 1024 * 1024)) && ! ( -f $dl/$zip && $(sha256_of "$dl/$zip") == "$zsha" ) ]]; then
            why='too little disk space (about 3 GB free needed)'
        elif [[ ! -f $dl/$zip || $(sha256_of "$dl/$zip") != "$zsha" ]] && kit_offline "$offline"; then
            why=offline
        else
            # (fetch_verified always downloads: a file here already with the pinned SHA-256 is used as it is)
            charsel_get() { [[ -f $1 && $(sha256_of "$1") == "$2" ]] && return 0; say "  downloading ${1##*/} ..."; fetch_verified "$1" "$2" "$3"; }
            charsel_get "$dl/$zip" "$zsha" "$url/$zip" || why="the download of Snail's release failed ($KIT_FETCH_WHY)"
            [[ -z $why ]] && { charsel_get "$dl/characters.json" "$osha" "$ourl" || why="the download of characters.json failed ($KIT_FETCH_WHY)"; }
            [[ -z $why && -n $iurl ]] && { charsel_get "$dl/code.ips" "$isha" "$iurl" && ips=$dl/code.ips || why="the download of the newer code patch failed ($KIT_FETCH_WHY)"; }
        fi
        if [[ -z $why ]]; then
            mkdir -p "$new"
            # (the zip: <release>/mods/OoT Character Select/{romfs,exefs}; only that folder is taken, by Python's zipfile:
            # no unzip on every system. Names with '..' or a leading '/' are refused)
            if ! "$PY" -I -B -c '
import os, sys, zipfile, shutil
z, top, out = sys.argv[1], sys.argv[2].rstrip("/") + "/", sys.argv[3]
with zipfile.ZipFile(z) as f:
    for i in f.infolist():
        if not i.filename.startswith(top) or i.is_dir():
            continue
        rel = i.filename[len(top):]
        if not rel or rel.startswith("/") or ".." in rel.split("/"):
            sys.exit("bad name in the zip: " + i.filename)
        dst = os.path.join(out, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with f.open(i) as a, open(dst, "wb") as b:
            shutil.copyfileobj(a, b, 1 << 20)
' "$dl/$zip" "$zfolder" "$new" || [[ ! -d $new/romfs ]]; then
                why="unpacking $zip failed"
            fi
        fi
    fi
    if [[ -z $why ]]; then
        local err
        if ! err=$(charsel_py complete "$new" "$part" "$json" "$ips" "$want" "$rel" 2>&1 >/dev/null); then why=${err:-the folder could not be made ready}; fi
    fi
    if [[ -z $why ]]; then
        ((derive)) || curl -fsSL --max-time 30 -o "$new/LICENSE.txt" "https://raw.githubusercontent.com/snaildev99/OoT-Character-Select/$rel/LICENSE" 2>/dev/null || rm -f "$new/LICENSE.txt"
        # (what was there is kept until the game is built with the new one: charsel_undo / charsel_confirm)
        if charsel_save_previous "$folder" && mv "$new" "$dir"; then
            ((derive)) || rm -f "$dl/$zip"
        else
            charsel_undo
            why='it could not be put in place: close the game and start again'
        fi
    fi
    rm -rf "$new"
    if [[ $why == offline ]]; then say 'offline: Character Select is downloaded at the next start with internet; the game starts without it.'
    elif [[ -n $why ]]; then warn "Character Select was not installed: $why. The game and co-op work without it; start again to retry."
    else say "Character Select $rel by Snail -> $dir, $what (on; the Mods tab switches it off)"
    fi
    return 0
}
((check_only)) || charsel_step

game_stamp=$GAME/coop-built-from.txt
# The mods' code patches the game is built with (patches 0034/0035: mods/.../exefs/*.ips and *.pchtxt, folders starting
# with '-', '_' or '.' left out), with the Mods tab's list (its switches and order decide which go in): a new, changed or
# switched one builds the game again. No code mods: nothing added, the stamp as before (kit-paths.ps1 Get-KitExefsStampLine).
exefs_stamp() {
    local mods=$GAME/mods rows=() f rel
    local -A code=()
    [[ -d $mods ]] || return 0
    while IFS= read -r -d '' f; do
        rel=${f#"$mods"/}
        [[ ${rel%/*} == */exefs ]] || continue
        [[ /${rel%/exefs/*} =~ /[-_.] ]] && continue
        rows+=("$rel:$(sha256_of "$f")")
        code[$(mod_fold "${rel%/exefs/*}")]=1
    done < <(find "$mods" -maxdepth 6 -type f \( -iname '*.ips' -o -iname '*.pchtxt' \) -print0)
    ((${#rows[@]})) || return 0
    # The list's rows for these mods only (its first row per name, in its order, as the game reads it): switching or
    # moving a mod without code patches builds nothing again (kit-paths.ps1 Get-KitCodeModsListHash).
    local list=none line name fold kept=() first=1
    local -A seen=()
    if [[ -f $mods/coop-mods.list ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            line=${line%$'\r'}
            if ((first)); then first=0; [[ $line == 'coop-mods 1' ]] || break; continue; fi
            case $line in 'on '?*) name=${line#on } ;; 'off '?*) name=${line#off } ;; *) continue ;; esac
            local sw=${line%% *}
            fold=$(mod_fold "$name")
            [[ -n ${seen[$fold]+x} ]] && continue
            seen[$fold]=1
            [[ -n ${code[$fold]+x} ]] && kept+=("$sw $fold")
        done < "$mods/coop-mods.list"
        # (rows that change nothing are no change, player report 210: the game puts the mods the list does not name under
        # the named ones, by folded name, on; rows all "on" in that same order, as Character Select switched off and on
        # again leaves, read as no list. kit-paths.ps1 Get-KitCodeModsListHash)
        if ((${#kept[@]})); then
            local sorted=() i same=1
            mapfile -t sorted < <(printf '%s\n' "${!code[@]}" | LC_ALL=C sort)
            for i in "${!kept[@]}"; do [[ ${kept[$i]} == "on ${sorted[$i]-}" ]] || { same=0; break; }; done
            ((same)) || list=$(printf '%s' "$(printf '%s\n' "${kept[@]}")" | sha256sum | cut -d' ' -f1)
        fi
    fi
    printf ' exefs=%s;list=%s' "$(printf '%s\n' "${rows[@]}" | LC_ALL=C sort | paste -sd, -)" "$list"
}
# A mod's name as patch 0011's host/mods.c compares them: ASCII letters without case, '\' as '/'.
mod_fold() { local s=${1//\\//}; printf '%s' "$s" | LC_ALL=C tr 'A-Z' 'a-z'; }
game_want="$src_stamp$(exefs_stamp)"
ready=0
[[ -n $(kit_game_exe) && $(cat "$game_stamp" 2>/dev/null) == "$game_want" ]] && ready=1

# NVIDIA DLSS (#234), as hookshot-build.ps1: only in a game built with NVIDIA's DLSS SDK, which nobody may pass on, so
# each build downloads it once NVIDIA's license is accepted. --dlss asks once and data/dlss.txt keeps the answer (the
# builds after an update keep DLSS); --no-dlss takes it back. Not in coop-built-from.txt (release, pin and patches).
# A game with DLSS has its runtime in data/game/ngx (make_game.py's place_ngx_runtime; gone again without the SDK).
dlss_note=$DATA/dlss.txt
if ((no_dlss)); then
    rm -f "$dlss_note"
elif ((dlss)) && [[ ! -f $dlss_note ]]; then
    kit_tty || fail 'NVIDIA DLSS needs NVIDIA'"'"'s license accepted: run bash scripts/build.sh --dlss from a terminal.'
    say ''
    warn "NVIDIA DLSS (NVIDIA RTX graphics cards): the build downloads NVIDIA's DLSS SDK, under NVIDIA's own license:"
    # (the link as the pinned Longshot's scripts/ngx_sdk.py names it, so a pin that moves NVIDIA's commit moves it too)
    ngx_commit=$(sed -n 's/^COMMIT *= *"\([0-9a-f]\{40\}\)".*/\1/p' "$SRC/scripts/ngx_sdk.py" 2>/dev/null | head -n 1 || true)
    say "  https://github.com/NVIDIA/DLSS/blob/${ngx_commit:-main}/LICENSE.txt (the build prints NVIDIA's notice too)."
    say "  The game folder then holds NVIDIA's files: never share it, and never post builds made with them."
    read -r -p "  Accept NVIDIA's license and build with DLSS? (y/n) " answer || answer=''
    [[ ${answer,,} =~ ^(y|yes|j|ja)$ ]] || fail "NVIDIA's license not accepted: nothing changed (the game stays as it is, without DLSS)."
    printf 'nvidia-license=accepted\n' >"$dlss_note"
fi
want_dlss=0
((!no_dlss)) && [[ -f $dlss_note ]] && want_dlss=1
game_dlss=0
compgen -G "$GAME/ngx/libnvidia-ngx-dlss.so.*" >/dev/null && game_dlss=1
if ((ready && want_dlss != game_dlss && (want_dlss || no_dlss))); then
    if ((want_dlss)); then say 'NVIDIA DLSS: the game has no DLSS yet, so it is built again with it (much shorter than the first build).'
    else say 'NVIDIA DLSS off: the game is built again without it.'; fi
    ready=0
fi
# The game built from another stamp than this start wants (player report 210: a mod with code patches switched on or
# off on the Mods tab, an update): the build there is kept first (a copy of the files a build makes: the ones Longshot's
# finish copies in, .longshot_installed.json, and exefs_patches.txt; copies, never hard links), and one of exactly this
# stamp kept from before is put back instead of a build (seconds where a build is 5 to 60 minutes). The newest 3 stay
# in data/game-variants. As the Windows kit's Save-KitGameVariant and Restore-KitGameVariant.
variants=$DATA/game-variants
variant_files() {
    if [[ -f $GAME/.longshot_installed.json ]]; then
        "$PY" -I -B -c 'import json, sys
for k in sorted(json.load(open(sys.argv[1], encoding="utf-8"))): print(k)' "$GAME/.longshot_installed.json" 2>/dev/null || true
    else
        printf '%s\n' longshot longshot_vk.so libSDL2-2.0.so.0 image.bin
    fi
    printf 'exefs_patches.txt\n.longshot_installed.json\n'   # (the note too: a build kept after a put back still knows its files)
}
variant_text() { printf '%s\ndlss=%s' "$(printf '%s\n' "$1" | tr -d '\r' | sed 's/[[:space:]]*$//')" "$2"; }
variant_dir() { printf '%s/%s' "$variants" "$(printf '%s' "$1" | sha256sum | cut -c1-20)"; }
variant_save() {
    local have text dir part f bytes=0 free_kb
    have=$(cat "$game_stamp" 2>/dev/null) || return 0
    [[ -n $have && -n $(kit_game_exe) ]] || return 0
    text=$(variant_text "$have" "$game_dlss")
    dir=$(variant_dir "$text")
    if [[ -f $dir/stamp.txt && $(cat "$dir/stamp.txt") == "$text" ]]; then touch "$dir/stamp.txt"; return 0; fi
    while IFS= read -r f; do [[ -n $f && -f $GAME/$f ]] && bytes=$((bytes + $(stat -c %s "$GAME/$f"))); done < <(variant_files)
    mkdir -p "$variants"
    free_kb=$(df -Pk "$variants" | awk 'NR == 2 { print $4 }')
    if [[ -n $free_kb ]] && ((free_kb < bytes / 1024 + 5 * 1024 * 1024)); then
        say "  (this build is not kept for later: only $((free_kb / 1024 / 1024)) GB free)"; return 0
    fi
    part=$dir.part
    rm -rf "$part"
    while IFS= read -r f; do
        [[ -n $f && $f != /* && /$f/ != */../* && -f $GAME/$f ]] || continue
        { mkdir -p "$part/files/$(dirname -- "$f")" && cp -f -- "$GAME/$f" "$part/files/$f"; } ||
            { rm -rf "$part"; say '  (this build is not kept for later: the copy failed)'; return 0; }
    done < <(variant_files)
    [[ -f $part/files/longshot && -f $part/files/image.bin ]] || { rm -rf "$part"; return 0; }
    cp -f "$game_stamp" "$part/built-from.txt"
    printf '%s' "$text" >"$part/stamp.txt"   # (last: a copy stopped half way is a .part folder, never taken)
    { rm -rf "$dir" && mv "$part" "$dir"; } || return 0
    rm -rf "$variants"/*.part
    ls -1t "$variants"/*/stamp.txt 2>/dev/null | tail -n +4 | while IFS= read -r f; do rm -rf "$(dirname -- "$f")"; done
    say "  The game as it was built ($((bytes / 1024 / 1024)) MB of it) is kept, so going back to these mods takes seconds."
}
variant_restore() {   # $1: a kept build: staged as <name>.new, then swapped in; its stamp last
    local d=$1/files f staged=()
    while IFS= read -r f; do
        { mkdir -p "$GAME/$(dirname -- "$f")" && cp -f -- "$d/$f" "$GAME/$f.new"; } ||
            { for f in "${staged[@]}"; do rm -f "$GAME/$f.new"; done; return 1; }
        staged+=("$f")
    done < <(cd "$d" && find . -type f | sed 's|^\./||')
    ((${#staged[@]})) || return 1
    for f in "${staged[@]}"; do mv -f -- "$GAME/$f.new" "$GAME/$f"; done
    [[ -f $d/exefs_patches.txt ]] || rm -f "$GAME/exefs_patches.txt"
    # (Longshot's note comes back with them: its file list for the next keep; its stats differ from the copies, so its next
    # build copies those files again, as after any copy)
    [[ -f $d/.longshot_installed.json ]] || rm -f "$GAME/.longshot_installed.json"
    cp -f "$1/built-from.txt" "$game_stamp"
    touch "$1/stamp.txt"
}
if ((!ready && !rebuild && !check_only)) && [[ -n $(kit_game_exe) && -f $game_stamp && $(cat "$game_stamp") != "$game_want" ]]; then
    say ''
    say '  Keeping the game as it is built now, for later (about 650 MB, a few seconds) ...'
    variant_save
    kept_text=$(variant_text "$game_want" "$want_dlss")
    kept=$(variant_dir "$kept_text")
    if ((want_dlss == game_dlss)) && [[ -f $kept/stamp.txt && $(cat "$kept/stamp.txt") == "$kept_text" && -f $kept/built-from.txt && -f $kept/files/image.bin ]]; then
        step 'The game: putting back the build made before with these mods (seconds, instead of building it again)'
        if variant_restore "$kept" && [[ $(cat "$game_stamp") == "$game_want" ]]; then
            ready=1
            say "  Put back into $GAME."
        else
            warn 'the kept build could not be put back: the game is built again instead.'
        fi
    fi
fi
# Why the game is built now and about how long (player report 210: "a whole long process that takes forever").
build_why() {
    local have want
    have=$(tr -d '\r' <"$game_stamp" 2>/dev/null) || have=''
    want=$(printf '%s' "$game_want" | tr -d '\r')
    if [[ -z $have ]]; then
        printf 'the first build of the co-op game (15 to 60 minutes, once).'
    elif [[ ${have%% *} != "${want%% *}" ]]; then   # (the first word is the builder's SHA-256, LINUX.txt: the Longshot release)
        printf 'another Longshot version than the game was built for (about 15 to 45 minutes).'
    elif [[ $have == "$want" ]]; then
        printf 'built again as asked (5 to 60 minutes).'
    elif [[ ${have%% exefs=*} == "${want%% exefs=*}" ]]; then
        printf "the mods' code patches changed (a mod with an exefs folder, such as Character Select, put in, changed, or switched on the Mods tab), about 5 to 25 minutes. This build is kept: switching back later takes seconds."
    else
        printf "this update changes the co-op game's code (about 5 to 20 minutes)."
    fi
}
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
    kit_tty || fail 'The game is not built yet and needs your game file. Start start-squadala.sh from a terminal once (Steam Deck: in Desktop Mode, right-click it, "Run in Konsole"), then it starts from anywhere.'
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

# The game's build; game_fail stops it with its reason. After charsel_step changed the mods folder, a build that fails
# puts that folder back (charsel_undo) and the game starts as it was when that leaves it as built (review-geno-0118);
# else the start stops as before.
game_fail() { game_why=$*; return 1; }
game_build() {
        ((${#game_args[@]})) || { game_fail 'No game files given.'; return 1; }
        step 'Checking your game files'
        make_game "${game_args[@]}" --check ||
            { game_fail 'Longshot cannot build from these files (it said why above). Co-op needs the game program 0400486024DEC000, update v131072, build e0f3ccfa: dump that version of your game and give its .nup.'; return 1; }
        if ((check_only)); then say ''; say 'Your game files are right. Start again without --check-only to build.'; exit 0; fi
        printf '%s\n' "$game" >"$remembered"
        free_kb=$(df -Pk "$DATA" | awk 'NR == 2 { print $4 }')
        if [[ -n $free_kb && $free_kb -lt $((60 * 1024 * 1024)) && ! -d $WORK/sa ]]; then
            warn "only $((free_kb / 1024 / 1024)) GB free here; the first build wants about 60 GB."
        fi
        step "Building the game: $(build_why) The PC stays usable."
        build_args=("${game_args[@]}" --out "$GAME" --work "$WORK" --language "$language" --log "$LOGS/build.log")
        [[ -n $jobs ]] && build_args+=(--jobs "$jobs")
        # (NVIDIA DLSS: make_game.py fetches NVIDIA's SDK into data/work/ngx-sdk, only what is missing, and builds the
        # renderer with it; the license was accepted above, data/dlss.txt)
        ((want_dlss)) && build_args+=(--fetch-ngx-sdk --accept-nvidia-license)
        mkdir -p "$GAME"
        ((want_dlss)) && export LONGSHOT_NGX_SDK=$WORK/ngx-sdk
        # (make_game.py's own words for a failed SDK download are the Builder's, "untick NVIDIA DLSS": the kit's way is --no-dlss)
        no_dlss_hint=''
        ((want_dlss)) && no_dlss_hint=" This build was with NVIDIA DLSS: if NVIDIA's SDK could not be downloaded (offline?), bash start-squadala.sh --no-dlss builds the game without it."
        make_game "${build_args[@]}" || { game_fail "The build stopped. Its log: $LOGS/build.log$no_dlss_hint"; return 1; }
        [[ -n $(kit_game_exe) ]] || { game_fail "The build finished but there is no game in $GAME. Its log: $LOGS/build.log"; return 1; }
        printf '%s' "$game_want" >"$game_stamp"
}
if ((ready && !rebuild && !check_only)); then
    step 'The game: built already'
    charsel_confirm
elif game_build; then
    charsel_confirm
elif charsel_undo && game_want="$src_stamp$(exefs_stamp)" && [[ -n $(kit_game_exe) && $(cat "$game_stamp" 2>/dev/null) == "$game_want" ]]; then
    warn "$game_why"
    warn 'Character Select: the game could not be built with it, so it is taken back out for now and the game starts as it was (the next start tries again; character_select = no in data/coop-options.ini stops it).'
else
    fail "$game_why"
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
            # (kit-mirror: with retries; a file with another SHA-256 is deleted)
            OOT_COOP_FETCH_LOCAL=$fetch_local fetch_verified "$o_tgz" "$o_sha" "$o_url/$o_file" || o_why="the download failed ($KIT_FETCH_WHY)"
        fi
        o_had=$(cat "$o_dir/coop-ootrr.txt" 2>/dev/null || true)
        o_new=$GAME/tools/ootrr.new o_old=$GAME/tools/ootrr.old
        # (a swap stopped between its two moves: the old folder goes back first)
        [[ ! -d $o_dir && -f $o_old/ootrr ]] && mv "$o_old" "$o_dir"
        if [[ -z $o_why ]]; then
            # All or nothing (Windows: kit-paths.ps1 Install-KitOotrr): unpacked beside the old one, tried, then swapped,
            # so a failure keeps the OOTRR that was there; the old way deleted it first (player report 2026-11-19).
            rm -rf "$o_new" && mkdir -p "$o_new"
            # (one folder, ootrr-<version>-linux-x86_64/: ootrr, LICENSE, NOTICE, README.md, install.sh, ootrr.desktop)
            if ! tar -xzf "$o_tgz" -C "$o_new" --strip-components=1 || [[ ! -f $o_new/ootrr ]]; then
                o_why="unpacking $o_file failed"
            else
                chmod +x "$o_new/ootrr"
                # ('shuffle 1 1' prints one number at once and touches no file: the program runs on this system)
                if ! timeout 30 "$o_new/ootrr" shuffle 1 1 >/dev/null 2>&1; then
                    o_why="ootrr did not run here (a 1-second test failed)"
                else
                    printf '%s\n' "$o_want" >"$o_new/coop-ootrr.txt"
                    rm -rf "$o_old"
                    if { [[ ! -e $o_dir ]] || mv "$o_dir" "$o_old"; } && mv "$o_new" "$o_dir"; then
                        rm -rf "$o_old"
                    else
                        [[ ! -e $o_dir && -d $o_old ]] && mv "$o_old" "$o_dir"
                        o_why="it could not be put in place: close the game and start again"
                    fi
                fi
            fi
            rm -rf "$o_new"
        fi
        if [[ -z $o_why ]]; then
            # Another release than the one before: its seeds are other worlds and the old seed files do not build
            # with it, so every built seed goes and the game makes it again at its next start
            # (kit-paths.ps1 Remove-KitRandoSeeds on Windows).
            if [[ -n $o_had && ${o_had%% *} != "$o_rel" ]]; then
                for seed_dir in "$GAME"/mods/randomizer-*/; do
                    [[ -f ${seed_dir}coop-seed.txt ]] || continue
                    [[ -f ${seed_dir}coop-rando.txt ]] || continue  # (put in by hand: the player's, kept)
                    seed_name=$(basename "$seed_dir")
                    rm -rf "$seed_dir" "$GAME/mods/oot-remake-randomizer/$seed_name.ootrr" \
                        "$GAME/mods/oot-remake-randomizer/$seed_name.spoiler.txt"
                    say "  randomizer: seed $seed_name removed for OOTRR $o_rel: the game makes it again at its next start (another world: start a new save)"
                done
            fi
        fi
        # (how it went, for the game's chat and collect-logs: data/game/tools/ootrr-setup.txt, as on Windows)
        mkdir -p "$GAME/tools"
        if [[ $o_why == offline ]]; then
            printf 'failed %s: this PC had no internet at the last start\n' "$o_rel" >"$GAME/tools/ootrr-setup.txt"
            say "  offline: using the tools already here (the randomizer, OOTRR $o_rel, is set up at the next start with internet)"
        elif [[ -n $o_why ]]; then
            printf 'failed %s: %s\n' "$o_rel" "$o_why" >"$GAME/tools/ootrr-setup.txt"
            warn "the randomizer (OOTRR $o_rel) was not set up: $o_why. Co-op works without it; start again to retry."
        else
            printf 'ok %s\n' "$o_rel" >"$GAME/tools/ootrr-setup.txt"
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
# (the pack tool's own files too: a pack made by another version of the tool is made again, as the
# Windows kit's Test-KitPuppetPackStale does; the look files and the copies' marks change with it)
# (not Longshot's patches, src_stamp: the pack is made from the game file and the pack tool only, and every update that
# changed a patch made it again, 1 to 4 minutes; player report 210. The Windows kit never keyed it on them.)
pack_stamp="pack:2 menus:1 tool:$(cd "$DATA/tools/puppet_pack" && LC_ALL=C sha256sum -- *.py | sha256sum | cut -d' ' -f1)"
# (the Character Select mod's characters.json, its NPC models there and its row in the Mods tab's list: the pack's
# character puppets follow all three (tools/puppet_pack find_characters), so putting the mod in, replacing it, taking
# its NPC part out or in (charsel_step: ui / full), or switching it in the Mods tab makes the pack again, as the Windows
# kit's Get-KitCharacterSelectSeen. C #58, lane 051234: a pack made with the NPC models kept them after a UI-only switch)
for cs_json in "$GAME"/mods/*/characters.json; do
    [[ -f $cs_json && -d ${cs_json%/*}/romfs/Model ]] || continue
    cs_name=${cs_json#"$GAME"/mods/}; cs_name=${cs_name%/characters.json}
    [[ $cs_name == [-_.]* ]] && continue
    cs_row=$(awk -v n="$cs_name" 'NR == 1 { sub(/\r$/, ""); if ($0 != "coop-mods 1") exit } NR > 1 { sub(/\r$/, ""); s = $1; r = substr($0, length(s) + 2)
        if ((s == "on" || s == "off") && tolower(r) == tolower(n)) { print s; exit } }' "$GAME/mods/coop-mods.list" 2>/dev/null || true)
    cs_models=$(cd "${cs_json%/*}/romfs/Model" && LC_ALL=C ls -1 | { grep -E '^Player_(Child|Adult)_C[0-9]{3}\.Player_(Child|Adult)\.bfres\.zs$' || true; } | sha256sum | cut -d' ' -f1)
    pack_stamp+=" characters:$(sha256_of "$cs_json") models:$cs_models list:${cs_row:-on}"; break
done
# A file the pack's manifest names that is gone (a pack tool stopped half way, a hand-copied pack): the look files
# (coop_look/gear.bin, a colour's .bin) and each tunic's files, as the Windows kit's Get-KitPuppetPackGaps (pack-retry).
pack_gap() {
    [[ -f $mod/coop_puppet.json ]] || return 0
    "$PY" -I -B -c '
import json, os, sys
d = sys.argv[1]
try:
    m = json.load(open(os.path.join(d, "coop_puppet.json"), encoding="utf-8"))
except Exception:
    print("coop_puppet.json"); sys.exit()
want = list((m.get("look_files") or {}).values())
want += ["romfs/" + f for fs in (m.get("tunics") or {}).values() for f in (fs or [])]
gone = [f for f in want if f and not os.path.isfile(os.path.join(d, f))]
if gone:
    print(gone[0])
' "$mod" 2>/dev/null || true
}
# The game file gone or never named (pack-retry: players whose pack was never made): the usual places, the folder
# the remembered file was in first; only a file dumpcheck.py says is the game (its header), the remembered name first.
# Without dumpcheck.py nothing is taken: an unchecked file is never used (as on Windows).
find_pack_game() {
    local places=() f same='' good=()
    [[ -f $DATA/tools/puppet_pack/dumpcheck.py ]] || return 0
    [[ -n $pack_from ]] && places+=("$(dirname -- "$pack_from")")
    places+=("$KIT" "$(dirname "$KIT")" "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents")
    while IFS= read -r f; do
        "$PY" -I -B "$DATA/tools/puppet_pack/dumpcheck.py" --user-dir "$DATA" --romfs "$f" >/dev/null 2>&1 || continue
        good+=("$f")
        [[ -n $pack_from && $(basename -- "$f") == "$(basename -- "$pack_from")" ]] && same=$f
    done < <(find "${places[@]}" -maxdepth 2 -iname '*.nup' -type f 2>/dev/null | sort -u)
    if [[ -n $same ]]; then printf '%s\n' "$same"
    elif ((${#good[@]} == 1)); then printf '%s\n' "${good[0]}"
    fi
}
# The drag prompt is asked once, not at every start: data/pack-asked.txt keeps what it asked about (the game file named
# and the pack tool's hash), so a new game file or tool asks again; start-squadala.sh --pack removes it and asks.
# A pack made before for exactly this stamp (player report 210: Character Select switched off and on again on the Mods
# tab made the pack twice, 1 to 4 minutes each): kept in data/pack-variants when the pack is made again (pack_save,
# moved: no copy), and moved back here. The newest 2 stay. start-squadala.sh --pack (OOT_COOP_PACK_FRESH) always makes it.
pack_keep=$DATA/pack-variants
pack_keep_dir() { printf '%s/%s' "$pack_keep" "$(printf '%s' "$1" | sha256sum | cut -c1-20)"; }
pack_saved=''
pack_save() {
    local s d
    [[ -f $mod/coop_puppet.json && -f $mod/.coop-stamp && -z $(pack_gap) ]] || return 0
    s=$(cat "$mod/.coop-stamp")
    d=$(pack_keep_dir "$s")
    rm -rf "$d"
    { mkdir -p "$d" && mv "$mod" "$d/coop-puppet" && printf '%s' "$s" >"$d/key.txt"; } || { rm -rf "$d"; return 0; }
    pack_saved=$d
    ls -1t "$pack_keep"/*/key.txt 2>/dev/null | tail -n +3 | while IFS= read -r s; do rm -rf "$(dirname -- "$s")"; done
}
if [[ -z ${OOT_COOP_PACK_FRESH-} ]] && ! [[ -f $mod/coop_puppet.json && $(cat "$mod/.coop-stamp" 2>/dev/null) == "$pack_stamp" && -z $(pack_gap) ]]; then
    kd=$(pack_keep_dir "$pack_stamp")
    if [[ -f $kd/key.txt && $(cat "$kd/key.txt") == "$pack_stamp" && -f $kd/coop-puppet/coop_puppet.json ]]; then
        pack_save
        rm -rf "$mod"
        mkdir -p "$(dirname -- "$mod")"
        if mv "$kd/coop-puppet" "$mod" && [[ -z $(pack_gap) ]]; then
            rm -rf "$kd"
            # (its size tables dated now: a seed the game built beside another pack's tables is built again below)
            find "$mod/romfs/System/Resource" -name '*.rsizetable.zs' -type f -exec touch {} + 2>/dev/null || true
            say '  puppet pack: the one made before for these characters is put back (seconds, instead of making it again)'
        fi
    fi
fi
pack_asked=$DATA/pack-asked.txt
ask_key="${pack_from:-}|${pack_stamp##* tool:}"
if [[ ! -e ${pack_from:-} ]] && ! [[ -f $mod/coop_puppet.json && $(cat "$mod/.coop-stamp" 2>/dev/null) == "$pack_stamp" && -z $(pack_gap) ]]; then
    found=$(find_pack_game)
    if [[ -n $found ]]; then
        say "  Found your game file: $found (saved in data/game-files.txt)"
        pack_from=$found
        printf '%s\n' "$pack_from" >"$remembered"
    elif [[ $(cat "$pack_asked" 2>/dev/null) == "$ask_key" ]]; then
        say '  (Not asked again at every start: bash start-squadala.sh --pack asks for your game file and makes the pack.)'
    elif kit_tty && [[ -z ${CI-} ]]; then
        say "  The puppet pack (how you see the other players) is made from your game file, and the kit cannot find it$([[ -n $pack_from ]] && printf ' (not at %s any more)' "$pack_from")."
        a=''
        read -r -t 120 -p "  Drag your game file (.nup) onto this window and press Enter (Enter alone: start without the pack): " a || a=''
        a=${a#\'} a=${a%\'} a=${a#\"} a=${a%\"} a=${a#file://}
        a=${a%"${a##*[![:space:]]}"}
        if [[ -n $a && -e $a ]]; then
            pack_from=$a
            printf '%s\n' "$pack_from" >"$remembered"
            rm -f "$pack_asked"
        else
            [[ -z $a ]] || warn "not found: $a"
            printf '%s\n' "$ask_key" >"$pack_asked"
        fi
    fi
fi
# The pack with the title's Host/Join/Co-op Settings rows and the pause menu's Multiplayer tab (--title-rows
# --pause-tab, as the Windows kit's Get-KitPackArgs). They make the pack replace files the game reads at boot (each
# language's Mals archive, the Title and Common layout archives) with larger ones; start.sh's merge_tables.py takes
# them out of play when the size table the game reads cannot hold them (another mod's table wins).
gap=$(pack_gap)
if [[ -f $mod/coop_puppet.json && $(cat "$mod/.coop-stamp" 2>/dev/null) == "$pack_stamp" && -z $gap ]]; then
    say '  puppet pack: built already'
elif [[ -n $pack_from && -e $pack_from ]]; then
    [[ -n $gap && -f $mod/coop_puppet.json ]] && say "  puppet pack: incomplete ($gap is missing): made again"
    if [[ ! -f $mod/coop_puppet.json ]]; then say '  puppet pack: none yet'
    elif [[ $(cat "$mod/.coop-stamp" 2>/dev/null) != "$pack_stamp" ]]; then say '  puppet pack: made for another pack tool or other Character Select characters'
    fi
    say '  Making the puppet pack (how you see the other players; 1 to 4 minutes) ...'
    [[ -n ${OOT_COOP_PACK_FRESH-} ]] || pack_save   # (kept: switching Character Select back puts it back in seconds)
    if "$PY" -I -B "$DATA/tools/puppet_pack/build.py" --romfs "$pack_from" --mod-dir "$mod" --title-rows --pause-tab 2>&1 | sed 's/^/    /'; then
        printf '%s' "$pack_stamp" >"$mod/.coop-stamp"
        rm -f "$pack_asked"
    else
        rm -f "$mod/.coop-stamp"
        # (the pack there before goes back in, so a make that failed, a full disk or a tool error, leaves the pack the game
        # had; with its own stamp, another one, so the next start tries again)
        if [[ -n $pack_saved && -f $pack_saved/coop-puppet/coop_puppet.json ]] && rm -rf "$mod" && mv "$pack_saved/coop-puppet" "$mod"; then
            rm -rf "$pack_saved"
            warn 'the puppet pack could not be made again (the lines above say why): the one from before stays, so the game starts with it. Every start tries again.'
        else
            warn 'the puppet pack could not be made (the lines above say why): the game starts, and the other players show as names only until it is made. Every start tries again.'
        fi
    fi
else
    if [[ -n $pack_from ]]; then
        warn "the puppet pack was not made: your game file is not at $pack_from any more (data/game-files.txt names it; moved, renamed or its drive not mounted). Put it back, or start again with --game <your .nup>. Until then the other players show as names only."
    else
        warn 'the puppet pack was not made: the kit does not know your game file (no data/game-files.txt). Start again with --game <your .nup>. Until then the other players show as names only.'
    fi
fi
# Randomizer seeds built before this puppet pack: the game uses one size table, the seed's, and without the pack's
# entries it aborts while it loads. Built again with every other mod's table beside it (as the module does,
# coop_rando StageSizeTables), from the seed file the game kept; removed when that cannot be done (the game builds
# it at its next start). (kit-paths.ps1 Update-KitRandoSeeds on Windows)
rando_tables() {   # the size tables of the mods the game loads besides the seeds
    local t rel
    while IFS= read -r t; do
        rel=${t#"$GAME/mods/"}
        [[ $rel =~ ^(randomizer-[^/]*|oot-remake-randomizer)/ || /$rel =~ /[-_] ]] || printf '%s\n' "$t"
    done < <(find "$GAME/mods" -mindepth 4 -maxdepth 7 -path '*/romfs/System/Resource/*.rsizetable.zs' -type f 2>/dev/null)
}
pack_table=$(ls -t "$mod"/romfs/System/Resource/*.rsizetable.zs 2>/dev/null | head -n 1 || true)
# What a seed is built beside, by content (data/game/mods/randomizer-*/coop-seed-tables.txt, written below; the Windows
# kit's Get-KitSeedTablesText): a seed with it is built again only when one of those tables changed (player report 210:
# a pack put back with an older date, or a table written again with the same entries). One without it: the date rule.
seed_tables_text() {
    local t
    while IFS= read -r t; do
        [[ -n $t ]] && printf '%s %s\n' "${t#"$GAME/mods/"}" "$(sha256_of "$t")"
    done < <(rando_tables) | LC_ALL=C sort
}
seed_tables_now=''
for seed_dir in "$GAME"/mods/randomizer-*/; do
    # (only seeds the game built, coop-rando.txt: one put in by hand gets the pack's entries from merge_tables.py)
    [[ -n $pack_table && -f ${seed_dir}coop-seed.txt && -f ${seed_dir}coop-rando.txt ]] || continue
    seed_name=$(basename "$seed_dir")
    seed_table=$(ls "${seed_dir}"romfs/System/Resource/*.rsizetable.zs 2>/dev/null | head -n 1 || true)
    if [[ -f ${seed_dir}coop-seed-tables.txt ]]; then
        [[ -n $seed_tables_now ]] || seed_tables_now=$(seed_tables_text)
        [[ $(cat "${seed_dir}coop-seed-tables.txt") == "$seed_tables_now" ]] && continue
    elif [[ -n $seed_table && ! $pack_table -nt $seed_table ]]; then
        continue
    fi
    control=$GAME/mods/oot-remake-randomizer stage=$GAME/mods/oot-remake-randomizer/_seed-build
    rm -rf "$stage"
    ok=''
    if [[ -x $GAME/tools/ootrr/ootrr && -f $control/$seed_name.ootrr && -d $GAME/romfs/System ]]; then
        n=0
        while IFS= read -r t; do
            [[ -n $t ]] || continue
            mkdir -p "$stage/$n/romfs/System/Resource" && cp "$t" "$stage/$n/romfs/System/Resource/" && n=$((n + 1))
        done < <(rando_tables)
        if "$GAME/tools/ootrr/ootrr" build "$control/$seed_name.ootrr" "$GAME/romfs" "$stage" "$seed_name" --host hookshot \
            >"$control/last-build.txt" 2>&1 && [[ -f $stage/$seed_name/coop-seed.txt ]]; then
            # the game's own files OOTRR's build does not write: the preset, and the spoiler log and settings
            # string the players share (the same seed file: the same seed)
            for keep in coop-rando.txt spoiler.txt settings.txt; do
                [[ -f ${seed_dir}$keep ]] && cp "${seed_dir}$keep" "$stage/$seed_name/"
            done
            [[ -n $seed_tables_now ]] || seed_tables_now=$(seed_tables_text)
            printf '%s\n' "$seed_tables_now" >"$stage/$seed_name/coop-seed-tables.txt"
            rm -rf "$seed_dir" && mv "$stage/$seed_name" "$GAME/mods/$seed_name" && ok=1
        fi
    fi
    rm -rf "$stage"
    if [[ -n $ok ]]; then say "  randomizer: seed $seed_name built again for the new puppet pack"
    else rm -rf "$seed_dir"; say "  randomizer: seed $seed_name removed (could not build it again for the new puppet pack): the game builds it at its next start"
    fi
done
say ''
say 'The game is ready.'
