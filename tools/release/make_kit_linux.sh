#!/usr/bin/env bash
# Builds the Linux co-op kit for Longshot: one folder and a zip of it, which a player
# unzips and starts with `bash start-oot-coop.sh`. The Linux twin of make_kit_hookshot.ps1, in the
# same layout and wording (docs/setup-linux.md). Nothing of the game or of Longshot is in it: the
# kit downloads Longshot's own Linux builder (data/longshot/LINUX.txt) and builds the game on the
# player's PC from their own game file.
#
#   tools/release/make_kit_linux.sh --module build/hookshot/hookshot_coop.so --server build/server/coop_server \
#       --licenses build [--release v0.1.0-beta1] [--out dist] [--no-zip]
#
# --licenses: the CMake build folder that fetched ENet and fmt (_deps/enet-src, _deps/fmt-src): their
# licence texts go in data/licenses/.
# Out: <out>/oot-coop-<release>-linux/ and, unless --no-zip, <out>/oot-coop-<release>-linux-x86_64.zip
# and its .sha256 (the names scripts/update.sh looks for). The zip keeps the scripts' executable bit;
# a player whose unzip drops it still starts the kit with `bash start-oot-coop.sh` (README.txt).
#
# The kit:
#   start-oot-coop.sh, README.txt, VERSION.txt, MANIFEST.txt
#   scripts/      start.sh, build.sh, update.sh, collect-logs.sh, lib.sh, apply_patch.py, coop.ini.template
#   data/         coop-options.ini, server/coop_server, longshot/ (PIN, LINUX.txt, patches/, hookshot_coop.so),
#                 tools/puppet_pack/, docs/ (SETUP-LINUX.md, KNOWN-ISSUES.md), licenses/
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
module='' server='' licenses='' release='' out=$repo/dist zip=1
while (($#)); do
    case $1 in
        --module) module=$2; shift ;;
        --server) server=$2; shift ;;
        --licenses) licenses=$2; shift ;;
        --release) release=$2; shift ;;
        --out) out=$2; shift ;;
        --no-zip) zip=0 ;;
        *) echo "make_kit_linux: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
die() { echo "make_kit_linux: $*" >&2; exit 1; }
[[ -f $module ]] || die "--module: no hookshot_coop.so at '$module'"
[[ -f $server ]] || die "--server: no coop_server at '$server'"
[[ -d $licenses ]] || die "--licenses: no build folder at '$licenses'"
file -b "$module" 2>/dev/null | grep -q 'ELF 64-bit.*x86-64.*shared object' || die "$module is not an x86-64 Linux shared object"
file -b "$server" 2>/dev/null | grep -q 'ELF 64-bit.*x86-64' || die "$server is not an x86-64 Linux program"
enet_licence=$licenses/_deps/enet-src/LICENSE
fmt_licence=$(find "$licenses/_deps" "$repo/build"*/_deps -maxdepth 2 -path '*fmt-src/LICENSE*' -type f 2>/dev/null | head -n1 || true)
[[ -f $enet_licence ]] || die "ENet's LICENSE not found in $licenses/_deps/enet-src"
[[ -n $fmt_licence ]] || die "fmt's LICENSE not found under $licenses/_deps/fmt-src"
src=$repo/tools/release/kit/linux-longshot
pin=$repo/hookshot/PIN
linux_pin=$repo/hookshot/LINUX.txt
[[ $(sed -n 's/^tag=//p' "$pin") == "$(sed -n 's/^tag=//p' "$linux_pin")" ]] ||
    die "hookshot/PIN and hookshot/LINUX.txt name different Longshot releases"

name=oot-coop-${release:-dev}-linux
kit=$out/$name
rm -rf "$kit"
mkdir -p "$kit/scripts" "$kit/data/server" "$kit/data/longshot/patches" "$kit/data/tools/puppet_pack" \
    "$kit/data/docs" "$kit/data/licenses"
text() {   # text FROM TO: LF line ends
    mkdir -p "$(dirname "$2")"
    tr -d '\r' <"$1" >"$2"
}
text "$src/start-oot-coop.sh" "$kit/start-oot-coop.sh"
text "$src/README.txt" "$kit/README.txt"
for f in start.sh build.sh update.sh collect-logs.sh lib.sh; do text "$src/scripts/$f" "$kit/scripts/$f"; done
text "$repo/tools/release/kit/scripts/apply_patch.py" "$kit/scripts/apply_patch.py"
text "$repo/tools/release/kit/coop.ini.template" "$kit/scripts/coop.ini.template"
text "$repo/tools/release/kit/coop-options.ini" "$kit/data/coop-options.ini"
chmod 755 "$kit/start-oot-coop.sh" "$kit"/scripts/*.sh
install -m 755 "$server" "$kit/data/server/coop_server"
install -m 644 "$module" "$kit/data/longshot/hookshot_coop.so"
text "$pin" "$kit/data/longshot/PIN"
text "$linux_pin" "$kit/data/longshot/LINUX.txt"
for p in "$repo"/hookshot/patches/*.patch; do cp -- "$p" "$kit/data/longshot/patches/"; done   # (byte for byte)
# The puppet pack tool: make_kit_hookshot.ps1's list, plus every module build.py imports.
pack=$repo/tools/puppet_pack
files=(ainb.py build.py __init__.py byml.py dumpcheck.py dungeon_rooms.py hud.py nup.py rstb.py sarc.py texture.py
    title_menu.py tunic.py tunic_extra.py zs.py)
while IFS= read -r m; do files+=("$m.py"); done < <(sed -n 's/#.*//; s/^[[:space:]]*from \. import //p' "$pack/build.py" | tr ',' '\n' | tr -d ' ' | grep .)
for f in $(printf '%s\n' "${files[@]}" | sort -u); do
    [[ -f $pack/$f ]] || die "tools/puppet_pack/$f (needed by build.py) is not in the repo"
    text "$pack/$f" "$kit/data/tools/puppet_pack/$f"
