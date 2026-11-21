#!/usr/bin/env bash
# Tests for the Linux Longshot kit (tools/release/kit/linux-longshot): a kit is laid out as
# make_kit_linux.sh lays it out, with a stand-in for Longshot's builder AppImage (a script whose
# --appimage-extract unpacks a small tree: a python3 that is this PC's, a make_game.py and a
# linux_tools.py that note how they were run), one real patch, and a local release list for the
# updater. Then the kit's scripts run as a player would. No network, no game, no real build.
#   bash tools/release/tests/linux_longshot_kit_test.sh       (TEST_PYTHON: a Python 3 to use, default python3)
# start.sh's checks are Linux ones: those cases run only on Linux (CI's linux gcc job).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
release=$(dirname "$here")
repo=$(dirname "$(dirname "$release")")
src=$release/kit/linux-longshot
failed=0
check() {  # check "what" command...
    local what=$1
    shift
    if "$@"; then echo "ok   $what"; else echo "FAIL $what"; failed=$((failed + 1)); fi
}
has() { grep -qF -- "$2" "$1"; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-ls-test-XXXXXX")
trap '[ -n "${KEEP_TMP-}" ] || rm -rf "$tmp"' EXIT
[ -n "${KEEP_TMP-}" ] && echo "tmp: $tmp"
py=$(command -v "${TEST_PYTHON:-python3}")   # absolute: PATH gets $tmp/bin first later, and the shim there must not find itself
[[ -n $py ]] && "$py" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null || { echo "no Python 3 (${TEST_PYTHON:-python3}): set TEST_PYTHON"; exit 1; }
mkdir -p "$tmp/bin"
printf '#!/usr/bin/env bash\nexec "%s" "$@"\n' "$py" >"$tmp/bin/python3"
chmod +x "$tmp/bin/python3"
export PATH="$tmp/bin:$PATH" HOME=$tmp/home XDG_DATA_HOME=$tmp/xdg
mkdir -p "$HOME" "$XDG_DATA_HOME"   # start.sh skips the menu entry when the XDG folder has no parent
unset DISPLAY WAYLAND_DISPLAY
log=$tmp/calls.log
export FAKE_LOG=$log

# --- lib.sh ----------------------------------------------------------------------------------
lib=$src/scripts/lib.sh
check 'versions: v0.1.0 > v0.1.0-beta2 > v0.1.0-beta1; beta9 < beta10' bash -c '. "$1"
    [ "$(kit_version_compare v0.1.0 v0.1.0-beta2)" = 1 ] && [ "$(kit_version_compare v0.1.0-beta2 v0.1.0-beta1)" = 1 ] &&
    [ "$(kit_version_compare v0.1.0-beta9 v0.1.0-beta10)" = -1 ] && [ "$(kit_version_compare ci-1234 v0.1.0)" = -1 ]' _ "$lib"
printf '# a comment\ntag=v3.0.0-rc.3\nurl=https://x/y=z\n' >"$tmp/pin"
check 'pin_get reads key=value lines (a value may hold =)' bash -c '. "$1"; [ "$(pin_get "$2" tag)" = v3.0.0-rc.3 ] && [ "$(pin_get "$2" url)" = https://x/y=z ] && ! pin_get "$2" nope' _ "$lib" "$tmp/pin"
check 'kit_offline: 1 offline, 0 online; behind a proxy (https_proxy, ALL_PROXY) online with no probe' bash -c '. "$1"
    kit_offline 1 && ! kit_offline 0 && ! https_proxy=http://127.0.0.1:9 kit_offline "" && ! ALL_PROXY=socks5://127.0.0.1:9 kit_offline ""' _ "$lib"

check 'kit_steam_input_hint: Steam running, not started by Steam: the warning' bash -c '. "$1"
    out=$(env -u SteamGameId -u SteamAppId -u SteamClientLaunch -u SteamOverlayGameId bash -c ". \"\$1\"; kit_steam_input_hint true" _ "$1")
    [[ $out == *"Steam is open"* && $out == *"Steam Controller"* && $out == *"Steam Input off"* ]]' _ "$lib"
check 'kit_steam_input_hint: started by Steam (any of its four names), Steam not running, or a failing check: nothing, never a stop' bash -c '. "$1"
    for n in SteamGameId SteamAppId SteamClientLaunch SteamOverlayGameId; do
        [[ -z $(env "$n=1" bash -c ". \"\$1\"; kit_steam_input_hint true" _ "$1") ]] || exit 1
    done
    [[ -z $(kit_steam_input_hint false) ]] && kit_steam_input_hint false && kit_steam_input_hint true >/dev/null' _ "$lib"

# --- A kit, as make_kit_linux.sh lays it out ----------------------------------------------------
# The stand-in builder: what --appimage-extract unpacks.
payload=$tmp/payload
L=$payload/usr/share/longshot
mkdir -p "$payload/usr/python/bin" "$L/nx2" "$L/builder/linux" "$L/host"
printf '#!/usr/bin/env bash\nexec python3 "$@"\n' >"$payload/usr/python/bin/python3"
chmod +x "$payload/usr/python/bin/python3"
printf 'v3.0.0-rc.3\n' >"$L/VERSION"
printf '/* the host */\nint host;\n' >"$L/host/hos.c"
cat >"$L/nx2/make_game.py" <<'EOF'
import os, sys
a = sys.argv[1:]
env = "LONGSHOT_TOOLS=%s VK_PREBUILT=%s EMSDK=%s OOT=%s" % (os.environ.get("LONGSHOT_TOOLS", "-"), os.environ.get("LONGSHOT_VK_PREBUILT", "-"),
                                                       os.environ.get("EMSDK", "-"), ",".join(k for k in os.environ if k.startswith("OOT_COOP")) or "-")
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write("make_game %s | %s | patched=%s ngx=%s\n" % (" ".join(a), env, os.path.exists("host/coop_test.c"), os.environ.get("LONGSHOT_NGX_SDK", "-")))
game = a[a.index("--nup") + 1] if "--nup" in a else ""
if "--check" in a:
    print("@@stage check Checking")
    print("@@check %s" % ("bad wrong version" if "bad" in game else "ok"))
    sys.exit(1 if "bad" in game else 0)
if os.environ.get("FAKE_BUILD_FAIL"):   # (a build that stops: Character Select's undo)
    print("@@error the build stopped (test)")
    sys.exit(2)
out = a[a.index("--out") + 1]
os.makedirs(out, exist_ok=True)
for n in ("longshot", "image.bin", "longshot_vk.so"):
    with open(os.path.join(out, n), "w") as f:
        f.write("#!/bin/sh\necho started \"$OOT_COOP_KIT\" \"$LONGSHOT_NO_UPDATE_CHECK\" \"$(pwd)\" >> \"$FAKE_LOG\"\n" if n == "longshot" else n)
os.chmod(os.path.join(out, "longshot"), 0o755)
# (as place_ngx_runtime: DLSS's runtime in <out>/ngx with the SDK, gone again without it)
ngx = os.path.join(out, "ngx", "libnvidia-ngx-dlss.so.310.1.0")
if os.environ.get("LONGSHOT_NGX_SDK") and "--fetch-ngx-sdk" in a and "--accept-nvidia-license" in a:
    os.makedirs(os.path.dirname(ngx), exist_ok=True)
    open(ngx, "w").close()
elif os.path.exists(ngx):
    os.remove(ngx)
print("@@stage build Building")
print("built")
EOF
cat >"$L/builder/linux/linux_tools.py" <<'EOF'
import os
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write("linux_tools home=%s tools=%s\n" % (os.environ.get("LONGSHOT_BUILDER_HOME", "-"), os.environ.get("LONGSHOT_TOOLS", "-")))
print("export EMSDK='/opt/emsdk'")
print("export RX_CPM='/opt/cpm'")
EOF
mkdir -p "$L/nx2"
cat >"$L/nx2/build.py" <<'EOF'
import os
d = os.path.join(os.environ["OOT_WORK"], "sa", "build")
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "longshot_vk.so"), "w").write("vk")
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write("build.py VK_ONLY=%s prebuilt=%s\n" % (os.environ.get("RX_VK_ONLY"), os.environ.get("LONGSHOT_VK_PREBUILT", "-")))
EOF
appimage=$tmp/Longshot-test.AppImage
printf '#!/usr/bin/env bash\n[ "$1" = --appimage-extract ] || exit 9\nmkdir -p squashfs-root && cp -a "%s/." squashfs-root/\n' "$payload" >"$appimage"
sha=$(sha256sum "$appimage" | cut -d' ' -f1)

