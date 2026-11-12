#!/usr/bin/env bash
# Clones or updates the emulator source to the commit in lichii/PIN and applies
# lichii/patches/*.patch in order, as commits on a local branch "coop".
# Refuses if the source tree has uncommitted changes. Never pushes anywhere.
# The same steps as fetch.ps1, for Linux (and macOS).
#
#   lichii/fetch.sh [DEST]
#
# The default folder is fritchii-src next to the main checkout of this repo.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

fail() {
    echo "fetch.sh: $*" >&2
    exit 1
}

# The pin.
repo=""; tag=""; commit=""
while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
        repo=*) repo="${line#repo=}" ;;
        tag=*) tag="${line#tag=}" ;;
        commit=*) commit="${line#commit=}" ;;
    esac
done < "$here/PIN"
[ -n "$repo" ] || fail "lichii/PIN has no repo="
[ -n "$tag" ] || fail "lichii/PIN has no tag="
[ -n "$commit" ] || fail "lichii/PIN has no commit="

# Default destination: beside the main checkout (works from a worktree too).
dest="${1:-}"
if [ -z "$dest" ]; then
    common="$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)" ||
        fail "not inside a git checkout"
    main_root="$(dirname "$common")"
    dest="$(dirname "$main_root")/fritchii-src"
fi
echo "fetch.sh: emulator source in $dest"

if [ ! -e "$dest" ]; then
    git clone "$repo" "$dest" || fail "git clone failed"
else
    [ -d "$dest/.git" ] || fail "$dest exists and is not a git checkout"
    dirty="$(git -C "$dest" status --porcelain)" || fail "git status failed in $dest"
    if [ -n "$dirty" ]; then
        printf '%s\n' "$dirty" | head -n 10
        fail "$dest has uncommitted changes; commit, stash or remove them first"
    fi
    [ ! -d "$dest/.git/rebase-apply" ] || fail "a git am is in progress in $dest"
fi

# Get the pinned commit if it is not here yet.
if ! git -C "$dest" cat-file -e "${commit}^{commit}" 2>/dev/null; then
    git -C "$dest" fetch --tags "$repo" || fail "git fetch failed"
    git -C "$dest" cat-file -e "${commit}^{commit}" 2>/dev/null ||
        fail "commit $commit not found in $repo"
fi
tag_commit="$(git -C "$dest" rev-parse --verify --quiet "refs/tags/${tag}^{commit}" || true)"
if [ -n "$tag_commit" ] && [ "$tag_commit" != "$commit" ]; then
    echo "fetch.sh: warning: tag $tag is $tag_commit here, the pin says $commit"
fi

# Fresh branch at the pin, then our patches on top. The local commits get a
# fixed identity so whatever git identity is set globally is never used.
git -C "$dest" checkout --quiet -B coop "$commit" || fail "git checkout failed"
count=0
for patch in "$here"/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "fetch.sh: applying $(basename "$patch")"
    if ! git -C "$dest" -c user.name=coop-fetch -c user.email=coop-fetch@localhost am --quiet "$patch"; then
        git -C "$dest" am --abort || true
        fail "$(basename "$patch") does not apply to $commit"
    fi
    count=$((count + 1))
done
echo "fetch.sh: $dest is at $tag ($commit) with $count patch(es) on branch coop"