done
text "$repo/docs/setup-linux.md" "$kit/data/docs/SETUP-LINUX.md"
text "$repo/docs/known-issues.md" "$kit/data/docs/KNOWN-ISSUES.md"
cp -- "$repo/LICENSE" "$kit/data/licenses/oot-remake-multiplayer-GPL-3.0.txt"
text "$repo/THIRD_PARTY_NOTICES.md" "$kit/data/licenses/THIRD_PARTY_NOTICES.md"
cp -- "$enet_licence" "$kit/data/licenses/ENet-MIT.txt"
cp -- "$fmt_licence" "$kit/data/licenses/fmt-MIT.txt"

commit=$(git -C "$repo" rev-parse HEAD)
ls_commit=$(sed -n 's/^commit=//p' "$pin")
ls_tag=$(sed -n 's/^tag=//p' "$pin")
cat >"$kit/data/licenses/SOURCE.txt" <<EOF
Source code for this build${release:+ ($release)}

The co-op code (the module, the server, the puppet pack tool, these scripts), GPL-3.0:
  https://github.com/daurosk/oot-remake-multiplayer/tree/$commit
  ${release:+(tag $release) }commit $commit; data/longshot/hookshot_coop.so is its CMake target hookshot_coop.

Longshot: Team MagicBean's static recompilation (it continues Hookshot), GPL-3.0. Not in this kit: start-oot-coop.sh
downloads Longshot $ls_tag's own Linux builder (data/longshot/LINUX.txt) and builds the game on your PC from your own game:
  https://git.magicbean.dev/MagicBeanDev/Longshot/src/commit/$ls_commit
  with the patches in data/longshot/patches (also hookshot/patches in the co-op repo).

If a link stops working, open an issue on the co-op repo and the source will be sent to you.
EOF
sha16() { sha256sum "$1" | cut -c1-16; }
cat >"$kit/VERSION.txt" <<EOF
kit: $name${release:+ (release $release)}
kind: longshot-linux
built: $(date '+%Y-%m-%d %H:%M:%S %z')
co-op repo: $commit ($(git -C "$repo" rev-parse --abbrev-ref HEAD))$( [[ -n $(git -C "$repo" status --porcelain --untracked-files=no) ]] && echo ' with uncommitted changes')
longshot pin: $ls_commit ($ls_tag)
hookshot_coop.so: $(sha16 "$module") (sha256, first 16)
coop_server: $(sha16 "$server") (sha256, first 16)
EOF

# Nothing of the game, of an emulator or of anyone's data, and no compiled Python.
find "$kit" -name __pycache__ -prune -exec rm -rf {} +
bad=$(find "$kit" -type f \( -iname '*.keys' -o -iname '*.nsp' -o -iname '*.nup' -o -iname '*.xci' -o -iname '*.nca' \
    -o -iname '*.nro' -o -iname '*.nso' -o -iname '*.zs' -o -iname '*.pack' -o -iname '*.bfres' -o -iname '*.sarc' \
    -o -iname '*.AppImage' -o -iname 'lichii*' -o -iname '*.exe' -o -iname '*.dll' -o -iname '*.bat' -o -iname '*.cmd' -o -iname '*.ps1' \) | head -n5)
[[ -z $bad ]] || die "the kit holds files that must not ship: $bad"
home=${HOME%/}
if [[ -n $home && $home != / ]] && grep -rlF -- "$home/" "$kit" >/dev/null 2>&1; then
    die "the build PC's home folder appears in: $(grep -rlF -- "$home/" "$kit" | head -n3 | tr '\n' ' ')"
fi

# MANIFEST.txt: the kit's own files (the player's coop-options.ini is in it: the updater merges it).
(cd "$kit" && { echo MANIFEST.txt; find . -type f ! -name MANIFEST.txt | sed 's|^\./||'; } | LC_ALL=C sort) >"$kit/MANIFEST.txt.tmp"
{ echo "# Files of $name, for scripts/update.sh. Do not edit."; cat "$kit/MANIFEST.txt.tmp"; } >"$kit/MANIFEST.txt"
rm -f "$kit/MANIFEST.txt.tmp"

if ((zip)); then
    command -v zip >/dev/null || die 'zip is not installed (sudo apt install zip)'
    z=$out/$name-x86_64.zip
    rm -f "$z" "$z.sha256"
    (cd "$out" && zip -q -r -X "$z" "$name")
    printf '%s  %s\n' "$(sha256sum "$z" | cut -d' ' -f1)" "${z##*/}" >"$z.sha256"
    echo "Zip: $z ($(du -h "$z" | cut -f1)), SHA-256 $(cut -d' ' -f1 "$z.sha256")"
fi
echo "Kit: $kit"