kit="$tmp/OoT Coop kit"   # (a space in the path, as players' folders have)
mkdir -p "$kit/scripts" "$kit/data/longshot/patches" "$kit/data/server" "$kit/data/tools/puppet_pack"
cp "$src/start-squadala.sh" "$src/README.txt" "$src/revert.sh" "$kit/"
check "the kit has one start file (no start-oot-coop.sh from v0.11.4 on)" [ ! -e "$src/start-oot-coop.sh" ]
cp "$src"/scripts/*.sh "$src/scripts/problem_report.py" "$release/kit/scripts/apply_patch.py" "$kit/scripts/"
cp "$release/kit/coop.ini.template" "$kit/scripts/"
cp "$release/kit/coop-options.ini" "$kit/data/coop-options.ini"
printf 'server\n' >"$kit/data/server/coop_server"
printf 'module\n' >"$kit/data/longshot/hookshot_coop.so"
printf 'tag=v3.0.0-rc.3\ncommit=0c74253d\n' >"$kit/data/longshot/PIN"
printf 'tag=v3.0.0-rc.3\nappimage=Longshot-test.AppImage\nurl=%s\nsha256=%s\n' "$appimage" "$sha" >"$kit/data/longshot/LINUX.txt"
cat >"$kit/data/longshot/patches/0001-test.patch" <<'EOF'
diff --git a/host/coop_test.c b/host/coop_test.c
new file mode 100644
--- /dev/null
+++ b/host/coop_test.c
@@ -0,0 +1 @@
+int coop_test;
EOF
cat >"$kit/data/tools/puppet_pack/build.py" <<'EOF'
import os, sys
d = sys.argv[sys.argv.index("--mod-dir") + 1]
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "coop_puppet.json"), "w").write("{}")
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write("puppet_pack %s\n" % " ".join(sys.argv[1:]))
EOF
# (dumpcheck.py's stand-in: a file that starts with "nup" is the game; the pack's .nup search takes no other)
cat >"$kit/data/tools/puppet_pack/dumpcheck.py" <<'EOF'
import sys
p = sys.argv[sys.argv.index("--romfs") + 1]
sys.exit(0 if open(p, "rb").read(3) == b"nup" else 1)
EOF
printf 'kit: oot-coop-v0.1.0-beta2-linux (release v0.1.0-beta2)\n' >"$kit/VERSION.txt"
export OOT_COOP_FETCH_LOCAL=1
export KIT_FETCH_WAIT=0   # (kit-mirror: no waits between the retries here)
D=$kit/data

# --- build.sh ------------------------------------------------------------------------------
mkdir -p "$HOME/Downloads"
printf 'nup' >"$HOME/Downloads/OoT [04004860224DEC000].nup"
OOT_COOP_SECRET=x OOT_COOP_LANGUAGE=ja-JP bash "$kit/scripts/build.sh" </dev/null >"$tmp/build1.txt" 2>&1
code=$?
check 'build: a first build ends well' [ $code -eq 0 ]
check 'build: the builder is unpacked, its 95 MB file deleted' bash -c '[ -x "$1/builder/image/usr/python/bin/python3" ] && [ ! -e "$1/builder/Longshot-test.AppImage" ]' _ "$D"
check 'build: the patch is applied to the source copy, not to the image' bash -c '[ -f "$1/builder/source/host/coop_test.c" ] && [ ! -e "$1/builder/image/usr/share/longshot/host/coop_test.c" ]' _ "$D"
check 'build: the .nup in Downloads is found by itself' has "$tmp/build1.txt" 'Found your game:'
check "build: the tools' downloads stay in the kit (LONGSHOT_BUILDER_HOME)" has "$log" "linux_tools home=$D/builder/home tools=-"
check 'build: make_game checks the game first, then builds into data/game' bash -c 'grep -q "^make_game --nup .* --check" "$1" && grep -q "^make_game --nup .* --out $2/game --work $2/work" "$1"' _ "$log" "$D"
check "build: make_game runs on the patched source with Longshot's tools, no LONGSHOT_TOOLS, no prebuilt renderer, no OOT_COOP_ variable" \
    bash -c 'grep "^make_game" "$1" | grep -v -- --check | grep -q "LONGSHOT_TOOLS=- VK_PREBUILT=- EMSDK=/opt/emsdk OOT=- | patched=True"' _ "$log"
check "build: the player's OOT_COOP_LANGUAGE still reaches make_game (read before the OOT_COOP_ variables are cleared)" \
    bash -c 'grep "^make_game" "$1" | grep -v -- --check | grep -q -- "--language ja-JP "' _ "$log"
check 'build: stages shown, @@ lines for the launcher left out' bash -c 'grep -q "  -- Building" "$1" && ! grep -q "@@" "$1"' _ "$tmp/build1.txt"
check 'build: the module is put beside the game' [ "$(cat "$D/game/hookshot_coop.so" 2>/dev/null)" = module ]
check 'build: coop.ini beside the game, empty server and name left as comments, port 27510' \
    bash -c 'grep -q "^# server =$" "$1" && grep -q "^# name =$" "$1" && grep -q "^port = 27510$" "$1"' _ "$D/game/coop.ini"
check 'build: the puppet pack goes in data/game/mods/contents/<title>/coop-puppet, with the title rows and the pause tab (--title-rows --pause-tab)' bash -c '[ -f "$1/game/mods/contents/0400486024DEC000/coop-puppet/coop_puppet.json" ] && grep -q "puppet_pack --romfs .*nup --mod-dir " "$2" && grep -q -- "--mod-dir .* --title-rows --pause-tab" "$2"' _ "$D" "$log"
check 'build: the game file is remembered' has "$D/game-files.txt" "Downloads/OoT [04004860224DEC000].nup"

: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build2.txt" 2>&1; export CODE=$?
check 'build: a second start builds nothing' bash -c '[ $CODE -eq 0 ] && grep -q "built already" "$1" && ! grep -q "^make_game" "$2" && grep -q "puppet pack: built already" "$1"' _ "$tmp/build2.txt" "$log"

# NVIDIA DLSS (#234): --dlss asks for NVIDIA's license once (a terminal only), data/dlss.txt keeps the answer, the game is
# built again with the SDK; --no-dlss takes it back. The game's stamp stays the release, pin and patches.
: >"$log"
bash "$kit/scripts/build.sh" --dlss </dev/null >"$tmp/dlss1.txt" 2>&1; export CODE=$?
check 'dlss: --dlss without a terminal asks nothing, builds nothing, says how' \
    bash -c '[ $CODE -ne 0 ] && grep -q "from a terminal" "$1" && [ ! -f "$2/dlss.txt" ] && ! grep -q "^make_game" "$3"' _ "$tmp/dlss1.txt" "$D" "$log"
stamp_before=$(cat "$D/game/coop-built-from.txt")
printf 'nvidia-license=accepted\n' >"$D/dlss.txt"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/dlss2.txt" 2>&1; export CODE=$?
check 'dlss: license accepted (data/dlss.txt): the game is built again with the SDK in data/work/ngx-sdk' \
    bash -c '[ $CODE -eq 0 ] && grep "^make_game" "$1" | grep -v -- --check | grep -q -- "--fetch-ngx-sdk --accept-nvidia-license .*ngx=$2/work/ngx-sdk" && compgen -G "$2/game/ngx/libnvidia-ngx-dlss.so.*" >/dev/null' _ "$log" "$D"
check 'dlss: the game stamp is still the release, pin and patches' [ "$(cat "$D/game/coop-built-from.txt")" = "$stamp_before" ]
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/dlss3.txt" 2>&1; export CODE=$?
check 'dlss: the next start builds nothing' bash -c '[ $CODE -eq 0 ] && grep -q "built already" "$1" && ! grep -q "^make_game" "$2"' _ "$tmp/dlss3.txt" "$log"
: >"$log"
bash "$kit/scripts/build.sh" --no-dlss </dev/null >"$tmp/dlss4.txt" 2>&1; export CODE=$?
check 'dlss: --no-dlss forgets the license, builds again without the SDK, NVIDIA'"'"'s runtime gone' \
    bash -c '[ $CODE -eq 0 ] && [ ! -f "$2/dlss.txt" ] && grep "^make_game" "$1" | grep -v -- --check | grep -q "ngx=-$" && ! grep -q -- --fetch-ngx-sdk "$1" && ! compgen -G "$2/game/ngx/libnvidia-ngx-dlss.so.*" >/dev/null' _ "$log" "$D"
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/dlss5.txt" 2>&1; export CODE=$?
check 'dlss: off, the next start builds nothing' bash -c '[ $CODE -eq 0 ] && grep -q "built already" "$1" && ! grep -q "^make_game" "$2"' _ "$tmp/dlss5.txt" "$log"

# pack-retry: the pack is made again on a start whenever it is missing, incomplete or from another tool, and the
# game file is looked for when it moved; a pack tool error is said and the start goes on.
P=$D/game/mods/contents/0400486024DEC000/coop-puppet
nup_was=$HOME/Downloads/'OoT [04004860224DEC000].nup'
printf '{"look_files": {"gear": "coop_look/gear.bin"}, "tunics": {"red": ["Tex/r.bntx.zs"]}}' >"$P/coop_puppet.json"
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack1.txt" 2>&1; export CODE=$?
check 'pack: an incomplete pack (gear.bin gone) is made again, said plainly' bash -c '[ $CODE -eq 0 ] && grep -q "incomplete (coop_look/gear.bin is missing)" "$1" && grep -q "^puppet_pack" "$2" && grep -q "Making the puppet pack" "$1"' _ "$tmp/pack1.txt" "$log"
rm -rf "$P"
mkdir -p "$HOME/Desktop/games"
mv "$nup_was" "$HOME/Desktop/games/"
printf 'manual' >"$HOME/Downloads/Manual.nup"   # (a .nup that is not the game: the check leaves it out)
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack2.txt" 2>&1; export CODE=$?
check 'pack: no pack and the .nup moved to the Desktop: found there, remembered, the pack made' bash -c '[ $CODE -eq 0 ] && grep -q "Found your game file: .*Desktop/games/OoT" "$1" && grep -q "Desktop/games/OoT" "$3" && grep -q "^puppet_pack --romfs .*Desktop/games/OoT" "$2"' _ "$tmp/pack2.txt" "$log" "$D/game-files.txt"
mkdir -p "$tmp/away/far" && mv "$HOME/Desktop/games/OoT [04004860224DEC000].nup" "$tmp/away/far/"   # (deeper than the search looks)
# (only Manual.nup is left in the search's places: it is not the game, so it is not taken; no dumpcheck.py: nothing is)
rm -rf "$P"
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack3.txt" 2>&1; export CODE=$?
check 'pack: only a .nup that is not the game left: not taken, said, the start goes on (no prompt without a terminal)' bash -c '[ $CODE -eq 0 ] && grep -q "puppet pack was not made: your game file is not at" "$1" && ! grep -q "^puppet_pack" "$2" && ! grep -q Manual "$3"' _ "$tmp/pack3.txt" "$log" "$D/game-files.txt"
mv "$D/tools/puppet_pack/dumpcheck.py" "$tmp/dumpcheck.py"
printf 'nup' >"$HOME/Downloads/Unchecked.nup"
rm -rf "$P"; : >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack3b.txt" 2>&1; export CODE=$?
check 'pack: no dumpcheck.py: an unchecked .nup is never taken' bash -c '[ $CODE -eq 0 ] && ! grep -q "^puppet_pack" "$2" && ! grep -q Unchecked "$3"' _ "$tmp/pack3b.txt" "$log" "$D/game-files.txt"
mv "$tmp/dumpcheck.py" "$D/tools/puppet_pack/dumpcheck.py"
rm -f "$HOME/Downloads/Unchecked.nup" "$HOME/Downloads/Manual.nup"
mv "$tmp/away/far/OoT [04004860224DEC000].nup" "$nup_was"
printf '%s\n' "$nup_was" >"$D/game-files.txt"
cp "$D/tools/puppet_pack/build.py" "$tmp/build.py.good"
printf 'import sys\nprint("BuildError: Tex/x.bntx.zs: broken")\nsys.exit(1)\n' >"$D/tools/puppet_pack/build.py"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack4.txt" 2>&1; export CODE=$?
check 'pack: a pack tool error is said plainly and the start goes on, the next start tries again' bash -c '[ $CODE -eq 0 ] && grep -q "BuildError" "$1" && grep -q "could not be made (the lines above say why): the game starts" "$1" && [ ! -f "$2/.coop-stamp" ]' _ "$tmp/pack4.txt" "$P"
cp "$tmp/build.py.good" "$D/tools/puppet_pack/build.py"
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack5.txt" 2>&1; export CODE=$?
check 'pack: the tool fixed: the next start makes the pack' bash -c '[ $CODE -eq 0 ] && grep -q "^puppet_pack" "$2" && [ -f "$3/.coop-stamp" ]' _ "$tmp/pack5.txt" "$log" "$P"
# (review of row 210: the pack there is kept while a new one is made, so a make that fails puts it back, with its own
# stamp: the game keeps its pack, and the next start tries again)
printf 'import sys\nprint("BuildError: disk full")\nsys.exit(1)\n' >"$D/tools/puppet_pack/build.py"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack6.txt" 2>&1; export CODE=$?
check 'pack: a pack there and the tool fails: the pack from before stays (its own stamp), said plainly' \
    bash -c '[ $CODE -eq 0 ] && grep -q "BuildError: disk full" "$1" && grep -q "could not be made again (the lines above say why): the one from before stays" "$1" &&
        [ -f "$2/coop_puppet.json" ] && [ -f "$2/.coop-stamp" ]' _ "$tmp/pack6.txt" "$P"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack7.txt" 2>&1
check 'pack: still the broken tool: the next start tries again' bash -c 'grep -q "BuildError: disk full" "$1" && [ -f "$2/coop_puppet.json" ]' _ "$tmp/pack7.txt" "$P"
cp "$tmp/build.py.good" "$D/tools/puppet_pack/build.py"
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/pack8.txt" 2>&1; export CODE=$?
check 'pack: the tool back: the pack from before (made by this tool) is current again, not made again' \
    bash -c '[ $CODE -eq 0 ] && ! grep -q "^puppet_pack" "$2" && grep -q "puppet pack: built already" "$1"' _ "$tmp/pack8.txt" "$log"

# OoT Character Select by Snail (data/longshot/CHARACTER-SELECT.txt): fetched from his release onto the PC, never
# carried. Offline with his zip and characters.json already in data/builder/charsel (their SHA-256 match): no network.
# Owner, 0.11.9: every kit gets the Characters page (the default part); the NPC characters only on a yes (no terminal:
# no); his customizer is left out of his code patch.
CSR="$tmp/cs-rel/CS-v1/mods/OoT Character Select"
mkdir -p "$CSR/romfs/Model" "$CSR/romfs/Tex" "$CSR/romfs/UI" "$CSR/exefs" "$D/builder/charsel"
printf 'npc model' >"$CSR/romfs/Model/Player_Child_C000.Player_Child.bfres.zs"
printf 'link' >"$CSR/romfs/Model/Player_Child.Player_Child.bfres.zs"
printf 'tex' >"$CSR/romfs/Tex/Npc_A.bntx.zs"
printf 'ui' >"$CSR/romfs/UI/LayoutArchiveCategory.Product.100.byml.zs"
# His code patch as IPS32: a kept record, one record in each customizer region his source names, a kept run record.
cs_ips() { "$py" -c '
import sys
regions = [0x804D756C, 0x8289D600, 0x832E1620, 0x832E39D0, 0x8103AE80, 0x83BCDC10, 0x83BCCE70]
skip = int(sys.argv[2])
out = bytearray(b"IPS32")
rec = lambda at, b: (at.to_bytes(4, "big") + len(b).to_bytes(2, "big") + b)
out += rec(0x14910, b"keep v1")
for i, a in enumerate(regions):
    if i != skip:
        out += rec(a - 0x8001FF00, b"\x01\x02\x03\x04")
out += bytes([0x00, 0x9B, 0x77, 0xB0, 0, 0, 0, 16, 0xAA]) + b"EEOF"
open(sys.argv[1], "wb").write(out)' "$1" "${2:--1}"; }
cs_ips "$CSR/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips"
cp "$CSR/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips" "$tmp/cs-his.ips"
printf 'install' >"$tmp/cs-rel/CS-v1/INSTALL.txt"
(cd "$tmp/cs-rel" && "$py" -c 'import os, zipfile
with zipfile.ZipFile("CS-v1.zip", "w") as z:
    for r, _, fs in os.walk("CS-v1"):
        for f in fs: z.write(os.path.join(r, f))')
cp "$tmp/cs-rel/CS-v1.zip" "$D/builder/charsel/CS-v1.zip"
printf '{"format": 1, "mod": "OoT Character Select", "characters": []}' >"$D/builder/charsel/characters.json"
cs_zsha=$(sha256sum "$D/builder/charsel/CS-v1.zip" | cut -d' ' -f1) cs_osha=$(sha256sum "$D/builder/charsel/characters.json" | cut -d' ' -f1)
cs_pin() { printf 'release=v1\nurl=https://example.invalid/cs\nzip=CS-v1.zip\nzip_sha256=%s\nzip_folder=CS-v1/mods/OoT Character Select\nfolder=OoT Character Select\noverlay_url=https://example.invalid/characters.json\noverlay_sha256=%s\nips_url=%s\nips_sha256=%s\n' \
    "$cs_zsha" "$cs_osha" "${1:-}" "${2:-}" >"$kit/data/longshot/CHARACTER-SELECT.txt"; }
cs_pin
rm -f "$D/character-select-answer.txt"
CS="$D/game/mods/OoT Character Select"
# (the kept records of an installed patch, one per line, and the whole of his own kept beside it)
cs_recs() { "$py" -c '
import sys
d = open(sys.argv[1], "rb").read(); o = 5; n = 0
assert d[:5] == b"IPS32"
while d[o:o + 4] != b"EEOF":
    k = int.from_bytes(d[o + 4:o + 6], "big"); o += 9 if k == 0 else 6 + k; n += 1
print(n)' "$1"; }
# review-geno-0118: the game build after the install fails -> the mods folder goes back to what the game was built
# with, one line says so, and the start goes on with the game as it was (not stopped).
stamp_was=$(cat "$D/game/coop-built-from.txt")
FAKE_BUILD_FAIL=1 OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs0.txt" 2>&1; export CODE=$?
check 'Character Select: a failed game build after the install takes it back out and the start goes on with the game as it was'     bash -c '[ $CODE -eq 0 ] && grep -q "no terminal: no" "$1" && grep -q "taken back out for now" "$1" && [ ! -e "$2" ] && [ ! -e "$3/game/mods/_OoT Character Select.none" ] && [ ! -e "$3/game/mods/_OoT Character Select.prev" ] &&
        [ "$(cat "$3/game/coop-built-from.txt")" = "$4" ]' _ "$tmp/build-cs0.txt" "$CS" "$D" "$stamp_was"
cp "$tmp/cs-rel/CS-v1.zip" "$D/builder/charsel/CS-v1.zip"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs1.txt" 2>&1; export CODE=$?
check 'Character Select: no -> the default part alone (the Characters page, Link), characters.json, stamped ui, credited, the answer kept' \
    bash -c '[ $CODE -eq 0 ] && [ -f "$2/romfs/UI/LayoutArchiveCategory.Product.100.byml.zs" ] && [ -f "$2/romfs/Model/Player_Child.Player_Child.bfres.zs" ] &&
        [ ! -e "$2/romfs/Model/Player_Child_C000.Player_Child.bfres.zs" ] && [ ! -e "$2/romfs/Tex" ] && [ -f "$2/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips" ] &&
        [ -f "$2/characters.json" ] && [ ! -e "$2/INSTALL.txt" ] && grep -q "^v1 .* ui cz1$" "$2/coop-charsel.txt" && grep -q "belong to Nintendo" "$2/CREDITS-coop.txt" &&
        grep -qx no "$3/character-select-answer.txt" && [ ! -e "$3/builder/charsel/CS-v1.zip" ] && [ ! -e "$3/game/mods/coop-mods.list" ]' _ "$tmp/build-cs1.txt" "$CS" "$D"
export CS_RECS=$(cs_recs "$CS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips")
check 'Character Select strip: the exefs patch keeps only the 2 records outside his customizer, his own whole beside it (outside exefs)' \
    bash -c '[ "$CS_RECS" = 2 ] && cmp -s "$1/coop-charsel-code.ips" "$2" && grep -q "keep v1" "$1/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips"' _ "$CS" "$tmp/cs-his.ips"
check 'Character Select: its code patch is in the game stamp (the build takes it in: patches 0034/0035)' grep -q 'exefs=OoT Character Select/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips:' "$D/game/coop-built-from.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs2.txt" 2>&1
check 'Character Select: a built game confirms the change (no kept state left)' bash -c '[ ! -e "$1/_OoT Character Select.none" ] && [ ! -e "$1/_OoT Character Select.prev" ]' _ "$D/game/mods"
check 'Character Select: there already -> not asked again, nothing done' bash -c 'grep -q "Character Select v1 -> .*(there already)" "$1" && ! grep -q "Download the NPC characters" "$1"' _ "$tmp/build-cs2.txt"
# Turned on later: the default part cannot give the NPC part, so his release is used again.
printf 'character_select = yes\n' >>"$D/coop-options.ini"
cp "$tmp/cs-rel/CS-v1.zip" "$D/builder/charsel/CS-v1.zip"
: >"$log"; OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs3.txt" 2>&1; export CODE=$?
export CS_RECS=$(cs_recs "$CS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips")
check 'Character Select: character_select = yes gets the NPC part too, his patch still stripped' \
    bash -c '[ $CODE -eq 0 ] && [ -f "$2/romfs/Model/Player_Child_C000.Player_Child.bfres.zs" ] && [ -f "$2/romfs/Tex/Npc_A.bntx.zs" ] && grep -q "^v1 .* full cz1$" "$2/coop-charsel.txt" &&
        [ "$CS_RECS" = 2 ]' _ "$tmp/build-cs3.txt" "$CS"
check 'Character Select pack: UI-only -> full (NPC models put in) makes the puppet pack again' bash -c 'grep -q "^puppet_pack" "$1" && grep -q "models:" "$2/.coop-stamp"' _ "$log" "$P"
# Off again: from the copy there (no zip here, nothing downloaded): the NPC part out, the Characters page kept.
sed -i 's/^character_select = yes$/character_select = no/' "$D/coop-options.ini"
: >"$log"; OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs4.txt" 2>&1; export CODE=$?
check 'Character Select: character_select = no keeps the Characters page, takes the NPC part out, no download' \
    bash -c '[ $CODE -eq 0 ] && [ ! -e "$2/romfs/Model/Player_Child_C000.Player_Child.bfres.zs" ] && [ ! -e "$2/romfs/Tex" ] && [ -f "$2/romfs/UI/LayoutArchiveCategory.Product.100.byml.zs" ] &&
        grep -q "^v1 .* ui cz1$" "$2/coop-charsel.txt" && grep -q "exefs=OoT Character Select/exefs/" "$3/game/coop-built-from.txt" && ! grep -q downloading "$1"' _ "$tmp/build-cs4.txt" "$CS" "$D"
# (player report 210: the UI-only pack was kept in data/pack-variants when the full one was made: put back, not made)
check 'Character Select pack: full -> UI-only (NPC models taken out) puts the UI-only pack made before back (not made again)' \
    bash -c '! grep -q "^puppet_pack" "$1" && grep -q "puppet pack: the one made before for these characters is put back" "$2"' _ "$log" "$tmp/build-cs4.txt"
built_on=$(cat "$D/game/coop-built-from.txt")
# The mod switched off in the Mods tab, then on again: the pack and the game made the first time, put back the second.
printf 'coop-mods 1\noff OoT Character Select\n' >"$D/game/mods/coop-mods.list"; : >"$log"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs5.txt" 2>&1
check 'Character Select pack: switched off in the Mods tab makes the puppet pack again' bash -c 'grep -q "^puppet_pack" "$1" && grep -q "list:off" "$2/.coop-stamp"' _ "$log" "$P"
check 'Character Select game: switched off in the Mods tab: built without its code, the build before kept, the line says why' \
    bash -c 'grep -q "^make_game .*--out" "$1" && grep -q "Building the game: the mods. code patches changed" "$2" &&
        grep -q "is kept, so going back to these mods takes seconds" "$2" && [ -f "$(ls -1d "$3"/game-variants/*/ | head -n 1)stamp.txt" ]' _ "$log" "$tmp/build-cs5.txt" "$D"
