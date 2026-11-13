# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the launcher plugin with a fake launcher api. Run with plain Python:

    python nx2navi/tests/test_plugin.py

Nothing is started: the fake api records what the plugin asked for.
"""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('launcher_plugin', HERE.parent / 'mod' / 'launcher_plugin.py')
plugin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plugin)
plugin.SERVER_START_WAIT = 0


class FakeProcess:
    def __init__(self, exit_code=None):
        self.exit_code = exit_code
        self.terminated = False

    def poll(self):
        return self.exit_code

    def terminate(self):
        self.terminated = True
        self.exit_code = 1


class FakeApi:
    """The launcher api of nx2navi/README.md, recording every call."""

    def __init__(self, mod_dir):
        self.mod_dir = Path(mod_dir)
        self.game_dir = self.mod_dir.parent.parent
        self.name = 'OoT Co-op'
        self.logs, self.messages, self.home_rows, self.menus = [], [], [], []
        self.plays, self.processes = [], []
        self.settings = {}
        self.running = False
        self.play_result = True
        self.process_exit = None
        self.exit_callbacks = []
        self.refreshed = 0

    def log(self, text):
        self.logs.append(text)

    def add_home_row(self, label, value='', desc='', on=None):
        self.home_rows.append({'label': label, 'value': value, 'desc': desc, 'on': on})

    def action(self, label, on, desc=''):
        return {'kind': 'action', 'label': label, 'on': on, 'desc': desc}

    def value(self, label, value, on=None, desc=''):
        return {'kind': 'value', 'label': label, 'value': value, 'on': on, 'desc': desc}

    def toggle(self, label, get, set, desc=''):
        return {'kind': 'toggle', 'label': label, 'get': get, 'set': set, 'desc': desc}

    def text(self, label, get, set, desc='', secret=False, prompt=''):
        return {'kind': 'text', 'label': label, 'get': get, 'set': set, 'desc': desc,
                'secret': secret, 'prompt': prompt}

    def divider(self):
        return {'kind': 'divider'}

    def show_menu(self, title, rows):
        self.menus.append((title, rows))

    def refresh(self):
        self.refreshed += 1

    def back(self):
        if self.menus:
            self.menus.pop()

    def home(self):
        self.menus.clear()

    def message(self, text):
        self.messages.append(text)

    def ask_text(self, title, prompt, initial='', secret=False):
        return None

    def play(self, env=None, dlls=None):
        self.plays.append({'env': env, 'dlls': dlls})
        if self.play_result:
            self.running = True
        return self.play_result

    def game_running(self):
        return self.running

    def start_process(self, cmd, cwd=None, console=True, env=None):
        process = FakeProcess(self.process_exit)
        self.processes.append({'cmd': cmd, 'cwd': cwd, 'console': console, 'env': env, 'process': process})
        return process

    def on_game_exit(self, callback):
        self.exit_callbacks.append(callback)

    def load_settings(self):
        return dict(self.settings)

    def save_settings(self, d):
        self.settings = dict(d)

    def open(self, path):
        pass


def rows_by_label(rows):
    return {r['label']: r for r in rows if r['kind'] != 'divider'}


class Rules(unittest.TestCase):
    def test_address_parsing(self):
        p = plugin.parse_join_address
        self.assertEqual(p('192.168.1.20'), ('192.168.1.20', ''))
        self.assertEqual(p('  26.12.34.56:27511 '), ('26.12.34.56', '27511'))
        self.assertEqual(p('udp://25.1.2.3:4000/'), ('25.1.2.3', '4000'))
        self.assertEqual(p('"192.168.1.20".'), ('192.168.1.20', ''))
        self.assertEqual(p('localhost'), ('127.0.0.1', ''))
        self.assertEqual(p('LOCALHOST:1234'), ('127.0.0.1', '1234'))
        self.assertEqual(p('my-pc.example.org'), ('my-pc.example.org', ''))
        self.assertEqual(p('192.168.1.20,'), ('192.168.1.20', ''))
        for bad in (None, '', '   ', '26.1.2', '300.1.2.3', '1.2.3.4:0', '1.2.3.4:65536',
                    '1.2.3.4:123456', '1.2.3.4:', 'host:port', '0.1.2.3', '224.0.0.1',
                    'fe80::1', 'bad!name'):
            self.assertIsNone(p(bad), bad)
        # Pasted text loses its spaces; what is left is then a host name.
        self.assertEqual(p('a b'), ('ab', ''))

    def test_address_kind(self):
        k = plugin.address_kind
        self.assertEqual(k('127.0.0.1'), 'loopback')
        self.assertEqual(k('169.254.1.1'), 'link-local')
        self.assertEqual(k('25.1.2.3'), 'hamachi')
        self.assertEqual(k('26.1.2.3'), 'radmin')
        self.assertEqual(k('100.64.0.1'), 'tailscale')
        self.assertEqual(k('100.128.0.1'), 'public')
        self.assertEqual(k('10.0.0.5'), 'lan')
        self.assertEqual(k('172.16.0.5'), 'lan')
        self.assertEqual(k('172.32.0.5'), 'public')
        self.assertEqual(k('192.168.0.5'), 'lan')
        self.assertEqual(k('8.8.8.8'), 'public')
        self.assertEqual(k('256.1.1.1'), '')
        self.assertEqual(k('host'), '')

    def test_join_addresses_order_and_format(self):
        found = plugin.join_addresses(['127.0.0.1', '192.168.1.20', '26.5.6.7', '169.254.3.4', '8.8.8.8'])
        self.assertEqual([a for a, _k, _l in found], ['192.168.1.20', '26.5.6.7'])
        # With what Windows says about the cards: the home network (a default gateway) first, then
        # the VPN, then a card without a gateway, and virtual cards (Hyper-V, WSL, a VM) last, said so.
        cards = [('172.27.240.1', False, True, 'vEthernet (WSL (Hyper-V firewall))'),
                 ('26.5.6.7', False, True, 'Radmin VPN'),
                 ('10.0.0.7', False, False, 'Ethernet 2'),
                 ('192.168.1.20', True, False, 'Ethernet'),
                 ('192.168.1.20', True, False, ''),
                 ('192.168.56.1', False, True, 'VMware Network Adapter VMnet1')]
        ordered = plugin.join_addresses(cards)
        self.assertEqual([a for a, _k, _l in ordered],
                         ['192.168.1.20', '26.5.6.7', '10.0.0.7', '172.27.240.1', '192.168.56.1'])
        self.assertEqual([k for _a, k, _l in ordered], ['lan', 'radmin', 'lan', 'virtual', 'virtual'])
        self.assertIn('NOT for friends', ordered[3][2])
        self.assertIn('nobody else can reach it', plugin.join_block(ordered, 27510))
        self.assertTrue(plugin.virtual_card('vEthernet (Default Switch)', 'Hyper-V Virtual Ethernet Adapter'))
        self.assertFalse(plugin.virtual_card('Wi-Fi', 'Intel(R) Wi-Fi 6E AX211 160MHz'))
        for entry in plugin.local_addresses():     # this PC's own, whatever they are: the shape
            self.assertEqual(len(entry), 4)
            self.assertTrue(plugin.address_kind(entry[0]))
        self.assertEqual(plugin.join_addresses(['192.168.1.20', '26.5.6.7'], bind='192.168.1.20')[0][0],
                         '192.168.1.20')
        self.assertEqual(plugin.format_join_address('26.5.6.7', 27510), '26.5.6.7')
        self.assertEqual(plugin.format_join_address('26.5.6.7', 4000), '26.5.6.7:4000')
        block = plugin.join_block(found, 4000, copied='26.5.6.7:4000')
        self.assertIn('26.5.6.7:4000', block)
        self.assertIn('127.0.0.1:4000', block)
        self.assertIn('Copied to the clipboard', block)
        self.assertIn('no network address found', plugin.join_block([], 27510))

    def test_name(self):
        v = plugin.valid_name
        self.assertTrue(v('Saria'))
        self.assertTrue(v('Link#2'))
        self.assertTrue(v('a' * 16))
        self.assertTrue(v('Zelda éè'))
        self.assertFalse(v(''))
        self.assertFalse(v('a' * 17))
        self.assertFalse(v('tab\tname'))
        self.assertFalse(v('line\nname'))
        self.assertFalse(v('\x7f'))

    def test_password(self):
        v = plugin.valid_password
        self.assertTrue(v('three-random-words'))
        self.assertTrue(v('p' * 128))
        self.assertFalse(v(''))
        self.assertFalse(v('p' * 129))
        self.assertFalse(v('é' * 65))   # 130 bytes
        self.assertTrue(v('é' * 64))    # 128 bytes
        self.assertFalse(v('two words'))
        self.assertFalse(v('tab\tword'))
        self.assertFalse(v('bell\x07'))

    def test_port_and_server(self):
        self.assertTrue(plugin.valid_port('27510'))
        self.assertTrue(plugin.valid_port(1))
        self.assertFalse(plugin.valid_port('0'))
        self.assertFalse(plugin.valid_port('65536'))
        self.assertFalse(plugin.valid_port('12a'))
        self.assertFalse(plugin.valid_port(''))
        self.assertTrue(plugin.valid_server('192.168.1.20'))
        self.assertTrue(plugin.valid_server('host.example:1'))
        self.assertFalse(plugin.valid_server('host:0'))
        self.assertFalse(plugin.valid_server('ho st'))
        self.assertFalse(plugin.valid_server(''))

    def test_join_state(self):
        s = plugin.join_state
        self.assertEqual(s(''), '')
        self.assertEqual(s('12:00 coop: on, server 1.2.3.4:27510 as Saria\n'), 'trying')
        self.assertEqual(s('coop: on, server x\ncoop: welcomed as player 2 on x\n'), 'welcomed')
        self.assertEqual(s('coop: on, server x\ncoop: server refused: wrong join password  \n'),
                         'refused: wrong join password')

    def test_server_command(self):
        args, env = plugin.server_command('srv.exe', 27510)
        # No password set: the open word (every session has a password; none = this public one).
        self.assertEqual(args, ['srv.exe', '--port', '27510', '--password-env', plugin.PASSWORD_ENV])
        self.assertEqual(env, {plugin.PASSWORD_ENV: plugin.OPEN_PASSWORD})
        args, env = plugin.server_command('srv.exe', 4000, allow='25.1.2.3, 26.0.0.0/8', bind=' 25.9.9.9 ',
                                          password='secret')
        self.assertEqual(args, ['srv.exe', '--port', '4000', '--allow', '25.1.2.3', '--allow', '26.0.0.0/8',
                                '--bind', '25.9.9.9', '--password-env', plugin.PASSWORD_ENV])
        self.assertEqual(env, {plugin.PASSWORD_ENV: 'secret'})
        self.assertNotIn('secret', args)   # never on the command line
        with self.assertRaises(ValueError):
            plugin.server_command('srv.exe', 1, allow='friends')
        with self.assertRaises(ValueError):
            plugin.server_command('srv.exe', 1, bind='26.0.0.0/8')


class Ini(unittest.TestCase):
    def test_write_and_read(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'sub' / 'coop.ini'
            plugin.write_coop_ini(path, '127.0.0.1', 27510, 'Saria', 'pw', True)
            raw = path.read_bytes()
            self.assertFalse(raw.startswith(b'\xef\xbb\xbf'))
            self.assertTrue(raw.endswith(b'\r\n'))
            self.assertNotIn(b'@', raw)
            lines = raw.decode('utf-8').split('\r\n')
            self.assertIn('server = 127.0.0.1', lines)
            self.assertIn('port = 27510', lines)
            self.assertIn('name = Saria', lines)
            self.assertIn('password = pw', lines)
            self.assertIn('share_progress = 1', lines)
            self.assertIn('overlay = 1', lines)   # on unless asked
            self.assertEqual(plugin.read_coop_ini(path),
                             {'server': '127.0.0.1', 'port': '27510', 'name': 'Saria', 'password': 'pw',
                              'share_progress': '1', 'overlay': '1', 'puppet': '1',
                              'follow': 'host', 'join_at_host': '1', 'objects': '1', 'host_save': '0'})   # shared enemies: on unless asked
            plugin.write_coop_ini(path, '26.1.2.3', 27510, 'Saria', '', host_save=True)
            self.assertEqual(plugin.read_coop_ini(path)['host_save'], '1')
            plugin.write_coop_ini(path, '127.0.0.1', 27510, 'Saria', '', objects=False)
            self.assertEqual(plugin.read_coop_ini(path)['objects'], '0')
            plugin.write_coop_ini(path, '127.0.0.1', 27510, 'Saria', '', follow='anyone', join_at_host=False)
            self.assertEqual(plugin.read_coop_ini(path)['follow'], 'anyone')
            self.assertEqual(plugin.read_coop_ini(path)['join_at_host'], '0')
            plugin.write_coop_ini(path, '127.0.0.1', 27510, 'Saria', '', follow='nonsense')
            self.assertEqual(plugin.read_coop_ini(path)['follow'], 'host')
            plugin.write_coop_ini(path, '127.0.0.1', 27510, 'Saria', '', False, overlay=False, puppet=False)
            self.assertEqual(plugin.read_coop_ini(path)['puppet'], '0')
            plugin.write_coop_ini(path, '127.0.0.1', 27510, 'Saria', '', False, overlay=False)
            self.assertEqual(plugin.read_coop_ini(path)['overlay'], '0')

    def test_empty_values_are_commented_out(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'coop.ini'
            plugin.write_coop_ini(path, '', 4000, 'Mido', '', False)
            lines = path.read_text(encoding='utf-8').splitlines()
            self.assertIn('# server =', lines)
            self.assertIn('# password =', lines)
            self.assertIn('share_progress = 0', lines)
            values = plugin.read_coop_ini(path)
            self.assertEqual(values['server'], '')
            self.assertEqual(values['password'], '')
            self.assertEqual(values['name'], 'Mido')

    def test_missing_file(self):
        self.assertEqual(plugin.read_coop_ini('no/such/coop.ini')['server'], '')


class Menus(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mod = Path(self.tmp.name) / 'mods' / 'OoT Co-op'
        (self.mod / 'bin').mkdir(parents=True)
        (self.mod / plugin.DLL).write_bytes(b'')
        (self.mod / plugin.SERVER_EXE).write_bytes(b'')
        # A puppet pack is there, as after the first Host or Join.
        for name in plugin.PUPPET_FILES:
            (self.mod / name).parent.mkdir(parents=True, exist_ok=True)
            (self.mod / name).write_text('{"pouch": false}', encoding='utf-8')
        self.api = FakeApi(self.mod)
        self.p = plugin.register(self.api)
        self.p.addresses = lambda: ['192.168.1.20', '26.5.6.7']
        self.copied = []
        self.p.clipboard = lambda text: (self.copied.append(text), True)[1]

    def tearDown(self):
        self.tmp.cleanup()

    def test_home_row(self):
        self.assertEqual(len(self.api.home_rows), 1)
        row = self.api.home_rows[0]
        self.assertEqual(row['label'], 'Multiplayer')
        self.assertIn('Multijoueur', row['desc'])
        self.assertEqual(len(self.api.exit_callbacks), 1)
        row['on']()
        title, rows = self.api.menus[-1]
        self.assertEqual(title, 'Multiplayer')
        self.assertTrue(callable(rows))
        shown = rows()
        labels = [r.get('label') for r in shown]
        self.assertEqual(labels[:4], ['Host over Steam', 'Host over Local Network', 'Join over Steam',
                                      'Join over Local Network'])
        self.assertNotIn('Player Name', labels)   # the name is chosen inside the two Local Network screens
        # Steam is shown and cannot be chosen: no action, a value saying so, and why.
        by_label = rows_by_label(shown)
        for label in ('Host over Steam', 'Join over Steam'):
            row = by_label[label]
            self.assertIsNone(row.get('on'))
            self.assertEqual(row['value'](), 'in the game: title screen')
            self.assertIn('title screen choose Multiplayer', row['desc'])
            self.assertNotIn('not available', row['desc'])
        plugin_object = plugin.Multiplayer(self.api)
        self.assertFalse(plugin_object.transport_available('steam'))
        self.assertTrue(plugin_object.transport_available('local'))
        self.assertNotIn('Back', labels)        # the launcher adds it
        self.assertNotIn('Stop Server', labels)  # nothing runs

    def open(self, which):
        which = {'Host': 'Host over Local Network', 'Join': 'Join over Local Network'}[which]
        self.api.home_rows[0]['on']()
        rows_by_label(self.api.menus[-1][1]())[which]['on']()
        title, rows = self.api.menus[-1]
        self.assertEqual(title, which)
        return rows

    def test_host_and_join_menus(self):
        host = rows_by_label(self.open('Host')())
        for label in ('Player Name', 'Port', 'Shared Progress', 'In-Game Overlay',
                      'Join Password', 'Addresses For Friends', 'Stop Server With Game', 'Start Hosting'):
            self.assertIn(label, host)
        self.assertEqual(host['In-Game Overlay']['kind'], 'toggle')
        self.assertTrue(host['In-Game Overlay']['get']())   # on by default
        self.assertIn('F9', host['In-Game Overlay']['desc'])
        self.assertIn('plugin interface', host['In-Game Overlay']['desc'])
        self.assertTrue(host['Join Password']['secret'])
        self.assertEqual(host['Port']['get'](), '27510')
        self.assertEqual(host['Addresses For Friends']['value'](), '192.168.1.20')   # the home network first, the VPN after it
        self.assertTrue(host['Shared Progress']['get']())   # on unless turned off
        self.assertIn('every player', host['Shared Progress']['desc'])
        join = rows_by_label(self.open('Join')())
        for label in ('Player Name', 'Port', 'Shared Progress', 'In-Game Overlay',
                      "Host's Address", 'Join Password', 'Join Game'):
            self.assertIn(label, join)
        self.assertTrue(join['Join Password']['secret'])

    def test_setters_validate_and_persist(self):
        rows = rows_by_label(self.open('Join')())
        rows['Player Name']['set']('x' * 17)
        self.assertEqual(self.p.settings['name'], '')
        self.assertEqual(len(self.api.messages), 1)
        rows['Player Name']['set'](' Saria ')
        self.assertEqual(self.api.settings['name'], 'Saria')
        rows['Port']['set']('70000')
        self.assertEqual(self.p.settings['port'], 27510)
        rows["Host's Address"]['set']('26.1.2')
        self.assertEqual(self.p.settings['host_address'], '')
        self.assertEqual(len(self.api.messages), 3)
        rows["Host's Address"]['set']('udp://26.1.2.3:4000')
        self.assertEqual((self.api.settings['host_address'], self.api.settings['port']), ('26.1.2.3', 4000))
        self.assertEqual(rows["Host's Address"]['get'](), '26.1.2.3:4000')
        rows["Host's Address"]['set']('192.168.1.9')   # no port: the usual one again
        self.assertEqual(self.api.settings['port'], 27510)
        rows['Join Password']['set']('two words')
        self.assertEqual(self.p.settings['join_password'], '')
        rows['Join Password']['set']('letmein')
        self.assertEqual(self.api.settings['join_password'], 'letmein')
        self.assertEqual(rows['Join Password']['get'](), 'set')   # never shown
        rows['Join Password']['set']('-')
        self.assertEqual(self.api.settings['join_password'], '')
        rows['Shared Progress']['set'](True)
        self.assertTrue(self.api.settings['share_progress'])
        # Follow and the join at the host: on unless turned off; 'anyone' (set by hand) is kept.
        self.assertTrue(rows['Follow The Host Through Exits']['get']())
        self.assertTrue(rows['Join At The Host']['get']())
        rows['Follow The Host Through Exits']['set'](False)
        self.assertEqual(self.api.settings['follow'], 'off')
        rows['Follow The Host Through Exits']['set'](True)
        self.assertEqual(self.api.settings['follow'], 'host')
        self.api.settings['follow'] = 'anyone'
        again = plugin.Multiplayer(self.api)
        again.set_follow(True)
        self.assertEqual(self.api.settings['follow'], 'anyone')
        again.set_follow(False)
        again.set_follow(True)
        self.assertEqual(self.api.settings['follow'], 'host')
        rows['Join At The Host']['set'](False)
        self.assertFalse(self.api.settings['join_at_host'])
        self.assertTrue(rows['Shared Enemies']['get']())   # on unless turned off (since coop-dll 0.7)
        rows['Shared Enemies']['set'](False)
        self.assertFalse(self.api.settings['objects'])
        rows['Shared Enemies']['set'](True)
        self.assertTrue(self.api.settings['objects'])
        # A new plugin reads them back.
        again = plugin.Multiplayer(self.api)
        self.assertEqual(again.settings['name'], 'Saria')
        self.assertEqual(again.settings['host_address'], '192.168.1.9')

    def test_show_other_players_toggle_and_the_puppet_pack(self):
        rows = rows_by_label(self.open('Join')())
        rows['Player Name']['set']('Saria')
        rows["Host's Address"]['set']('26.1.2.3')
        # On, with no pack and no pack tool in the mod: said, and the game still starts.
        (self.mod / 'coop_puppet.json').unlink()
        rows['Join Game']['on']()
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['puppet'], '1')
        self.assertTrue(any('models could not be made' in m for m in self.api.messages))
        self.assertEqual(len(self.api.plays), 1)
        self.assertFalse(plugin.puppet_pack_made(self.mod))
        # A pack made for this port is not made again.
        for name in plugin.PUPPET_FILES:
            (self.mod / name).parent.mkdir(parents=True, exist_ok=True)
            (self.mod / name).write_text('{"pouch": false}', encoding='utf-8')
        self.assertTrue(plugin.puppet_pack_made(self.mod))
        (self.mod / 'coop_puppet.json').write_text('{"pouch": true}', encoding='utf-8')
        self.assertFalse(plugin.puppet_pack_made(self.mod))   # the emulator kit's pack: made again
        # Off: nothing is made or said, and coop.ini says 0.
        self.api.running = False
        self.api.messages.clear()
        rows['Show Other Players']['set'](False)
        self.assertFalse(plugin.Multiplayer(self.api).settings['puppet'])
        rows['Join Game']['on']()
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['puppet'], '0')
        self.assertFalse(any('models could not be made' in m for m in self.api.messages))

    def test_overlay_toggle_is_saved_and_written(self):
        rows = rows_by_label(self.open('Join')())
        rows['Player Name']['set']('Saria')
        rows["Host's Address"]['set']('26.1.2.3')
        rows['In-Game Overlay']['set'](False)
        self.assertFalse(self.api.settings['overlay'])
        self.assertFalse(plugin.Multiplayer(self.api).settings['overlay'])   # read back
        rows['Join Game']['on']()
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['overlay'], '0')
        self.api.running = False
        rows['In-Game Overlay']['set'](True)
        rows['Join Game']['on']()
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['overlay'], '1')
        # Everything else in the file is as before.
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['share_progress'], '1')   # on unless turned off
        self.assertEqual(len(self.api.plays), 2)

    def test_join_needs_name_and_address(self):
        rows = rows_by_label(self.open('Join')())
        rows['Join Game']['on']()
        self.assertIn('player name', self.api.messages[-1])
        rows['Player Name']['set']('Saria')
        rows['Join Game']['on']()
        self.assertIn("host's address", self.api.messages[-1])
        self.assertEqual(self.api.plays, [])

    def test_join_writes_ini_and_plays(self):
        rows = rows_by_label(self.open('Join')())
        rows['Player Name']['set']('Saria')
        rows["Host's Address"]['set']('26.1.2.3:4000')
        rows['Join Password']['set']('letmein')
        rows['Join Game']['on']()
        self.assertEqual(self.api.messages, [])
        self.assertEqual(self.api.processes, [])   # a join starts no server
        ini = self.mod / 'coop.ini'
        self.assertEqual(plugin.read_coop_ini(ini),
                         {'server': '26.1.2.3', 'port': '4000', 'name': 'Saria', 'password': 'letmein',
                          'share_progress': '1', 'overlay': '1', 'puppet': '1',
                          'follow': 'host', 'join_at_host': '1', 'objects': '1',
                          # A joiner takes the host's save first (on unless turned off).
                          'host_save': '1'})
        self.assertEqual(self.api.plays, [{'env': {'OOT_COOP_INI': str(ini)}, 'dlls': [plugin.DLL]}])
        self.assertFalse(any('letmein' in line for line in self.api.logs))
        # Off: the player's own save, as before.
        self.api.running = False
        rows["Use The Host's Save"]['set'](False)
        self.assertFalse(self.api.settings['host_save'])
        rows['Join Game']['on']()
        self.assertEqual(plugin.read_coop_ini(ini)['host_save'], '0')
        self.assertEqual(len(self.api.plays), 2)
        self.api.plays.pop()
        # Already running: said, and not started twice.
        rows['Join Game']['on']()
        self.assertIn('already running', self.api.messages[-1])
        self.assertEqual(len(self.api.plays), 1)

    def test_host_starts_server_once_and_plays(self):
        rows = rows_by_label(self.open('Host')())
        rows['Player Name']['set']('Saria')
        rows['Join Password']['set']('letmein')
        rows['Start Hosting']['on']()
        self.assertEqual(len(self.api.processes), 1)
        started = self.api.processes[0]
        self.assertEqual(started['cmd'], [str(self.mod / plugin.SERVER_EXE), '--port', '27510',
                                          '--password-env', plugin.PASSWORD_ENV])
        self.assertTrue(started['console'])
        self.assertEqual(started['env'][plugin.PASSWORD_ENV], 'letmein')
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['server'], '127.0.0.1')
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['password'], 'letmein')
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['host_save'], '0')   # a host never
        self.assertEqual(len(self.api.plays), 1)
        self.assertIn('26.5.6.7', self.api.messages[-1])
        self.assertEqual(self.copied, ['192.168.1.20'])
        # The main menu now offers to stop it.
        self.api.home_rows[0]['on']()
        self.assertIn('Stop Server', rows_by_label(self.api.menus[-1][1]()))
        # The game closed; the server stays (the kit's way). Hosting again starts no second one.
        self.api.running = False
        self.api.exit_callbacks[0](0)
        self.assertFalse(started['process'].terminated)
        rows['Start Hosting']['on']()
        self.assertEqual(len(self.api.processes), 1)
        self.assertEqual(len(self.api.plays), 2)
        # With the toggle on it stops with the game.
        rows['Stop Server With Game']['set'](True)
        self.api.running = False
        self.api.exit_callbacks[0](0)
        self.assertTrue(started['process'].terminated)

    def test_host_plays_on_the_multiplayer_save_chosen_in_the_game(self):
        """The game's Multiplayer page keeps the save chosen for hosting as "mp_save" (a folder of
        coop-saves in the mod folder): Start Hosting gives the game that folder as its save
        folder, so the player's own saves are not written. None chosen, or gone: the game is
        started as before, and the log says so. A joiner never gets one."""
        rows = rows_by_label(self.open('Host')())
        rows['Player Name']['set']('Saria')
        rows['Start Hosting']['on']()
        self.assertEqual(self.api.plays[-1]['env'], {'OOT_COOP_INI': str(self.mod / 'coop.ini')})
        self.assertTrue(any('no multiplayer save chosen yet' in line for line in self.api.logs), self.api.logs)
        # Chosen in the game since (the game writes the settings file while the launcher is open).
        (self.mod / 'coop-saves' / 'Zelda' / 'save').mkdir(parents=True)
        self.api.settings['mp_save'] = 'Zelda'
        self.api.running = False
        rows['Start Hosting']['on']()
        self.assertEqual(self.api.plays[-1]['env'], {'OOT_COOP_INI': str(self.mod / 'coop.ini'),
                                                     'NX2NAVI_SAVE_DIR': str(self.mod / 'coop-saves' / 'Zelda')})
        self.assertTrue(any('hosting on the multiplayer save "Zelda"' in line for line in self.api.logs))
        # A name that is no folder of coop-saves is not followed anywhere.
        for bad in ('Gone', '..', 'a/b', '', 7):
            self.api.settings['mp_save'] = bad
            self.assertIsNone(self.p.host_save_dir(), bad)
        self.api.settings['mp_save'] = 'Zelda'
        # Join: the host's save, never ours.
        self.api.running = False
        rows = rows_by_label(self.open('Join')())
        rows["Host's Address"]['set']('192.168.1.20')
        rows['Join Game']['on']()
        self.assertNotIn('NX2NAVI_SAVE_DIR', self.api.plays[-1]['env'])

    def test_the_puppet_pack_is_made_when_the_launcher_starts(self):
        """For the players who host and join only inside the game: with "Show Other Players" on
        and no pack, registering the add-on makes it in the background, without a word on screen."""
        (self.mod / 'coop_puppet.json').unlink()
        made = []
        real = plugin.make_puppet_pack
        plugin.make_puppet_pack = lambda mod_dir, game_dir, log: made.append((Path(mod_dir), Path(game_dir))) or ''
        try:
            p = plugin.register(self.api)
            p.pack_thread.join(10)
            self.assertEqual(made, [(self.mod, self.api.game_dir)])
            self.assertTrue(any('puppet pack made' in line for line in self.api.logs))
            self.assertEqual(self.api.messages, [])
            # Off, or already there: nothing is started.
            self.api.settings = {'puppet': False}
            self.assertIsNone(plugin.register(self.api).pack_thread)
        finally:
            plugin.make_puppet_pack = real
        self.assertIsNone(self.p.pack_thread)   # setUp's pack was there

    def test_what_the_games_own_menu_keeps_survives(self):
        """The game's Multiplayer page writes into the same settings file: its language, and
        the passwords typed there under Windows' data protection. This menu keeps those keys
        through its own saves, uses such a password, and never writes it out as clear text."""
        self.api.settings = {'name': 'Saria', 'menu_language': 'fr', 'menu_language_game': 'fr',
                             'join_password': '', 'join_password_protected': 'QUJD',
                             'server_password_protected': 'REVG', 'host_address': '26.1.2.3'}
        real = plugin.unprotect
        plugin.unprotect = lambda stored: {'QUJD': 'from-the-game', 'REVG': ''}.get(stored, '')
        try:
            p = plugin.Multiplayer(self.api)
        finally:
            plugin.unprotect = real
        p.addresses = lambda: []
        p.register()
        self.assertEqual(p.settings['join_password'], 'from-the-game')
        self.assertEqual(p.settings['server_password'], '')   # not readable here: none
        p.set_name('Mido')
        self.assertEqual(self.api.settings['name'], 'Mido')
        self.assertEqual(self.api.settings['menu_language'], 'fr')
        self.assertEqual(self.api.settings['join_password_protected'], 'QUJD')
        self.assertEqual(self.api.settings['join_password'], '')
        self.assertNotIn('from-the-game', repr(self.api.settings))
        p.join()
        self.assertEqual(plugin.read_coop_ini(self.mod / 'coop.ini')['password'], 'from-the-game')
        # Changed here: this menu's value holds, the game's kept one goes.
        p.set_join_password('typed-here')
        self.assertEqual(self.api.settings['join_password'], 'typed-here')
        self.assertNotIn('join_password_protected', self.api.settings)
        self.assertEqual(self.api.settings['menu_language'], 'fr')
        # Not Windows' own data, or nothing: no password, no error.
        self.assertEqual(plugin.unprotect(''), '')
        self.assertEqual(plugin.unprotect('not base64 !'), '')
        self.assertEqual(plugin.unprotect('QUJD'), '')

    def test_host_failures_are_said(self):
        rows = rows_by_label(self.open('Host')())
        rows['Player Name']['set']('Saria')
        self.api.process_exit = 1                 # the server stops at once
        rows['Start Hosting']['on']()
        self.assertIn('stopped at once', self.api.messages[-1])
        self.assertEqual(self.api.plays, [])
        self.api.process_exit = None
        (self.mod / plugin.SERVER_EXE).unlink()   # no server program
        rows['Start Hosting']['on']()
        self.assertIn('server is missing', self.api.messages[-1])
        (self.mod / plugin.SERVER_EXE).write_bytes(b'')
        (self.mod / plugin.DLL).unlink()          # no DLL
        rows['Start Hosting']['on']()
        self.assertIn('DLL is missing', self.api.messages[-1])
        self.assertEqual(len(self.api.processes), 1)
        (self.mod / plugin.DLL).write_bytes(b'')
        self.api.play_result = False              # the game does not start
        rows['Start Hosting']['on']()
        self.assertIn('did not start', self.api.messages[-1])
        self.p.settings['server_allow'] = 'friends'
        self.p.server = None
        self.api.processes.clear()
        rows['Start Hosting']['on']()
        self.assertIn('server_allow', self.api.messages[-1])
        self.assertEqual(self.api.processes, [])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:])
