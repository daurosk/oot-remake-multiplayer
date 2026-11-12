#!/usr/bin/env bash
# Updates a Linux co-op kit to the newest release, keeping everything of the player's.
#   ./update.sh [--kit DIR] [--yes]   update this kit (asks before it starts, unless --yes)
#   ./update.sh --check               only say whether a newer release is out
#   ./update.sh --status              one line for play.sh: "newer <version>", "unfinished", or
#                                     nothing (none, or no network)
#   ./update.sh --launch              update without asking, for play.sh (exit 3: updated, start
#                                     the new kit)
# What it does (as the Windows kit's Update.bat): reads the kit's version from VERSION.txt, asks
# GitHub for the releases of daurosk/oot-remake-multiplayer and takes the newest one with a Linux
# kit (oot-coop-<version>-linux-<arch>.tar.gz and its .sha256). If that is newer, it downloads it,
# checks the SHA-256, unpacks it to a temp folder and copies the kit's own files (MANIFEST.txt)
# over this kit. Every file it replaces is first copied to backup-<old version>/; if anything
# fails, those are put back. VERSION.txt is written last, and update-in-progress.txt marks an
# update that has started, so a stopped one is done again by the next run.
# A new emulator/lichii.AppImage moves the shader cache (emulator/user/cache/shader) to the
# backup too: an old cache can crash a new emulator build.
# Never touched: emulator/user/ (keys, firmware, saves, settings, coop.ini, the puppet pack),
# logs/, backup-*/ and every file that is not part of the kit. coop-options.ini keeps your
# values; a switch the new kit adds is appended as a '#' line.
# For tests: OOT_COOP_UPDATE_API names the release list (a URL or a local .json file), and the
# download addresses in it may then be local files.
set -u
self_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KIT=$self_dir
action=update yes=0
while (($#)); do
    case $1 in
        --kit) KIT=$(cd "$2" && pwd); shift ;;
        --yes | -y) yes=1 ;;
        --check) action=check ;;
        --status) action=status ;;
        --launch) action=launch; yes=1 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
    shift
done
. "$KIT/scripts/kit-lib.sh" 2>/dev/null || . "$self_dir/scripts/kit-lib.sh"
REPO=daurosk/oot-remake-multiplayer
API=${OOT_COOP_UPDATE_API:-https://api.github.com/repos/$REPO/releases?per_page=30}
ARCH=$(uname -m)
MARKER=update-in-progress.txt

fetch() {  # fetch FROM TO
    case $1 in
        http://* | https://*)
            if command -v curl >/dev/null; then curl -fsSL --max-time 300 -o "$2" "$1"
            else wget -q -T 300 -O "$2" "$1"; fi ;;
        *) [[ -n ${OOT_COOP_UPDATE_API-} ]] && cp -- "${1#file://}" "$2" ;;
    esac
}
# Outside tests, only this repository's own release downloads are fetched.
url_ok() {
    [[ -n ${OOT_COOP_UPDATE_API-} ]] && return 0
    [[ $1 == "https://github.com/$REPO/releases/download/"* ]]
}