# (on again as the Mods tab writes it: an "on" row, which the game reads as no row: the same stamp as before the switch)
printf 'coop-mods 1\non OoT Character Select\n' >"$D/game/mods/coop-mods.list"; : >"$log"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs6.txt" 2>&1
rm -f "$D/game/mods/coop-mods.list"
check 'Character Select pack: switched on again puts the pack made before back (not made again)' \
    bash -c '! grep -q "^puppet_pack" "$1" && grep -q "list:on" "$2/.coop-stamp" && grep -q "is put back (seconds" "$3"' _ "$log" "$P" "$tmp/build-cs6.txt"
check 'Character Select game: switched on again puts the build made before back (no make_game), with its stamp' \
    bash -c '! grep -q "^make_game" "$1" && grep -q "putting back the build made before with these mods" "$2" && [ "$(cat "$3/game/coop-built-from.txt")" = "$4" ] &&
        [ -x "$3/game/longshot" ] && [ -f "$3/game/image.bin" ] && ! ls "$3"/game/*.new >/dev/null 2>&1' _ "$log" "$tmp/build-cs6.txt" "$D" "$built_on"
: >"$log"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >/dev/null 2>&1
check 'Character Select pack: nothing changed -> the pack is not made again' bash -c '! grep -q "^puppet_pack" "$1"' _ "$log"
# A 0.11.8 copy (four-field stamp, his patch as it came): kept full, stripped in place, no download.
sed -i 's/^character_select = no$/character_select = yes/' "$D/coop-options.ini"
cp "$tmp/cs-rel/CS-v1.zip" "$D/builder/charsel/CS-v1.zip"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >/dev/null 2>&1
printf 'v1 %s %s -\n' "$cs_zsha" "$cs_osha" >"$CS/coop-charsel.txt"
rm -f "$CS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips" "$CS/coop-charsel-code.ips"
cp "$tmp/cs-his.ips" "$CS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs5.txt" 2>&1; export CODE=$?
export CS_RECS=$(cs_recs "$CS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips")
check 'Character Select: a 0.11.8 full copy keeps its NPC part, his patch stripped, his own kept aside, no download' \
    bash -c '[ $CODE -eq 0 ] && [ -f "$2/romfs/Model/Player_Child_C000.Player_Child.bfres.zs" ] && grep -q "^v1 .* full cz1$" "$2/coop-charsel.txt" &&
        [ "$CS_RECS" = 2 ] && cmp -s "$2/coop-charsel-code.ips" "$3" && ! grep -q downloading "$1"' _ "$tmp/build-cs5.txt" "$CS" "$tmp/cs-his.ips"
# A newer patch of his laid out otherwise (no record at cz_head): refused, the copy there kept as it was.
cs_ips "$D/builder/charsel/code.ips" 0
cs_pin https://example.invalid/code.ips "$(sha256sum "$D/builder/charsel/code.ips" | cut -d' ' -f1)"
cp "$tmp/cs-rel/CS-v1.zip" "$D/builder/charsel/CS-v1.zip"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs6.txt" 2>&1; export CODE=$?
check 'Character Select strip: a patch without a customizer region is refused, the copy there kept' \
    bash -c '[ $CODE -eq 0 ] && grep -q "nothing at cz_head" "$1" && grep -q "^v1 .* - full cz1$" "$2/coop-charsel.txt"' _ "$tmp/build-cs6.txt" "$CS"
cs_pin
# A copy the player put in by hand (no stamp) is never touched.
rm -f "$CS/coop-charsel.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs7.txt" 2>&1
check 'Character Select: the player'"'"'s own copy is used as it is' bash -c 'grep -q "your own copy" "$1" && [ -f "$2/romfs/Tex/Npc_A.bntx.zs" ]' _ "$tmp/build-cs7.txt" "$CS"
rm -rf "$CS"
sed -i '/^character_select = /d' "$D/coop-options.ini"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >/dev/null 2>&1   # (the game built again without it)
rm -f "$kit/data/longshot/CHARACTER-SELECT.txt" "$D/character-select-answer.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-cs8.txt" 2>&1; export CODE=$?
check 'Character Select: a kit without CHARACTER-SELECT.txt asks nothing and fetches nothing' bash -c '[ $CODE -eq 0 ] && ! grep -q "Character Select" "$1"' _ "$tmp/build-cs8.txt"

# The randomizer the kit brings (data/longshot/OOTRR.txt): fetched and unpacked into data/game/tools/ootrr.
mkdir -p "$tmp/ootrr-rel/ootrr-0.5.0-linux-x86_64"
printf '#!/bin/sh\necho ootrr\n' >"$tmp/ootrr-rel/ootrr-0.5.0-linux-x86_64/ootrr"
printf 'GPL\n' >"$tmp/ootrr-rel/ootrr-0.5.0-linux-x86_64/LICENSE"
tar -czf "$tmp/ootrr-rel/OOTRR-test-linux.tar.gz" -C "$tmp/ootrr-rel" ootrr-0.5.0-linux-x86_64
o_sha=$(sha256sum "$tmp/ootrr-rel/OOTRR-test-linux.tar.gz" | cut -d' ' -f1)
printf 'release=v0.5.0\nurl=%s\nlinux=OOTRR-test-linux.tar.gz\nlinux_sha256=%s\n' "$tmp/ootrr-rel" "$o_sha" >"$kit/data/longshot/OOTRR.txt"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o1.txt" 2>&1; export CODE=$?
check 'build: the kit'"'"'s OOTRR is put in data/game/tools/ootrr, runnable, with its licence' \
    bash -c '[ $CODE -eq 0 ] && [ -x "$1/game/tools/ootrr/ootrr" ] && [ -f "$1/game/tools/ootrr/LICENSE" ] && grep -q "randomizer: OOTRR v0.5.0" "$2"' _ "$D" "$tmp/build-o1.txt"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o2.txt" 2>&1
check 'build: OOTRR there already is kept' has "$tmp/build-o2.txt" 'there already'
rm -rf "$D/game/tools/ootrr"
printf 'release=v0.5.0\nurl=%s\nlinux=OOTRR-test-linux.tar.gz\nlinux_sha256=%s\n' "$tmp/ootrr-rel" "$(printf '0%.0s' {1..64})" >"$kit/data/longshot/OOTRR.txt"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o3.txt" 2>&1; export CODE=$?
check 'build: an OOTRR whose SHA-256 differs only warns, and is not put in' \
    bash -c '[ $CODE -eq 0 ] && grep -q "SHA-256 differs" "$1" && [ ! -e "$2/game/tools/ootrr/ootrr" ]' _ "$tmp/build-o3.txt" "$D"
# No internet (OOT_COOP_OFFLINE=1): the OOTRR tarball already downloaded and matching its SHA-256 still goes in;
# without one there is no download try, one 'offline' line, and the start goes on.
printf 'release=v0.5.0\nurl=https://example.invalid/ootrr\nlinux=OOTRR-test-linux.tar.gz\nlinux_sha256=%s\n' "$o_sha" >"$kit/data/longshot/OOTRR.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o4.txt" 2>&1; export CODE=$?
check 'build offline: the OOTRR tarball already here (its SHA-256 matched) is put in, no download' \
    bash -c '[ $CODE -eq 0 ] && [ -x "$1/game/tools/ootrr/ootrr" ] && ! grep -q downloading "$2"' _ "$D" "$tmp/build-o4.txt"
# Another OOTRR release (0.5.0 -> 0.6.0): the built seeds and their seed files go (the game makes them again with
# it); another mod, a folder with no coop-seed.txt and a seed put in by hand (no coop-rando.txt) stay. The same
# release again removes nothing.
mkdir -p "$D/game/mods/randomizer-EF" "$D/game/mods/randomizer-half" "$D/game/mods/oot-remake-randomizer" "$D/game/mods/hd/romfs" "$D/game/mods/randomizer-IJ"
echo EF >"$D/game/mods/randomizer-EF/coop-seed.txt" && echo default >"$D/game/mods/randomizer-EF/coop-rando.txt"
echo IJ >"$D/game/mods/randomizer-IJ/coop-seed.txt"
echo '{}' >"$D/game/mods/oot-remake-randomizer/randomizer-EF.ootrr"
echo chests >"$D/game/mods/oot-remake-randomizer/randomizer-EF.spoiler.txt"
printf 'release=v0.6.0\nurl=https://example.invalid/ootrr\nlinux=OOTRR-test-linux.tar.gz\nlinux_sha256=%s\n' "$o_sha" >"$kit/data/longshot/OOTRR.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o6.txt" 2>&1; export CODE=$?
check 'build: another OOTRR release removes the built seeds (the game makes them again), keeps the rest' \
    bash -c '[ $CODE -eq 0 ] && grep -q "seed randomizer-EF removed for OOTRR v0.6.0" "$1" && [ ! -e "$2/game/mods/randomizer-EF" ] && [ ! -e "$2/game/mods/oot-remake-randomizer/randomizer-EF.ootrr" ] && [ ! -e "$2/game/mods/oot-remake-randomizer/randomizer-EF.spoiler.txt" ] && [ -d "$2/game/mods/randomizer-half" ] && [ -d "$2/game/mods/hd/romfs" ] && [ -f "$2/game/mods/randomizer-IJ/coop-seed.txt" ]' _ "$tmp/build-o6.txt" "$D"
rm -rf "$D/game/mods/randomizer-IJ"
mkdir -p "$D/game/mods/randomizer-GH" && echo GH >"$D/game/mods/randomizer-GH/coop-seed.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o7.txt" 2>&1
check 'build: the same OOTRR release again keeps the seeds' bash -c '[ -f "$1/game/mods/randomizer-GH/coop-seed.txt" ] && ! grep -q "removed for OOTRR" "$2"' _ "$D" "$tmp/build-o7.txt"
check 'build: a set-up OOTRR says ok in data/game/tools/ootrr-setup.txt' grep -qx 'ok v0.6.0' "$D/game/tools/ootrr-setup.txt"
# A new OOTRR release that does not run here (player report 2026-11-19: the old way deleted tools/ootrr first, so any
# failure after it left the game with none): unpacked beside the old one and tried; the old OOTRR, its stamp and the
# seeds stay, nothing half-made is left, and ootrr-setup.txt says why (the game's chat shows it).
mkdir -p "$tmp/ootrr-bad/ootrr-0.7.0-linux-x86_64"
printf '#!/bin/sh\nexit 3\n' >"$tmp/ootrr-bad/ootrr-0.7.0-linux-x86_64/ootrr"
tar -czf "$D/builder/OOTRR-bad-linux.tar.gz" -C "$tmp/ootrr-bad" ootrr-0.7.0-linux-x86_64
b_sha=$(sha256sum "$D/builder/OOTRR-bad-linux.tar.gz" | cut -d' ' -f1)
printf 'release=v0.7.0\nurl=https://example.invalid/ootrr\nlinux=OOTRR-bad-linux.tar.gz\nlinux_sha256=%s\n' "$b_sha" >"$kit/data/longshot/OOTRR.txt"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o8.txt" 2>&1; export CODE=$?
check 'build: an OOTRR that does not run keeps the old one and its seeds, says why, leaves nothing half-made' \
    bash -c '[ $CODE -eq 0 ] && grep -q "did not run here" "$1" && [ -x "$2/game/tools/ootrr/ootrr" ] && grep -q "^v0.6.0 " "$2/game/tools/ootrr/coop-ootrr.txt" && [ -f "$2/game/mods/randomizer-GH/coop-seed.txt" ] && [ ! -e "$2/game/tools/ootrr.new" ] && [ ! -e "$2/game/tools/ootrr.old" ] && grep -q "^failed v0.7.0: ootrr did not run here" "$2/game/tools/ootrr-setup.txt"' _ "$tmp/build-o8.txt" "$D"
# A swap stopped between its two moves (tools/ootrr.old, no tools/ootrr): the old folder goes back first.
mv "$D/game/tools/ootrr" "$D/game/tools/ootrr.old"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o9.txt" 2>&1
check 'build: a stopped OOTRR swap is put back' bash -c '[ -x "$1/game/tools/ootrr/ootrr" ] && [ ! -e "$1/game/tools/ootrr.old" ]' _ "$D"
rm -f "$D/builder/OOTRR-bad-linux.tar.gz"
printf 'release=v0.6.0\nurl=https://example.invalid/ootrr\nlinux=OOTRR-test-linux.tar.gz\nlinux_sha256=%s\n' "$o_sha" >"$kit/data/longshot/OOTRR.txt"
rm -rf "$D/game/mods/randomizer-GH" "$D/game/mods/randomizer-half" "$D/game/mods/hd"
rm -rf "$D/game/tools/ootrr" "$D/builder/OOTRR-test-linux.tar.gz"
OOT_COOP_OFFLINE=1 bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-o5.txt" 2>&1; export CODE=$?
check 'build offline: no OOTRR here -> no download try, one offline line, the game still ready' \
    bash -c '[ $CODE -eq 0 ] && grep -q "offline: using the tools already here" "$1" && ! grep -q -e downloading -e "not set up" "$1" && grep -q "The game is ready" "$1" && [ ! -e "$2/game/tools/ootrr" ]' _ "$tmp/build-o5.txt" "$D"
rm -f "$kit/data/longshot/OOTRR.txt"

# A randomizer seed older than the puppet pack (the pack made again): its size table lacks the pack's entries and
# the game aborts while it loads (lane1, 2026-11-14). The start builds it again beside the pack's table, or removes it.
G=$D/game R=romfs/System/Resource
mkdir -p "$G/tools/ootrr" "$G/romfs/System" "$G/mods/oot-remake-randomizer" "$G/mods/randomizer-AB/$R" "$G/mods/randomizer-CD/$R" \
    "$G/mods/_off/$R" "$G/mods/contents/0400486024DEC000/coop-puppet/$R"
cat >"$G/tools/ootrr/ootrr" <<'EOF'
#!/bin/sh
# build <seed file> <romfs> <load dir> <name> --host hookshot: the tables beside it, then the seed as OOTRR writes it
ls "$4"/*/romfs/System/Resource/ | grep rsizetable >"$4/../staged.txt"
cat "$4"/*/romfs/System/Resource/*.rsizetable.zs >>"$4/../staged.txt"
mkdir -p "$4/$5/romfs/System/Resource" && echo new >"$4/$5/romfs/System/Resource/T.rsizetable.zs" && echo AB >"$4/$5/coop-seed.txt"
EOF
chmod +x "$G/tools/ootrr/ootrr"
for s in AB CD; do echo old >"$G/mods/randomizer-$s/$R/T.rsizetable.zs"; echo "$s" >"$G/mods/randomizer-$s/coop-seed.txt"; done
echo default >"$G/mods/randomizer-AB/coop-rando.txt" && echo default >"$G/mods/randomizer-CD/coop-rando.txt"
# (a seed put in by hand, older than the pack too: not the kit's to build again or remove; merge_tables.py merges it)
mkdir -p "$G/mods/randomizer-KL/$R" && echo hand >"$G/mods/randomizer-KL/$R/T.rsizetable.zs" && echo KL >"$G/mods/randomizer-KL/coop-seed.txt"
echo chests >"$G/mods/randomizer-AB/spoiler.txt" && echo OOTRR1-XZLA >"$G/mods/randomizer-AB/settings.txt"
echo seed >"$G/mods/oot-remake-randomizer/randomizer-AB.ootrr"   # (none kept for CD: it cannot be built again)
echo off >"$G/mods/_off/$R/T.rsizetable.zs"
touch -d '1 hour ago' "$G/mods/randomizer-AB/$R/T.rsizetable.zs" "$G/mods/randomizer-CD/$R/T.rsizetable.zs" "$G/mods/randomizer-KL/$R/T.rsizetable.zs"
echo pack >"$G/mods/contents/0400486024DEC000/coop-puppet/$R/T.rsizetable.zs"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-r1.txt" 2>&1; export CODE=$?
check 'build: a seed older than the puppet pack is built again beside the pack'"'"'s size table (no off folder'"'"'s), its preset, spoiler.txt and settings.txt kept' \
    bash -c '[ $CODE -eq 0 ] && grep -q "seed randomizer-AB built again" "$1" && [ "$(cat "$2/mods/randomizer-AB/romfs/System/Resource/T.rsizetable.zs")" = new ] &&
        grep -qx pack "$2/mods/oot-remake-randomizer/staged.txt" && ! grep -q off "$2/mods/oot-remake-randomizer/staged.txt" &&
        [ -f "$2/mods/randomizer-AB/coop-rando.txt" ] &&
        [ "$(cat "$2/mods/randomizer-AB/spoiler.txt")" = chests ] && [ "$(cat "$2/mods/randomizer-AB/settings.txt")" = OOTRR1-XZLA ] && [ ! -e "$2/mods/oot-remake-randomizer/_seed-build" ]' _ "$tmp/build-r1.txt" "$G"
check 'build: a seed older than the pack that cannot be built again (no seed file kept) is removed, said once' \
    bash -c 'grep -q "seed randomizer-CD removed" "$1" && [ ! -e "$2/mods/randomizer-CD" ]' _ "$tmp/build-r1.txt" "$G"
check 'build: a hand-put seed older than the pack stays as it is' \
    bash -c '[ "$(cat "$2/mods/randomizer-KL/romfs/System/Resource/T.rsizetable.zs")" = hand ] && ! grep -q "randomizer-KL" "$1"' _ "$tmp/build-r1.txt" "$G"
rm -rf "$G/mods/randomizer-KL"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-r2.txt" 2>&1
check 'build: the next start leaves the rebuilt seed alone' bash -c '! grep -q "randomizer: seed" "$1"' _ "$tmp/build-r2.txt"
# (player report 210: by content, not by date) the seed keeps what it was built beside; the pack's table written again
# with the same bytes (newer date) builds nothing, the table changed with an older date (a pack put back) builds it again.
check 'build: a seed built again keeps its tables by content (coop-seed-tables.txt)' \
    grep -q '^contents/0400486024DEC000/coop-puppet/romfs/System/Resource/T.rsizetable.zs [0-9a-f]\{64\}$' "$G/mods/randomizer-AB/coop-seed-tables.txt"
touch -d '1 minute' "$G/mods/contents/0400486024DEC000/coop-puppet/$R/T.rsizetable.zs"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-r3.txt" 2>&1
check 'build: the pack table dated newer with the same bytes -> the seed is not built again' bash -c '! grep -q "randomizer: seed" "$1"' _ "$tmp/build-r3.txt"
echo pack2 >"$G/mods/contents/0400486024DEC000/coop-puppet/$R/T.rsizetable.zs"
touch -d '2 hours ago' "$G/mods/contents/0400486024DEC000/coop-puppet/$R/T.rsizetable.zs"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build-r4.txt" 2>&1
check 'build: the pack table changed but dated older (a pack put back) -> the seed is built again' \
    bash -c 'grep -q "seed randomizer-AB built again" "$1" && grep -qx pack2 "$2/mods/oot-remake-randomizer/staged.txt"' _ "$tmp/build-r4.txt" "$G"
rm -rf "$G/tools/ootrr" "$G/romfs" "$G/mods/randomizer-AB" "$G/mods/oot-remake-randomizer" "$G/mods/_off"

# A new patch set: refreshed in place, the game built again.
cat >"$kit/data/longshot/patches/0001-test.patch" <<'EOF'
diff --git a/host/coop_test.c b/host/coop_test.c
new file mode 100644
--- /dev/null
+++ b/host/coop_test.c
@@ -0,0 +1 @@
+int coop_test2;
EOF
: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build3.txt" 2>&1
check 'build: a changed patch is applied again and the game rebuilt' bash -c 'grep -q coop_test2 "$1/builder/source/host/coop_test.c" && grep -q "^make_game .*--out" "$2"' _ "$D" "$log"

# --renderer-only (CI): the patched renderer, no game.
: >"$log"
bash "$kit/scripts/build.sh" --renderer-only "$tmp/vk" </dev/null >"$tmp/vk.txt" 2>&1
check 'build --renderer-only: the renderer from the patched source, never the prebuilt one' bash -c '[ -f "$1/sa/build/longshot_vk.so" ] && grep -q "build.py VK_ONLY=1 prebuilt=-" "$2"' _ "$tmp/vk" "$log"

# A wrong game: Longshot's check says no, the build stops in plain words.
printf 'nup' >"$tmp/bad.nup"
bash "$kit/scripts/build.sh" --game "$tmp/bad.nup" --rebuild </dev/null >"$tmp/build4.txt" 2>&1; export CODE=$?
check 'build: a wrong game version stops with what to dump' bash -c '[ $CODE -ne 0 ] && grep -q "program 0400486024DEC000" "$1"' _ "$tmp/build4.txt"

# A damaged builder download: nothing unpacked.
kit2=$tmp/kit2
cp -a "$kit" "$kit2"
rm -rf "$kit2/data/builder" "$kit2/data/game"
sed -i "s/^sha256=.*/sha256=$(printf '0%.0s' {1..64})/" "$kit2/data/longshot/LINUX.txt"
bash "$kit2/scripts/build.sh" </dev/null >"$tmp/build5.txt" 2>&1; export CODE=$?
check 'build: a damaged builder download stops, nothing unpacked' bash -c '[ $CODE -ne 0 ] && grep -q "is damaged" "$1" && [ ! -d "$2/data/builder/image" ]' _ "$tmp/build5.txt" "$kit2"
# kit-mirror (0.11.1): Longshot's server failing (here a host blocked as in a test), the builder comes from the co-op
# mirror (here a folder), checked against the same SHA-256; a mirror with another file is not used.
rm -rf "$kit2" && cp -a "$kit" "$kit2" && rm -rf "$kit2/data/builder" "$kit2/data/game"
mkdir -p "$tmp/mirror" && cp "$appimage" "$tmp/mirror/Longshot-test.AppImage"
sed -i "s#^url=.*#url=https://git.magicbean.dev/MagicBeanDev/Longshot/releases/download/v3.0.0-rc.3/Longshot-test.AppImage#" "$kit2/data/longshot/LINUX.txt"
printf 'mirror=%s\n' "$tmp/nowhere" >>"$kit2/data/longshot/LINUX.txt"
OOT_COOP_TEST_BLOCK_HOST='magicbean\.dev$' OOT_COOP_MIRROR="$tmp/mirror" bash "$kit2/scripts/build.sh" </dev/null >"$tmp/build7.txt" 2>&1; export CODE=$?
check 'build: the Longshot server blocked: the builder from the mirror, 3 tries first' bash -c '[ $CODE -eq 0 ] && grep -q "git.magicbean.dev failed (try 2 of 3)" "$1" && grep -q "got it from this PC" "$1" && [ -d "$2/data/builder/image" ]' _ "$tmp/build7.txt" "$kit2"
rm -rf "$kit2/data/builder" "$kit2/data/game"
printf 'not the builder' >"$tmp/mirror/Longshot-test.AppImage"
OOT_COOP_TEST_BLOCK_HOST='magicbean\.dev$' OOT_COOP_MIRROR="$tmp/mirror" bash "$kit2/scripts/build.sh" </dev/null >"$tmp/build8.txt" 2>&1; export CODE=$?
check 'build: a wrong file on the mirror is not used; the text names each host, the mirror and a browser test' bash -c '[ $CODE -ne 0 ] && grep -q "git.magicbean.dev: the download failed; this PC: the file is damaged" "$1" && grep -q "co-op mirror on GitHub" "$1" && grep -q "open this in a browser" "$1" && [ ! -d "$2/data/builder/image" ] && [ ! -f "$2/data/builder/Longshot-test.AppImage.part" ]' _ "$tmp/build8.txt" "$kit2"
# No game file and no terminal (Steam's Game Mode on a first start): said, nothing asked.
rm -rf "$kit2" && cp -a "$kit" "$kit2" && rm -rf "$kit2/data/game" "$kit2/data/game-files.txt"
mv "$HOME/Downloads" "$HOME/Downloads-away"
bash "$kit2/scripts/build.sh" </dev/null >"$tmp/build6.txt" 2>&1; export CODE=$?
check 'build: no game and no terminal: says to start it from a terminal once' bash -c '[ $CODE -ne 0 ] && grep -q "needs your game file" "$1"' _ "$tmp/build6.txt"
mv "$HOME/Downloads-away" "$HOME/Downloads"
rm -rf "$kit2"

