#!/usr/bin/env bash
# Updates this Linux kit to the newest co-op release, keeping everything of yours.
#   bash scripts/update.sh            update (asks first)
#   bash scripts/update.sh --check    only say whether a newer release is out
#   bash scripts/update.sh --status   one line for start.sh: "newer <version>", "unfinished",
#                                     "failed <why>", or nothing
#   bash scripts/update.sh --launch   update without asking, for start.sh (exit 3: updated)
#   bash scripts/update.sh --version v0.11.0   install that release instead of the newest (older ones too)
#   bash scripts/update.sh --revert   go back to the version this kit had before (revert.sh)
# Going back (as update.ps1 on Windows): every update keeps the kit's own files of the version it replaces in
# previous/<version>/ (the last 2; never data/game, your saves, settings, coop-options.ini or logs); --revert puts
# the newest older one back (or a backup-<version>/ an update left). This updater (scripts/update.sh, revert.sh)
# stays the newer one, and stay-on-version.txt keeps the start from updating straight back: only a release newer
# than the one left is offered, and asked about. An update forward forgets it.
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
action=update yes=0 want_tag=''
while (($#)); do
    case $1 in
        --yes | -y) yes=1 ;;
        --check) action=check ;;
        --status) action=status ;;
        --launch) action=launch; yes=1 ;;
        --revert) action=revert ;;
        --version)
            shift
            want_tag=${1-}
            [[ $want_tag =~ ^v?[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || { echo 'Use: --version v0.11.0 (a release tag)'; exit 2; }
            [[ $want_tag == v* ]] || want_tag=v$want_tag ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
    shift
done
API=${OOT_COOP_UPDATE_API:-https://api.github.com/repos/$REPO/releases?per_page=30}
[[ -n ${OOT_COOP_UPDATE_API-} ]] && export OOT_COOP_FETCH_LOCAL=1
MARKER=update-in-progress.txt
STAY=stay-on-version.txt PREV=previous KEEP_PREV=2
# Never replaced when going back: the newer updater knows stay-on-version.txt (the old one would update straight back).
UPDATER_FILES='scripts/update.sh revert.sh'

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
        [[ -z ${WANT_TAG-} || ${tag,,} == "${WANT_TAG,,}" ]] || continue
        [[ $(kit_version_compare "$tag" 0.0.0-a) == 1 ]] || continue   # a version, not a draft name
        sha=$(grep -F -- "/$name.sha256" "$list.urls" | head -n1)
        [[ -n $sha ]] || continue
        url_ok "$url" && url_ok "$sha" || continue
        if [[ -z $best || $(kit_version_compare "$tag" "$best") == 1 ]]; then best=$tag best_zip=$url best_sha=$sha; fi
    done <"$list.urls"
    rm -f "$list.urls"
    [[ -n $best ]] && printf '%s %s %s\n' "$best" "$best_zip" "$best_sha"
}

# A release version, nothing else: it names folders below (previous/<version>/, backup-<version>/), so a damaged or
# hand-edited VERSION.txt ("(release ..)", "(release ../..)") must never become a path.
ver_ok() { [[ $1 =~ ^v?[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ && $1 != *..* ]]; }
current=$(kit_version)
ver_ok "$current" || current=''   # (then this kit counts as no release: the update goes on, nothing is kept)

# A path the updater never writes or removes.
player_path() {
    case $1 in
        data/game | data/game/* | data/work | data/work/* | data/builder | data/builder/* | data/logs | data/logs/* | \
            data/game-files.txt | data/.start.lock | backup-* | coop-logs-* | "$MARKER" | "$PREV" | "$PREV"/* | "$STAY") return 0 ;;
    esac
    return 1
}
plain_path() { [[ -n $1 && $1 != /* && ! $1 =~ (^|/)\.\.?(/|$) ]]; }
dir_version() {   # the release of a kit folder's VERSION.txt
    local first
    [[ -f $1/VERSION.txt ]] || return 0
    IFS= read -r first <"$1/VERSION.txt" || true
    [[ $first =~ \(release\ ([^\)]+)\) ]] || return 0
    local v=${BASH_REMATCH[1]}   # (ver_ok matches again, which changes BASH_REMATCH)
    if ver_ok "$v"; then printf '%s\n' "$v"; fi
}
# Removes a folder, only one directly inside previous/ (never the kit, never above it).
prev_rm() {
    local name=${1#"$KIT/$PREV/"}
    name=${name%/}
    [[ $1 == "$KIT/$PREV/"* && -n $name && $name != */* && $name != . && $name != .. ]] || { warn "not removed (outside $PREV/): $1"; return 1; }
    rm -rf -- "$KIT/$PREV/$name"
}
# A kept kit folder that holds its whole version: every file its own MANIFEST.txt names (a backup-*/ of an update
# that failed while copying holds only some).
kit_complete() {
    local rel
    [[ -f $1/MANIFEST.txt && -f $1/start-oot-coop.sh ]] || return 1
    while IFS= read -r rel; do
        rel=${rel%$'\r'}
        [[ -z $rel || $rel == '#'* || $rel == data/coop-options.ini ]] && continue
        plain_path "$rel" && ! player_path "$rel" || continue
        [[ -f $1/$rel ]] || return 1
    done <"$1/MANIFEST.txt"
}
# The version left, when the player went back to this one on purpose (stay-on-version.txt about this version).
stay_left() {
    local f=$KIT/$STAY s l
    [[ -f $f && -n $current ]] || return 0
    s=$(sed -n 's/^stay=//p' "$f" | tr -d '\r' | head -n1)
    l=$(sed -n 's/^left=//p' "$f" | tr -d '\r' | head -n1)
    [[ -n $s && -n $l && $(kit_version_compare "$s" "$current") == 0 ]] && printf '%s\n' "$l"
}
left=$(stay_left)
# A release the start offers: newer than this kit and, after going back, newer than the version left.
wanted() {
    [[ -n $1 && -n $current && $(kit_version_compare "$1" "$current") == 1 ]] || return 1
    [[ -z $left || $(kit_version_compare "$1" "$left") == 1 ]]
}
same_version_text() {
    say "Every player in a session needs the same version: $1 does not talk to other versions (the co-op protocol differs)."
    say "Tell your friends to go back too (revert.sh, or bash scripts/update.sh --version $1), or you cannot play together."
}
# How the game changes from kit $1 to kit $2: same, patches (the next start updates it: minutes) or pin (built again).
game_change() {
    local a b
    a=$(cat "$1/data/longshot/PIN" "$1/data/longshot/LINUX.txt" 2>/dev/null | tr -d '\r' | grep -E '^(release|commit|tag)=' || true)
    b=$(cat "$2/data/longshot/PIN" "$2/data/longshot/LINUX.txt" 2>/dev/null | tr -d '\r' | grep -E '^(release|commit|tag)=' || true)
    if [[ $a != "$b" ]]; then echo pin; return; fi
    a=$(cat "$1"/data/longshot/patches/*.patch 2>/dev/null | sha256sum | cut -d" " -f1)
    b=$(cat "$2"/data/longshot/patches/*.patch 2>/dev/null | sha256sum | cut -d" " -f1)
    if [[ $a != "$b" ]]; then echo patches; else echo same; fi
}
game_change_text() {
    case $1 in
        pin) say "The co-op game is built again for $2 on the next start (Longshot changed between these versions: 15 to 45 minutes). Your saves are kept." ;;
        patches) say "The next start updates the co-op game for $2 (a few minutes). Your saves are kept." ;;
    esac
}

if [[ $action == status ]]; then
    [[ -f $KIT/$MARKER ]] && { echo unfinished; exit 0; }
    found=$(newest_release)
    (($? == 2)) && { echo 'failed GitHub did not answer (no network, or GitHub turned the check away)'; exit 0; }
    read -r tag _ <<<"$found"
    wanted "${tag-}" && echo "newer $tag"
    exit 0
fi

back=0
if [[ $action == revert ]]; then
    [[ -f $KIT/$MARKER ]] && { echo 'An update of this kit did not finish: run bash scripts/update.sh to finish it first, then revert.sh.'; exit 1; }
    tag='' new=''
    for d in "$KIT/$PREV"/*/ "$KIT"/backup-*/; do
        d=${d%/}
        [[ -d $d && $d != *.part ]] && kit_complete "$d" || continue
        v=$(dir_version "$d")
        [[ -n $v ]] || continue
        [[ -n $current && $(kit_version_compare "$v" "$current") != -1 ]] && continue
        if [[ -z $tag || $(kit_version_compare "$v" "$tag") == 1 ]]; then tag=$v new=$d; fi
    done
    if [[ -z $tag ]]; then
        echo 'This kit keeps no older version (an update keeps the one it replaces, from 0.11 on). To install an older'
        echo "release: bash scripts/update.sh --version <tag>, such as --version v0.11.0 (https://github.com/$REPO/releases)."
        exit 1
    fi
    echo "This kit: ${current:-?}. Going back to $tag (kept in ${new#"$KIT"/}/)."
    same_version_text "$tag"
    game_change_text "$(game_change "$KIT" "$new")" "$tag"
    echo "Your saves, settings and coop-options.ini are kept, and so is ${current:-this version}: bash scripts/update.sh goes forward again."
    if ((!yes)); then
        read -r -p "Go back to $tag? [Y/n] " a || a=y
        [[ $a =~ ^[nN] ]] && { echo 'Nothing changed.'; exit 1; }
    fi
    kit_game_running && { echo 'Close the game of this kit first.'; exit 1; }
    back=1
    work=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-update-XXXXXX")
    trap 'rm -rf "$work"' EXIT
else
    if [[ -n $want_tag ]]; then
        found=$(WANT_TAG=$want_tag newest_release)
        if [[ -z $found && -z ${OOT_COOP_UPDATE_API-} ]]; then
            found=$(API="https://api.github.com/repos/$REPO/releases/tags/$want_tag" WANT_TAG=$want_tag newest_release)
        fi
        read -r tag zip_url sha_url <<<"$found"
        [[ -n ${tag-} ]] || { echo "No release $want_tag with a Linux kit was found (https://github.com/$REPO/releases)."; exit 1; }
        cmp=$(kit_version_compare "$tag" "${current:-0.0.0-a}")
        if [[ ! -f $KIT/$MARKER && $cmp == 0 ]]; then echo "This kit is $current already."; exit 0; fi
        [[ $cmp == -1 ]] && back=1
    else
        read -r tag zip_url sha_url < <(newest_release) || { echo 'Could not read the release list (no network, or GitHub is down).'; exit 1; }
        if [[ ! -f $KIT/$MARKER ]] && [[ -n $current && $(kit_version_compare "$tag" "$current") != 1 ]]; then
            echo "This kit is ${current:-not a release}; the newest release is $tag. Nothing to do."
            exit 0
        fi
    fi
    if [[ $action == check ]]; then
        if wanted "$tag" || [[ -n $want_tag ]]; then
            echo "Co-op $tag is out (this kit: ${current:-not a release}). Start start-oot-coop.sh to update."
        else
            echo "You stay on $current (you went back from $left); $tag is out. bash scripts/update.sh goes forward again."
        fi
        exit 0
    fi
    if [[ $action == launch && -n $left && $(kit_auto_update) == yes ]]; then
        # (auto_update = ask: start.sh asked already)
        a=n
        kit_tty && { read -r -t 20 -p "You went back to $current on purpose. Co-op $tag is out, newer than $left. Update now? [y/N] " a || a=n; }
        [[ $a =~ ^[yY] ]] || { echo "Staying on $current. bash scripts/update.sh updates when you want."; exit 0; }
    fi
    if ((back)); then
        same_version_text "$tag"
        echo "This version (${current:-?}) is kept in $PREV/: bash scripts/update.sh goes forward again."
    fi
    if ((!yes)); then
        if ((back)); then q="Go back from ${current:-?} to $tag?"; else q="Update this kit from ${current:-?} to $tag?"; fi
        read -r -p "$q Your saves and settings are kept. [Y/n] " a || a=n
        [[ $a =~ ^[nN] ]] && exit 1
    fi
    kit_game_running && { echo 'Close the game of this kit first.'; exit 1; }

    work=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-update-XXXXXX")
    trap 'rm -rf "$work"' EXIT
    echo "Downloading co-op $tag ..."
    fetch "$zip_url" "$work/kit.zip" && KIT_QUIET_FETCH=1 fetch "$sha_url" "$work/kit.sha256" || { echo 'The download failed.'; exit 1; }
    want=$(grep -oE '[0-9a-fA-F]{64}' "$work/kit.sha256" | head -n1 | tr 'A-F' 'a-f')
    [[ -n $want && $want == "$(sha256_of "$work/kit.zip")" ]] || { echo 'The download is damaged (its SHA-256 does not match); nothing was changed.'; exit 1; }
    echo "SHA-256 checked: $want"
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
fi
change=$(game_change "$KIT" "$new")
# Left as they are: the newer updater, when going back.
leave() { ((back)) && [[ " $UPDATER_FILES " == *" $1 "* ]]; }

# This version's own files to previous/<version>.part/ (renamed once the update went through), not from a
# half-done kit, and never the player's coop-options.ini.
part=''
if [[ -n $current && ! -f $KIT/$MARKER ]]; then
    part=$KIT/$PREV/$current.part
    { [[ ! -e $part ]] || prev_rm "$part"; } && mkdir -p "$part" &&
    { [[ -f $KIT/MANIFEST.txt ]] && tr -d '\r' <"$KIT/MANIFEST.txt"; tr -d '\r' <"$new/MANIFEST.txt"; printf '%s\n' MANIFEST.txt VERSION.txt; } |
        sort -u | while IFS= read -r rel; do
            [[ $rel != '#'* && $rel != data/coop-options.ini ]] && plain_path "$rel" && ! player_path "$rel" && [[ -f $KIT/$rel ]] || continue
            mkdir -p "$part/$(dirname "$rel")" && cp -p -- "$KIT/$rel" "$part/$rel" || exit 1
        done || { warn "could not keep $current in $PREV/; going on without it."; prev_rm "$part"; part=''; }
fi

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
    [[ -n $part ]] && prev_rm "$part"
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
        plain_path "$rel" && ! player_path "$rel" && ! leave "$rel" || continue
        grep -qxF -- "$rel" <(tr -d '\r' <"$new/MANIFEST.txt") && continue
        if [[ -f $KIT/$rel ]]; then keep_old "$rel" && rm -f -- "$KIT/$rel" || restore; fi
    done <"$KIT/MANIFEST.txt"
fi
# The new kit's files; MANIFEST.txt and VERSION.txt last.
mapfile -t files < <(tr -d '\r' <"$new/MANIFEST.txt" | grep -vxE 'MANIFEST.txt|VERSION.txt'; printf '%s\n' MANIFEST.txt VERSION.txt)
for rel in "${files[@]}"; do
    plain_path "$rel" && ! player_path "$rel" && ! leave "$rel" || continue
    [[ -f $new/$rel ]] || continue
    if [[ $rel == data/coop-options.ini && -f $KIT/$rel ]]; then
        # Going back: the player's file stays as it is (an older kit ignores a newer switch).
        ((back)) && continue
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
chmod +x "$KIT/start-oot-coop.sh" "$KIT"/scripts/*.sh "$KIT/revert.sh" 2>/dev/null || true
rm -f "$KIT/$MARKER"
# previous/<version>/, the last $KEEP_PREV of them.
if [[ -n $part && -d $part ]]; then
    { [[ ! -e ${part%.part} ]] || prev_rm "${part%.part}"; } && mv -- "$part" "${part%.part}" && touch -- "${part%.part}"
    i=0
    while IFS= read -r d; do
        i=$((i + 1))
        ((i > KEEP_PREV)) && prev_rm "$d"
    done < <(ls -1dt -- "$KIT/$PREV"/*/ 2>/dev/null)
fi
if ((back)); then
    l=$left
    [[ -n $l && $(kit_version_compare "$l" "${current:-0.0.0-a}") == 1 ]] || l=${current:-$tag}
    printf '# You went back to %s on purpose (revert.sh or update.sh --version). Starting the kit does not update to\n# %s or older again; a newer release is asked about first. bash scripts/update.sh goes forward and removes this file.\nstay=%s\nleft=%s\n' \
        "$tag" "$l" "$tag" "$l" >"$KIT/$STAY"
    echo "Done: this kit is co-op $tag again. The replaced files are in ${backup##*/}/."
    game_change_text "$change" "$tag"
    echo "Starting the kit stays on $tag now; it asks only when a release newer than $l comes out. bash scripts/update.sh goes forward."
    echo 'Tell the others in your session to go back too: everyone needs the same version.'
    exit 0
fi
rm -f -- "$KIT/$STAY"
echo "Done: this kit is co-op $tag. Your old files are in ${backup##*/}/."
game_change_text "$change" "$tag"
[[ -n $part && -n $current ]] && echo "Changed your mind? bash revert.sh goes back to $current (kept in $PREV/$current/)."
[[ $action == launch ]] && exit 3
exit 0
