# SPDX-License-Identifier: GPL-3.0-or-later
"""The NX2navi launcher's side of the co-op mod: one home row, "Multiplayer", with
Host and Join.

The mod's DLL is in the game at every start (mod.json lists it) and idles until a
session is named: by Host or Join here (OOT_COOP_INI in the game's environment), or by
the player inside the game, from the title screen's own "Multiplayer" entry, which
keeps its name, address and port in the same settings file as this menu.

It does what the emulator kit's Host.bat and Join.bat do (tools/release/kit, scripts
play.ps1, start-server.ps1, hosting.ps1 and coop-ini.ps1), with the launcher's menus
in place of the console questions: it asks for the player name, the port, the join
password and the host's address, writes coop.ini into the mod folder, starts the
co-op server for a host, and starts the game with the co-op DLL.

The rules for a name, an address and a password are the kit's, which are the ones the
game side and the server check again. Pure standard library. The launcher calls
register(api); nx2navi/README.md lists the api this file uses.
"""

import base64
import ctypes
import os
import re
import socket
import subprocess
import sys
import threading
import time
import unicodedata
from pathlib import Path

DEFAULT_PORT = 27510
DLL = 'bin/coop_nx2navi.dll'
SERVER_EXE = 'bin/oot-coop-server.exe'
INI_NAME = 'coop.ini'
LOG_NAME = 'coop-nx2navi.log'
# The server reads the join password from this variable of its own window only, so it
# is never on a command line (start-server.ps1).
PASSWORD_ENV = 'OOT_COOP_SERVER_PASSWORD'
# How long Host waits before it looks whether the server is still up. A server that
# cannot listen (the port is taken) stops at once.
SERVER_START_WAIT = 1.0

_SERVER = re.compile(r'^([A-Za-z0-9._-]{1,253})(:(\d{1,5}))?$')
_IPV4 = re.compile(r'^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$')
_CONTROL = re.compile(r'[\x00-\x1f\x7f]')


# A password typed in the game's own Multiplayer page is kept by the DLL under Windows'
# data protection for this user (dll/title_menu.cpp, Protect), never as clear text.
PROTECTED = {'join_password': 'join_password_protected', 'server_password': 'server_password_protected'}


def unprotect(stored):
    """The password behind a '*_protected' value of the settings, or '' when it cannot be
    read (another Windows user, another PC, not Windows)."""
    if os.name != 'nt' or not stored:
        return ''
    try:
        class Blob(ctypes.Structure):
            _fields_ = [('size', ctypes.c_uint32), ('data', ctypes.POINTER(ctypes.c_char))]
        raw = base64.b64decode(stored, validate=True)
        buffer = ctypes.create_string_buffer(raw, len(raw))
        source = Blob(len(raw), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_char)))
        out = Blob()
        crypt32 = ctypes.WinDLL('crypt32')
        kernel32 = ctypes.WinDLL('kernel32')
        kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        if not crypt32.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(out)):
            return ''
        try:
            return ctypes.string_at(out.data, out.size).decode('utf-8')
        finally:
            kernel32.LocalFree(ctypes.cast(out.data, ctypes.c_void_p))
    except (OSError, ValueError, AttributeError, UnicodeDecodeError):
        return ''


# --- The kit's rules (coop-ini.ps1, hosting.ps1) ---

def address_kind(address):
    """'loopback', 'link-local', 'hamachi' (25.x), 'radmin' (26.x), 'tailscale'
    (100.64.0.0/10), 'lan', 'public', or '' when it is not a usable IPv4 address."""
    m = _IPV4.match(address or '')
    if not m:
        return ''
    o = [int(x) for x in m.groups()]
    if any(x > 255 for x in o):
        return ''
    if o[0] == 127:
        return 'loopback'
    if o[0] == 169 and o[1] == 254:
        return 'link-local'
    if o[0] == 25:
        return 'hamachi'
    if o[0] == 26:
        return 'radmin'
    if o[0] == 100 and 64 <= o[1] <= 127:
        return 'tailscale'
    if o[0] == 10 or (o[0] == 172 and 16 <= o[1] <= 31) or (o[0] == 192 and o[1] == 168):
        return 'lan'
    if o[0] == 0 or o[0] >= 224:
        return ''
    return 'public'


VPN_NAMES = {'hamachi': 'Hamachi', 'radmin': 'Radmin VPN', 'tailscale': 'Tailscale'}


def valid_port(value):
    text = str(value)
    return bool(re.fullmatch(r'\d{1,5}', text)) and 1 <= int(text) <= 65535


def valid_server(value):
    """A host name or IPv4 address, with or without :port (Test-CoopServer)."""
    m = _SERVER.match(value or '')
    if not m:
        return False
    return not m.group(3) or 1 <= int(m.group(3)) <= 65535


def valid_name(value):
    """1 to 16 characters, no control characters (Test-CoopName). The game side counts
    code points, which is what len() counts."""
    if not value or _CONTROL.search(value):
        return False
    shown = sum(1 for ch in value if not unicodedata.combining(ch))
    return shown >= 1 and len(value) <= 16


def valid_password(value):
    """A join password: 1 to 128 bytes, no spaces or control characters
    (Test-CoopPassword). No password at all is '' and is not checked here."""
    if not value or re.search(r'[\s\x00-\x1f\x7f]', value):
        return False
    return len(value.encode('utf-8')) <= 128