# --- start.sh (Linux only: its checks read this PC) --------------------------------------------
if [[ $(uname -s) == Linux ]]; then
    : >"$log"
    OOT_COOP_SECRET=1 bash "$kit/start-squadala.sh" --no-update </dev/null >"$tmp/start1.txt" 2>&1; export CODE=$?
    check 'start: builds if needed, then starts the game in data/game' bash -c '[ $CODE -eq 0 ] && grep -q "^started $2 1 $3$" "$1"' _ "$log" "$D" "$D/game"
    check 'start: without a terminal, what it says goes to data/logs/start.log' has "$D/logs/start.log" 'start without a terminal'
    check 'start: an applications menu entry for this kit (Steam: Add a Non-Steam Game)' bash -c 'grep -qF "Exec=bash \"$2/start-squadala.sh\"" "$1" && grep -q "^Categories=Game;" "$1"' _ "$XDG_DATA_HOME/applications/oot-coop.desktop" "$kit"
    check 'start: "sh start-squadala.sh" works too (POSIX sh start file)' sh -n "$kit/start-squadala.sh"
    bash "$kit/start-squadala.sh" --nope </dev/null >"$tmp/start2.txt" 2>&1; export CODE=$?
    check 'start: an unknown option is refused' bash -c '[ $CODE -ne 0 ] && grep -q "Unknown option" "$1"' _ "$tmp/start2.txt"
    bash "$kit/start-squadala.sh" --help </dev/null >"$tmp/start3.txt" 2>&1
    check 'start --help lists the options' has "$tmp/start3.txt" '--no-update'
    check 'start --help lists --pack' has "$tmp/start3.txt" '--pack'
    check 'start --help lists --problem-reports' has "$tmp/start3.txt" '--problem-reports'
    check 'start: no problem-report address in the kit: nothing asked, nothing sent' bash -c '! grep -q "Send problem reports" "$1" && [ ! -e "$2/logs/problem-send.json" ]' _ "$tmp/start1.txt" "$D"
    bash "$kit/start-squadala.sh" --problem-reports nosend </dev/null >"$tmp/start4.txt" 2>&1
    check 'start --problem-reports nosend: sending off, kept' bash -c 'grep -q "\"send\": \"off\"" "$1"' _ "$D/logs/problem-send.json"
    bash "$kit/start-squadala.sh" --problem-reports maybe </dev/null >"$tmp/start5.txt" 2>&1; export CODE=$?
    check 'start --problem-reports: an unknown answer is refused' bash -c '[ $CODE -ne 0 ]'
    # A menu entry made by a kit up to v0.11.3 (Exec= start-oot-coop.sh, Name=OoT Co-op), another on the desktop and a
    # Steam entry: the start (update.sh --tidy) points them at start-squadala.sh, which the kit has alone now.
    printf '[Desktop Entry]\nName=OoT Co-op\nExec=bash "%s/start-oot-coop.sh"\n' "$kit" >"$XDG_DATA_HOME/applications/oot-coop.desktop"
    mkdir -p "$HOME/Desktop" && printf '[Desktop Entry]\nExec=bash "%s/start-oot-coop.sh"\nPath=%s\n' "$kit" "$kit" >"$HOME/Desktop/coop.desktop"
    printf '[Desktop Entry]\nExec=bash "/elsewhere/start-oot-coop.sh"\n' >"$HOME/Desktop/other.desktop"
    mkdir -p "$tmp/steam/userdata/7/config"
    { printf '\0shortcuts\0\0000\0\001Exe\0"%s/start-oot-coop.sh"\0\002\377\376\0' "$kit"; printf '\001Exe\0"/elsewhere/start-oot-coop.sh"\0\010\010'; } >"$tmp/steam/userdata/7/config/shortcuts.vdf"
    vsize=$(wc -c <"$tmp/steam/userdata/7/config/shortcuts.vdf")
    : >"$log"
    OOT_COOP_STEAM_DIR=$tmp/steam bash "$kit/start-squadala.sh" --no-update </dev/null >"$tmp/start4.txt" 2>&1
    check 'start: an old menu entry (start-oot-coop.sh) now runs start-squadala.sh, named Squadala' bash -c \
        'grep -qF "Exec=bash \"$2/start-squadala.sh\"" "$1" && grep -qx "Name=Squadala" "$1"' _ "$XDG_DATA_HOME/applications/oot-coop.desktop" "$kit"
    check 'start: a desktop entry for this kit runs start-squadala.sh; another one is left alone' bash -c \
        'grep -qF "$2/start-squadala.sh" "$1/coop.desktop" && grep -qF "/elsewhere/start-oot-coop.sh" "$1/other.desktop"' _ "$HOME/Desktop" "$kit"
    check 'start: the Steam entry runs start-squadala.sh, its other bytes and another entry as they were' bash -c \
        'LC_ALL=C grep -qaF "\"$2/start-squadala.sh\"" "$1" && ! LC_ALL=C grep -qaF "$2/start-oot-coop.sh" "$1" && LC_ALL=C grep -qaF "/elsewhere/start-oot-coop.sh" "$1" &&
         [ "$(wc -c <"$1")" -eq "$3" ] && [ -f "$1.before-squadala" ]' _ "$tmp/steam/userdata/7/config/shortcuts.vdf" "$kit" "$vsize"
    check 'start: it says which entries moved' bash -c 'grep -q "now starts start-squadala.sh" "$1"' _ "$tmp/start4.txt"
    : >"$log"
    bash "$kit/start-squadala.sh" --pack </dev/null >"$tmp/start5.txt" 2>&1; export CODE=$?
    check 'start --pack: the pack made again even when current, the game not started' bash -c '[ $CODE -eq 0 ] && grep -q "^puppet_pack" "$1" && ! grep -q "^started" "$1" && grep -q "the puppet pack is made" "$2"' _ "$log" "$tmp/start5.txt"
    chmod -w "$D"
    if [[ ! -w $D ]]; then
        bash "$kit/start-squadala.sh" --no-update </dev/null >"$tmp/start4.txt" 2>&1
        check 'start: a read-only kit folder is said' has "$tmp/start4.txt" 'read-only'
    fi
    chmod u+w "$D"
