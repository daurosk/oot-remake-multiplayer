#!/usr/bin/env bash
# Builds the co-op emulator on Linux: fetches Fritchii at lichii/PIN with our
# patches (fetch.sh), configures it with the co-op hook (LICHII_COOP=ON) the
# way Fritchii's own Linux release is configured, and builds it.
#
#   lichii/build-linux.sh [--src DIR] [--build DIR] [--hook-only] [--appimage FILE] [--no-fetch] [--cpm-cache DIR]
#
# --src DIR        emulator source (default: fritchii-src beside the main checkout)
# --build DIR      build folder (default: DIR/build/coop-linux)
# --hook-only      compile only the co-op hook's files (a quick check, as CI does)
# --appimage FILE  also make a self-contained AppImage of lichii (linuxdeploy and its Qt
#                  plugin are downloaded into the build folder); the kit ships it as
#                  emulator/lichii.AppImage
# --no-fetch       use the source as it is (it must already be at the pin with the patches)
# --cpm-cache DIR  keep the downloaded libraries (CPM) in DIR, outside the source (CI caches it)
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

src=""; build=""; hook_only=0; appimage=""; fetch=1; cpm_cache=""
while [ $# -gt 0 ]; do
    case "$1" in
        --src) src="$2"; shift 2 ;;
        --build) build="$2"; shift 2 ;;
        --hook-only) hook_only=1; shift ;;
        --appimage) appimage="$2"; shift 2 ;;
        --no-fetch) fetch=0; shift ;;
        --cpm-cache) cpm_cache="$2"; shift 2 ;;
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
# --cpm-cache DIR: Fritchii keeps the libraries CPM downloads in <source>/.cache/cpm (ignored
# by its .gitignore). With a folder outside the source (CI's cache), that becomes a link to
# it, so the cache never sits in the source folder before fetch.sh clones it.
if [ -n "$cpm_cache" ]; then
    mkdir -p "$cpm_cache" "$src/.cache"
    cpm_cache="$(cd "$cpm_cache" && pwd)"
    [ -L "$src/.cache/cpm" ] || [ ! -e "$src/.cache/cpm" ] || fail "$src/.cache/cpm exists; remove it to use --cpm-cache"
    ln -sfn "$cpm_cache" "$src/.cache/cpm"
fi
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
# The updater's menu text. Not "update-old": Fritchii v5.8.4 has that string in every build,
# with the updater compiled out too (src/yuzu/fritchii/update_installer.h:25).
if grep -aq 'Check for Updates' "$bin"; then
    fail "$bin has Fritchii's own updater"
fi
echo "build-linux.sh: built $bin"

if [ -n "$appimage" ]; then
    tools="$build/linuxdeploy"
    mkdir -p "$tools"
    arch="$(uname -m)"
    [ "$arch" = x86_64 ] || fail "the pinned linuxdeploy tools are x86_64 only (this is $arch)"
    # Fixed releases with their SHA-256 (never the moving "continuous" one): name, tag, sha256.
    for pin in \
        "linuxdeploy 1-alpha-20251107-1 c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d" \
        "linuxdeploy-plugin-qt 1-alpha-20250213-1 15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"; do
        read -r t tag want <<<"$pin"
        f="$tools/$t-$arch.AppImage"
        if [ ! -x "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$want" ]; then
            rm -f "$f"
            curl -fsSL -o "$f.part" "https://github.com/linuxdeploy/$t/releases/download/$tag/$t-$arch.AppImage"
            have="$(sha256sum "$f.part" | cut -d' ' -f1)"
            [ "$have" = "$want" ] || { rm -f "$f.part"; fail "$t $tag: SHA-256 $have, expected $want"; }
            mv "$f.part" "$f"
            chmod +x "$f"
        fi
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