def parse_join_address(text):
    """What a player pasted for the host's address, made plain (ConvertTo-JoinAddress):
    spaces, quotes, 'udp://', a trailing '/' or dot are dropped, 'localhost' becomes
    127.0.0.1. Returns (server, port) with port '' when none was given, or None when it
    is not an address."""
    if text is None:
        return None
    t = re.sub(r'[\s"\'<>]', '', text)
    t = re.sub(r'^(udp|http|https)://', '', t, flags=re.IGNORECASE)
    t = t.rstrip('/.,;')
    m = _SERVER.match(t)
    if not m:
        return None
    server = m.group(1).rstrip('.')
    port = m.group(3) or ''
    if not server:
        return None
    if port and not 1 <= int(port) <= 65535:
        return None
    if server.lower() == 'localhost':
        server = '127.0.0.1'
    # Digits and dots only: it must be a whole IPv4 address (26.1.2 is a typo).
    if re.fullmatch(r'[\d.]+', server) and not address_kind(server):
        return None
    return server, port


def format_join_address(address, port):
    """An address as the others type it: with :port only when it is not the usual one."""
    return f'{address}:{port}' if int(port) != DEFAULT_PORT else address


_VIRTUAL_CARDS = ('vethernet', 'hyper-v', 'wsl', 'virtual', 'vmware', 'vmnet', 'virtualbox', 'docker',
                  'loopback', 'npcap', 'tap-windows', 'teredo', 'isatap', 'bluetooth')


def virtual_card(name, description=''):
    """Whether a network card's name or description is a virtual machine's or a container's
    switch (Hyper-V and WSL "vEthernet", VMware, VirtualBox, Docker...): no friend's PC is
    behind it. The game's own Multiplayer page uses the same list (dll/menu_model.cpp)."""
    text = f'{name} | {description}'.lower()
    return any(mark in text for mark in _VIRTUAL_CARDS)


def join_addresses(local, bind=''):
    """The addresses to give the others, best first (Get-JoinAddresses, without the internet
    address: nothing outside this PC is asked): the home network's (a card with a default
    gateway), then VPN addresses, then other private ones, then those of virtual cards
    (Hyper-V, WSL, a virtual machine: said so, friends cannot reach them). A list of
    (address, kind, label). `local`: (address, has a gateway, virtual card, card name) from
    local_addresses, or plain addresses (taken as a card with a gateway)."""
    home, vpn, other, virtual = [], [], [], []
    seen = set()
    for entry in local:
        address, gateway, is_virtual, card = (entry, True, False, '') if isinstance(entry, str) else entry
        kind = address_kind(address)
        if (bind and address != bind) or address in seen:
            continue
        if kind in VPN_NAMES:
            name = VPN_NAMES[kind]
            vpn.append((address, kind, f'Over {name} (friends in your {name} network)'))
        elif kind != 'lan':
            continue
        elif is_virtual:
            virtual.append((address, 'virtual', 'A virtual network card of this PC'
                            + (f' ({card})' if card else '') + ': NOT for friends'))
        elif gateway:
            home.append((address, 'lan', 'Same house (friends on your home network or Wi-Fi)'))
        else:
            other.append((address, 'lan', 'Another network card of this PC' + (f' ({card})' if card else '')))
        seen.add(address)
    return home + vpn + other + virtual


def _adapter_addresses():
    """(address, has a default gateway, virtual card, card name) for every IPv4 address of a
    network card that is up, in Windows' own order (GetAdaptersAddresses). [] when Windows
    does not answer or this is not Windows."""
    try:
        import ctypes
        from ctypes import wintypes

        class SocketAddress(ctypes.Structure):
            _fields_ = [('sockaddr', ctypes.c_void_p), ('length', ctypes.c_int)]

        class Unicast(ctypes.Structure):
            pass
        Unicast._fields_ = [('length', wintypes.ULONG), ('flags', wintypes.DWORD),
                            ('next', ctypes.POINTER(Unicast)), ('address', SocketAddress)]

        class Adapter(ctypes.Structure):
            pass
        Adapter._fields_ = [
            ('length', wintypes.ULONG), ('if_index', wintypes.DWORD), ('next', ctypes.POINTER(Adapter)),
            ('adapter_name', ctypes.c_char_p), ('first_unicast', ctypes.POINTER(Unicast)),
            ('first_anycast', ctypes.c_void_p), ('first_multicast', ctypes.c_void_p),
            ('first_dns', ctypes.c_void_p), ('dns_suffix', ctypes.c_wchar_p),
            ('description', ctypes.c_wchar_p), ('friendly_name', ctypes.c_wchar_p),
            ('physical', ctypes.c_ubyte * 8), ('physical_length', wintypes.DWORD),
            ('flags', wintypes.DWORD), ('mtu', wintypes.DWORD), ('if_type', wintypes.DWORD),
            ('oper_status', ctypes.c_int), ('ipv6_if_index', wintypes.DWORD),
            ('zone_indices', wintypes.DWORD * 16), ('first_prefix', ctypes.c_void_p),
            ('transmit_speed', ctypes.c_ulonglong), ('receive_speed', ctypes.c_ulonglong),
            ('first_wins', ctypes.c_void_p), ('first_gateway', ctypes.c_void_p)]

        get = ctypes.WinDLL('iphlpapi').GetAdaptersAddresses
        get.argtypes = [wintypes.ULONG, wintypes.ULONG, ctypes.c_void_p, ctypes.c_void_p,
                        ctypes.POINTER(wintypes.ULONG)]
        get.restype = wintypes.ULONG
        flags = 0x80 | 0x02 | 0x04 | 0x08  # gateways; no anycast, multicast or DNS server lists
        size = wintypes.ULONG(16 * 1024)
        for _ in range(4):
            buffer = ctypes.create_string_buffer(size.value)
            result = get(socket.AF_INET, flags, None, buffer, ctypes.byref(size))
            if result != 111:  # ERROR_BUFFER_OVERFLOW: again with the size it named
                break
        if result != 0:
            return []
        found = []
        adapter = ctypes.cast(buffer, ctypes.POINTER(Adapter))
        while adapter:
            a = adapter.contents
            if a.oper_status == 1 and a.if_type != 24:  # up, and not the loopback
                name, description = a.friendly_name or '', a.description or ''
                unicast = a.first_unicast
                while unicast:
                    u = unicast.contents
                    if u.address.sockaddr and u.address.length >= 8:
                        raw = ctypes.string_at(u.address.sockaddr, 8)
                        if int.from_bytes(raw[:2], 'little') == socket.AF_INET:
                            found.append((socket.inet_ntoa(raw[4:8]), bool(a.first_gateway),
                                          virtual_card(name, description), name))
                    unicast = u.next
            adapter = a.next
        return found
    except (OSError, AttributeError, ValueError, ImportError):
        return []