else
    echo "skip start.sh cases (not Linux: $(uname -s))"
fi

# --- update.sh -------------------------------------------------------------------------------
new=$tmp/rel/Squadala-v0.1.0-beta3-linux
mkdir -p "$new"
cp -a "$kit/start-squadala.sh" "$kit/README.txt" "$kit/revert.sh" "$kit/scripts" "$new/"
mkdir -p "$new/data/longshot/patches" "$new/data/server"
cp -a "$kit/data/longshot/." "$new/data/longshot/"
printf 'module v12\n' >"$new/data/longshot/hookshot_coop.so"
printf 'server\n' >"$new/data/server/coop_server"
{ cat "$release/kit/coop-options.ini"; printf '# A new switch.\nLICHII_COOP_NEW_THING=1\n'; } >"$new/data/coop-options.ini"
printf 'kit: Squadala-v0.1.0-beta3-linux (release v0.1.0-beta3)\n' >"$new/VERSION.txt"
(cd "$new" && { echo MANIFEST.txt; find . -type f ! -name MANIFEST.txt | sed 's|^\./||'; } | sort >MANIFEST.txt)
(cd "$tmp/rel" && "$py" -c 'import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
for d, _, fs in os.walk(sys.argv[2]):
    for f in fs: z.write(os.path.join(d, f))
z.close()' Squadala-v0.1.0-beta3-linux.zip Squadala-v0.1.0-beta3-linux)
(cd "$tmp/rel" && sha256sum Squadala-v0.1.0-beta3-linux.zip >Squadala-v0.1.0-beta3-linux.zip.sha256)
# From v0.11.3 a release has the kit under both names (the same bytes; oot-coop-... for the older kits' update.sh).
# Here the oot-coop- one is listed first and does not match its .sha256: the update must take the Squadala- one.
mkdir -p "$tmp/rel/old-name"
cp "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip" "$tmp/rel/old-name/oot-coop-v0.1.0-beta3-linux.zip"
(cd "$tmp/rel/old-name" && sha256sum oot-coop-v0.1.0-beta3-linux.zip >oot-coop-v0.1.0-beta3-linux.zip.sha256)
printf 'not the kit\n' >"$tmp/rel/oot-coop-v0.1.0-beta3-linux.zip"
printf '%064d  oot-coop-v0.1.0-beta3-linux.zip\n' 0 >"$tmp/rel/oot-coop-v0.1.0-beta3-linux.zip.sha256"
cat >"$tmp/rel/releases.json" <<EOF
[{"tag_name": "v0.1.0-beta3", "assets": [
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta3-linux.zip"},
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta3-linux.zip.sha256"},
  {"browser_download_url": "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip"},
  {"browser_download_url": "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip.sha256"},
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta3.zip"}]}]
EOF
# This kit as v0.11.3 had it: start-oot-coop.sh (a one-line call of start-squadala.sh) in it and in its MANIFEST.txt,
# and a menu entry that starts it; the update to the new layout removes it and moves the entry.
printf '#!/bin/sh\nhere=$(cd "$(dirname "$0")" && pwd)\nexec sh "$here/start-squadala.sh" "$@"\n' >"$kit/start-oot-coop.sh"
printf '[Desktop Entry]\nExec=bash "%s/start-oot-coop.sh"\n' "$kit" >"$XDG_DATA_HOME/applications/kit-old.desktop"
(cd "$kit" && { echo MANIFEST.txt; find . -path ./data/game -prune -o -path ./data/builder -prune -o -path ./data/work -prune -o -path ./data/logs -prune -o -type f ! -name MANIFEST.txt -print | sed 's|^\./||'; } | sort >MANIFEST.txt)
bash "$kit/scripts/update.sh" --tidy >/dev/null 2>&1
check 'update --tidy: a v0.11.3 kit keeps start-oot-coop.sh (its MANIFEST.txt lists it) and its entry' bash -c \
    '[ -f "$1/start-oot-coop.sh" ] && grep -qF "$1/start-oot-coop.sh" "$2"' _ "$kit" "$XDG_DATA_HOME/applications/kit-old.desktop"
printf 'my save\n' >"$D/game/save.bin"
sed -i 's/^auto_update = ask$/auto_update = yes/' "$D/coop-options.ini"
export OOT_COOP_UPDATE_API=$tmp/rel/releases.json
check 'update --status: the newer Linux zip is seen' [ "$(bash "$kit/scripts/update.sh" --status)" = 'newer v0.1.0-beta3' ]
bash "$kit/scripts/update.sh" --launch </dev/null >"$tmp/update.txt" 2>&1
check 'update --launch: done, exit 3 (start the new kit)' [ $? -eq 3 ]
check 'update: the new module and VERSION.txt are in' bash -c '[ "$(cat "$1/data/longshot/hookshot_coop.so")" = "module v12" ] && grep -q beta3 "$1/VERSION.txt"' _ "$kit"
check 'update: the built game and saves are kept' [ "$(cat "$D/game/save.bin")" = 'my save' ]
check "update: the player's settings stay; the new switch is added as a comment" bash -c 'grep -q "^auto_update = yes$" "$1" && grep -q "^# LICHII_COOP_NEW_THING=1$" "$1"' _ "$D/coop-options.ini"
check 'update: the old files are in backup-v0.1.0-beta2/' [ -f "$kit/backup-v0.1.0-beta2/data/longshot/hookshot_coop.so" ]
check 'update: the start file keeps working after a zip without exec bits' bash -n "$kit/start-squadala.sh"
check 'update: of the same release under both names, the Squadala- zip is taken' bash -c '! grep -qi "damaged\|does not match" "$1"' _ "$tmp/update.txt"
check 'update --status after it: nothing newer' [ -z "$(bash "$kit/scripts/update.sh" --status)" ]
check 'update: one start file, start-oot-coop.sh is gone (kept in previous/)' bash -c \
    '[ ! -e "$1/start-oot-coop.sh" ] && [ -f "$1/start-squadala.sh" ] && [ -f "$1/previous/v0.1.0-beta2/start-oot-coop.sh" ]' _ "$kit"
check 'update: the menu entry that started start-oot-coop.sh starts start-squadala.sh' grep -qF "$kit/start-squadala.sh" "$XDG_DATA_HOME/applications/kit-old.desktop"
# revert-b: the version replaced is kept in previous/, revert.sh puts it back, the start does not update straight back.
P=$kit/previous/v0.1.0-beta2
check 'update: the version it replaced is kept in previous/v0.1.0-beta2' bash -c '[ "$(cat "$1/data/longshot/hookshot_coop.so")" = module ] && grep -q beta2 "$1/VERSION.txt" && [ -f "$1/start-squadala.sh" ]' _ "$P"
check "update: nothing of the player's in it (game, saves, coop-options.ini)" bash -c '[ ! -e "$1/data/game" ] && [ ! -e "$1/data/coop-options.ini" ] && [ ! -e "$1/data/logs" ]' _ "$P"
check 'update: it says how to go back' grep -q 'bash revert.sh goes back to v0.1.0-beta2' "$tmp/update.txt"
bash "$kit/revert.sh" --yes </dev/null >"$tmp/revert.txt" 2>&1
check 'revert.sh: back on beta2' bash -c 'grep -q beta2 "$1/VERSION.txt" && [ "$(cat "$1/data/longshot/hookshot_coop.so")" = module ]' _ "$kit"
check 'revert.sh: its start-oot-coop.sh is back (that version had it)' bash -c 'grep -q "start-squadala.sh" "$1/start-oot-coop.sh"' _ "$kit"
check 'revert.sh: says every player needs the same version, tell your friends' bash -c 'grep -q "SAME version" "$1" && grep -q "Tell your friends" "$1"' _ "$tmp/revert.txt"
check 'revert.sh: saves and settings kept' bash -c '[ "$(cat "$1/game/save.bin")" = "my save" ] && grep -q "^auto_update = yes$" "$1/coop-options.ini"' _ "$D"
check 'revert.sh: stay on beta2, left beta3' bash -c 'grep -qx "stay=v0.1.0-beta2" "$1" && grep -qx "left=v0.1.0-beta3" "$1"' _ "$kit/stay-on-version.txt"
check 'revert.sh: beta3 is kept, to go forward again' grep -q beta3 "$kit/previous/v0.1.0-beta3/VERSION.txt"
export OOT_COOP_UPDATE_API=$tmp/rel/releases.json
check 'revert: the start check is quiet about beta3, the version left' [ -z "$(bash "$kit/scripts/update.sh" --status)" ]
bash "$kit/scripts/update.sh" --launch </dev/null >"$tmp/launch2.txt" 2>&1
check 'revert: auto_update = yes with no terminal stays (exit 0, still beta2)' bash -c '[ $2 -eq 0 ] && grep -q beta2 "$1/VERSION.txt" && grep -q Staying "$3"' _ "$kit" $? "$tmp/launch2.txt"
bash "$kit/scripts/update.sh" --yes </dev/null >"$tmp/forward.txt" 2>&1
check 'revert: update.sh goes forward to beta3 and forgets the stay' bash -c 'grep -q beta3 "$1/VERSION.txt" && [ ! -e "$1/stay-on-version.txt" ] && [ "$(cat "$1/data/longshot/hookshot_coop.so")" = "module v12" ]' _ "$kit"
check 'revert: the last 2 versions are kept' [ "$(ls -1 "$kit/previous" | wc -l)" -eq 2 ]
# --version: an older release by its tag (here beta2 again, zipped from what previous/ kept).
(cd "$kit/previous" && cp -a v0.1.0-beta2 "$tmp/rel/oot-coop-v0.1.0-beta2-linux" && cd "$tmp/rel" && "$py" -c 'import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
for d, _, fs in os.walk(sys.argv[2]):
    for f in fs: z.write(os.path.join(d, f))
z.close()' oot-coop-v0.1.0-beta2-linux.zip oot-coop-v0.1.0-beta2-linux && sha256sum oot-coop-v0.1.0-beta2-linux.zip >oot-coop-v0.1.0-beta2-linux.zip.sha256)
cat >"$tmp/rel/releases2.json" <<EOF
[{"tag_name": "v0.1.0-beta3", "assets": [
  {"browser_download_url": "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip"},
  {"browser_download_url": "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip.sha256"}]},
 {"tag_name": "v0.1.0-beta2", "assets": [
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta2-linux.zip"},
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta2-linux.zip.sha256"}]}]
EOF
export OOT_COOP_UPDATE_API=$tmp/rel/releases2.json
bash "$kit/scripts/update.sh" --yes --version v0.1.0-beta2 </dev/null >"$tmp/version.txt" 2>&1
check 'update --version: an older release by its tag, SHA-256 checked, stays on it' bash -c 'grep -q beta2 "$1/VERSION.txt" && grep -q "SHA-256 checked" "$2" && grep -qx "stay=v0.1.0-beta2" "$1/stay-on-version.txt"' _ "$kit" "$tmp/version.txt"
check 'update --version: no such release -> said' bash -c 'bash "$1/scripts/update.sh" --yes --version v0.0.9 </dev/null | grep -q "No release v0.0.9" && grep -q beta2 "$1/VERSION.txt"' _ "$kit"
rm -rf "$kit/previous" "$kit"/backup-*
# (a backup missing a file its MANIFEST.txt names, as an update that failed while copying leaves one: never gone back to)
mkdir -p "$kit/backup-v0.1.0-beta1"
cp "$kit/start-squadala.sh" "$kit/backup-v0.1.0-beta1/"
printf 'kit: oot-coop-v0.1.0-beta1-linux (release v0.1.0-beta1)\n' >"$kit/backup-v0.1.0-beta1/VERSION.txt"
printf 'MANIFEST.txt\nVERSION.txt\nstart-squadala.sh\nscripts/start.sh\n' >"$kit/backup-v0.1.0-beta1/MANIFEST.txt"
check 'revert.sh: nothing kept (a partial backup does not count) -> says --version' bash -c 'bash "$1/revert.sh" --yes </dev/null | grep -q -- "--version" && grep -q beta2 "$1/VERSION.txt"' _ "$kit"
rm -rf "$kit"/backup-*
# (a v0.11.2 to v0.11.4 kit's MANIFEST.txt names MANIFEST.txt.tmp, its build's temp file, which never shipped: that kit
# still counts as whole, so revert.sh finds it)
# (on a copy: the revert replaces the kit's files and must not touch the kit the later cases use)
lk=$tmp/legacy-kit
rm -rf "$lk"
cp -a "$kit" "$lk"
rm -rf "$lk"/backup-* "$lk/previous"
mkdir -p "$lk/backup-v0.1.0-beta1"
cp "$lk/start-squadala.sh" "$lk/backup-v0.1.0-beta1/"
printf 'kit: oot-coop-v0.1.0-beta1-linux (release v0.1.0-beta1)\n' >"$lk/backup-v0.1.0-beta1/VERSION.txt"
printf '# Files of x\nMANIFEST.txt\nMANIFEST.txt.tmp\nVERSION.txt\nstart-squadala.sh\n' >"$lk/backup-v0.1.0-beta1/MANIFEST.txt"
bash "$lk/revert.sh" --yes </dev/null >"$tmp/revert-legacy.txt" 2>&1
check 'revert.sh: a kept kit whose MANIFEST.txt lists MANIFEST.txt.tmp (v0.11.2 to v0.11.4) is gone back to' bash -c 'grep -q beta1 "$1/VERSION.txt" && ! grep -q "keeps no older" "$2"' _ "$lk" "$tmp/revert-legacy.txt"
rm -rf "$lk"
# A damaged or hand-edited VERSION.txt never names a folder: the update goes on, nothing outside previous/ is removed.
printf 'beside the kit\n' >"$tmp/beside-the-kit.txt"
for bad in .. ../..; do
    printf 'kit: oot-coop-x (release %s)\n' "$bad" >"$kit/VERSION.txt"
    bash "$kit/scripts/update.sh" --yes </dev/null >"$tmp/hostile.txt" 2>&1
    check "VERSION.txt '(release $bad)': the update goes on, the kit, its saves and the folder above are still there" bash -c \
        'grep -q beta3 "$1/VERSION.txt" && [ -f "$1/start-squadala.sh" ] && [ "$(cat "$1/data/game/save.bin")" = "my save" ] && [ -f "$2" ]' _ "$kit" "$tmp/beside-the-kit.txt"
    check "VERSION.txt '(release $bad)': nothing kept under that name" bash -c '[ ! -e "$1/previous" ] || [ -z "$(ls -A "$1/previous")" ]' _ "$kit"
done

# A v0.11.2 kit (its own update.sh, lib.sh and start-oot-coop.sh: tests/fixtures/linux-longshot-v0.11.2). That updater
# takes only oot-coop-<tag>-linux.zip and wants start-oot-coop.sh in the new kit: a release from v0.11.4 on (Squadala-
# names only) is not seen at all, so it updates to v0.11.3 (the last one with the old name, a kit with both start
# files: modelled here as beta3), whose update.sh then takes the newer one.
old="$tmp/old kit 0.11.2"
rm -rf "$old" && cp -a "$kit" "$old" && rm -rf "$old/previous" "$old"/backup-* "$old/start-squadala.sh" "$old/stay-on-version.txt"
fix=$release/tests/fixtures/linux-longshot-v0.11.2
cp "$fix/scripts/update.sh" "$fix/scripts/lib.sh" "$old/scripts/" && cp "$fix/start-oot-coop.sh" "$old/"
printf 'kit: oot-coop-v0.1.0-beta2-linux (release v0.1.0-beta2)\n' >"$old/VERSION.txt"
(cd "$old" && { echo MANIFEST.txt; find . -path ./data/game -prune -o -path ./data/builder -prune -o -path ./data/work -prune -o -path ./data/logs -prune -o -type f ! -name MANIFEST.txt -print | sed 's|^\./||'; } | sort >MANIFEST.txt)
cat >"$tmp/rel/releases-new-only.json" <<EOF
[{"tag_name": "v0.1.0-beta3", "assets": [
  {"browser_download_url": "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip"},
  {"browser_download_url": "$tmp/rel/Squadala-v0.1.0-beta3-linux.zip.sha256"}]}]
EOF
check 'v0.11.2 updater: a release with only the Squadala- names is not seen' \
    [ -z "$(OOT_COOP_UPDATE_API=$tmp/rel/releases-new-only.json bash "$old/scripts/update.sh" --status)" ]
# The v0.11.3 layout under the old name: the new kit plus start-oot-coop.sh, a one-line call of start-squadala.sh.
v3=$tmp/rel/v3/Squadala-v0.1.0-beta3-linux
mkdir -p "$(dirname "$v3")" && cp -a "$new" "$v3"
printf '#!/bin/sh\nhere=$(cd "$(dirname "$0")" && pwd)\nexec sh "$here/start-squadala.sh" "$@"\n' >"$v3/start-oot-coop.sh"
(cd "$v3" && { echo MANIFEST.txt; find . -type f ! -name MANIFEST.txt | sed 's|^\./||'; } | sort >MANIFEST.txt)
(cd "$tmp/rel/v3" && "$py" -c 'import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
for d, _, fs in os.walk(sys.argv[2]):
    for f in fs: z.write(os.path.join(d, f))
z.close()' oot-coop-v0.1.0-beta3-linux.zip Squadala-v0.1.0-beta3-linux && sha256sum oot-coop-v0.1.0-beta3-linux.zip >oot-coop-v0.1.0-beta3-linux.zip.sha256)
cat >"$tmp/rel/releases-old.json" <<EOF
[{"tag_name": "v0.1.0-beta3", "assets": [
  {"browser_download_url": "$tmp/rel/v3/oot-coop-v0.1.0-beta3-linux.zip"},
  {"browser_download_url": "$tmp/rel/v3/oot-coop-v0.1.0-beta3-linux.zip.sha256"}]}]
EOF
export OOT_COOP_UPDATE_API=$tmp/rel/releases-old.json
check 'v0.11.2 updater: sees the new release (its oot-coop- name)' [ "$(bash "$old/scripts/update.sh" --status)" = 'newer v0.1.0-beta3' ]
bash "$old/scripts/update.sh" --yes </dev/null >"$tmp/update-old.txt" 2>&1; export CODE=$?
check 'v0.11.2 updater: installs it' bash -c '[ $CODE -eq 0 ] && grep -q "Squadala-v0.1.0-beta3-linux (release v0.1.0-beta3)" "$1/VERSION.txt"' _ "$old"
check 'v0.11.2 updater: start-squadala.sh is in, start-oot-coop.sh is the new one-line call of it' bash -c \
    '[ -f "$1/start-squadala.sh" ] && grep -q "exec sh \"\$here/start-squadala.sh\"" "$1/start-oot-coop.sh"' _ "$old"
check 'v0.11.2 updater: the game, saves and settings are kept' bash -c \
    '[ "$(cat "$1/data/game/save.bin")" = "my save" ] && grep -q "^auto_update = yes$" "$1/data/coop-options.ini" && [ -n "$(ls "$1/data/game")" ]' _ "$old"
check 'v0.11.2 updater: the new updater is in (it knows the Squadala- name)' grep -q 'Squadala|oot-coop' "$old/scripts/update.sh"
check 'v0.11.2 updater: nothing left to update' [ -z "$(bash "$old/scripts/update.sh" --status)" ]
rm -rf "$old"
unset OOT_COOP_UPDATE_API

# --- collect-logs.sh -------------------------------------------------------------------------
printf 'coop: joined 192.168.1.20\npassword = hunter2\n' >"$D/game/longshot_log.txt"
mkdir -p "$D/game/mods/oot-remake-randomizer" && printf 'error: seed failed at 192.168.1.20\n' >"$D/game/mods/oot-remake-randomizer/last-build.txt"
bash "$kit/scripts/collect-logs.sh" >"$tmp/logs.txt" 2>&1
out=$(ls "$kit"/coop-logs-*.zip 2>/dev/null | head -n1)
check 'collect-logs: a .zip beside start-squadala.sh (the /bugreport bot takes a .zip only)' [ -n "$out" ]
if [[ -n $out ]]; then
    mkdir -p "$tmp/logs" && python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$out" "$tmp/logs"
    check 'collect-logs: the game log is in, its password and LAN address masked' bash -c 'grep -q "<lan-1>" "$1" && grep -q "(removed)" "$1" && ! grep -q hunter2 "$1"' _ "$tmp/logs/game/longshot_log.txt"
    check 'collect-logs: OOTRR'"'"'s last-build.txt is in, masked' bash -c 'grep -q "<lan-1>" "$1" && grep -q "seed failed" "$1"' _ "$tmp/logs/game/last-build.txt"
fi
check 'collect-logs: the last line names the file to send' bash -c '[ "$(tail -n1 "$1")" = "Send this file: $2" ]' _ "$tmp/logs.txt" "$out"
check 'collect-logs: it says to attach it to /bugreport on the Discord, no DM' bash -c 'grep -q "type /bugreport" "$1" && grep -q "discord.gg/GwtRKAqng5" "$1" && ! grep -qw "DM" "$1"' _ "$tmp/logs.txt"
check 'collect-logs: with the game log in, no help about finding it' bash -c '! grep -q "not in this file" "$1"' _ "$tmp/logs.txt"
# No game log: where to find it and how to add it; one copied into data/logs goes in.
rm -f "$D/game/longshot_log.txt" "$kit"/coop-logs-*.zip
bash "$kit/scripts/collect-logs.sh" >"$tmp/logs2.txt" 2>&1
check 'collect-logs, no game log: says so and how to add it' bash -c 'grep -q "longshot_log.txt) is not in this file" "$1" && grep -q "Copy longshot_log.txt into" "$1"' _ "$tmp/logs2.txt"
rm -f "$kit"/coop-logs-*.zip
printf 'coop: module loaded\n' >"$D/logs/longshot_log.txt"
bash "$kit/scripts/collect-logs.sh" >"$tmp/logs3.txt" 2>&1
out=$(ls "$kit"/coop-logs-*.zip 2>/dev/null | head -n1)
check 'collect-logs: a log copied into data/logs goes in' bash -c 'python3 -c "import sys, zipfile; print(*zipfile.ZipFile(sys.argv[1]).namelist(), sep=chr(10))" "$1" | grep -qx "game/copied/longshot_log.txt" && ! grep -q "not in this file" "$2"' _ "$out" "$tmp/logs3.txt"
rm -f "$D/logs/longshot_log.txt"

# --- The real make_kit_linux.sh: its MANIFEST.txt lists only files that are in the kit (Linux only: ELF stand-ins) ---
if [[ $(uname -s) == Linux ]]; then
    mk=$tmp/mk
    mkdir -p "$mk/build/_deps/enet-src" "$mk/build/_deps/fmt-src"
    echo MIT >"$mk/build/_deps/enet-src/LICENSE"
    echo MIT >"$mk/build/_deps/fmt-src/LICENSE"
    so=$(ls /lib/x86_64-linux-gnu/libc.so.6 /usr/lib/x86_64-linux-gnu/libm.so.6 2>/dev/null | head -n1)
    cp "$so" "$mk/hookshot_coop.so"
    cp "$(command -v ls)" "$mk/coop_server"
    bash "$release/make_kit_linux.sh" --module "$mk/hookshot_coop.so" --server "$mk/coop_server" --licenses "$mk/build"         --release v0.0.0-test --out "$mk/out" --no-zip >"$tmp/mk.txt" 2>&1 || tail -n5 "$tmp/mk.txt"
    mkit=$mk/out/Squadala-v0.0.0-test-linux
    check 'make_kit_linux.sh: a kit was built' [ -f "$mkit/MANIFEST.txt" ]
    check 'make_kit_linux.sh: MANIFEST.txt names no MANIFEST.txt.tmp' bash -c '! grep -q "MANIFEST.txt.tmp" "$1/MANIFEST.txt"' _ "$mkit"
    check 'make_kit_linux.sh: MANIFEST.txt lists MANIFEST.txt once and every file it names is in the kit' bash -c         '[ "$(grep -cx MANIFEST.txt "$1/MANIFEST.txt")" = 1 ] && ! grep -v "^#" "$1/MANIFEST.txt" | while IFS= read -r f; do [ -f "$1/$f" ] || echo "$f"; done | grep -q .' _ "$mkit"
    check 'make_kit_linux.sh: no temp file left in the kit' bash -c '[ -z "$(find "$1" -name "*.tmp")" ]' _ "$mkit"
else
    echo "skip make_kit_linux.sh cases (not Linux)"
fi

# --- The scripts parse ---------------------------------------------------------------------
for f in "$src/start-squadala.sh" "$src"/scripts/*.sh "$release/make_kit_linux.sh"; do
    check "bash -n ${f#"$repo"/}" bash -n "$f"
done

if ((failed)); then echo "$failed check(s) failed"; exit 1; fi
echo 'all checks passed'
