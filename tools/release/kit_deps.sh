# The source folder of a dependency CMake's FetchContent gave a build, and its licence text, for the Linux kit scripts
# (make_kit_linux.sh; tools/release/kit_deps.ps1 is the same for the Windows ones). Sourced, not run.
# Read from the build's CMakeCache.txt, as CMake itself decides it:
#   1. FETCHCONTENT_SOURCE_DIR_<NAME> (a source given from outside: an offline configure, -DFETCHCONTENT_SOURCE_DIR_FMT=...);
#   2. else <FETCHCONTENT_BASE_DIR>/<name>-src (FetchContent's own download; the base is <build>/_deps unless set).

# kit_dep_source <build dir> <name>: the folder (printed; it may not exist)
kit_dep_source() {
    local build=$1 name=$2 up base dir
    up=$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')
    base=$build/_deps
    if [[ -f $build/CMakeCache.txt ]]; then
        dir=$(sed -n "s/^FETCHCONTENT_SOURCE_DIR_${up}:[A-Z]*=//p" "$build/CMakeCache.txt" | head -n1)
        if [[ -n $dir && -d $dir ]]; then printf '%s\n' "$dir"; return; fi
        local b
        b=$(sed -n 's/^FETCHCONTENT_BASE_DIR:[A-Z]*=//p' "$build/CMakeCache.txt" | head -n1)
        [[ -n $b ]] && base=$b
    fi
    printf '%s\n' "$base/$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')-src"
}

# kit_dep_license <name> <build dir>...: the first LICENSE* file of that dependency in the first build that has one ("" if none)
kit_dep_license() {
    local name=$1 build src f
    shift
    for build in "$@"; do
        [[ -n $build && -d $build ]] || continue
        src=$(kit_dep_source "$build" "$name")
        f=$(find "$src" -maxdepth 1 -name 'LICENSE*' -type f 2>/dev/null | sort | head -n1 || true)   # (pipefail: no folder)
        if [[ -n $f ]]; then printf '%s\n' "$f"; return; fi
    done
    printf '\n'
}

# kit_build_of <file>: the nearest folder above it with a CMakeCache.txt ("" if none)
kit_build_of() {
    local d
    d=$(cd "$(dirname "$1")" 2>/dev/null && pwd) || { printf '\n'; return; }
    while [[ -n $d && $d != / ]]; do
        [[ -f $d/CMakeCache.txt ]] && { printf '%s\n' "$d"; return; }
        d=$(dirname "$d")
    done
    printf '\n'
}
