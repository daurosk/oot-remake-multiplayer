#!/usr/bin/env bash
# Builds the co-op emulator on Linux: fetches Fritchii at lichii/PIN with our
# patches (fetch.sh), configures it with the co-op hook (LICHII_COOP=ON) the
# way Fritchii's own Linux release is configured, and builds it.
#
#   lichii/build-linux.sh [--src DIR] [--build DIR] [--hook-only] [--appimage FILE] [--no-fetch]
#
# --src DIR        emulator source (default: fritchii-src beside the main checkout)
# --build DIR      build folder (default: DIR/build/coop-linux)
# --hook-only      compile only the co-op hook's files (a quick check, as CI does)
# --appimage FILE  also make a self-contained AppImage of lichii (linuxdeploy and its Qt
#                  plugin are downloaded into the build folder); the kit ships it as
#                  emulator/lichii.AppImage
# --no-fetch       use the source as it is (it must already be at the pin with the patches)
#
# Needs: cmake 3.24+, ninja, a C++20 compiler (gcc 13+ or clang 17+), Qt 6 (6.8 as
# Fritchii's release; set QT_ROOT_DIR or CMAKE_PREFIX_PATH, or install qt6 dev packages)
# and the packages lichii/README.md lists for Linux.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

fail() {
    echo "build-linux.sh: $*" >&2
    exit 1
}

src=""; build=""; hook_only=0; appimage=""; fetch=1
while [ $# -gt 0 ]; do
    case "$1" in
        --src) src="$2"; shift 2 ;;
        --build) build="$2"; shift 2 ;;
        --hook-only) hook_only=1; shift ;;
        --appimage) appimage="$2"; shift 2 ;;
        --no-fetch) fetch=0; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) fail "unknown option $1" ;;
    esac
done

if [ -z "$src" ]; then
    common="$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)" ||
        fail "not inside a git checkout"
    src="$(dirname "$(dirname "$common")")/fritchii-src"
fi
if [ "$fetch" = 1 ]; then
    "$here/fetch.sh" "$src"
fi
[ -f "$src/CMakeLists.txt" ] || fail "$src is not the emulator source"
src="$(cd "$src" && pwd)"
build="${build:-$src/build/coop-linux}"

prefix=()
if [ -n "${QT_ROOT_DIR:-}" ]; then
    prefix=("-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR")
fi

# The flags of Fritchii's Linux release (.github/workflows/release-binaries.yml), with the
# updater off (patch 0001 refuses it with LICHII_COOP) and the co-op hook on.
# trim_paths.cmake keeps the build machine's folders out of the program.
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DYUZU_TESTS=OFF \
    -DYUZU_CMD=OFF \
    -DYUZU_ROOM=OFF \
    -DYUZU_ROOM_STANDALONE=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF \
    -DVulkanHeaders_FORCE_BUNDLED=ON \
    -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
    -DENABLE_UPDATE_CHECKER=OFF \
    -DUSE_DISCORD_PRESENCE=OFF \
    -DLICHII_COOP=ON \
    "-DLICHII_COOP_DIR=$repo" \
    "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$repo/cmake/trim_paths.cmake" \
    "${prefix[@]}"

jobs="${JOBS:-$(nproc)}"
if [ "$hook_only" = 1 ]; then
    # The object files of the hook (core and the Qt panel), and what they need first.
    mapfile -t objects < <(ninja -C "$build" -t targets all |
        sed -n 's#^\([^:]*/lichii/hook/[^:]*\.o\):.*#\1#p' | sort -u)
    [ "${#objects[@]}" -gt 0 ] || fail "no co-op hook objects in $build (is LICHII_COOP on?)"
    echo "build-linux.sh: compiling ${#objects[@]} hook files"
    ninja -C "$build" -j "$jobs" "${objects[@]}"
    echo "build-linux.sh: the co-op hook compiles"
    exit 0
fi

ninja -C "$build" -j "$jobs" yuzu
bin="$build/bin/lichii"
[ -x "$bin" ] || fail "no $bin after the build"
grep -aq 'coop: loaded' "$bin" || fail "$bin has no co-op hook"
if grep -aqE 'Check for Updates|update-old' "$bin"; then
    fail "$bin has Fritchii's own updater"
fi
echo "build-linux.sh: built $bin"

if [ -n "$appimage" ]; then
    tools="$build/linuxdeploy"
    mkdir -p "$tools"
    arch="$(uname -m)"
    for t in linuxdeploy linuxdeploy-plugin-qt; do
        f="$tools/$t-$arch.AppImage"
        [ -x "$f" ] || {
            curl -fsSL -o "$f" "https://github.com/linuxdeploy/$t/releases/download/continuous/$t-$arch.AppImage"
            chmod +x "$f"
        }
    done
    icon="$build/io.github.zortos293.lichii.png"
    cp "$src/dist/lichii.png" "$icon"
    rm -rf "$build/AppDir"
    out="$(cd "$(dirname "$appimage")" && pwd)/$(basename "$appimage")"
    (
        cd "$build"
        export APPIMAGE_EXTRACT_AND_RUN=1
        [ -n "${QT_ROOT_DIR:-}" ] && export QMAKE="$QT_ROOT_DIR/bin/qmake"
        export EXTRA_QT_MODULES=waylandcompositor
        export EXTRA_PLATFORM_PLUGINS='libqwayland-egl.so;libqwayland-generic.so'
        export LDAI_OUTPUT="$out" OUTPUT="$out" ARCH="$arch"
        "$tools/linuxdeploy-$arch.AppImage" --appdir AppDir \
            --executable "$bin" \
            --desktop-file "$src/dist/io.github.zortos293.lichii.desktop" \
            --icon-file "$icon" \
            --plugin qt \
            --output appimage
    )
    [ -s "$out" ] || fail "no AppImage at $out"
    chmod +x "$out"
    echo "build-linux.sh: AppImage $out"
fi
