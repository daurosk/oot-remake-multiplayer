#!/usr/bin/env bash
# Builds the puppet Link packs from this PC's own copy of the game into the kit's emulator (a
# LayeredFS mod in the emulator's load folder). Nothing from the game is in the kit: the packs
# are made here, from your game's .nup file (read directly, no keys needed) or a RomFS dump,
# and stay on this PC. Needs Python 3 with the zstandard package.
# Usage: ./make-puppet-pack.sh                        finds the .nup or dump, else asks
#        ./make-puppet-pack.sh --romfs <.nup or folder>  no questions
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$KIT/scripts/kit-lib.sh"
romfs=''
[[ ${1-} == --romfs || ${1-} == -Romfs ]] && romfs=${2-}
user=$KIT/emulator/user
tool=$KIT/tools/puppet_pack/build.py
check=$KIT/tools/puppet_pack/dumpcheck.py
[[ -f $tool ]] || { echo "Not found: $tool"; exit 1; }
mkdir -p "$user"
# A copied kit must write into its own load folder, not the first copy's.
fixed=$(kit_repair_paths "$user")
[[ -n $fixed ]] && echo "This kit was copied: pointed $(echo $fixed | tr ' ' ',') at $user"
python=''
for p in python3 python; do
    if command -v "$p" >/dev/null && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then python=$p; break; fi
done
if [[ -z $python ]]; then
    echo 'No Python 3 found. Install it with your package manager, for example:'
    echo '  sudo apt install python3 python3-zstandard     (Debian, Ubuntu)'
    echo '  sudo pacman -S python python-zstandard         (Arch, SteamOS desktop mode)'
    exit 1
fi
if ! "$python" -c 'import zstandard' 2>/dev/null; then
    echo 'Python is missing the zstandard package. Install it, for example:'
    echo '  sudo apt install python3-zstandard   or   sudo pacman -S python-zstandard'
    echo "  or: $python -m pip install --user zstandard"
    exit 1
fi
# The game check: finds the game's .nup in the emulator's game list folders, else the
# emulator's RomFS dump, and says, one line per problem, what is wrong and what to do.
run_check() {
    local out code
    out=$("$python" -I "$check" --user-dir "$user" "$@")
    code=$?
    found=''
    while IFS= read -r line; do
        if [[ $line == romfs=* ]]; then found=${line#romfs=}; elif [[ -n $line ]]; then echo "$line"; fi
    done <<<"$out"
    return $code
}
if [[ -z $romfs ]]; then
    if run_check; then
        romfs=$found
        echo "Found the game files: $romfs"
    else
        echo
        echo "Type (or drag in here) your game's .nup file, or the folder of a RomFS dump (the one that holds Pack, Model, AS, RSDB and System); press Enter to stop."
        read -r -p 'Game .nup or romfs folder: ' answer || exit 1
        [[ -n $answer ]] || exit 1
        answer=${answer%\'}; answer=${answer#\'}; answer=${answer%\"}; answer=${answer#\"}
        romfs=$answer
    fi
fi
run_check --romfs "$romfs" || { echo 'Fix it as above, then run ./make-puppet-pack.sh again.'; exit 1; }
romfs=$found
load=$(kit_load_dir "$user")
echo "Building the puppet packs into $load ..."
"$python" -I "$tool" --romfs "$romfs" --user-dir "$user" || { echo "The pack build failed ($?)."; exit 1; }
echo 'Done. Start Host.sh or Join.sh; the emulator log shows "LayeredFS patches applied" when it loads the game.'
