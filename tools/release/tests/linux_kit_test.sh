#!/usr/bin/env bash
# Tests for the Linux kit (tools/release/kit/linux, tools/release/make_kit.sh). Builds kits with
# a stub emulator (a script that notes where it was started) and a stub server, then runs the
# kit's scripts as a player would: play.sh, Host.sh, start-server.sh, collect-logs.sh, update.sh
# against a local release list. No network, no game.
#   bash tools/release/tests/linux_kit_test.sh
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
release=$(dirname "$here")
failed=0
check() {  # check "what" command...
    local what=$1
    shift
    if "$@"; then echo "ok   $what"; else echo "FAIL $what"; failed=$((failed + 1)); fi
}
has() { grep -qF -- "$2" "$1"; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-linux-test-XXXXXX")
trap 'pkill -f -- "$tmp" 2>/dev/null; [ -n "${KEEP_TMP-}" ] || rm -rf "$tmp"' EXIT
[ -n "${KEEP_TMP-}" ] && echo "tmp: $tmp"
. "$release/kit/linux/kit-lib.sh"
. "$release/kit/linux/hosting-lib.sh"

# --- coop.ini rules ---------------------------------------------------------------------
check 'a host, an IP and host:port are servers' bash -c '. "$1"; coop_test_server example.com && coop_test_server 192.168.1.20 && coop_test_server 10.0.0.1:27510' _ "$release/kit/linux/kit-lib.sh"
check 'a space, a bad port or nothing is no server' bash -c '. "$1"; ! coop_test_server "my pc" && ! coop_test_server a:70000 && ! coop_test_server ""' _ "$release/kit/linux/kit-lib.sh"
check 'names: 1 to 16 code points, no control characters' bash -c '. "$1"; coop_test_name Saria && coop_test_name "Ruto Zora" && coop_test_name "ÉÉÉÉÉÉÉÉÉÉÉÉÉÉÉÉ" && ! coop_test_name "" && ! coop_test_name 12345678901234567 && ! coop_test_name "$(printf "a\tb")"' _ "$release/kit/linux/kit-lib.sh"
coop_write_ini "$release/kit/coop.ini.template" "$tmp/coop.ini" 127.0.0.1 27510 Saria
coop_read_ini "$tmp/coop.ini"
check 'coop.ini is written and read back' [ "$COOP_SERVER|$COOP_PORT|$COOP_NAME" = '127.0.0.1|27510|Saria' ]
coop_write_ini "$release/kit/coop.ini.template" "$tmp/coop.ini" '' 27510 ''
check 'an empty server or name stays a comment' bash -c 'grep -q "^# server =$" "$1" && grep -q "^# name =$" "$1" && grep -q "^port = 27510$" "$1"' _ "$tmp/coop.ini"
printf 'auto_update = yes\nLICHII_COOP_PUPPET=1\n# EDEN_NX2_GPL=1\nEDEN_NX2_GPL = 1\nEDEN_OTHER=1\n' >"$tmp/options.ini"
check 'coop-options.ini: LICHII_COOP_ and EDEN_NX2_ lines only' [ "$(kit_read_options "$tmp/options.ini" 2>/dev/null | tr '\n' ,)" = 'LICHII_COOP_PUPPET=1,EDEN_NX2_GPL=1,' ]
check 'auto_update is read' [ "$(kit_auto_update "$tmp/options.ini")" = yes ]
printf 'pvp = off\nupnp = no\nserver_password = abc\nLICHII_COOP_PUPPET=1\n' >"$tmp/options2.ini"
check "the kit's own lines (pvp, upnp, server_password) are no switches and no warning" [ "$(kit_read_options "$tmp/options2.ini" 2>&1 | tr '\n' ,)" = 'LICHII_COOP_PUPPET=1,' ]
coop_write_ini "$release/kit/coop.ini.template" "$tmp/coop.ini" 127.0.0.1 27510 Saria hunter2
coop_read_ini "$tmp/coop.ini"
check 'coop.ini keeps the join password' [ "$COOP_PASSWORD" = hunter2 ]
coop_write_ini "$release/kit/coop.ini.template" "$tmp/coop.ini" 127.0.0.1 27510 Saria
check 'no join password: the line stays a comment' bash -c 'grep -q "^# password =$" "$1" && ! grep -q @PASSWORD@ "$1"' _ "$tmp/coop.ini"

# --- Hosting: invite codes, passwords, the host's lines ------------------------------------------
# The server's own vectors (tests/server/nat_test.cpp), as tools/release/tests/hosting_test.ps1.
lib=$release/kit/linux/hosting-lib.sh
good='CHFSA-6JFPP-5AATB-UCVDW-EXFYG-ZH2J3-KSGWK-S2GA'
kit_invite_decode "$good"
check 'invite: a made password' [ "$INV_ADDRESS|$INV_PORT|$INV_PASSWORD" = '203.0.113.5|27510|ABCDEFGHJKLMNPQRSTUV' ]
kit_invite_decode ' cmddg 3acpp 5be7d jqkuy lmmun fzgj5 5pfx5 y86ve qrwxf efr. '
check "invite: the host's own password, pasted loosely" [ "$INV_ADDRESS|$INV_PASSWORD" = '198.51.100.2|three-random-words' ]
all=1
letters=$(kit_invite_letters "$good")
for ((k = 0; k < ${#letters}; k++)); do
    c=${letters:k:1}
    if [[ $c == A ]]; then r=B; else r=A; fi
    kit_invite_decode "${letters:0:k}$r${letters:k+1}" && all=0
done
check 'invite: every mistyped letter is caught' [ "$all" = 1 ]
check 'invite: a missing letter is caught' bash -c '. "$1"; ! kit_invite_decode "${2%?}"' _ "$lib" "$good"
kit_invite_decode 'EHDDG-3ACVT-ASCAS-DASCS-NB2JP-PE7EA-GJEUQ-WC2NA-MLPZ3-JS'
check 'invite: an answer code is named as one' bash -c '[[ $1 == *ANSWER* ]]' _ "$INV_ERROR"
check 'invite: told apart from relay codes and addresses' bash -c '. "$1"; ! kit_is_invite ABCD-EFGH && ! kit_is_invite 203.0.113.5 && kit_is_invite "$2"' _ "$lib" "$good"
p1=$(kit_new_password)
p2=$(kit_new_password)
check 'a made password is 20 letters and new each time' bash -c '[[ $1 =~ ^[A-HJ-NP-Z2-9]{20}$ && $1 != "$2" ]]' _ "$p1" "$p2"
check 'join passwords: no spaces, at most 128 bytes' bash -c '. "$1"; coop_test_password abc && ! coop_test_password "a b" && ! coop_test_password "" && ! coop_test_password "$(printf "%0129d" 0)"' _ "$lib"
check 'a pasted address is made plain' [ "$(kit_join_address ' udp://26.1.2.3:27511/ ')|$(kit_join_address localhost.)" = '26.1.2.3 27511|127.0.0.1 ' ]
check 'a typo in an address is caught' bash -c '. "$1"; ! kit_join_address 26.1.2 && ! kit_join_address 1.2.3.400 && ! kit_join_address a:0' _ "$lib"
printf '# upnp = no\n# invite = no\n' >"$tmp/access.ini"
kit_server_access "$tmp/access.ini"
check 'defaults: upnp and invite on, no password line' [ "$KA_UPNP$KA_INVITE$KA_PASSWORD_SET" = 110 ]
kit_hosting_args secret
check 'upnp and invite: on with a join password' [ "${HOSTING_ARGS[*]}" = '--upnp --invite' ]
kit_hosting_args ''
check 'upnp and invite: refused without a join password' bash -c '[ "$1" = 0 ] && [[ $2 == *"NOT used"* && $2 == *"No invite code"* ]]' _ "${#HOSTING_ARGS[@]}" "${HOSTING_NOTES[*]}"
printf 'upnp = no\ninvite = no\nstun_servers = a.example:1, bad host;x\nserver_password =\n' >"$tmp/access.ini"
kit_server_access "$tmp/access.ini"
check 'upnp = no, invite = no and an empty server_password are read' [ "$KA_UPNP$KA_INVITE$KA_PASSWORD_SET|$KA_PASSWORD|$KA_STUN" = '001||a.example:1, bad host;x' ]
KA_INVITE=1
kit_hosting_args secret
check 'stun servers are checked' bash -c '[ "$1" = "--invite --stun a.example:1" ] && [[ $2 == *"not a name:port"* ]]' _ "${HOSTING_ARGS[*]}" "${HOSTING_NOTES[*]}"
printf 'x\ncoop: on, server 127.0.0.1:27510\n' >"$tmp/eden.txt"
check 'join state: trying' [ "$(kit_join_state "$tmp/eden.txt")" = trying ]
echo 'coop: server refused: wrong join password' >>"$tmp/eden.txt"
check 'join state: refused, with why' [ "$(kit_join_state "$tmp/eden.txt")" = 'refused: wrong join password' ]
echo 'coop: welcomed as player 2' >>"$tmp/eden.txt"
check 'join state: welcomed' [ "$(kit_join_state "$tmp/eden.txt")" = welcomed ]
check 'versions: beta2 < beta10 < release, numbers win' bash -c '. "$1"; [ "$(kit_version_compare v0.1.0-beta2 v0.1.0-beta10)" = -1 ] && [ "$(kit_version_compare v0.1.0 v0.1.0-beta9)" = 1 ] && [ "$(kit_version_compare v0.2.0-beta1 v0.1.9)" = 1 ] && [ "$(kit_version_compare v0.1.0-beta7 v0.1.0-beta7)" = 0 ] && [ "$(kit_version_compare junk v0.0.1)" = -1 ]' _ "$release/kit/linux/kit-lib.sh"

# --- Mods (issue 136) -----------------------------------------------------------------------
u=$tmp/mods/emulator/user
g=$u/load/0400486024DEC000
mkdir -p "$u/config"
mods() { kit_mods "$u" | sed 's|.*/||' | LC_ALL=C sort | tr '\n' ,; }
mkdir -p "$g/coop-puppet/romfs" && echo x >"$g/coop-puppet/romfs/a"
mkdir -p "$g/Water Temple fix/romfs" && echo x >"$g/Water Temple fix/romfs/b"
mkdir -p "$g/Empty/exefs"
check 'the puppet pack, a texture mod and an empty folder are no code mods' [ "$(mods)" = '' ]
mkdir -p "$g/Hyrule-Cheat-Menu/exefs" && echo x >"$g/Hyrule-Cheat-Menu/exefs/subsdk8"
mkdir -p "$g/Cheats only/cheats" && echo x >"$g/Cheats only/cheats/0123.txt"
echo '[x]' >"$g/cheat_hearts.txt"
mkdir -p "$g/Both/ExeFS" "$g/Both/romfs" && echo x >"$g/Both/ExeFS/main.ips" && echo x >"$g/Both/romfs/c"
check 'exefs (any case), cheats folders and cheat_* files count' [ "$(mods)" = 'Both,Cheats only,Hyrule-Cheat-Menu,cheat_hearts.txt,' ]
printf '[DisabledAddOns]\r\nsize=2\r\n1\\title_id=288309953924349952\r\n1\\disabled\\1\\d=Hyrule-Cheat-Menu\r\n1\\disabled\\2\\d=cheat_hearts.txt\r\n1\\disabled\\size=2\r\n2\\title_id=12345\r\n2\\disabled\\1\\d=Both\r\n' >"$u/config/qt-config.ini"
check 'mods turned off in the game'"'"'s Add-Ons do not count, another game'"'"'s list does not matter' [ "$(mods)" = 'Both,Cheats only,' ]
rm -rf "$g/Both" "$g/Cheats only" "$g/Hyrule-Cheat-Menu" "$g/cheat_hearts.txt" "$u/config/qt-config.ini"
# Dark's Dynamic FPS: an IPS32 for this build over its call and routine; Fritchii uses its built-in.
ips32() {  # ips32 FILE OFFSET...
    local f=$1 o
    shift
    mkdir -p "$(dirname "$f")"
    { printf 'IPS32'
      for o in "$@"; do printf "\\x$(printf %02x $(((o >> 24) & 255)))\\x$(printf %02x $(((o >> 16) & 255)))\\x$(printf %02x $(((o >> 8) & 255)))\\x$(printf %02x $((o & 255)))\\x00\\x04\\x1f\\x20\\x03\\xd5"; done
      printf 'EEOF'; } >"$f"
}
ips32 "$g/DynamicFPS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips" $((0x6e6dc4)) $((0x3c46300))
check 'an IPS32 file'"'"'s write ranges are read' [ "$(kit_ips_ranges "$g/DynamicFPS/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips" | tr '\n' ,)" = "$((0x6e6dc4)) $((0x6e6dc8)),$((0x3c46300)) $((0x3c46304))," ]
mkdir -p "$g/DynTxt/exefs"
printf '@nsobid-E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348\n@flag offset_shift 0x100\n@enabled\n// the call\n006E6CC4 4F80D594\n@stop\n' >"$g/DynTxt/exefs/dyn.pchtxt"
check 'Dark'"'"'s Dynamic FPS (IPS32 or IPSwitch) is skipped' [ "$(mods)" = '' ]
ips32 "$g/Other/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips" $((0x10000))
check 'a patch elsewhere is a code mod' [ "$(mods)" = 'Other,' ]
printf '[DisabledAddOns]\nsize=1\n1\\title_id=288309953924349952\n1\\disabled\\1\\d=Dynamic FPS by Dark (built-in)\n' >"$u/config/qt-config.ini"
check 'with the built-in turned off, Dark'"'"'s mods count' [ "$(mods)" = 'DynTxt,DynamicFPS,Other,' ]
rm -rf "$g/Other" "$g/DynamicFPS" "$g/DynTxt"
sd=$tmp/mods/sd
mkdir -p "$sd/atmosphere/contents/0400486024DEC000/romfs" && echo x >"$sd/atmosphere/contents/0400486024DEC000/romfs/x"
printf '[Data%%20Storage]\nsdmc_directory=%s/\n' "$sd" >"$u/config/qt-config.ini"
check 'game files on the SD card are fine' [ "$(mods)" = '' ]
mkdir -p "$sd/atmosphere/contents/0400486024DEC000/exefs" && echo x >"$sd/atmosphere/contents/0400486024DEC000/exefs/subsdk9"
check 'code on the SD card counts' [ "$(mods)" = '0400486024DEC000,' ]

# --- A copied kit's folders -------------------------------------------------------------
mkdir -p "$tmp/copy/emulator/user/config"
printf '[Data%%20Storage]\nload_directory=/home/a/first-kit/emulator/user/load/\nsdmc_directory=/media/sd/\nnand_directory=/home/a/first-kit/emulator/user/nand/\n' >"$tmp/copy/emulator/user/config/qt-config.ini"
fixed=$(kit_repair_paths "$tmp/copy/emulator/user" | tr '\n' ,)
check 'a copied kit: its own load and nand folders, the chosen SD left alone' bash -c '[ "$1" = "load_directory,nand_directory," ] && grep -qxF "load_directory=$2/load/" "$3" && grep -qxF "sdmc_directory=/media/sd/" "$3"' _ "$fixed" "$tmp/copy/emulator/user" "$tmp/copy/emulator/user/config/qt-config.ini"

# --- make_kit.sh ----------------------------------------------------------------------------
stub=$tmp/stub
mkdir -p "$stub"
cat >"$stub/lichii.AppImage" <<'EOF'
#!/usr/bin/env bash
pwd >started.txt
env | grep -E '^(LICHII_COOP|EDEN_NX2)_' | sort >>started.txt
EOF
cat >"$stub/coop_server" <<'EOF'
#!/usr/bin/env bash
echo "coop_server on port $2"
echo "args: $*"
echo "password env: ${OOT_COOP_SERVER_PASSWORD:+server-set} ${OOT_COOP_JOIN_PASSWORD:+join-$([ "$OOT_COOP_JOIN_PASSWORD" = ABCDEFGHJKLMNPQRSTUV ] && echo right || echo wrong)}"
case " $* " in *" --invite "*) echo "coop_server: invite code: CHFSA-6JFPP-5AATB-UCVDW-EXFYG-ZH2J3-KSGWK-S2GA" ;; esac
case " $* " in *" --join "*) echo "join: answer code: EHDDG-3ACVT-ASCAS" ;; esac
echo "refused a connection, reason 1"
sleep "${STUB_SERVER_SLEEP:-0}"
exit 0
EOF
chmod +x "$stub/lichii.AppImage" "$stub/coop_server"
# A stand-in for the emulator's source: only its licence texts.
mkdir -p "$stub/emu/LICENSES"
for f in LICENSE.txt LICENSES/LGPL-3.0-or-later.txt LICENSES/GPL-3.0-or-later.txt; do echo "licence $f" >"$stub/emu/$f"; done
mk() { bash "$release/make_kit.sh" --appimage "$stub/lichii.AppImage" --server "$stub/coop_server" --out "$tmp/out" --emu-source "$stub/emu" "$@" >/dev/null 2>"$tmp/mk.err"; }
check 'make_kit.sh builds v0.1.0-beta6' mk --release v0.1.0-beta6
mkdir -p "$tmp/applic" && printf 'usr/lib/libopus.so.0  Ubuntu package libopus0\n' >"$tmp/applic/BUNDLED.txt" && echo 'opus copyright' >"$tmp/applic/libopus0.copyright"
check 'make_kit.sh --appimage-licenses puts the AppImage list and texts in licenses/appimage/' bash -c 'bash "$1/make_kit.sh" --appimage "$2/lichii.AppImage" --server "$2/coop_server" --out "$3/out-lic" --release v0.1.0-beta6 --no-tar --emu-source "$2/emu" --appimage-licenses "$3/applic" >/dev/null 2>&1 && [ -f "$3/out-lic/oot-coop-v0.1.0-beta6-linux/licenses/appimage/BUNDLED.txt" ] && [ -f "$3/out-lic/oot-coop-v0.1.0-beta6-linux/licenses/appimage/libopus0.copyright" ]' _ "$release" "$stub" "$tmp"
check 'make_kit.sh refuses a licences folder without BUNDLED.txt' bash -c '! bash "$1/make_kit.sh" --appimage "$2/lichii.AppImage" --server "$2/coop_server" --out "$3/out-lic2" --no-tar --emu-source "$2/emu" --appimage-licenses "$3" >/dev/null 2>&1' _ "$release" "$stub" "$tmp"
check 'make_kit.sh refuses a kit without the LGPL text (no --emu-source)' bash -c '! bash "$1/make_kit.sh" --appimage "$2/lichii.AppImage" --server "$2/coop_server" --out "$3/out-lic3" --no-tar >/dev/null 2>&1' _ "$release" "$stub" "$tmp"
kit6=$tmp/out/oot-coop-v0.1.0-beta6-linux
arch=$(uname -m)
check 'the kit has its scripts, emulator, server, tool and docs' bash -c 'cd "$1" && for f in Host.sh Join.sh play.sh start-server.sh collect-logs.sh update.sh make-puppet-pack.sh scripts/kit-lib.sh scripts/coop.ini.template emulator/lichii.AppImage server/coop_server tools/puppet_pack/build.py SETUP.md KNOWN-ISSUES.md SOURCE.txt VERSION.txt MANIFEST.txt coop-options.ini emulator/user/config/coop.ini LICENSE.txt; do [ -f "$f" ] || { echo "missing $f"; exit 1; }; done; [ -x play.sh ] && [ -x emulator/lichii.AppImage ] && [ -d emulator/user/keys ]' _ "$kit6"
check 'VERSION.txt names the release' has "$kit6/VERSION.txt" 'kit: oot-coop-v0.1.0-beta6-linux (release v0.1.0-beta6)'
check 'the port is filled in, no @PORT@ left' bash -c '! grep -rl "@PORT@" "$1" --include=*.sh && grep -q "^port = 27510$" "$1/emulator/user/config/coop.ini" && grep -q "^port = @PORT@$" "$1/scripts/coop.ini.template"' _ "$kit6"
check 'MANIFEST.txt lists the kit'"'"'s files, never user/' bash -c 'grep -qx play.sh "$1/MANIFEST.txt" && grep -qx emulator/lichii.AppImage "$1/MANIFEST.txt" && ! grep -q "^emulator/user" "$1/MANIFEST.txt"' _ "$kit6"
check 'the tarball and its sha256 match' bash -c 'cd "$1" && sha256sum -c "oot-coop-v0.1.0-beta6-linux-$2.tar.gz.sha256" >/dev/null' _ "$tmp/out" "$arch"

# --- play.sh -----------------------------------------------------------------------------------
waitfor() { for _ in $(seq 50); do [[ -f $1 ]] && return 0; sleep 0.1; done; return 1; }
(cd "$kit6" && bash ./play.sh --no-update --server 192.168.1.20 --name Saria </dev/null >"$tmp/play.out" 2>&1)
check 'play.sh writes coop.ini' bash -c 'grep -qx "server = 192.168.1.20" "$1" && grep -qx "name = Saria" "$1"' _ "$kit6/emulator/user/config/coop.ini"
check 'play.sh starts the emulator in emulator/ (its user/ folder is the portable one)' bash -c 'for i in $(seq 50); do [ -f "$1/started.txt" ] && break; sleep 0.1; done; [ "$(head -n1 "$1/started.txt")" = "$(cd "$1" && pwd)" ]' _ "$kit6/emulator"
check 'switches from coop-options.ini reach the emulator' has "$kit6/emulator/started.txt" 'LICHII_COOP_'
check 'no puppet pack: said, and the puppet switch is off' bash -c 'grep -q "Puppet pack: not made yet" "$1" && ! grep -q "^LICHII_COOP_PUPPET=" "$2"' _ "$tmp/play.out" "$kit6/emulator/started.txt"
rm -f "$kit6/emulator/started.txt"
mkdir -p "$kit6/emulator/user/load/0400486024DEC000/Cheat-Menu/exefs" && echo x >"$kit6/emulator/user/load/0400486024DEC000/Cheat-Menu/exefs/subsdk8"
(cd "$kit6" && bash ./play.sh --no-update --server 192.168.1.20 --name Saria </dev/null >"$tmp/play2.out" 2>&1)
code=$?
sleep 0.5
check 'a code mod: play.sh stops and names it, no emulator' bash -c '[ "$1" != 0 ] && grep -q "Code and cheat mods are not supported in co-op; texture and model mods are fine." "$2" && grep -q "Cheat-Menu" "$2" && [ ! -f "$3" ]' _ "$code" "$tmp/play2.out" "$kit6/emulator/started.txt"
rm -rf "$kit6/emulator/user/load/0400486024DEC000/Cheat-Menu"
(cd "$kit6" && bash ./play.sh --no-update --server 'my pc' --name Saria </dev/null >"$tmp/play3.out" 2>&1)
check 'a bad address is refused' has "$tmp/play3.out" 'Bad server address: my pc'

# --- start-server.sh -------------------------------------------------------------------------
(cd "$kit6" && bash ./start-server.sh 27600 >"$tmp/server.out" 2>&1)
log=$(ls "$kit6"/logs/server-*.log 2>/dev/null | head -n1)
check 'start-server.sh runs the server on the port and logs it' bash -c '[ -n "$1" ] && grep -q "coop_server on port 27600" "$1" && grep -q "server exited with code 0" "$1"' _ "$log"
check 'a refused older game is told which release to update to' has "$log" 'every player needs v0.1.0-beta6'
check 'no server_password line: a made join password, UPnP and the invite code' bash -c 'grep -q "args: --port 27600 --password-env OOT_COOP_SERVER_PASSWORD --upnp --invite" "$1" && grep -q "password env: server-set" "$1" && grep -q "Join password for this session: [A-HJ-NP-Z2-9]\{20\}" "$2"' _ "$log" "$tmp/server.out"
check 'the invite code is shown, never in the log' bash -c 'grep -q "invite code: CHFSA" "$2" && ! grep -q CHFSA "$1" && grep -q "not in the log" "$1"' _ "$log" "$tmp/server.out"
check 'the invite code file goes when the server stops' [ ! -e "$kit6/logs/invite-code.txt" ]
rm -f "$kit6"/logs/server-*.log
cp "$kit6/coop-options.ini" "$tmp/options.keep"
echo 'server_password =' >>"$kit6/coop-options.ini"
(cd "$kit6" && bash ./start-server.sh 27600 >"$tmp/server2.out" 2>&1)
log=$(ls "$kit6"/logs/server-*.log 2>/dev/null | head -n1)
check 'server_password = (empty): no password, no UPnP, no invite code, said why' bash -c 'grep -q "args: --port 27600 *$" "$1" && grep -q "UPnP) is NOT used" "$2" && grep -q "No invite code" "$2"' _ "$log" "$tmp/server2.out"
cp "$tmp/options.keep" "$kit6/coop-options.ini"
rm -f "$kit6"/logs/server-*.log

# --- Host.sh and Join.sh with an invite code ---------------------------------------------------
(cd "$kit6" && STUB_SERVER_SLEEP=4 bash ./Host.sh --no-update --name Saria </dev/null >"$tmp/host.out" 2>&1)
check 'Host.sh: a new join password, the server in the background, the invite code shown' bash -c 'grep -q "a new random one for this session" "$1" && grep -q "INVITE CODE" "$1" && grep -q "^  CHFSA-6JFPP-5AATB-UCVDW-EXFYG-ZH2J3-KSGWK-S2GA$" "$1" && grep -q "start-server.sh --answer" "$1"' _ "$tmp/host.out"
check "Host.sh: the host's game uses the same join password" bash -c 'grep -q "^password = [A-HJ-NP-Z2-9]\{20\}$" "$1" && grep -qx "server = 127.0.0.1" "$1"' _ "$kit6/emulator/user/config/coop.ini"
check 'Host.sh: the answer-code pipe is there while the server runs' [ -p "$kit6/logs/.server-input" ]
# pgrep and file modes are Linux's; Git Bash on Windows has neither, so these two run in CI.
linux=0
[[ $(uname -s) == Linux ]] && linux=1
((linux)) && check './start-server.sh --answer reaches the background server' bash -c 'cd "$1" && bash ./start-server.sh --answer EHDDG-ABCDE | grep -q "Given to the server"' _ "$kit6"
((linux)) && check 'the console lines (with the password) are only for this user' bash -c '[ "$(stat -c %a "$1")" = 600 ]' _ "$kit6/logs/server-console.txt"
sleep 5
check 'the server stopped: console file, invite file and pipe are gone' bash -c '[ ! -e "$1/server-console.txt" ] && [ ! -e "$1/invite-code.txt" ] && [ ! -e "$1/.server-input" ]' _ "$kit6/logs"
rm -f "$kit6/emulator/started.txt"
(cd "$kit6" && STUB_SERVER_SLEEP=2 bash ./Join.sh --no-update --name Link --server "$good" </dev/null >"$tmp/join.out" 2>&1)
jlog=$(ls "$kit6"/logs/join-*.log 2>/dev/null | head -n1)
check 'Join.sh with an invite code: the game goes to the join helper with the password' bash -c 'grep -qx "server = 127.0.0.1" "$1" && grep -qx "port = 27512" "$1" && grep -qx "password = ABCDEFGHJKLMNPQRSTUV" "$1"' _ "$kit6/emulator/user/config/coop.ini"
check 'Join.sh: the join helper gets the host, and the password in its environment only' bash -c '[ -n "$1" ] && grep -q "args: --join 203.0.113.5:27510 --game-port 27512 --password-env OOT_COOP_JOIN_PASSWORD" "$1" && grep -q "join-right" "$1" && ! grep -q ABCDEFGHJKLMNPQRSTUV "$1" && ! grep "args:" "$1" | grep -q ABCDEFGHJKLMNPQRSTUV' _ "$jlog"
check 'Join.sh: the emulator starts' waitfor "$kit6/emulator/started.txt"
(cd "$kit6" && bash ./Join.sh --no-update --name Link --server 'EHDDG-3ACVT-ASCAS-DASCS-NB2JP-PE7EA-GJEUQ-WC2NA-MLPZ3-JS' </dev/null >"$tmp/join2.out" 2>&1)
check 'Join.sh with an answer code: says where it goes' has "$tmp/join2.out" 'ANSWER code'
sleep 3

# --- collect-logs.sh -------------------------------------------------------------------------
echo 'a log line from 192.168.1.20 and 203.0.113.5' >"$kit6/emulator/user/log/eden_log.txt"
cp "$kit6/coop-options.ini" "$tmp/options.keep"
echo 'relay_key = s3cret-key' >>"$kit6/coop-options.ini"
(cd "$kit6" && bash ./collect-logs.sh >"$tmp/collect.out" 2>&1)
packed=$(ls "$kit6"/coop-logs-*.tar.gz 2>/dev/null | head -n1)
check 'collect-logs.sh packs the logs, coop.ini and a note about this PC' bash -c '[ -n "$1" ] && tar -tzf "$1" | grep -q "emulator/log/eden_log.txt" && tar -tzf "$1" | grep -q "emulator/coop.ini" && tar -tzf "$1" | grep -q about-this-pc.txt' _ "$packed"
check 'collect-logs.sh masks the join password, everywhere, and the addresses' bash -c 'tar -xzOf "$1" ./emulator/coop.ini | grep -qx "password = (removed)" && tar -xzOf "$1" ./coop-options.ini | grep -qx "relay_key = (removed)" && [[ $1 != *"$(hostname)"* ]] && ! tar -xzOf "$1" | grep -q -e ABCDEFGHJKLMNPQRSTUV -e 203.0.113.5 && tar -xzOf "$1" ./emulator/log/eden_log.txt | grep -q "<lan-1> and <public-1>"' _ "$packed"
cp "$tmp/options.keep" "$kit6/coop-options.ini"

# --- update.sh ---------------------------------------------------------------------------------
echo 'my keys' >"$kit6/emulator/user/keys/prod.keys"
mkdir -p "$kit6/emulator/user/cache/shader" && echo old >"$kit6/emulator/user/cache/shader/a.bin"
sed -i 's/^#*[[:space:]]*auto_update.*/auto_update = no/' "$kit6/coop-options.ini"
echo 'v7 emulator' >>"$stub/lichii.AppImage"
check 'make_kit.sh builds v0.1.0-beta7' mk --release v0.1.0-beta7
printf '\n# A new switch in beta7.\n# LICHII_COOP_NEW_THING=1\n' >>"$tmp/out/oot-coop-v0.1.0-beta7-linux/coop-options.ini"
# (the new switch must be in the tarball too: rebuild it from the folder)
(cd "$tmp/out" && tar -czf "oot-coop-v0.1.0-beta7-linux-$arch.tar.gz" oot-coop-v0.1.0-beta7-linux && sha256sum "oot-coop-v0.1.0-beta7-linux-$arch.tar.gz" >"oot-coop-v0.1.0-beta7-linux-$arch.tar.gz.sha256")
cat >"$tmp/releases.json" <<EOF
[{"tag_name": "v0.1.0-beta7", "draft": false, "assets": [
  {"name": "oot-coop-v0.1.0-beta7-linux-$arch.tar.gz", "browser_download_url": "$tmp/out/oot-coop-v0.1.0-beta7-linux-$arch.tar.gz"},
  {"name": "oot-coop-v0.1.0-beta7-linux-$arch.tar.gz.sha256", "browser_download_url": "$tmp/out/oot-coop-v0.1.0-beta7-linux-$arch.tar.gz.sha256"},
  {"name": "oot-coop-v0.1.0-beta7.zip", "browser_download_url": "$tmp/out/windows.zip"}]},
 {"tag_name": "v0.1.0-beta6", "draft": false, "assets": [
  {"name": "oot-coop-v0.1.0-beta6-linux-$arch.tar.gz", "browser_download_url": "$tmp/out/oot-coop-v0.1.0-beta6-linux-$arch.tar.gz"},
  {"name": "oot-coop-v0.1.0-beta6-linux-$arch.tar.gz.sha256", "browser_download_url": "$tmp/out/oot-coop-v0.1.0-beta6-linux-$arch.tar.gz.sha256"}]}]
EOF
export OOT_COOP_UPDATE_API=$tmp/releases.json
check 'update.sh --status names the newer release' [ "$(bash "$kit6/update.sh" --status)" = 'newer v0.1.0-beta7' ]
(cd "$kit6" && bash ./play.sh --server 127.0.0.1 --name Saria </dev/null >"$tmp/play4.out" 2>&1)
check 'auto_update = no: play.sh says one is out and starts this kit' bash -c 'grep -q "Co-op v0.1.0-beta7 is out. Close the games and run ./update.sh" "$1" && grep -q "Emulator started" "$1"' _ "$tmp/play4.out"
sleep 0.5
bash "$kit6/update.sh" --yes >"$tmp/update.out" 2>&1
check 'update.sh --yes updates the kit' bash -c 'head -n1 "$1/VERSION.txt" | grep -q "release v0.1.0-beta7" && cmp -s "$1/emulator/lichii.AppImage" "$2"' _ "$kit6" "$stub/lichii.AppImage"
check 'the keys, coop.ini and logs are kept' bash -c '[ "$(cat "$1/emulator/user/keys/prod.keys")" = "my keys" ] && grep -qx "name = Saria" "$1/emulator/user/config/coop.ini" && ls "$1"/logs/server-*.log >/dev/null' _ "$kit6"
check 'coop-options.ini keeps auto_update = no and gains the new switch behind #' bash -c 'grep -qx "auto_update = no" "$1" && grep -qx "# LICHII_COOP_NEW_THING=1" "$1"' _ "$kit6/coop-options.ini"
check 'a new emulator: the shader cache went to the backup' bash -c '[ ! -e "$1/emulator/user/cache/shader" ] && [ -f "$1/backup-v0.1.0-beta6/emulator-user-cache/shader/a.bin" ] && [ -f "$1/backup-v0.1.0-beta6/VERSION.txt" ]' _ "$kit6"
check 'nothing newer now' bash -c '[ -z "$(bash "$1/update.sh" --status)" ]' _ "$kit6"
# A damaged download changes nothing.
mk --release v0.1.0-beta8 || true
t8=oot-coop-v0.1.0-beta8-linux-$arch.tar.gz
mkdir -p "$tmp/bad"
echo "0000000000000000000000000000000000000000000000000000000000000000  $t8" >"$tmp/bad/$t8.sha256"
cat >"$tmp/bad.json" <<EOF
[{"tag_name": "v0.1.0-beta8", "draft": false, "assets": [
  {"name": "$t8", "browser_download_url": "$tmp/out/$t8"},
  {"name": "$t8.sha256", "browser_download_url": "$tmp/bad/$t8.sha256"}]}]
EOF
OOT_COOP_UPDATE_API=$tmp/bad.json bash "$kit6/update.sh" --yes >"$tmp/update2.out" 2>&1
check 'a damaged download: said, the kit unchanged' bash -c 'grep -q "damaged" "$1" && head -n1 "$2/VERSION.txt" | grep -q "release v0.1.0-beta7"' _ "$tmp/update2.out" "$kit6"
unset OOT_COOP_UPDATE_API

# --- The scripts parse ---------------------------------------------------------------------
for f in "$release"/kit/linux/*.sh "$release/make_kit.sh"; do
    check "bash -n ${f##*/}" bash -n "$f"
done

if ((failed)); then echo "$failed check(s) failed"; exit 1; fi
echo 'all checks passed'
