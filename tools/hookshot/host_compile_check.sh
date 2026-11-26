#!/usr/bin/env bash
# The Linux side of tools/hookshot/host_compile_check.ps1: the Longshot files our patches touch (host/*.c, host/posix/*.c,
# gpu/vk/*.cpp, and the files that include a header a patch changes), compiled for Linux syntax only with Longshot's own
# Linux compiler and nx2/build.py's own flags. The linux-longshot job runs it after the kit's build.sh --renderer-only,
# which left Longshot's builder, its patched source and its Linux tools in the kit's data/builder.
#
#   bash tools/hookshot/host_compile_check.sh <unzipped Linux kit>
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
kit=${1:?usage: host_compile_check.sh <unzipped Linux kit>}
B=$kit/data/builder
SRC=$B/source
PY=$B/image/usr/python/bin/python3
[[ -x $PY && -f $SRC/nx2/build.py && -f $SRC/.coop-stamp ]] || {
    echo "host_compile_check: no patched Longshot source in $B (run the kit's scripts/build.sh --renderer-only first)"; exit 1; }
# (build.sh's step 3: Longshot's own tool environment, already downloaded into data/builder/home)
export LONGSHOT_BUILDER_HOME=$B/home
tools_env=$(env -u LONGSHOT_TOOLS "$PY" -I "$SRC/builder/linux/linux_tools.py" env --install)
eval "$tools_env"
unset LONGSHOT_TOOLS LONGSHOT_VK_PREBUILT
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
OOT_WORK=$work "$PY" -I -B "$here/host_compile_check.py" "$SRC" "$repo/hookshot/patches"
