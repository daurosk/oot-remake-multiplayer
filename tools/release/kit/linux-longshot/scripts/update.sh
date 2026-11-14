#!/usr/bin/env bash
# Updates this Linux kit to the newest co-op release, keeping everything of yours.
#   bash scripts/update.sh            update (asks first)
#   bash scripts/update.sh --check    only say whether a newer release is out
#   bash scripts/update.sh --status   one line for start.sh: "newer <version>", "unfinished",
#                                     "failed <why>", or nothing
#   bash scripts/update.sh --launch   update without asking, for start.sh (exit 3: updated)
# As tools/release/kit/linux/update.sh, for the zip: it reads the kit's version from VERSION.txt,
# asks GitHub for the releases of daurosk/oot-remake-multiplayer and takes the newest with a Linux
# zip (oot-coop-<version>-linux.zip and its .sha256). If that is newer, it downloads it,
# checks the SHA-256 (and the release signature when the GitHub CLI is logged in; verify_signature = no in
# data/coop-options.ini skips that, not the SHA-256), unpacks it to a temp folder and copies the kit's own files (MANIFEST.txt)
# over this kit. Every file it replaces is first copied to backup-<old version>/; if anything fails,
# those are put back. VERSION.txt is written last; update-in-progress.txt marks an update that has
# started, so a stopped one is done again by the next start.
# Never touched: data/game (the built game, your saves and settings), data/work, data/builder,
# data/logs, data/game-files.txt, backup-*/ and every file that is not part of the kit.
# data/coop-options.ini keeps your values; a switch the new kit adds is appended as a '#' line.
# The next start then builds again only what the new version changed (scripts/build.sh's stamps).
# For tests: OOT_COOP_UPDATE_API names the release list (a URL or a local .json file), and the
# download addresses in it may then be local files (OOT_COOP_FETCH_LOCAL is set for them).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
action=update yes=0
while (($#)); do
    case $1 in
        --yes | -y) yes=1 ;;
        --check) action=check ;;
        --status) action=status ;;
        --launch) action=launch; yes=1 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
    shift
