#!/usr/bin/env bash
# Lists what the co-op AppImage bundles and gathers the licence texts of those libraries, for
# the Linux kit's licenses/appimage/ folder (THIRD_PARTY_NOTICES.md, "In the Linux kit").
#   tools/release/appimage_licenses.sh <lichii.AppImage> <out folder>
# Run on the machine that built the AppImage (Debian or Ubuntu, as CI does): each bundled
# library is looked up in the package that installed it (dpkg -S), and that package's
# /usr/share/doc/<package>/copyright is copied. Qt and its plugins come from the Qt install
# (not a package) and are named as Qt. out/BUNDLED.txt lists every bundled file with its source.
set -euo pipefail
app=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=$2
[[ -f $app ]] || { echo "No AppImage: $1" >&2; exit 1; }
command -v dpkg >/dev/null || { echo 'Needs dpkg (Debian or Ubuntu, as the CI build).' >&2; exit 1; }
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
chmod +x "$app"
(cd "$work" && "$app" --appimage-extract >/dev/null)
root=$work/squashfs-root
list=$out/BUNDLED.txt
{
    echo "Libraries and plugins inside emulator/lichii.AppImage, and where each comes from."
    echo "<file in the AppImage>  <source>; the licence texts are in this folder (<package>.copyright)."
    echo "The AppImage's own start program (AppRun, the type 2 runtime) is linuxdeploy's (MIT)."
    echo
} >"$list"
unknown=0
while IFS= read -r f; do
    rel=${f#"$root"/}
    base=${f##*/}
    if [[ $rel == */plugins/* || $base == libQt6* ]]; then
        echo "$rel  Qt 6 (The Qt Company), LGPL-3.0: ../SPDX-LGPL-3.0-or-later.txt" >>"$list"
        continue
    fi
    # Other files of the Qt install (ICU, which Qt ships, under the Unicode licence).
    if [[ -n ${QT_ROOT_DIR-} && -e $QT_ROOT_DIR/lib/$base ]]; then
        echo "$rel  from the Qt 6 install (The Qt Company's build; ICU: Unicode-3.0, https://github.com/unicode-org/icu)" >>"$list"
        continue
    fi
    pkg=$(dpkg -S "*/$base" 2>/dev/null | head -n1 | cut -d: -f1 || true)
    if [[ -z $pkg ]]; then
        echo "$rel  not from a package (built with the emulator: see SOURCE.txt and ../SPDX-*.txt)" >>"$list"
        unknown=$((unknown + 1))
        continue
    fi
    version=$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || echo '?')
    echo "$rel  Ubuntu package $pkg $version: $pkg.copyright" >>"$list"
    [[ -f $out/$pkg.copyright || ! -f /usr/share/doc/$pkg/copyright ]] || cp "/usr/share/doc/$pkg/copyright" "$out/$pkg.copyright"
done < <(find "$root" \( -type f -o -type l \) -name '*.so*' | LC_ALL=C sort)
echo "appimage_licenses.sh: $(grep -c '  ' "$list") files listed, $(ls "$out" | grep -c '\.copyright$') licence texts, $unknown not from a package"
