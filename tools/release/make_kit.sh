#!/usr/bin/env bash
# Builds the Linux co-op kit: one folder and oot-coop-<release>-linux-<arch>.tar.gz (with its
# .sha256) to copy to each Linux PC or Steam Deck. The Windows kit is make_kit.ps1; both are
# the same kit with each system's own start scripts.
# It holds the co-op server, the patched emulator as an AppImage with an empty portable user/
# folder, Host.sh, Join.sh, play.sh, start-server.sh, collect-logs.sh, update.sh,
# make-puppet-pack.sh with tools/puppet_pack, coop-options.ini, the player guides, the licence
# texts, SOURCE.txt, MANIFEST.txt (the kit's own files, for update.sh) and VERSION.txt.
# No game files, keys or firmware go in, and no puppet pack.
#
#   tools/release/make_kit.sh --appimage <lichii.AppImage> --server <coop_server> [options]
#
# --appimage  the co-op emulator's AppImage (lichii/README.md, "Linux")
# --server    the Linux coop_server (cmake --build build --target coop_server)
# --out       where the kit folder goes (default ../coop-kit beside the repo)
# --release   a release name such as v0.1.0-test7: the kit is oot-coop-<release>-linux and
#             VERSION.txt names the release (update.sh compares it)
# --port      the default server port written into the kit (27510)
# --emu-source  the emulator's source checkout, for SOURCE.txt and its licence texts
# --no-tar    skip the tarball
set -eu
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
appimage='' server='' out='' release='' port=27510 emu_source='' tar=1
while (($#)); do
    case $1 in
        --appimage) appimage=$2; shift ;;
        --server) server=$2; shift ;;
        --out) out=$2; shift ;;
        --release) release=$2; shift ;;
        --port) port=$2; shift ;;
        --emu-source) emu_source=$2; shift ;;
        --no-tar) tar=0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
die() { echo "$*" >&2; exit 1; }
[[ -f $appimage ]] || die "No AppImage: pass --appimage <lichii.AppImage> (lichii/README.md, Linux)."
[[ -f $server ]] || die "No coop_server: pass --server <build/server/coop_server>."
[[ $release =~ ^[A-Za-z0-9._-]*$ ]] || die "Bad release name: $release"
[[ $port =~ ^[0-9]+$ ]] && ((port >= 1 && port <= 65535)) || die "Bad port: $port"
[[ -n $out ]] || out=$(dirname "$repo")/coop-kit
mkdir -p "$out"
out=$(cd "$out" && pwd)
case $out/ in "$repo"/*) die "The kit must go outside the repo (it holds the emulator): $out" ;; esac
arch=$(uname -m)
stamp=${release:-$(date +%Y%m%d-%H%M)}
name=oot-coop-$stamp-linux
kit=$out/$name
[[ ! -e $kit ]] || die "$kit already exists; remove it first."
mkdir -p "$kit"
echo "Kit: $kit"

text() {  # text FROM TO [mode]: LF line ends, @PORT@ filled in
    mkdir -p "$(dirname "$2")"
    sed "s/@PORT@/$port/g" "$1" | tr -d '\r' >"$2"
    [[ -z ${3-} ]] || chmod "$3" "$2"
}
src=$here/kit/linux
mkdir -p "$kit/emulator/user/config" "$kit/emulator/user/keys" "$kit/emulator/user/log" "$kit/server" "$kit/logs"
cp "$appimage" "$kit/emulator/lichii.AppImage"
chmod 755 "$kit/emulator/lichii.AppImage"
cp "$server" "$kit/server/coop_server"
chmod 755 "$kit/server/coop_server"
for f in Host.sh Join.sh play.sh start-server.sh collect-logs.sh update.sh make-puppet-pack.sh; do text "$src/$f" "$kit/$f" 755; done
text "$src/kit-lib.sh" "$kit/scripts/kit-lib.sh"
text "$here/kit/coop.ini.template" "$kit/scripts/coop.ini.template"
text "$here/kit/coop-options.ini" "$kit/coop-options.ini"
# The empty coop.ini, as the Windows kit has it.
. "$src/kit-lib.sh"
coop_write_ini "$kit/scripts/coop.ini.template" "$kit/emulator/user/config/coop.ini" '' "$port" ''
for f in build.py __init__.py byml.py dumpcheck.py nup.py rstb.py sarc.py zs.py; do
    text "$repo/tools/puppet_pack/$f" "$kit/tools/puppet_pack/$f"
done
for pair in setup.md:SETUP.md play-lan.md:PLAY-LAN.md play-online.md:PLAY-ONLINE.md known-issues.md:KNOWN-ISSUES.md; do
    text "$repo/docs/${pair%%:*}" "$kit/${pair#*:}"
done
cp "$repo/LICENSE" "$kit/LICENSE.txt"
text "$repo/THIRD_PARTY_NOTICES.md" "$kit/THIRD_PARTY_NOTICES.md"
mkdir -p "$kit/licenses"
cp "$repo/LICENSE" "$kit/licenses/oot-remake-multiplayer-GPL-3.0.txt"
if [[ -n $emu_source && -f $emu_source/LICENSE.txt ]]; then
    cp "$emu_source/LICENSE.txt" "$kit/licenses/Fritchii-GPL-3.0.txt"
    for f in "$emu_source"/LICENSES/*.txt; do [[ -f $f ]] && cp "$f" "$kit/licenses/SPDX-${f##*/}"; done