def local_addresses():
    """This PC's IPv4 addresses with what Windows says about each one's network card:
    (address, has a default gateway, virtual card, card name). Nothing is sent: the UDP
    socket is only asked which address it would use, in case Windows gave no list."""
    found = _adapter_addresses()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(('192.0.2.1', 9))  # a documentation address; UDP connect sends nothing
            route = s.getsockname()[0]
            if route not in [entry[0] for entry in found]:
                found.append((route, True, False, ''))
    except OSError:
        pass
    if not found:
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                if info[4][0] not in [entry[0] for entry in found]:
                    found.append((info[4][0], True, False, ''))
        except OSError:
            pass
    return found


def join_block(addresses, port, bind='', allow='', copied=''):
    """The text a host reads: who gets which address (Format-JoinBlock)."""
    lines = ['Give your friends ONE of these (they type it in Join, "Host\'s Address"):', '']
    for address, _kind, label in addresses:
        lines.append(f'  {format_join_address(address, port)}   {label}')
    if not addresses:
        lines.append('  (no network address found: start your VPN program, or run ipconfig)')
    lines.append(f'  {format_join_address(bind or "127.0.0.1", port)}   A second game on this same PC')
    lines.append('')
    if copied:
        lines.append(f'Copied to the clipboard: {copied}')
    if any(kind == 'virtual' for _a, kind, _l in addresses):
        lines.append('An address marked "virtual network card" belongs to Hyper-V, WSL or a virtual '
                     'machine on this PC: nobody else can reach it.')
    if not any(kind in VPN_NAMES for _a, kind, _l in addresses):
        lines.append('Friends outside your house: easiest and safest is one Radmin VPN or Hamachi '
                     'network with a password for all of you. Over the internet without a VPN, '
                     'forward this UDP port on your router and give them your public address.')
    if allow:
        lines.append(f'Only these addresses may join (server_allow): {allow}')
    return '\n'.join(lines)


def join_state(log_text):
    """How far a join got, from the game side's log: 'welcomed', 'refused: <why>',
    'trying' (co-op started, no answer yet) or '' (Get-JoinState)."""
    state = ''
    for line in (log_text or '').splitlines():
        if 'coop: on, server ' in line:
            state = 'trying'
        elif 'coop: welcomed as player' in line:
            state = 'welcomed'
        else:
            m = re.search(r'coop: server (refused: .*?)\s*$', line)
            if m:
                state = m.group(1)
    return state


# --- coop.ini (coop.ini.template, Write-CoopIni) ---

INI_TEMPLATE = '''\
# Co-op settings for the NX2navi mod. The launcher's Multiplayer menu writes this file;
# you can also edit it. Comments go on their own line: a '#' after a value is read as
# part of the value.
#
# server: the address of the PC that runs the co-op server (LAN or VPN address), or
#         127.0.0.1 when it is this PC. Co-op is off while no server is set.
# port:   the server's UDP port.
# name:   1 to 16 characters, different for each player.
server = @SERVER@
port = @PORT@
name = @NAME@
# password: the join password, when the host set one. The password itself never goes
#         over the network, only a proof of it.
password = @PASSWORD@
# share_progress: 1 shares items, songs, keys, opened chests, dungeon switches and doors and
#         story flags with the other players (every player should use the same). 0: off.
share_progress = @SHARE@
# overlay: 1 shows the co-op panel and chat box inside the game (an NX2navi build
#         with its plugin interface; an older build ignores it). F9 hides the panel
#         in the game, Enter opens the chat box. 0: no panel.
overlay = @OVERLAY@
# puppet: 1 shows the other players as a second Link (the same NX2navi builds; it
#         needs the puppet pack, which the Multiplayer menu makes from your own game
#         files into this mod's romfs folder). 0: they are not shown.
puppet = @PUPPET@
# follow: whom your game follows through exits: when that player walks into another
#         area, your game loads it too, at the entrance they took. host (the usual
#         one), anyone (every player you share an area with) or off.
follow = @FOLLOW@
# join_at_host: 1 puts your Link next to the host once, when you get into the game
#         while the host is playing. 0: you stay where your save put you.
join_at_host = @JOIN_AT_HOST@
# objects: 1 shares the enemies with the other players (who an enemy attacks, where
#         it is, its life and its death; needs an NX2navi build with plugin interface
#         version 2). Every player should use the same setting. 0: each game its own.
objects = @OBJECTS@
# host_save: 1 when you join: your game first takes the host's save into a co-op
#         save folder of its own (coop-save in this folder; your own saves are never
#         touched) and starts again on it, so only the host chooses a save. 0: you
#         load a save of your own. A host has 0.
host_save = @HOST_SAVE@
'''


FOLLOW_MODES = ('host', 'anyone', 'off')

# The Multiplayer menu's four entries (the in-game menu, when there is one, uses the same).
HOST_STEAM = 'Host over Steam'
HOST_LOCAL = 'Host over Local Network'
JOIN_STEAM = 'Join over Steam'
JOIN_LOCAL = 'Join over Local Network'