# The newest release with a Linux kit for this machine: prints "tag tar_url sha_url".
newest_release() {
    local list
    list=$(mktemp)
    fetch "$API" "$list" 2>/dev/null || { rm -f "$list"; return 1; }
    grep -o '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]*"' "$list" | sed 's/.*:[[:space:]]*"//; s/"$//' >"$list.urls"
    rm -f "$list"
    local url name tag best='' best_tar='' best_sha=''
    while IFS= read -r url; do
        name=${url##*/}
        [[ $name =~ ^oot-coop-(.+)-linux-$ARCH\.tar\.gz$ ]] || continue
        tag=${BASH_REMATCH[1]}
        [[ $(kit_version_compare "$tag" 0.0.0-a) == 1 ]] || continue  # a version, not a draft name
        local sha
        sha=$(grep -F -- "/$name.sha256" "$list.urls" | head -n1)
        [[ -n $sha ]] || continue
        url_ok "$url" && url_ok "$sha" || continue
        if [[ -z $best || $(kit_version_compare "$tag" "$best") == 1 ]]; then best=$tag best_tar=$url best_sha=$sha; fi
    done <"$list.urls"
    rm -f "$list.urls"
    [[ -n $best ]] && printf '%s %s %s\n' "$best" "$best_tar" "$best_sha"
}

current=$(kit_version "$KIT")
if [[ $action == status ]]; then
    if [[ -f $KIT/$MARKER ]]; then echo unfinished; exit 0; fi
    read -r tag _ < <(newest_release) || exit 0
    [[ -n $current && $(kit_version_compare "$tag" "$current") == 1 ]] && echo "newer $tag"
    exit 0
fi

read -r tag tar_url sha_url < <(newest_release) || { echo 'Could not read the release list (no network, or GitHub is down).'; exit 1; }
if [[ ! -f $KIT/$MARKER ]] && [[ -n $current && $(kit_version_compare "$tag" "$current") != 1 ]]; then
    echo "This kit is ${current:-not a release}; the newest release is $tag. Nothing to do."
    exit 0
fi
if [[ $action == check ]]; then
    echo "Co-op $tag is out (this kit: ${current:-not a release}). Run ./update.sh to update."
    exit 0
fi
if ((!yes)); then
    read -r -p "Update this kit from ${current:-?} to $tag? Your saves, keys and settings are kept. [Y/n] " a || a=n
    [[ $a =~ ^[nN] ]] && exit 1
fi
if pgrep -f -- "$KIT/(emulator/lichii.AppImage|server/coop_server)" >/dev/null 2>&1; then
    echo 'Close the emulator and the server of this kit first.'
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-update-XXXXXX")
trap 'rm -rf "$work"' EXIT
echo "Downloading co-op $tag ..."
fetch "$tar_url" "$work/kit.tar.gz" && fetch "$sha_url" "$work/kit.sha256" || { echo 'The download failed.'; exit 1; }
want=$(grep -oE '[0-9a-fA-F]{64}' "$work/kit.sha256" | head -n1 | tr 'A-F' 'a-f')
have=$(sha256sum "$work/kit.tar.gz" | cut -d' ' -f1)
[[ -n $want && $want == "$have" ]] || { echo 'The download is damaged (its SHA-256 does not match); nothing was changed.'; exit 1; }
mkdir "$work/new"
tar -xzf "$work/kit.tar.gz" -C "$work/new" || { echo 'The download is damaged (it does not unpack); nothing was changed.'; exit 1; }
new=$(find "$work/new" -mindepth 1 -maxdepth 1 -type d | head -n1)
[[ -n $new && -f $new/MANIFEST.txt && -f $new/VERSION.txt ]] || { echo 'The download is not a co-op kit; nothing was changed.'; exit 1; }

# A path the updater never writes or removes.
player_path() {
    case $1 in
        emulator/user | emulator/user/* | logs | logs/* | backup-* | coop-logs-* | "$MARKER") return 0 ;;
    esac
    return 1
}
plain_path() { [[ -n $1 && $1 != /* && ! $1 =~ (^|/)\.\.?(/|$) ]]; }

backup=$KIT/backup-${current:-unknown}
mkdir -p "$backup"
echo "Updating this kit from ${current:-?} to $tag (old files go to ${backup##*/}/) ..." | tee "$KIT/$MARKER" >/dev/null
echo "Updating this kit from ${current:-?} to $tag."
restore() {
    echo 'The update failed; putting the old files back.'
    (cd "$backup" && find . -type f) | while IFS= read -r f; do
        f=${f#./}
        mkdir -p "$KIT/$(dirname "$f")" && cp -p -- "$backup/$f" "$KIT/$f"
    done
    rm -f "$KIT/$MARKER"
    exit 1
}
keep_old() {  # keep_old REL: copy this kit's file to the backup before it changes
    [[ -f $KIT/$1 && ! -f $backup/$1 ]] || return 0
    mkdir -p "$backup/$(dirname "$1")" && cp -p -- "$KIT/$1" "$backup/$1"
}

# A new emulator: the old shader cache goes to the backup.
if [[ -f $KIT/emulator/lichii.AppImage ]] && ! cmp -s "$KIT/emulator/lichii.AppImage" "$new/emulator/lichii.AppImage"; then
    if [[ -d $KIT/emulator/user/cache/shader ]]; then
        mkdir -p "$backup/emulator-user-cache" && mv "$KIT/emulator/user/cache/shader" "$backup/emulator-user-cache/shader"
        echo 'A new emulator: the shader cache was moved to the backup (it is built again as you play).'
    fi
fi

# Files that left the kit: moved to the backup.
if [[ -f $KIT/MANIFEST.txt ]]; then
    while IFS= read -r rel; do
        rel=${rel%$'\r'}
        plain_path "$rel" && ! player_path "$rel" || continue
        grep -qxF -- "$rel" "$new/MANIFEST.txt" && continue
        if [[ -f $KIT/$rel ]]; then keep_old "$rel" && rm -f -- "$KIT/$rel" || restore; fi
    done <"$KIT/MANIFEST.txt"
fi
# The new kit's files, VERSION.txt and MANIFEST.txt last.
mapfile -t files < <(grep -vxE 'MANIFEST.txt|VERSION.txt' "$new/MANIFEST.txt" | tr -d '\r'; printf '%s\n' MANIFEST.txt VERSION.txt)
for rel in "${files[@]}"; do
    plain_path "$rel" && ! player_path "$rel" || continue
    [[ -f $new/$rel ]] || continue
    if [[ $rel == coop-options.ini && -f $KIT/$rel ]]; then
        # The player's values stay; a switch the new kit names and this file does not is appended
        # behind '#', with the comment above it in the new file.
        keep_old "$rel" || restore
        awk 'NR == FNR { if (match($0, /(LICHII_COOP|EDEN_NX2)_[A-Z0-9_]+|auto_update/)) seen[substr($0, RSTART, RLENGTH)] = 1; next }
             /^[[:space:]]*#/ && !match($0, /(LICHII_COOP|EDEN_NX2)_[A-Z0-9_]+[[:space:]]*=/) { note = note $0 "\n"; next }
             match($0, /(LICHII_COOP|EDEN_NX2)_[A-Z0-9_]+|auto_update/) {
                 n = substr($0, RSTART, RLENGTH)
                 if (!(n in seen)) { line = $0; sub(/^[[:space:]]*#?[[:space:]]*/, "", line); add = add note "# " line "\n"; seen[n] = 1 }
             }
             { note = "" }
             END { if (add != "") printf "\n%s", add }' "$KIT/$rel" "$new/$rel" >"$work/options.add" || restore
        cat "$work/options.add" >>"$KIT/$rel" || restore
        continue
    fi
    keep_old "$rel" || restore
    mkdir -p "$KIT/$(dirname "$rel")" && cp -p -- "$new/$rel" "$KIT/$rel" || restore
done
rm -f "$KIT/$MARKER"
echo "Done: this kit is co-op $tag. Your old files are in ${backup##*/}/."
[[ $action == launch ]] && exit 3
exit 0
