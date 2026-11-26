#!/usr/bin/env bash
# Tests for tools/release/kit_deps.sh: a dependency's source folder and licence as the build's CMakeCache.txt says
# (FETCHCONTENT_SOURCE_DIR_<NAME>, FETCHCONTENT_BASE_DIR, else <build>/_deps/<name>-src). Made-up builds in a temp folder.
#   bash tools/release/tests/kit_deps_test.sh      (exits 1 when a check fails)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../kit_deps.sh"
failed=0
check() { if [[ $1 == "$2" ]]; then echo "ok   $3"; else echo "FAIL $3: got '$1', wanted '$2'"; failed=$((failed + 1)); fi; }
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mk() { mkdir -p "$(dirname "$1")"; printf 'x' >"$1"; }

# 1. FetchContent's own download
mk "$root/b1/_deps/fmt-src/LICENSE"; mk "$root/b1/_deps/enet-src/LICENSE"
check "$(kit_dep_license fmt "$root/b1")" "$root/b1/_deps/fmt-src/LICENSE" 'fmt in _deps/fmt-src'
check "$(kit_dep_license enet "$root/b1")" "$root/b1/_deps/enet-src/LICENSE" 'enet in _deps/enet-src'

# 2. An offline configure: sources given from outside, no _deps (the case that failed)
mk "$root/deps/fmt-src/LICENSE.rst"; mk "$root/deps/enet-src/LICENSE"
mkdir -p "$root/b2"
printf '%s\n' '# This is the CMakeCache file.' 'FETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON' \
    "FETCHCONTENT_SOURCE_DIR_FMT:PATH=$root/deps/fmt-src" "FETCHCONTENT_SOURCE_DIR_ENET:PATH=$root/deps/enet-src" >"$root/b2/CMakeCache.txt"
check "$(kit_dep_license fmt "$root/b2")" "$root/deps/fmt-src/LICENSE.rst" 'fmt from FETCHCONTENT_SOURCE_DIR_FMT'
check "$(kit_dep_license enet "$root/b2")" "$root/deps/enet-src/LICENSE" 'enet from FETCHCONTENT_SOURCE_DIR_ENET'

# 3. FETCHCONTENT_BASE_DIR moved
mk "$root/base3/fmt-src/LICENSE"; mkdir -p "$root/b3"
printf '%s\n' "FETCHCONTENT_BASE_DIR:PATH=$root/base3" >"$root/b3/CMakeCache.txt"
check "$(kit_dep_license fmt "$root/b3")" "$root/base3/fmt-src/LICENSE" 'fmt under FETCHCONTENT_BASE_DIR'

# 4. A given source that is not there: _deps
mk "$root/b4/_deps/fmt-src/LICENSE"
printf '%s\n' 'FETCHCONTENT_SOURCE_DIR_FMT:PATH=/nowhere/fmt-src' >"$root/b4/CMakeCache.txt"
check "$(kit_dep_license fmt "$root/b4")" "$root/b4/_deps/fmt-src/LICENSE" 'missing given source: _deps'

# 5. None: empty; the next build is tried; a file's build is the folder above it with CMakeCache.txt
mkdir -p "$root/b5"
check "$(kit_dep_license fmt "$root/b5")" '' 'no licence: empty'
check "$(kit_dep_license fmt "$root/b5" '' "$root/b2")" "$root/deps/fmt-src/LICENSE.rst" 'the next build is tried'
mk "$root/b2/server/coop_server"
check "$(kit_build_of "$root/b2/server/coop_server")" "$root/b2" 'a file'"'"'s build folder'

if ((failed)); then echo "$failed check(s) failed"; exit 1; fi
echo 'kit_deps: all checks passed'