def write_coop_ini(path, server, port, name, password='', share_progress=True, overlay=True,
                   puppet=True, follow='host', join_at_host=True, objects=True, host_save=False):
    """Writes coop.ini. An empty server, name or password leaves that line commented
    out, so co-op is off without an error in the log. CRLF, UTF-8, no byte order mark."""
    values = {'@SERVER@': server, '@PORT@': str(port), '@NAME@': name, '@PASSWORD@': password,
              '@SHARE@': '1' if share_progress else '0', '@OVERLAY@': '1' if overlay else '0',
              '@PUPPET@': '1' if puppet else '0',
              '@FOLLOW@': follow if follow in FOLLOW_MODES else 'host',
              '@JOIN_AT_HOST@': '1' if join_at_host else '0', '@OBJECTS@': '1' if objects else '0',
              '@HOST_SAVE@': '1' if host_save else '0'}
    out = []
    for line in INI_TEMPLATE.splitlines():
        for mark, value in values.items():
            if mark in line:
                line = line.replace(mark, value) if value else '# ' + line.replace(mark, '').rstrip()
        out.append(line)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(('\r\n'.join(out) + '\r\n').encode('utf-8'))


def read_coop_ini(path):
    """{server, port, name, password, share_progress, overlay, puppet, follow, join_at_host,
    objects, host_save} from a coop.ini; missing keys are '' (Read-CoopIni)."""
    values = {'server': '', 'port': '', 'name': '', 'password': '', 'share_progress': '', 'overlay': '',
              'puppet': '', 'follow': '', 'join_at_host': '', 'objects': '', 'host_save': ''}
    try:
        text = Path(path).read_text(encoding='utf-8')
    except OSError:
        return values
    for line in text.splitlines():
        m = re.match(r'^\s*(server|port|name|password|share_progress|overlay|puppet|follow|join_at_host|objects'
                     r'|host_save)\s*=\s*(.*?)\s*$', line)
        if m:
            values[m.group(1)] = m.group(2)
    return values


# --- The puppet pack ---

PUPPET_TOOL = 'tools/puppet_pack'
PUPPET_FILES = ('coop_puppet.json', 'romfs/Pack/Actor/CoopPuppet_Child.pack.zs',
                'romfs/Pack/Actor/CoopPuppet_Adult.pack.zs')


def puppet_pack_made(mod_dir):
    """True when the mod folder holds a puppet pack made for this port (its manifest says the
    puppets do not wear the local game's equipment: tools/puppet_pack/build.py, --no-pouch)."""
    mod_dir = Path(mod_dir)
    if not all((mod_dir / f).is_file() for f in PUPPET_FILES):
        return False
    try:
        import json
        return json.loads((mod_dir / 'coop_puppet.json').read_text(encoding='utf-8')).get('pouch') is False
    except (OSError, ValueError, AttributeError):
        return False


def make_puppet_pack(mod_dir, game_dir, log=lambda text: None):
    """Makes the puppet pack from the player's own game files (<game>/romfs: romfs.bin and
    index.json) into <mod>/romfs, with the tool shipped in the mod. Returns '' or why not.
    Nothing of the game leaves the PC, and the game's files are only read."""
    mod_dir, game_dir = Path(mod_dir), Path(game_dir)
    tool = mod_dir / PUPPET_TOOL / 'build.py'
    if not tool.is_file():
        return f'the pack tool is missing ({tool}); install the mod again'
    if not (game_dir / 'romfs' / 'index.json').is_file():
        return f'no game files in {game_dir / "romfs"} (romfs.bin and index.json)'
    import contextlib
    import importlib
    import io
    folder = str((mod_dir / 'tools').resolve())
    sys.path.insert(0, folder)
    out = io.StringIO()
    code = 1
    try:
        for name in [n for n in sys.modules if n == 'puppet_pack' or n.startswith('puppet_pack.')]:
            del sys.modules[name]
        build = importlib.import_module('puppet_pack.build')
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = build.main(['--romfs', str(game_dir), '--mod-dir', str(mod_dir)])
    except SystemExit as e:
        return str(e) or 'the pack tool stopped'
    except Exception as e:   # said to the player, never raised into the launcher
        return f'{type(e).__name__}: {e}'
    finally:
        if folder in sys.path:
            sys.path.remove(folder)
        for line in out.getvalue().splitlines():
            log('puppet pack: ' + line)
    if code != 0:
        errors = [line for line in out.getvalue().splitlines() if line.startswith('error:')]
        return errors[-1][7:] if errors else f'the pack tool ended with code {code}'
    return ''


# --- The server process ---

# The join password of a session whose host set none (dll/settings.hpp kOpenPassword): the
# server gives every session one, so "none" is this public word on both sides. The DLL sends
# it when coop.ini has no password.
OPEN_PASSWORD = 'oot-coop-open'


def server_command(exe, port, allow='', bind='', password=''):
    """The server's command line and the extra environment, as start-server.ps1 builds
    them. Raises ValueError with the kit's words for a bad server_allow or server_bind."""
    args = [str(exe), '--port', str(port)]
    for a in [x.strip() for x in (allow or '').split(',') if x.strip()]:
        if not re.fullmatch(r'\d{1,3}(\.\d{1,3}){3}(/\d{1,2})?', a):
            raise ValueError(f"server_allow: '{a}' is not an IPv4 address (25.1.2.3) or range (26.0.0.0/8).")
        args += ['--allow', a]
    bind = (bind or '').strip()
    if bind:
        if not re.fullmatch(r'\d{1,3}(\.\d{1,3}){3}', bind):
            raise ValueError(f"server_bind: '{bind}' is not an IPv4 address (25.1.2.3).")
        args += ['--bind', bind]
    env = {PASSWORD_ENV: password or OPEN_PASSWORD}
    args += ['--password-env', PASSWORD_ENV]
    return args, env