done
API=${OOT_COOP_UPDATE_API:-https://api.github.com/repos/$REPO/releases?per_page=30}
[[ -n ${OOT_COOP_UPDATE_API-} ]] && export OOT_COOP_FETCH_LOCAL=1
MARKER=update-in-progress.txt

# Outside tests, only this repository's own release downloads are fetched.
url_ok() {
    [[ -n ${OOT_COOP_UPDATE_API-} ]] && return 0
    [[ $1 == "https://github.com/$REPO/releases/download/"* ]]
}

# The newest release with a Linux zip: prints "tag zip_url sha_url". Exit 2: no release list.
newest_release() {
    local list
    list=$(mktemp)
    KIT_QUIET_FETCH=1 fetch "$API" "$list" 2>/dev/null || { rm -f "$list"; return 2; }
    grep -o '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]*"' "$list" | sed 's/.*:[[:space:]]*"//; s/"$//' >"$list.urls"
    rm -f "$list"
    local url name tag sha best='' best_zip='' best_sha=''
    while IFS= read -r url; do
        name=${url##*/}
        [[ $name =~ ^oot-coop-(.+)-linux\.zip$ ]] || continue
        tag=${BASH_REMATCH[1]}
        [[ $(kit_version_compare "$tag" 0.0.0-a) == 1 ]] || continue   # a version, not a draft name
        sha=$(grep -F -- "/$name.sha256" "$list.urls" | head -n1)
        [[ -n $sha ]] || continue
        url_ok "$url" && url_ok "$sha" || continue
        if [[ -z $best || $(kit_version_compare "$tag" "$best") == 1 ]]; then best=$tag best_zip=$url best_sha=$sha; fi
    done <"$list.urls"
    rm -f "$list.urls"
    [[ -n $best ]] && printf '%s %s %s\n' "$best" "$best_zip" "$best_sha"
}

current=$(kit_version)
if [[ $action == status ]]; then
    [[ -f $KIT/$MARKER ]] && { echo unfinished; exit 0; }
    found=$(newest_release)
    (($? == 2)) && { echo 'failed GitHub did not answer (no network, or GitHub turned the check away)'; exit 0; }
    read -r tag _ <<<"$found"
    [[ -n ${tag-} && -n $current && $(kit_version_compare "$tag" "$current") == 1 ]] && echo "newer $tag"
    exit 0
fi

read -r tag zip_url sha_url < <(newest_release) || { echo 'Could not read the release list (no network, or GitHub is down).'; exit 1; }
if [[ ! -f $KIT/$MARKER ]] && [[ -n $current && $(kit_version_compare "$tag" "$current") != 1 ]]; then
    echo "This kit is ${current:-not a release}; the newest release is $tag. Nothing to do."
    exit 0
fi
if [[ $action == check ]]; then
    echo "Co-op $tag is out (this kit: ${current:-not a release}). Start start-oot-coop.sh to update."
    exit 0
fi
if ((!yes)); then
    read -r -p "Update this kit from ${current:-?} to $tag? Your saves and settings are kept. [Y/n] " a || a=n
    [[ $a =~ ^[nN] ]] && exit 1
fi
kit_game_running && { echo 'Close the game of this kit first.'; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-update-XXXXXX")
trap 'rm -rf "$work"' EXIT
echo "Downloading co-op $tag ..."
fetch "$zip_url" "$work/kit.zip" && KIT_QUIET_FETCH=1 fetch "$sha_url" "$work/kit.sha256" || { echo 'The download failed.'; exit 1; }
want=$(grep -oE '[0-9a-fA-F]{64}' "$work/kit.sha256" | head -n1 | tr 'A-F' 'a-f')
[[ -n $want && $want == "$(sha256_of "$work/kit.zip")" ]] || { echo 'The download is damaged (its SHA-256 does not match); nothing was changed.'; exit 1; }
# The release signature, when gh can check it (as update.ps1 and the old Linux kit's update.sh do).
verify=auto
grep -qE '^[[:space:]]*verify_signature[[:space:]]*=[[:space:]]*no[[:space:]]*$' "$DATA/coop-options.ini" 2>/dev/null && verify=no
if [[ $verify == no ]]; then
    echo 'Signature check skipped (verify_signature = no).'
elif [[ $zip_url != https://* ]] || ! command -v gh >/dev/null; then
    :  # a local test download, or no GitHub CLI: the SHA-256 is the check
elif [[ -z ${OOT_COOP_VERIFY_NO_AUTH_CHECK-} ]] && ! gh auth status >/dev/null 2>&1; then
    echo "Signature not checked: the GitHub CLI is not logged in ('gh auth login'). The SHA-256 is checked."
else
    cp "$work/kit.zip" "$work/${zip_url##*/}"
    if gh attestation verify "$work/${zip_url##*/}" --repo "$REPO" --signer-workflow "$REPO/.github/workflows/release-attest.yml" >"$work/verify.txt" 2>&1; then
        echo 'Signature: OK (signed by our release workflow).'
    else
        tail -n 2 "$work/verify.txt"
        echo "The download's release signature does not check. Nothing changed."
        echo 'A release only minutes old may not be signed yet: try again later. To update on the SHA-256 alone,'
        echo "put 'verify_signature = no' in data/coop-options.ini."
        exit 1
    fi
fi
unzip_to "$work/kit.zip" "$work/new" >/dev/null || { echo 'The download is damaged (it does not unpack); nothing was changed.'; exit 1; }
new=$(find "$work/new" -mindepth 1 -maxdepth 1 -type d | head -n1)
[[ -n $new && -f $new/MANIFEST.txt && -f $new/VERSION.txt && -f $new/start-oot-coop.sh ]] ||
    { echo 'The download is not a Linux co-op kit; nothing was changed.'; exit 1; }

# A path the updater never writes or removes.
player_path() {
    case $1 in
        data/game | data/game/* | data/work | data/work/* | data/builder | data/builder/* | data/logs | data/logs/* | \
            data/game-files.txt | data/.start.lock | backup-* | coop-logs-* | "$MARKER") return 0 ;;
    esac
    return 1
}
plain_path() { [[ -n $1 && $1 != /* && ! $1 =~ (^|/)\.\.?(/|$) ]]; }

backup=$KIT/backup-${current:-unknown}
mkdir -p "$backup"
echo "Updating this kit from ${current:-?} to $tag (old files go to ${backup##*/}/)." | tee "$KIT/$MARKER"
restore() {
    echo 'The update failed; putting the old files back.'
    (cd "$backup" && find . -type f) | while IFS= read -r f; do
        f=${f#./}
        mkdir -p "$KIT/$(dirname "$f")" && cp -p -- "$backup/$f" "$KIT/$f"
    done
    rm -f "$KIT/$MARKER"
    exit 1
}
keep_old() {   # keep_old REL: this kit's file to the backup before it changes
    [[ -f $KIT/$1 && ! -f $backup/$1 ]] || return 0
    mkdir -p "$backup/$(dirname "$1")" && cp -p -- "$KIT/$1" "$backup/$1"
}

# Files that left the kit: moved to the backup.
if [[ -f $KIT/MANIFEST.txt ]]; then
    while IFS= read -r rel; do
        rel=${rel%$'\r'}
        plain_path "$rel" && ! player_path "$rel" || continue
        grep -qxF -- "$rel" "$new/MANIFEST.txt" && continue
        if [[ -f $KIT/$rel ]]; then keep_old "$rel" && rm -f -- "$KIT/$rel" || restore; fi
    done <"$KIT/MANIFEST.txt"
fi
# The new kit's files; MANIFEST.txt and VERSION.txt last.
mapfile -t files < <(grep -vxE 'MANIFEST.txt|VERSION.txt' "$new/MANIFEST.txt" | tr -d '\r'; printf '%s\n' MANIFEST.txt VERSION.txt)
for rel in "${files[@]}"; do
    plain_path "$rel" && ! player_path "$rel" || continue
    [[ -f $new/$rel ]] || continue
    if [[ $rel == data/coop-options.ini && -f $KIT/$rel ]]; then
        # The player's values stay; a switch the new kit names and this file does not is appended
        # behind '#', with the comment above it in the new file.
        keep_old "$rel" || restore
        awk 'NR == FNR { if (match($0, /LICHII_COOP_[A-Z0-9_]+|auto_update/)) seen[substr($0, RSTART, RLENGTH)] = 1; next }
             /^[[:space:]]*#/ && !match($0, /LICHII_COOP_[A-Z0-9_]+[[:space:]]*=/) { note = note $0 "\n"; next }
             match($0, /LICHII_COOP_[A-Z0-9_]+|auto_update/) {
                 n = substr($0, RSTART, RLENGTH)
                 if (!(n in seen)) { line = $0; sub(/^[[:space:]]*#?[[:space:]]*/, "", line); add = add note "# " line "\n"; seen[n] = 1 }
             }
             { note = "" }
             END { if (add != "") printf "\n%s", add }' "$KIT/$rel" "$new/$rel" >"$work/options.add" || restore
        cat "$work/options.add" >>"$KIT/$rel" || restore
        continue
    fi
    keep_old "$rel" || restore
    # (beside, then renamed over: start.sh, which runs this, is read by bash as it goes and keeps
    # reading its old file; writing into that file would change the script under its feet)
    mkdir -p "$KIT/$(dirname "$rel")" && cp -p -- "$new/$rel" "$KIT/$rel.update-new" &&
        mv -f -- "$KIT/$rel.update-new" "$KIT/$rel" || restore
done
chmod +x "$KIT/start-oot-coop.sh" "$KIT"/scripts/*.sh 2>/dev/null || true
rm -f "$KIT/$MARKER"
echo "Done: this kit is co-op $tag. Your old files are in ${backup##*/}/."
[[ $action == launch ]] && exit 3
exit 0
