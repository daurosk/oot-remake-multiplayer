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
    f.write("make_game %s | %s | patched=%s\n" % (" ".join(a), env, os.path.exists("host/coop_test.c")))
game = a[a.index("--nup") + 1] if "--nup" in a else ""
if "--check" in a:
    print("@@stage check Checking")
    print("@@check %s" % ("bad wrong version" if "bad" in game else "ok"))
    sys.exit(1 if "bad" in game else 0)
out = a[a.index("--out") + 1]
os.makedirs(out, exist_ok=True)
for n in ("longshot", "image.bin", "longshot_vk.so"):
    with open(os.path.join(out, n), "w") as f:
        f.write("#!/bin/sh\necho started \"$OOT_COOP_KIT\" \"$LONGSHOT_NO_UPDATE_CHECK\" \"$(pwd)\" >> \"$FAKE_LOG\"\n" if n == "longshot" else n)
os.chmod(os.path.join(out, "longshot"), 0o755)
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
cp "$src/start-oot-coop.sh" "$src/README.txt" "$kit/"
cp "$src"/scripts/*.sh "$release/kit/scripts/apply_patch.py" "$kit/scripts/"
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
printf 'kit: oot-coop-v0.1.0-beta2-linux (release v0.1.0-beta2)\n' >"$kit/VERSION.txt"
export OOT_COOP_FETCH_LOCAL=1
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
check 'build: the puppet pack goes in data/game/mods/contents/<title>/coop-puppet' bash -c '[ -f "$1/game/mods/contents/0400486024DEC000/coop-puppet/coop_puppet.json" ] && grep -q "puppet_pack --romfs .*nup --mod-dir .* --title-rows" "$2"' _ "$D" "$log"
check 'build: the game file is remembered' has "$D/game-files.txt" "Downloads/OoT [04004860224DEC000].nup"

: >"$log"
bash "$kit/scripts/build.sh" </dev/null >"$tmp/build2.txt" 2>&1; export CODE=$?
check 'build: a second start builds nothing' bash -c '[ $CODE -eq 0 ] && grep -q "built already" "$1" && ! grep -q "^make_game" "$2" && grep -q "puppet pack: built already" "$1"' _ "$tmp/build2.txt" "$log"

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
    OOT_COOP_SECRET=1 bash "$kit/start-oot-coop.sh" --no-update </dev/null >"$tmp/start1.txt" 2>&1; export CODE=$?
    check 'start: builds if needed, then starts the game in data/game' bash -c '[ $CODE -eq 0 ] && grep -q "^started $2 1 $3$" "$1"' _ "$log" "$D" "$D/game"
    check 'start: without a terminal, what it says goes to data/logs/start.log' has "$D/logs/start.log" 'start without a terminal'
    check 'start: an applications menu entry for this kit (Steam: Add a Non-Steam Game)' bash -c 'grep -qF "Exec=bash \"$2/start-oot-coop.sh\"" "$1" && grep -q "^Categories=Game;" "$1"' _ "$XDG_DATA_HOME/applications/oot-coop.desktop" "$kit"
    check 'start: "sh start-oot-coop.sh" works too (POSIX sh start file)' sh -n "$kit/start-oot-coop.sh"
    bash "$kit/start-oot-coop.sh" --nope </dev/null >"$tmp/start2.txt" 2>&1; export CODE=$?
    check 'start: an unknown option is refused' bash -c '[ $CODE -ne 0 ] && grep -q "Unknown option" "$1"' _ "$tmp/start2.txt"
    bash "$kit/start-oot-coop.sh" --help </dev/null >"$tmp/start3.txt" 2>&1
    check 'start --help lists the options' has "$tmp/start3.txt" '--no-update'
    chmod -w "$D"
    if [[ ! -w $D ]]; then
        bash "$kit/start-oot-coop.sh" --no-update </dev/null >"$tmp/start4.txt" 2>&1
        check 'start: a read-only kit folder is said' has "$tmp/start4.txt" 'read-only'
    fi
    chmod u+w "$D"
else
    echo "skip start.sh cases (not Linux: $(uname -s))"
fi

# --- update.sh -------------------------------------------------------------------------------
new=$tmp/rel/oot-coop-v0.1.0-beta3-linux
mkdir -p "$new"
cp -a "$kit/start-oot-coop.sh" "$kit/README.txt" "$kit/scripts" "$new/"
mkdir -p "$new/data/longshot/patches" "$new/data/server"
cp -a "$kit/data/longshot/." "$new/data/longshot/"
printf 'module v12\n' >"$new/data/longshot/hookshot_coop.so"
printf 'server\n' >"$new/data/server/coop_server"
{ cat "$release/kit/coop-options.ini"; printf '# A new switch.\nLICHII_COOP_NEW_THING=1\n'; } >"$new/data/coop-options.ini"
printf 'kit: oot-coop-v0.1.0-beta3-linux (release v0.1.0-beta3)\n' >"$new/VERSION.txt"
(cd "$new" && { echo MANIFEST.txt; find . -type f ! -name MANIFEST.txt | sed 's|^\./||'; } | sort >MANIFEST.txt)
(cd "$tmp/rel" && "$py" -c 'import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
for d, _, fs in os.walk(sys.argv[2]):
    for f in fs: z.write(os.path.join(d, f))
z.close()' oot-coop-v0.1.0-beta3-linux.zip oot-coop-v0.1.0-beta3-linux)
(cd "$tmp/rel" && sha256sum oot-coop-v0.1.0-beta3-linux.zip >oot-coop-v0.1.0-beta3-linux.zip.sha256)
cat >"$tmp/rel/releases.json" <<EOF
[{"tag_name": "v0.1.0-beta3", "assets": [
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta3-linux.zip"},
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta3-linux.zip.sha256"},
  {"browser_download_url": "$tmp/rel/oot-coop-v0.1.0-beta3.zip"}]}]
EOF
(cd "$kit" && { echo MANIFEST.txt; find . -path ./data/game -prune -o -path ./data/builder -prune -o -path ./data/work -prune -o -path ./data/logs -prune -o -type f ! -name MANIFEST.txt -print | sed 's|^\./||'; } | sort >MANIFEST.txt)
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
check 'update: the start file keeps working after a zip without exec bits' bash -n "$kit/start-oot-coop.sh"
check 'update --status after it: nothing newer' [ -z "$(bash "$kit/scripts/update.sh" --status)" ]
unset OOT_COOP_UPDATE_API

# --- collect-logs.sh -------------------------------------------------------------------------
printf 'coop: joined 192.168.1.20\npassword = hunter2\n' >"$D/game/longshot_log.txt"
bash "$kit/scripts/collect-logs.sh" >"$tmp/logs.txt" 2>&1
out=$(ls "$kit"/coop-logs-*.tar.gz 2>/dev/null | head -n1)
check 'collect-logs: a .tar.gz beside start-oot-coop.sh' [ -n "$out" ]
if [[ -n $out ]]; then
    mkdir -p "$tmp/logs" && tar -xzf "$out" -C "$tmp/logs"
    check 'collect-logs: the game log is in, its password and LAN address masked' bash -c 'grep -q "<lan-1>" "$1" && grep -q "(removed)" "$1" && ! grep -q hunter2 "$1"' _ "$tmp/logs/game/longshot_log.txt"
fi

# --- The scripts parse ---------------------------------------------------------------------
for f in "$src/start-oot-coop.sh" "$src"/scripts/*.sh "$release/make_kit_linux.sh"; do
    check "bash -n ${f#"$repo"/}" bash -n "$f"
done

if ((failed)); then echo "$failed check(s) failed"; exit 1; fi
echo 'all checks passed'