def processes_running(exe):
    """The ids of the running processes whose program file is `exe` (the kit's
    Get-Process check). Empty when there are none or Windows cannot be asked."""
    if os.name != 'nt':
        return []
    try:
        want = os.path.normcase(os.path.abspath(str(exe)))
        psapi = ctypes.WinDLL('psapi')
        kernel32 = ctypes.WinDLL('kernel32')
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel32.QueryFullProcessImageNameW.argtypes = [
            ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
        ids = (ctypes.c_uint32 * 8192)()
        used = ctypes.c_uint32(0)
        if not psapi.EnumProcesses(ids, ctypes.sizeof(ids), ctypes.byref(used)):
            return []
        found = []
        for pid in ids[:used.value // 4]:
            handle = kernel32.OpenProcess(0x1000, 0, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
            if not handle:
                continue
            try:
                buffer = ctypes.create_unicode_buffer(32768)
                size = ctypes.c_uint32(len(buffer))
                if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                    if os.path.normcase(os.path.abspath(buffer.value)) == want:
                        found.append(int(pid))
            finally:
                kernel32.CloseHandle(handle)
        return found
    except (OSError, AttributeError, ValueError):
        return []


def stop_processes(pids):
    """Ends the processes with these ids. Returns how many were ended."""
    if os.name != 'nt':
        return 0
    ended = 0
    try:
        kernel32 = ctypes.WinDLL('kernel32')
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel32.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        for pid in pids:
            handle = kernel32.OpenProcess(0x0001, 0, pid)  # PROCESS_TERMINATE
            if handle:
                if kernel32.TerminateProcess(handle, 0):
                    ended += 1
                kernel32.CloseHandle(handle)
    except (OSError, AttributeError):
        pass
    return ended


def copy_to_clipboard(text):
    """Puts text on the clipboard with Windows' clip.exe. False when it cannot."""
    if os.name != 'nt':
        return False
    try:
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        subprocess.run(['clip'], input=text.encode('utf-16-le'), check=True, timeout=5,
                       creationflags=flags)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


# --- The menus ---

class Multiplayer:
    def __init__(self, api):
        self.api = api
        self.mod_dir = Path(api.mod_dir)
        self.settings = self._load()
        self.server = None           # the Popen of the server this launcher started
        self.pack_thread = None      # the puppet pack being made since the launcher started
        self.addresses = local_addresses  # replaced in tests
        self.clipboard = copy_to_clipboard

    # Settings: everything the kit keeps between starts (coop.ini and
    # coop-options.ini there), the passwords included, as plain text.
    def _load(self):
        defaults = {'name': '', 'port': DEFAULT_PORT, 'host_address': '', 'join_password': '',
                    'server_password': '', 'share_progress': True, 'overlay': True, 'puppet': True,
                    'follow': 'host', 'join_at_host': True, 'objects': True, 'host_save': True,
                    'stop_server_with_game': False,
                    'server_allow': '', 'server_bind': ''}
        try:
            saved = self.api.load_settings() or {}
        except Exception as e:  # a broken settings file must not take the menu away
            self.api.log(f'co-op: settings not read ({e}); starting from the defaults')
            saved = {}
        for key, value in saved.items():
            if key in defaults and isinstance(value, type(defaults[key])):
                defaults[key] = value
        # What the game's own Multiplayer page keeps in the same file (its language, the
        # passwords typed there, protected): carried through every save of ours.
        self.extra = {key: value for key, value in saved.items() if key not in defaults}
        # A password typed in the game is used here too, and stays protected in the file.
        self.from_game = {}
        for plain, kept in PROTECTED.items():
            if not defaults[plain] and isinstance(saved.get(kept), str):
                password = unprotect(saved[kept])
                if password:
                    defaults[plain] = self.from_game[plain] = password
        if not valid_port(defaults['port']):
            defaults['port'] = DEFAULT_PORT
        if defaults['follow'] not in FOLLOW_MODES:
            defaults['follow'] = 'host'
        return defaults

    def _save(self):
        try:
            out = dict(self.extra)
            out.update(self.settings)
            for plain, kept in PROTECTED.items():
                if self.from_game.get(plain) and self.settings[plain] == self.from_game[plain]:
                    out[plain] = ''          # still the game's: not written as clear text
                else:
                    out.pop(kept, None)      # changed here: this menu's value holds
                    self.extra.pop(kept, None)
                    self.from_game.pop(plain, None)
            self.api.save_settings(out)
        except Exception as e:
            self.api.message(f'The co-op settings could not be saved: {e}')

    @property
    def ini(self):
        return self.mod_dir / INI_NAME

    def register(self):
        self.api.add_home_row('Multiplayer', value=self.home_value,
                              desc='Multijoueur / co-op: host a game or join a friend\'s game.',
                              on=self.open_menu)
        self.api.on_game_exit(self.on_game_exit)
        self._pack_at_start()

    def _pack_at_start(self):
        """The puppet pack, made quietly when the launcher starts with the mod enabled: a
        player who hosts or joins only from the game's own Multiplayer page never passes
        Host or Join here, and the game reads a mod's romfs at its start, so the DLL
        cannot make it."""
        self.pack_thread = None
        game_dir = getattr(self.api, 'game_dir', None)
        if not self.settings['puppet'] or not game_dir or puppet_pack_made(self.mod_dir):
            return

        def work():
            why = make_puppet_pack(self.mod_dir, game_dir, self.api.log)
            self.api.log(f'co-op: the puppet pack was not made: {why}' if why
                         else 'co-op: puppet pack made in ' + str(self.mod_dir / 'romfs'))

        self.pack_thread = threading.Thread(target=work, name='coop-puppet-pack', daemon=True)
        self.pack_thread.start()

    def home_value(self):
        if self.api.game_running():
            return self.connection()
        return 'server running' if self.server_running() else ''

    def connection(self):
        """The join state from the game side's log, in plain words."""
        if not self.api.game_running():
            return 'not playing'
        try:
            text = (self.mod_dir / LOG_NAME).read_text(encoding='utf-8', errors='replace')
        except OSError:
            text = ''
        state = join_state(text)
        if state == 'welcomed':
            return 'connected'
        if state == 'trying':
            return 'connecting...'
        return state or 'co-op not started'

    def open_menu(self):
        self.api.show_menu('Multiplayer', self.main_rows)

    # The ways two games find each other. 'local' is the co-op server on the host's PC, reached
    # by address (ENet over UDP: a home network, a virtual LAN or a forwarded port); the player
    # chooses a name in its two screens. 'steam' is done inside the game, not here: the game's
    # title screen has Multiplayer > Host on Steam / Join on Steam (the game's own Steam link:
    # NX2navi's plugin interface version 7, with Steam running), where the friends list and the
    # invites are. So these two rows are shown, cannot be chosen, and say where to go.
    TRANSPORTS = ('steam', 'local')
    STEAM_UNAVAILABLE = 'in the game: title screen'

    def transport_available(self, transport):
        return transport == 'local'

    def main_rows(self):
        api = self.api
        steam = ('Hosting and joining over Steam is done inside the game: start the game, and on '
                 'its title screen choose Multiplayer, then Host on Steam or Join on Steam (Steam '
                 'must be running; an NX2navi build with its Steam link is needed). The Local '
                 'Network entries here work without Steam, also over the internet with a virtual '
                 'LAN (Radmin VPN, Hamachi, Tailscale).')
        rows = [
            # Shown, not selectable: a value row with no action.
            api.value(HOST_STEAM, lambda: self.STEAM_UNAVAILABLE, desc=steam),
            api.action(HOST_LOCAL, lambda: api.show_menu(HOST_LOCAL, self.host_rows),
                       desc='Start the co-op server on this PC and play on it. Your friends use '
                            'Join over Local Network with the address this screen shows. You choose '
                            'your player name there.'),
            api.value(JOIN_STEAM, lambda: self.STEAM_UNAVAILABLE, desc=steam),
            api.action(JOIN_LOCAL, lambda: api.show_menu(JOIN_LOCAL, self.join_rows),
                       desc='Play on a friend\'s server: you need the address they give you. You '
                            'choose your player name there.'),
            api.divider(),
            api.value('Connection', self.connection, desc='Whether the running game reached the server.'),
        ]
        if self.server_running():
            rows.append(api.action('Stop Server', self.stop_server,
                                   desc='Stops the co-op server on this PC. Everyone on it is disconnected.'))
        return rows

    def shared_rows(self):
        api = self.api
        return [
            api.text('Player Name', lambda: self.settings['name'], self.set_name,
                     desc='1 to 16 characters, different for each player.',
                     prompt='Your player name, 1 to 16 characters'),
            api.text('Port', lambda: str(self.settings['port']), self.set_port,
                     desc=f'The server\'s UDP port. The usual one is {DEFAULT_PORT}.',
                     prompt='UDP port, 1 to 65535'),
            api.toggle('Shared Progress', lambda: bool(self.settings['share_progress']),
                       self.set_share_progress,
                       desc='Items, songs, keys, opened chests, dungeon switches and doors and story '
                            'flags are the same for every player: what one finds, all get. '
                            'Every player should use the same setting.'),
            api.toggle('In-Game Overlay', lambda: bool(self.settings['overlay']), self.set_overlay,
                       desc='Shows the other players (scene, distance) and a chat box inside the game. '
                            'Needs an NX2navi build with its plugin interface; an older build plays '
                            'without it. In the game: F9 hides or shows it, Enter opens the chat box.'),
            api.toggle('Show Other Players', lambda: bool(self.settings['puppet']), self.set_puppet,
                       desc='Shows each other player in your scene as a second Link that follows them. '
                            'Needs an NX2navi build with its plugin interface. The first Host or Join '
                            'makes the models for it from your own game files, into this mod\'s romfs '
                            'folder; nothing of your game leaves this PC.'),
            api.toggle('Follow The Host Through Exits', lambda: self.settings['follow'] != 'off',
                       self.set_follow,
                       desc='When the host walks into another area, your game loads that area too, at '
                            'the entrance the host took. It waits for your cutscene or talk to end. '
                            'Needs an NX2navi build with its plugin interface. In the game\'s chat box, '
                            '/teleport <name> takes you to a player at any time.'),
            api.toggle('Join At The Host', lambda: bool(self.settings['join_at_host']),
                       self.set_join_at_host,
                       desc='Puts your Link next to the host once, when you get into the game while '
                            'the host is playing, in whatever area the host is.'),
            api.toggle('Shared Enemies', lambda: bool(self.settings['objects']),
                       self.set_objects,
                       desc='Everyone sees the same enemies: where they are, their life and their death, '
                            'and boss rooms together. Experimental and not verified in this port. Needs '
                            'an NX2navi build with plugin interface version 2. Every player should use '
                            'the same setting.'),
        ]

    def host_rows(self):
        api = self.api
        return self.shared_rows() + [
            api.text('Join Password', lambda: 'set' if self.settings['server_password'] else 'none',
                     self.set_server_password, secret=True,
                     desc='Optional. Players must type it to join your server. Send it to your '
                          'friends with the address.',
                     prompt='Join password for your server: no spaces, 12 characters or more is best. '
                            'Empty or - for none'),
            api.value('Addresses For Friends', self.best_address, on=self.show_addresses,
                      desc='The address your friends type in Join.'),
            api.toggle('Stop Server With Game', lambda: bool(self.settings['stop_server_with_game']),
                       self.set_stop_with_game,
                       desc='Off: the server keeps running when you close the game, so your friends '
                            'stay in the room (close its window or use Stop Server). On: it stops '
                            'when the game closes.'),
            api.divider(),
            api.action('Start Hosting', self.host, desc='Starts the server in its own window, then the game.'),
        ]

    def join_rows(self):
        api = self.api
        return self.shared_rows() + [
            api.text('Host\'s Address', self.shown_host_address, self.set_host_address,
                     desc='What the host sent you. It looks like 26.12.34.56 or 192.168.1.20, '
                          'sometimes with :port at the end.',
                     prompt='The host\'s address (paste what the host sent you)'),
            api.text('Join Password', lambda: 'set' if self.settings['join_password'] else 'none',
                     self.set_join_password, secret=True,
                     desc='Only if the host gave you one.',
                     prompt='Join password the host gave you. Empty or - for none'),
            api.toggle('Use The Host\'s Save', lambda: bool(self.settings['host_save']), self.set_host_save,
                       desc='On: only the host chooses a save. Your game takes the host\'s save into a '
                            'co-op save folder of its own (your saves are never touched), starts again by '
                            'itself, and Continue puts you next to the host. Off: you load a save of '
                            'your own.'),
            api.divider(),
            api.action('Join Game', self.join, desc='Starts the game connected to the host.'),
        ]

    # --- Setters: each says what is wrong and keeps the old value ---

    def _changed(self):
        self._save()
        self.api.refresh()

    def set_name(self, value):
        name = (value or '').strip()
        if not valid_name(name):
            self.api.message('A player name is 1 to 16 characters, and different from the other players\'.')
            return
        self.settings['name'] = name
        self._changed()

    def set_port(self, value):
        text = (value or '').strip()
        if not valid_port(text):
            self.api.message(f'The port must be a number from 1 to 65535. The usual one is {DEFAULT_PORT}.')
            return
        self.settings['port'] = int(text)
        self._changed()

    def set_share_progress(self, value):
        self.settings['share_progress'] = bool(value)
        self._changed()

    def set_overlay(self, value):
        self.settings['overlay'] = bool(value)
        self._changed()

    def set_puppet(self, value):
        self.settings['puppet'] = bool(value)
        self._changed()

    def set_follow(self, value):
        # 'anyone' can only be set by hand (the launcher's settings file); the toggle keeps it.
        if not value:
            self.settings['follow'] = 'off'
        elif self.settings['follow'] == 'off':
            self.settings['follow'] = 'host'
        self._changed()

    def set_objects(self, value):
        self.settings['objects'] = bool(value)
        self._changed()

    def set_host_save(self, value):
        self.settings['host_save'] = bool(value)
        self._changed()

    def set_join_at_host(self, value):
        self.settings['join_at_host'] = bool(value)
        self._changed()

    def set_stop_with_game(self, value):
        self.settings['stop_server_with_game'] = bool(value)
        self._changed()

    def _set_password(self, key, value):
        password = (value or '').strip()
        if password == '-':
            password = ''
        if password and not valid_password(password):
            self.api.message('A join password has no spaces and at most 128 characters.')
            return
        self.settings[key] = password
        self._changed()

    def set_server_password(self, value):
        self._set_password('server_password', value)

    def set_join_password(self, value):
        self._set_password('join_password', value)

    def shown_host_address(self):
        if not self.settings['host_address']:
            return ''
        return format_join_address(self.settings['host_address'], self.settings['port'])

    def set_host_address(self, value):
        parsed = parse_join_address(value)
        if not parsed or not valid_server(parsed[0]):
            self.api.message(f"'{(value or '').strip()}' is not an address. It looks like 26.12.34.56 or "
                             '192.168.1.20 (four numbers with dots), sometimes with :port at the end. '
                             'Ask the host to read it from Host, "Addresses For Friends".')
            return
        self.settings['host_address'] = parsed[0]
        # A new address without :port means the usual port (play.ps1).
        self.settings['port'] = int(parsed[1]) if parsed[1] else DEFAULT_PORT
        self._changed()

    # --- Host ---

    def host_addresses(self):
        return join_addresses(self.addresses(), self.settings['server_bind'].strip())

    def best_address(self):
        found = self.host_addresses()
        if not found:
            return 'none found'
        return format_join_address(found[0][0], self.settings['port'])

    def show_addresses(self, copy=False):
        found = self.host_addresses()
        copied = ''
        if copy and found:
            best = format_join_address(found[0][0], self.settings['port'])
            if self.clipboard(best):
                copied = best
        self.api.message(join_block(found, self.settings['port'], self.settings['server_bind'].strip(),
                                    self.settings['server_allow'].strip(), copied))

    def server_exe(self):
        return self.mod_dir / SERVER_EXE

    def server_running(self):
        if self.server is not None and self.server.poll() is None:
            return True
        return bool(processes_running(self.server_exe()))

    def start_server(self):
        """True when this mod's server runs afterwards. Never starts a second one."""
        exe = self.server_exe()
        if self.server_running():
            self.api.log('co-op: the server from this mod is already running')
            return True
        if not exe.is_file():
            self.api.message(f'The co-op server is missing: {exe}\nInstall the mod again.')
            return False
        try:
            args, extra = server_command(exe, self.settings['port'], self.settings['server_allow'],
                                         self.settings['server_bind'], self.settings['server_password'])
        except ValueError as e:
            self.api.message(f'{e} The server was not started.')
            return False
        # The whole environment plus the password, so this works whether the launcher
        # adds `env` to its own environment or uses it as it is.
        env = dict(os.environ)
        env.update(extra)
        try:
            self.server = self.api.start_process(args, cwd=str(self.mod_dir), console=True, env=env)
        except Exception as e:
            self.server = None
            self.api.message(f'The co-op server could not be started: {e}')
            return False
        time.sleep(SERVER_START_WAIT)
        if self.server is None or self.server.poll() is not None:
            self.server = None
            self.api.message(f'The co-op server stopped at once. UDP port {self.settings["port"]} may be '
                             'in use by another program (or another co-op server): close it or pick '
                             'another port.')
            return False
        self.api.log(f'co-op: server started on UDP port {self.settings["port"]}')
        return True

    def stop_server(self):
        stopped = False
        if self.server is not None and self.server.poll() is None:
            try:
                self.server.terminate()
                stopped = True
            except OSError as e:
                self.api.log(f'co-op: the server could not be stopped: {e}')
        self.server = None
        if not stopped:
            stopped = stop_processes(processes_running(self.server_exe())) > 0
        self.api.log('co-op: server stopped' if stopped else 'co-op: no server of this mod was running')
        self.api.refresh()

    def on_game_exit(self, _exit_code=None):
        # The kit leaves its server running when the emulator closes (the others stay
        # in the room); the toggle is for players who want it gone with the game.
        if self.settings['stop_server_with_game'] and self.server is not None:
            self.stop_server()

    def _ready(self):
        """The checks Host and Join share. True when the game may start."""
        if self.api.game_running():
            self.api.message('The game is already running; close it first to use new settings.')
            return False
        if not valid_name(self.settings['name']):
            self.api.message('Set your player name first ("Player Name", 1 to 16 characters).')
            return False
        if not valid_port(self.settings['port']):
            self.api.message('Set the port first ("Port", 1 to 65535).')
            return False
        if not (self.mod_dir / DLL).is_file():
            self.api.message(f'The co-op DLL is missing: {self.mod_dir / DLL}\nInstall the mod again.')
            return False
        return True

    def _puppet_pack(self):
        """Makes the puppet pack when "Show Other Players" is on and the mod has none yet.
        A failure is said and the game still starts: the others are then not shown."""
        if self.pack_thread is not None:
            self.pack_thread.join()   # the one started with the launcher: not twice at once
            self.pack_thread = None
        if not self.settings['puppet'] or puppet_pack_made(self.mod_dir):
            return
        game_dir = getattr(self.api, 'game_dir', None)
        why = make_puppet_pack(self.mod_dir, game_dir, self.api.log) if game_dir else 'no game folder'
        if why:
            self.api.log(f'co-op: the puppet pack was not made: {why}')
            self.api.message(f'The other players\' models could not be made ({why}). '
                             'They will not be shown in the game.')
        else:
            self.api.log('co-op: puppet pack made in ' + str(self.mod_dir / 'romfs'))

    def host_save_dir(self):
        """The folder of the multiplayer save a host plays on: the one chosen on the game's own
        Multiplayer page (Host, "Save"; "mp_save" in the settings file names a folder of
        coop-saves in the mod folder). None when none was chosen yet or it is gone."""
        try:
            saved = self.api.load_settings() or {}   # the game's page may have written since we read
        except Exception:
            saved = self.extra
        name = saved.get('mp_save')
        if not isinstance(name, str) or not name or name in ('.', '..') or any(c in name for c in '\\/:*?"<>|'):
            return None
        folder = self.mod_dir / 'coop-saves' / name
        return folder if folder.is_dir() else None

    def _play(self, server, password, host_save=False, save_dir=None):
        self._puppet_pack()
        try:
            write_coop_ini(self.ini, server, self.settings['port'], self.settings['name'], password,
                           self.settings['share_progress'], self.settings['overlay'],
                           self.settings['puppet'], self.settings['follow'],
                           self.settings['join_at_host'], self.settings['objects'], host_save)
        except OSError as e:
            self.api.message(f'coop.ini could not be written ({self.ini}): {e}')
            return False
        self.api.log(f'co-op: server {server}, port {self.settings["port"]}, name {self.settings["name"]}, '
                     f'join password {"set" if password else "none"}')
        try:
            env = {'OOT_COOP_INI': str(self.ini)}
            if save_dir is not None:
                # A host's multiplayer save: the game keeps its saves there for this run
                # (NX2navi reads the variable at its start), so the player's own are not written.
                env['NX2NAVI_SAVE_DIR'] = str(save_dir)
            started = self.api.play(env=env, dlls=[DLL])
        except Exception as e:
            self.api.message(f'The game could not be started: {e}')
            return False
        if not started:
            self.api.message('The game did not start. Start it once without co-op to see what is wrong.')
            return False
        return True

    def host(self):
        if not self._ready():
            return
        password = self.settings['server_password']
        if password and not valid_password(password):
            self.api.message('The join password has a space or is longer than 128 characters; '
                             'the server was not started.')
            return
        if not self.start_server():
            return
        self.show_addresses(copy=True)
        # The multiplayer save chosen in the game (its Multiplayer page: Host, Save): the session
        # is played on it. None chosen yet: said in the log, and the game uses the player's own
        # saves, as before the game had that page.
        save_dir = self.host_save_dir()
        if save_dir is not None:
            self.api.log(f'co-op: hosting on the multiplayer save "{save_dir.name}" ({save_dir})')
        else:
            self.api.log('co-op: no multiplayer save chosen yet (in the game: Multiplayer, Host over Local '
                         'Network, Save), so this session uses your own saves')
        # server_bind: the server hears only that address, not 127.0.0.1.
        self._play(self.settings['server_bind'].strip() or '127.0.0.1', password, save_dir=save_dir)
        self.api.refresh()

    def join(self):
        if not self._ready():
            return
        server = self.settings['host_address']
        if not server or not valid_server(server):
            self.api.message('Set the host\'s address first ("Host\'s Address"): paste what the host sent you.')
            return
        password = self.settings['join_password']
        if password and not valid_password(password):
            self.api.message('The join password has a space or is too long. Ask the host for it again.')
            return
        if self._play(server, password, host_save=bool(self.settings['host_save'])):
            shown = format_join_address(server, self.settings['port'])
            self.api.log(f'co-op: joining {shown}; "Connection" in the Multiplayer menu says when it worked')
        self.api.refresh()


def register(api):
    plugin = Multiplayer(api)
    plugin.register()
    return plugin