else
    echo 'Warning: no --emu-source with LICENSE.txt; add the emulator licence texts by hand.' >&2
fi

# Where the source is (GPL-3.0, section 6), and what this kit is.
commit=$(git -C "$repo" rev-parse HEAD)
pin_tag=$(sed -n 's/^tag=//p' "$repo/lichii/PIN")
pin_commit=$(sed -n 's/^commit=//p' "$repo/lichii/PIN")
emu_commit=unknown
[[ -n $emu_source ]] && emu_commit=$(git -C "$emu_source" rev-parse HEAD 2>/dev/null || echo unknown)
cat >"$kit/SOURCE.txt" <<EOF
Source code for this build${release:+ ($release)}

Everything in this kit that is GPL-3.0 can be rebuilt from public source.

The co-op code (hook, server, puppet pack tool, these scripts):
  https://github.com/daurosk/oot-remake-multiplayer/tree/$commit
  ${release:+(tag $release) }commit $commit

The emulator: Fritchii by Frizti, with the small patch from the repo above:
  https://github.com/Frizti/Fritchii/tree/$pin_tag   (commit $pin_commit)
  Patches: lichii/patches in the co-op repo; lichii/fetch.sh fetches that tag and applies
  them, and lichii/README.md (Linux) says how to build the AppImage with the co-op hook.
  This kit's emulator was built from $emu_commit (that tag plus the patches).

If a link stops working, open an issue on the co-op repo and the source will be sent to you.
EOF
{
    echo "kit: $name${release:+ (release $release)}"
    echo "built: $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "system: linux-$arch"
    echo "co-op repo (this script): $commit"
    echo "Fritchii pin: $pin_tag $pin_commit"
    echo "emulator: lichii.AppImage sha256 $(sha256sum "$kit/emulator/lichii.AppImage" | cut -d' ' -f1)"
    echo "server: coop_server sha256 $(sha256sum "$kit/server/coop_server" | cut -d' ' -f1)"
} >"$kit/VERSION.txt"
# The kit's own files, for update.sh (never the player's user/ folder or logs).
(cd "$kit" && find . -type f ! -path './emulator/user/*' ! -path './logs/*' | sed 's|^\./||' | LC_ALL=C sort) >"$kit/MANIFEST.txt"
echo MANIFEST.txt >>"$kit/MANIFEST.txt"

# The build machine's home folder must not be in the kit (paths compiled into programs).
home_hits=$(grep -rlaF -- "$HOME" "$kit" 2>/dev/null | grep -v '/SOURCE.txt$' || true)
[[ -z $home_hits ]] || echo "Warning: these files name $HOME (build paths): $home_hits" >&2

if ((tar)); then
    tarball=$out/oot-coop-$stamp-linux-$arch.tar.gz
    tar -czf "$tarball" -C "$out" "$name"
    (cd "$out" && sha256sum "${tarball##*/}") >"$tarball.sha256"
    echo "Wrote $tarball and its .sha256"
fi
