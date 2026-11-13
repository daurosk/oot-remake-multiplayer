# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the built coop_nx2navi.dll against the built server, without the game.

    python nx2navi/build.py --tests      builds everything and runs this
    python nx2navi/tests/test_dll.py     runs it again

Each case starts the real server on a free local UDP port and one or two stand-in
game processes (fake_game.cpp) that load the DLL as the launcher does, then reads
the DLL's log. Nothing of the real game or the installed NX2navi is started.
"""

import math
import os
import time
import re
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / 'build' / 'nx2navi' / 'bin'
DLL = BIN / 'coop_nx2navi.dll'
SERVER = BIN / 'oot-coop-server.exe'
DEFAULT_PASSWORD = 'oot-coop-open'   # the open word of a session without a password (settings.hpp)
FAKE = BIN / 'coop_nx2navi_fake_game.exe'
# The stand-in built as a game with NX2navi's plugin interface (fake_game.cpp,
# COOP_FAKE_PLUGIN_HOST): it records what the DLL draws.
HOST = BIN / 'coop_nx2navi_fake_host.exe'
PANEL_PEER = r'P\d {name}: spot00, (\d+\.\d) units, (north|north-east|east|south-east|south|south-west|west|north-west|right here)'


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Case(unittest.TestCase):
    def setUp(self):
        for program in (DLL, SERVER, FAKE, HOST):
            if not program.is_file():
                self.skipTest(f'{program} is not built: run nx2navi/build.py --tests')
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.port = free_udp_port()
        self.server = None
        self.server_log = self.root / 'server.txt'

    def tearDown(self):
        self.stop_server()
        self.tmp.cleanup()

    def start_server(self, password=''):
        password = password or DEFAULT_PASSWORD   # the server starts only with one
        args = [str(SERVER), '--port', str(self.port), '--bind', '127.0.0.1', '--run-for-ms', '60000']
        env = dict(os.environ)
        if password:
            env['OOT_COOP_SERVER_PASSWORD'] = password
            args += ['--password-env', 'OOT_COOP_SERVER_PASSWORD']
        self.server_out = open(self.server_log, 'w')
        self.server = subprocess.Popen(args, stdout=self.server_out, stderr=subprocess.STDOUT, env=env)

    def stop_server(self):
        if self.server is not None:
            self.server.terminate()
            self.server.wait(10)
            self.server_out.close()
            self.server = None

    def player(self, name, *fake_args, password='', share=False, server='127.0.0.1', exe=FAKE,
               overlay=None, puppet=None, more=(), idle=False, env_more=None):
        """Starts one stand-in game with its own mod folder. Returns (process, folder).
        exe=HOST starts the one with the plugin interface; its drawing goes to
        <folder>/draw.txt."""
        folder = self.root / name
        (folder / 'bin').mkdir(parents=True, exist_ok=True)
        dll = folder / 'bin' / DLL.name
        dll.write_bytes(DLL.read_bytes())
        lines = [f'server = {server}', f'port = {self.port}', f'name = {name}',
                 f'share_progress = {1 if share else 0}']
        if overlay is not None:
            lines.append(f'overlay = {overlay}')
        if puppet is not None:
            lines.append(f'puppet = {puppet}')
        if password:
            lines.append(f'password = {password}')   # none: the DLL sends the open word
        lines += list(more)
        (folder / 'coop.ini').write_bytes(('\r\n'.join(lines) + '\r\n').encode('utf-8'))
        env = dict(os.environ)
        env.pop('OOT_COOP_INI', None)
        for name in ('OOT_COOP_SAVE_READY', 'OOT_COOP_PASSWORD', 'OOT_COOP_LANG', 'NX2NAVI_LANG', 'NX2NAVI_LOCALE'):
            env.pop(name, None)
        # No game files here, so no puppet pack: taken as made (one case looks without it).
        env['OOT_COOP_ASSUME_PACK'] = '1'
        if not idle:
            # As the launcher's Host and Join start the game: the session is named. Without
            # it the DLL idles (the launcher's normal Play), whatever coop.ini says.
            env['OOT_COOP_INI'] = str(folder / 'coop.ini')
        env.update(env_more or {})
        args = [str(exe), str(dll), *fake_args]
        if exe == HOST:
            args += ['--draw-out', str(folder / 'draw.txt')]
        process = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding='utf-8', errors='replace')
        return process, folder

    def finish(self, process, folder):
        """Waits for the stand-in game; returns (exit code, its output, the DLL's log)."""
        out, _ = process.communicate(timeout=60)
        log = folder / 'coop-nx2navi.log'
        text = log.read_text(encoding='utf-8') if log.is_file() else ''
        return process.returncode, out, text

    @staticmethod
    def drawn(folder):
        """{'pictures': {seconds: [lines]}, 'last': [lines], 'texts': [distinct texts]}
        from the plugin host's draw.txt."""
        result = {'pictures': {}, 'last': [], 'texts': []}
        path = folder / 'draw.txt'
        if not path.is_file():
            return result
        current = None
        for line in path.read_text(encoding='utf-8').splitlines():
            m = re.match(r'== picture at ([\d.]+) s ==$', line)
            if m:
                current = result['pictures'].setdefault(float(m.group(1)), [])
            elif line == '== last picture ==':
                current = result['last']
            elif line == '== texts ==':
                current = result['texts']
            elif current is not None:
                current.append(line)
        return result

    @staticmethod
    def texts_of(picture):
        """The texts of one recorded picture, top to bottom."""
        return [line.split(' | ', 1)[1] for line in picture if line.startswith('text ')]

    def test_no_game_memory(self):
        """Not the game: it joins the session, sends nothing about a player, never
        crashes, says goodbye and unloads."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--no-game', '--seconds', '5'))
        self.assertEqual(code, 0, out + log)
        self.assertIn('unloaded cleanly', out)
        self.assertIn('best state 2', out)
        self.assertIn('coop: on, server 127.0.0.1:', log)
        self.assertIn('coop: welcomed as player 1', log)
        self.assertIn('game not found yet', log)
        self.assertNotIn('game found', log)
        self.assertNotIn('player found', log)
        self.assertRegex(log, r'coop: status: player 1 of 1.*game not found')
        self.assertRegex(log, r'coop: stopped; \d+ frames, 0 with the player, 0 messages sent')
        frames = int(re.search(r'stopped; (\d+) frames', log).group(1))
        self.assertGreater(frames, 200)   # about 60 a second for 5 s
        self.assertLess(frames, 400)
        self.stop_server()
        server = self.server_log.read_text()
        self.assertIn('listening on port', server)

    def test_two_players_see_each_other(self):
        """With a stand-in for main where NX2navi maps it: the game is found by its
        build id, the player is read and sampled, and each side's presence gets the
        other's scene and position through the real server."""
        self.start_server()
        a = self.player('Saria', '--seconds', '14', '--radius', '3')
        b = self.player('Mido', '--seconds', '14', '--radius', '8')
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        for log, other in ((log_a, 'Mido'), (log_b, 'Saria')):
            self.assertIn('game found: main at 0x7000000000', log)
            self.assertIn('build id E0F3CCFAF50065AE', log)
            self.assertIn('player found in spot00', log)
            self.assertRegex(log, rf"player \d '{other}' joined|welcomed as player 2")
            # The other player's scene and distance came through presence.
            self.assertRegex(log, rf'status: .*you: spot00.*P\d {other}: spot00, \d+\.\d units away')
            sent = int(re.search(r'stopped; \d+ frames, \d+ with the player, (\d+) messages sent',
                                 log).group(1))
            self.assertGreater(sent, 300)   # a sample a frame while walking
        distance = float(re.findall(r'P\d Mido: spot00, (\d+\.\d) units away', log_a)[-1])
        self.assertGreater(distance, 3.0)   # circles of radius 3 and 8 at about the same angle
        self.assertLess(distance, 12.0)

    def test_other_game_build(self):
        """main of another build: said once, and nothing is read or sent."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--other-build', '--seconds', '6'))
        self.assertEqual(code, 0, out + log)
        self.assertIn('is not supported', log)
        self.assertEqual(log.count('is not supported'), 1)
        self.assertIn('welcomed as player 1', log)
        self.assertRegex(log, r'stopped; \d+ frames, 0 with the player, 0 messages sent')

    def test_wrong_password_is_refused(self):
        self.start_server(password='right-one')
        code, out, log = self.finish(*self.player('Saria', '--no-game', '--seconds', '4',
                                                  password='wrong-one'))
        self.assertEqual(code, 0, out + log)
        self.assertIn('coop: server refused: wrong join password', log)
        self.assertNotIn('welcomed', log)
        self.assertNotIn('right-one', log)
        self.assertNotIn('wrong-one', log)   # the password is never logged

    def test_right_password_and_shared_progress_without_a_game(self):
        """Shared progress on, but no game: the flag code never touches memory."""
        self.start_server(password='right-one')
        code, out, log = self.finish(*self.player('Saria', '--no-game', '--seconds', '4',
                                                  password='right-one', share=True))
        self.assertEqual(code, 0, out + log)
        self.assertIn('shared progress on', log)
        self.assertIn('welcomed as player 1', log)
        self.assertRegex(log, r'stopped; \d+ frames, 0 with the player, 0 messages sent')

    def test_shared_progress_with_a_game_that_has_no_flags(self):
        """Shared progress on and a game with no flag manager: every flag read
        fails quietly, presence still runs."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--seconds', '5', share=True))
        self.assertEqual(code, 0, out + log)
        self.assertIn('player found in spot00', log)
        self.assertRegex(log, r'stopped; \d+ frames, [1-9]\d+ with the player')

    def test_no_server_keeps_trying_and_no_ini_is_off(self):
        # Nothing listens on the port: it keeps trying, and still stops cleanly.
        code, out, log = self.finish(*self.player('Saria', '--no-game', '--seconds', '3'))
        self.assertEqual(code, 0, out + log)
        self.assertIn('coop: on, server', log)
        self.assertNotIn('welcomed', log)
        self.assertIn('best state 1', out)
        # A bad coop.ini: off, with the line named.
        process, folder = self.player('Mido', '--no-game', '--seconds', '1', server='bad address')
        code, out, log = self.finish(process, folder)
        self.assertEqual(code, 0, out + log)
        self.assertIn('coop.ini line 1', log)
        self.assertIn('coop: off', log)
        self.assertIn('best state 0', out)

    # --- A game with NX2navi's plugin interface (the fake host) ---

    def test_overlay_panel_two_players_in_one_scene(self):
        """Both games have the interface (nx2navi_plugin_init): the panel shows the
        connection, our scene and the other player's scene with the distance and a
        world-axis direction. F9 hides and shows it. Mido's DLL is loaded twice and
        offered the interface twice, which changes nothing."""
        self.start_server()
        a = self.player('Saria', '--seconds', '14', '--radius', '3', '--snapshot', '8',
                        '--key', '9', 'f9', '--snapshot', '10', '--key', '11', 'f9',
                        '--snapshot', '12', exe=HOST)
        b = self.player('Mido', '--seconds', '14', '--radius', '8', '--twice', exe=HOST)
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        for log, out in ((log_a, out_a), (log_b, out_b)):
            self.assertEqual(log.count('plugin interface version 1 from nx2navi_plugin_init (game 111'),
                             1)
            self.assertEqual(log.count('overlay: on'), 1)
            self.assertEqual(log.count('coop-dll 0.9 in'), 1)   # one thread, one log
            self.assertIn('overlay: on (bottom left; F9 hides and shows it, Enter opens the chat box)',
                          log)
            # This stand-in serves interface version 1: no function hooks, so no map markers.
            self.assertEqual(log.count('minimap: off (it needs the plugin interface version 2)'), 1)
            # The puppets' frame callback (puppet = 1 unless coop.ini says 0). With another
            # player in our scene the director asks for its first game call, the game's
            # malloc; this stand-in has no game function, refuses it, and puppets go off.
            self.assertIn('callbacks frame 1 overlay 1 key 1 text 1; input captured 0', out)
            self.assertEqual(log.count('game thread: frame callback registered'), 1)
            self.assertIn('guest call of main+0x14c6c90 refused', out)
            self.assertEqual(log.count('puppets are off for this run'), 1)
            self.assertIn('callbacks left after stop 0, input captured 0', out)
            self.assertIn('game log: co-op overlay on', out)
            self.assertIn('unloaded cleanly', out)
            # Co-op itself is what it was without the interface.
            self.assertIn('player found in spot00', log)
            self.assertRegex(log, r'status: .*you: spot00.*P\d (Mido|Saria): spot00, \d+\.\d units away')
        drawn = self.drawn(a[1])
        for at in (8.0, 12.0):
            picture = drawn['pictures'][at]
            texts = self.texts_of(picture)
            # the background: at the bottom left, 16 from the left and from the bottom of 1080
            box = picture[0].split()
            self.assertEqual(box[:2] + box[4:5], ['rect_filled', '16', '1064'], picture)
            self.assertRegex(picture[0], r' [0-9a-f]{2}000000$')                   # black, see-through
            self.assertNotRegex(picture[0], r' ff000000$')
            self.assertRegex(texts[0], r'^Co-op: connected as P\d "Saria"$')
            self.assertEqual(texts[1], 'You: spot00')
            peer = re.fullmatch(PANEL_PEER.format(name='Mido'), texts[2])
            self.assertIsNotNone(peer, texts)
            distance = float(peer.group(1))
            self.assertGreater(distance, 3.0)   # circles of radius 3 and 8
            self.assertLess(distance, 12.0)
            self.assertEqual(texts[-1], 'Enter: chat   F9: hide')
        self.assertEqual(self.texts_of(drawn['pictures'][10.0]), [])   # hidden with F9
        self.assertEqual(drawn['pictures'][10.0], [])
        # The join notice was drawn, and the other side drew us.
        self.assertTrue(any(re.fullmatch(r'Mido joined|Connected as P\d', t) for t in drawn['texts']))
        self.assertTrue(any(re.fullmatch(PANEL_PEER.format(name='Saria'), t)
                            for t in self.drawn(b[1])['texts']))

    def test_overlay_chat_round_trip(self):
        """Enter opens the box and captures the input, typed text goes in, Backspace
        takes a character out, Esc cancels, Enter sends with the protocol's CHAT; the
        other game shows it as a notice and in the history of its open box. One game
        got the interface from nx2navi_plugin_init, the other from the export."""
        self.start_server()
        a = self.player('Saria', '--seconds', '16',
                        '--key', '5', 'enter', '--type', '5.3', 'hello from Saria é',
                        '--key', '6', 'enter',
                        '--key', '7', 'enter', '--type', '7.2', 'never sent', '--key', '7.6', 'esc',
                        '--key', '8', 'enter', '--type', '8.2', 'abcd', '--key', '8.5', 'backspace',
                        '--key', '8.7', 'enter',
                        '--key', '12', 'enter', '--snapshot', '12.5', '--key', '13', 'esc',
                        exe=HOST)
        b = self.player('Mido', '--seconds', '16', '--no-init',
                        '--key', '10', 'enter', '--type', '10.2', 'hi Saria', '--key', '10.6', 'enter',
                        '--snapshot', '10.3', exe=HOST)
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        self.assertIn('plugin interface version 1 from nx2navi_plugin_init', log_a)
        self.assertIn("plugin interface version 1 from the game's export", log_b)
        # Sent and received, in both logs; the cancelled and corrected text never went.
        self.assertIn('coop: chat: Saria: hello from Saria é', log_a)
        self.assertIn('coop: chat: Saria: hello from Saria é', log_b)
        self.assertIn('coop: chat: Saria: abc\n', log_b)
        self.assertIn('coop: chat: Mido: hi Saria', log_a)
        for log in (log_a, log_b):
            self.assertNotIn('never sent', log)
            self.assertNotIn('abcd', log)
        # The input was captured for each box and given back every time.
        for out in (out_a, out_b):
            self.assertIn('input captured 0', out)
            self.assertIn('callbacks left after stop 0, input captured 0', out)
        self.assertIn('(8 capture calls)', out_a)   # 4 boxes, each opened and closed
        self.assertIn('(2 capture calls)', out_b)
        # A drew the reply as a notice, and its open box shows the history.
        drawn_a = self.drawn(a[1])
        self.assertIn('Mido: hi Saria', drawn_a['texts'])
        box = self.texts_of(drawn_a['pictures'][12.5])
        self.assertEqual(box[-2:], ['> _', 'Enter sends, Esc cancels'])
        history = box[box.index('> _') - 3:box.index('> _')]
        self.assertEqual(history, ['Saria: hello from Saria é', 'Saria: abc', 'Mido: hi Saria'])
        # B, while typing: its box with the text so far; and the notice of A's line.
        drawn_b = self.drawn(b[1])
        self.assertTrue(any(t.startswith('> hi') and t.endswith('_')
                            for t in self.texts_of(drawn_b['pictures'][10.3])))
        self.assertIn('Saria: hello from Saria é', drawn_b['texts'])

    def test_overlay_off_in_coop_ini(self):
        """overlay = 0: the interface is there but nothing is drawn and no key is taken. The
        game-thread callback is registered all the same (teleport needs it, puppets or not)."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--seconds', '4', '--key', '2', 'enter',
                                                  exe=HOST, overlay=0, puppet=0))
        self.assertEqual(code, 0, out + log)
        self.assertIn('overlay: off (overlay = 0 in coop.ini)', log)
        self.assertIn('puppet: off (puppet = 0 in coop.ini)', log)
        self.assertNotIn('overlay: on', log)
        self.assertIn('welcomed as player 1', log)
        self.assertIn('player found in spot00', log)
        self.assertIn('callbacks frame 1 overlay 0 key 0 text 0; input captured 0 (0 capture calls)', out)
        self.assertNotIn('guest call of', out)
        self.assertEqual(self.drawn(self.root / 'Saria')['texts'], [])

    def test_puppet_off_in_coop_ini(self):
        """puppet = 0: the overlay's callbacks are there; the frame callback too, for
        teleport alone (the puppet director is never asked)."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--seconds', '4', exe=HOST, puppet=0))
        self.assertEqual(code, 0, out + log)
        self.assertIn('puppet: off (puppet = 0 in coop.ini)', log)
        self.assertIn('game thread: frame callback registered', log)
        self.assertNotIn('puppet: on', log)
        self.assertIn('callbacks frame 1 overlay 1 key 1 text 1', out)
        self.assertNotIn('guest call of', out)

    def test_no_puppet_pack_no_puppets(self):
        """A player who never passed the launcher's Host or Join has no puppet pack: the other
        players are then not made as Link (the game has no such actor), and the log says how
        to get them."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--seconds', '4', exe=HOST,
                                                  env_more={'OOT_COOP_ASSUME_PACK': '0'}))
        self.assertEqual(code, 0, out + log)
        self.assertEqual(log.count('puppet: off: no puppet pack in the mod folder yet'), 1)
        self.assertNotIn('puppet: on', log)
        self.assertIn('welcomed as player 1', log)
        # With a pack made for this port in the mod folder: on.
        folder = self.root / 'Mido'
        actors = folder / 'romfs' / 'Pack' / 'Actor'
        actors.mkdir(parents=True)
        for name in ('CoopPuppet_Child.pack.zs', 'CoopPuppet_Adult.pack.zs'):
            (actors / name).write_bytes(b'pack')
        (folder / 'coop_puppet.json').write_text('{"pouch": false}', encoding='utf-8')
        code, out, log = self.finish(*self.player('Mido', '--seconds', '4', exe=HOST,
                                                  env_more={'OOT_COOP_ASSUME_PACK': '0'}))
        self.assertEqual(code, 0, out + log)
        self.assertNotIn('no puppet pack', log)
        self.assertIn('puppet: on', log)
        # The emulator kit's pack (the puppets wear the local game's gear) is not one for this port.
        (folder / 'coop_puppet.json').write_text('{"pouch": true}', encoding='utf-8')
        log = self.finish(*self.player('Mido', '--seconds', '3', exe=HOST,
                                       env_more={'OOT_COOP_ASSUME_PACK': '0'}))[2]
        self.assertIn('puppet: off: no puppet pack in the mod folder yet', log)

    def test_puppet_alone_registers_the_frame_callback(self):
        """overlay = 0, puppet = 1: only the frame callback; alone in the room nothing is
        asked of the game."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--seconds', '4', exe=HOST, overlay=0))
        self.assertEqual(code, 0, out + log)
        self.assertIn('game thread: frame callback registered', log)
        self.assertIn('teleport: on (/teleport <player> in chat); join at the host: on; follow through '
                      'exits: anyone', log)
        self.assertIn('callbacks frame 1 overlay 0 key 0 text 0', out)
        self.assertNotIn('guest call of', out)
        self.assertIn('callbacks left after stop 0', out)

    def test_join_at_the_host_and_the_teleport_command(self):
        """The joiner's Link is put where the host stands, once, by itself; the host then
        types /teleport and is put where the joiner stands. Each move is the position
        written into Link's actor and the actor's set-matrix function called on the
        game's thread; the command is not sent as chat. The stand-in walks on, so its
        Link is back on its circle a frame later: what is checked is the write and call."""
        self.start_server()
        a = self.player('Saria', '--seconds', '16', '--radius', '3',
                        '--key', '11', 'enter', '--type', '11.2', '/teleport mi', '--key', '11.8', 'enter',
                        exe=HOST)
        time.sleep(1.5)   # Saria is in first: the server names her the host
        b = self.player('Mido', '--seconds', '14.5', '--radius', '8', exe=HOST)
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        self.assertIn('status: player 1 of', log_a)

        def moves(out):
            return [math.hypot(float(x), float(z)) for x, _, z in re.findall(
                r'guest call of main\+0x4c6580 \(set matrix\) with Link at (\S+) (\S+) (\S+)', out)]

        # The join: once, by itself, to the host's circle (radius 3), from Mido's (radius 8).
        self.assertEqual(log_b.count('teleport: joining Saria (the host)'), 1, log_b)
        self.assertEqual(log_b.count('teleport: next to Saria'), 1, log_b)
        self.assertRegex(log_a, r'teleport: Mido asked to come here: spot00 at ')
        self.assertEqual(len(moves(out_b)), 1, out_b)
        # Beside the host, not on its spot: 1.3 behind and 0.5 aside of where it stood, facing as it did.
        self.assertAlmostEqual(moves(out_b)[0], 3.0, delta=1.6)
        self.assertRegex(log_b, r'teleport: moved Link to \S+ \S+ \S+ \(beside the player, facing as they do\)')
        at = re.search(r'teleport: Mido asked to come here: spot00 at (\S+) (\S+) (\S+)', log_a)
        to = re.search(r'teleport: moved Link to (\S+) (\S+) (\S+)', log_b)
        apart = math.hypot(float(at.group(1)) - float(to.group(1)), float(at.group(3)) - float(to.group(3)))
        self.assertAlmostEqual(apart, math.hypot(1.3, 0.5), delta=0.15)
        self.assertAlmostEqual(float(at.group(2)), float(to.group(2)), delta=0.11)
        # The host joins nobody by itself; its one move is the command's, to Mido's circle.
        self.assertNotIn('teleport: joining', log_a)
        self.assertEqual(log_a.count('teleport: next to Mido'), 1, log_a)
        self.assertEqual(len(moves(out_a)), 1, out_a)
        self.assertAlmostEqual(moves(out_a)[0], 8.0, delta=1.6)
        self.assertRegex(log_b, r'teleport: Saria asked to come here: spot00 at ')
        self.assertNotIn('chat: Saria', log_b)   # a command, never chat
        self.assertNotIn('chat: Saria: /teleport', log_a)
        # Both in one scene the whole time: nobody "took an exit".
        self.assertNotIn('took an exit', log_a + log_b)
        # The puppet director's refused call (this stand-in has no malloc) did not stop teleport.
        self.assertIn('puppets are off for this run', log_b)

    def test_interface_version_5_camera_reset_language_and_no_caption_hook(self):
        """A game with interface version 5: after the joiner was put beside the host the mod
        asks for the game's own camera reset; the page's language is the one the game says it
        runs in, whatever the environment says; and the mod hooks nothing for the caption
        (the runtime's update_menu sets it). A version 4 game gets no reset and reads the
        language itself."""
        self.start_server()
        a = self.player('Saria', '--seconds', '12', '--radius', '3', '--v5', exe=HOST)
        time.sleep(1.5)   # Saria is in first: the server names her the host
        b = self.player('Mido', '--seconds', '10.5', '--radius', '8', '--v5', exe=HOST,
                        env_more={'NX2NAVI_LANG': 'en-US'})
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        self.assertIn('(beside the player, facing as they do)', log_b)
        self.assertIn("teleport: the camera is put behind Link (the plugin interface's camera_reset)", log_b)
        self.assertEqual(out_b.count('fake_host: camera reset asked'), 1, out_b)
        self.assertNotIn('camera reset asked', out_a)           # the host was not moved
        self.assertIn("menu: language French (the game's: fr)", log_b)   # not NX2NAVI_LANG's English
        self.assertNotIn('hook of main+0x34031d0', out_a + out_b)
        # A version 4 game: no reset, and the language as the mod reads the settings itself.
        c = self.player('Ruto', '--seconds', '6', '--v4', exe=HOST, env_more={'NX2NAVI_LANG': 'en-US'})
        code_c, out_c, log_c = self.finish(*c)
        self.assertEqual(code_c, 0, out_c + log_c)
        self.assertNotIn('camera reset asked', out_c)
        self.assertIn("menu: language English (the game's: en)", log_c)

    def test_join_at_host_and_follow_off_in_coop_ini(self):
        """join_at_host = 0 and follow = off: said in the log, and the joiner stays put."""
        self.start_server()
        a = self.player('Saria', '--seconds', '9', exe=HOST)
        time.sleep(1.5)
        b = self.player('Mido', '--seconds', '7.5', '--radius', '8', exe=HOST,
                        more=('join_at_host = 0', 'follow = off'))
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        self.assertIn('teleport: on (/teleport <player> in chat); join at the host: off; follow through '
                      'exits: off', log_b)
        self.assertNotIn('teleport: joining', log_b)
        self.assertNotIn('main+0x4c6580', out_b)

    # --- Interface version 3: the title screen's Multiplayer entry (the fake host with --v3) ---

    @staticmethod
    def pages(out):
        """The pages the stand-in printed: [(what, title, [row texts])], what = 'open' or 'update'."""
        found = []
        for line in out.splitlines():
            m = re.match(r'fake_host: menu (open|update): (.*)$', line)
            if m:
                parts = m.group(2).split(' || ')
                found.append((m.group(1), parts[0], parts[1:]))
        return found

    def test_idle_until_the_player_hosts_or_joins(self):
        """Started as the launcher's normal Play starts the game (the DLL in mod.json, no
        OOT_COOP_INI): the title menu entry and nothing else, whatever coop.ini says."""
        self.start_server()
        game = self.root / 'Saria' / 'game'
        game.mkdir(parents=True)
        (game / 'nx2navi.ini').write_text('[language]\nlang = fr\n', encoding='utf-8')
        code, out, log = self.finish(*self.player('Saria', '--seconds', '4', '--v3', '--game-dir', str(game),
                                                  exe=HOST, idle=True))
        self.assertEqual(code, 0, out + log)
        self.assertIn('coop: idle: no session yet', log)
        self.assertIn("menu: language French (the game's: fr)", log)
        self.assertIn("menu: 'Multijoueur' is on the title screen", log)
        self.assertIn("fake_host: title menu entry 'Multijoueur'", out)
        self.assertNotIn('coop: on, server', log)
        self.assertNotIn('welcomed', log)
        self.assertNotIn('game found', log)          # nothing is read
        self.assertNotIn('hook of main', out)        # nothing is hooked
        self.assertIn('best state 0', out)
        self.assertIn('callbacks frame 0 overlay 0 key 0 text 0; input captured 0', out)
        self.assertIn('callbacks left after stop 0', out)
        self.assertIn('unloaded cleanly', out)
        self.stop_server()
        self.assertNotIn('joined', self.server_log.read_text())

    def test_english_for_another_language_and_no_entry_before_version_3(self):
        code, out, log = self.finish(*self.player('Saria', '--seconds', '2', '--v3', exe=HOST, idle=True,
                                                  env_more={'NX2NAVI_LANG': 'de'}))
        self.assertEqual(code, 0, out + log)
        self.assertIn("menu: 'Multiplayer' is on the title screen", log)
        # An older game: said once, and the launcher's Host and Join still work.
        code, out, log = self.finish(*self.player('Mido', '--seconds', '2', exe=HOST, idle=True))
        self.assertEqual(code, 0, out + log)
        self.assertEqual(log.count('menu: no Multiplayer entry on the title screen: it needs '
                                   "NX2navi's plugin interface version 3 (this game serves 1)"), 1)
        self.assertNotIn('title menu entry', out)

    def test_host_from_the_title_menu(self):
        """Multijoueur -> Heberger en reseau local -> the port typed -> Demarrer: the DLL starts
        the mod's own server, connects to it, closes the page, and stops the server when the
        game ends."""
        folder = self.root / 'Saria'
        (folder / 'bin').mkdir(parents=True)
        (folder / 'bin' / SERVER.name).write_bytes(SERVER.read_bytes())
        (folder / 'settings.json').write_text('{\n  "name": "Saria",\n  "puppet": false,\n  "kept": [1, 2]\n}',
                                              encoding='utf-8')
        keys = []
        for i in range(5):
            keys += ['--key', f'{2.5 + i * 0.05:.2f}', 'backspace']
        process, folder = self.player(
            'Saria', '--seconds', '11', '--v3', '--exit',
            '--title', '1.0',
            '--menu', '1.5', '1',                      # Heberger en reseau local
            '--menu', '2.2', '1', *keys,               # Port: 27510 taken out...
            '--type', '3.0', str(self.port), '--key', '3.4', 'enter',   # ...and this run's typed
            '--menu', '3.8', '3',                      # the address to give
            '--menu', '4.3', '6',                      # Demarrer
            '--title', '8.5', '--menu', '9.0', '1', '--menu', '9.3', '6',   # again: already running
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'fr'})
        code, out, log = self.finish(process, folder)
        self.assertEqual(code, 0, out + log)
        pages = self.pages(out)
        self.assertGreaterEqual(len(pages), 5, out)
        what, title, rows = pages[0]
        self.assertEqual((what, title), ('open', 'Multijoueur'))
        self.assertEqual(len(rows), 8)   # only the rows the page needs
        self.assertIn("0 action 'Héberger sur Steam' = 'Indisponible'", rows[0])
        self.assertIn('Pas encore disponible', rows[0])
        self.assertIn("1 action 'Héberger en réseau local' = 'Ouvrir'", rows[1])
        self.assertIn("2 action 'Rejoindre via Steam' = 'Indisponible'", rows[2])
        self.assertIn("3 action 'Rejoindre en réseau local' = 'Ouvrir'", rows[3])
        # A version 3 game cannot be started again on a save folder: no multiplayer saves.
        self.assertIn("4 action 'Sauvegardes multijoueur' = 'Indisponible'", rows[4])
        self.assertEqual(rows[5], '5 separator')
        self.assertIn("6 action 'État' = 'Hors ligne'", rows[6])
        self.assertIn('Lance le serveur co-op sur ce PC et y joue.\\nVos amis rejoignent', rows[1])
        self.assertNotIn('Langue', out)
        what, title, rows = pages[1]
        self.assertEqual((what, title), ('update', 'Héberger en réseau local'))
        self.assertIn("0 text 'Nom' = 'Saria'", rows[0])
        self.assertIn("1 text 'Port' = '27510'", rows[1])
        self.assertIn("2 text 'Mot de passe' = 'aucun'", rows[2])
        self.assertNotIn(' max ', ' '.join(rows))   # a version 3 game: the typing is ours
        self.assertIn("3 action 'Adresse à donner' = '", rows[3])
        self.assertIn("4 action 'Sauvegarde' = 'La vôtre'", rows[4])   # so a host plays on its own
        self.assertIn("6 action 'Démarrer' = 'Démarrer'", rows[6])
        self.assertIn("7 action 'Retour'", rows[7])
        # Typing: a cursor while it is typed, the new port afterwards (and in the address to give).
        self.assertTrue(any(f"1 text 'Port' = '{self.port}_'" in r[1] for _, _, r in pages), out)
        self.assertTrue(any(f"1 text 'Port' = '{self.port}'" in r[1] for _, _, r in pages), out)
        self.assertNotIn('menu update refused', out)
        self.assertIn('fake_host: menu closed', out)
        # No update_menu from another thread than the game's is checked here; the page was
        # closed by the DLL once the session ran.
        self.assertIn(f'server: started on UDP port {self.port}', log)
        self.assertIn(f"coop: on, server 127.0.0.1:{self.port} as 'Saria'", log)
        self.assertIn('coop: welcomed as player 1', log)
        self.assertIn('puppet: off (puppet = 0 in coop.ini)', log)   # the launcher's toggles hold
        # A session started from the page shares enemies unless the player turned them off: the
        # settings file says nothing about them here, so they are on (as time of day and drops are),
        # and the coop.ini the mod writes for the session says so.
        self.assertNotIn('objects: off (objects = 0', log)
        self.assertRegex(log, r'objects: (on \(shared enemies|off: shared enemies need the plugin interface)')
        ini = (folder / 'coop.ini').read_text(encoding='utf-8')
        self.assertIn('objects = 1', ini)
        self.assertNotIn('objects = 0', ini)
        self.assertIn('menu: page closed', log)
        # The second visit: the session is said, and Start does not start another.
        self.assertTrue(any("6 action 'État' = 'Hébergement (J1)'" in r[6] for _, _, r in pages), out)
        self.assertTrue(any('Une session tourne déjà' in r[6] for _, _, r in pages), out)
        self.assertNotIn('restart asked', out)
        self.assertEqual(log.count('server: started on UDP port'), 1)
        # The game ends: goodbye, and the server with it.
        self.assertIn('fake_host: the game ends: 1 exit callbacks', out)
        self.assertIn('coop: the game ends', log)
        self.assertIn('server: stopped with the game', log)
        self.assertIn('callbacks left after stop 0', out)
        self.assertIn('unloaded cleanly', out)
        server_log = (folder / 'coop-server.log').read_text(errors='replace')
        self.assertIn('listening on port', server_log)
        # What the page keeps for the next start, with the launcher's own keys untouched.
        saved = (folder / 'settings.json').read_text(encoding='utf-8')
        self.assertIn(f'"port": {self.port}', saved)
        self.assertIn('"name": "Saria"', saved)
        self.assertIn('"kept": [1, 2]', saved)
        self.assertIn('"puppet": false', saved)
        ini = (folder / 'coop.ini').read_text(encoding='utf-8')
        self.assertIn('server = 127.0.0.1', ini)
        self.assertIn('host_save = 0', ini)

    def test_join_from_the_title_menu_takes_the_hosts_save(self):
        """The host (started by the launcher's Host) has a save; a second game joins from its
        title menu with a password: the host's newest slot arrives BEFORE the session and is
        written into the joiner's co-op profile, its own saves untouched. This stand-in has no
        window to close, so the restart into that profile is refused and the session goes on."""
        self.start_server(password='s3cret')
        account = '0123456789abcdef0123456789abcdef'
        host_profile = self.root / 'host-profile'
        own_profile = self.root / 'own-profile'
        for slot, text, age in (('slot_00', b'old progress', 500), ('slot_02', b'HOST PROGRESS ' * 3000, 5)):
            d = host_profile / 'save' / account / slot
            d.mkdir(parents=True)
            (d / 'progress.sav').write_bytes(text)
            (d / 'caption.sav').write_bytes(b'caption of ' + slot.encode())
            old = time.time() - age
            os.utime(d / 'progress.sav', (old, old))
        own = own_profile / 'save' / account
        (own / 'slot_00').mkdir(parents=True)
        (own / 'slot_00' / 'progress.sav').write_bytes(b'my own progress')
        (own / 'option.sav').write_bytes(b'my options')
        (own / 'account.sav').write_bytes(b'my account')
        (own_profile / 'account.uid').write_bytes(b'uid')
        a = self.player('Host', '--seconds', '16', '--v3', '--profile', str(host_profile), exe=HOST,
                        password='s3cret')
        time.sleep(1.5)
        keys = []
        for i in range(5):
            keys += ['--key', f'{3.0 + i * 0.05:.2f}', 'backspace']
        b = self.player(
            'Mido', '--seconds', '13', '--v3', '--profile', str(own_profile),
            '--title', '1.0',
            '--menu', '1.4', '3',                                              # Join over Local Network
            '--menu', '1.8', '1', '--type', '2.0', 'localhost', '--key', '2.4', 'enter',   # Address
            '--menu', '2.8', '2', *keys, '--type', '3.4', str(self.port), '--key', '3.7', 'enter',
            '--menu', '4.0', '3', '--type', '4.2', 's3cret', '--key', '4.6', 'enter',      # Password
            '--menu', '5.0', '6',                                              # Join
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'en-US'})
        code_b, out_b, log_b = self.finish(*b)
        code_a, out_a, log_a = self.finish(*a)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        pages = self.pages(out_b)
        self.assertEqual(pages[0][1], 'Multiplayer')
        self.assertTrue(any(t == 'Join over Local Network' for _, t, _ in pages), out_b)
        self.assertTrue(any("1 text 'Address' = '127.0.0.1'" in r[1] for _, _, r in pages), out_b)
        self.assertTrue(any("3 text 'Password' = 's3cret_'" in r[3] for _, _, r in pages), out_b)
        self.assertTrue(any("3 text 'Password' = 'set'" in r[3] for _, _, r in pages), out_b)
        self.assertTrue(any("6 action 'Join' = 'Receiving...'" in r[6] for _, _, r in pages), out_b)
        self.assertTrue(any("4 action 'Save' = 'The host's'" in r[4] for _, _, r in pages), out_b)
        # The host answered from its game's save folder: the newest slot.
        self.assertIn('save: player 2 asked for our save', log_a)
        self.assertRegex(log_a, r'save: sending slot_02 \(\d+ bytes, \d+ parts\) to player 2')
        # The joiner: asked before any session, wrote the co-op profile, left its own alone.
        order = [log_b.index(s) for s in ('save: asking the host at 127.0.0.1', 'save: welcomed as player 2',
                                          "save: the host's save is in", 'coop: on, server')]
        self.assertEqual(order, sorted(order), log_b)
        # Kept among the joiner's multiplayer saves, named after the host: this host plays on
        # its own profile's save, so it tells no save name.
        self.assertNotIn("is told this save's name", log_a)
        self.assertIn('save: the host told no name for its save', log_b)
        folder_b = self.root / 'Mido'
        kept = folder_b / 'coop-saves' / '@Host'
        target = kept / 'save' / account
        self.assertEqual((target / 'slot_00' / 'progress.sav').read_bytes(), b'HOST PROGRESS ' * 3000)
        self.assertEqual((target / 'slot_00' / 'caption.sav').read_bytes(), b'caption of slot_02')
        self.assertEqual((target / 'option.sav').read_bytes(), b'my options')
        self.assertEqual((kept / 'account.uid').read_bytes(), b'uid')
        self.assertFalse((folder_b / 'coop-save').exists())
        self.assertEqual((own / 'slot_00' / 'progress.sav').read_bytes(), b'my own progress')
        self.assertEqual(sorted(p.name for p in own.iterdir()), ['account.sav', 'option.sav', 'slot_00'])
        self.assertIn('the game starts again with NX2NAVI_PROFILE_DIR', log_b)   # this stand-in is version 3
        self.assertIn("restart: not possible here (the game's window was not found)", log_b)
        self.assertIn('coop: welcomed as player 2', log_b)
        # The password: in no file as clear text (kept protected for this Windows user).
        saved = (folder_b / 'settings.json').read_text(encoding='utf-8')
        self.assertNotIn('s3cret', saved)
        self.assertIn('"join_password_protected": "', saved)
        self.assertIn('"host_address": "127.0.0.1"', saved)
        self.assertNotIn('s3cret', (folder_b / 'coop.ini').read_text(encoding='utf-8'))
        self.assertIn('host_save = 1', (folder_b / 'coop.ini').read_text(encoding='utf-8'))

    def test_version_4_dims_the_steam_rows_moves_the_cursor_and_draws_name_tags(self):
        """Interface version 4: the Steam entries are disabled rows on the first page (an update
        enables a row again for the next page's fields), a page change says where the cursor
        goes, and each other player in our scene has their name over their head."""
        self.start_server()
        a = self.player('Saria', '--seconds', '12', '--radius', '3', '--v4',
                        '--title', '5.0', '--menu', '5.4', '3', '--menu', '5.8', '7', '--snapshot', '9',
                        exe=HOST, env_more={'NX2NAVI_LANG': 'en-US'})
        b = self.player('Mido', '--seconds', '12', '--radius', '8', '--v4', exe=HOST)
        code_a, out_a, log_a = self.finish(*a)
        code_b, out_b, log_b = self.finish(*b)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        pages = self.pages(out_a)
        self.assertEqual(pages[0][1], 'Multiplayer')
        self.assertIn("0 action 'Host over Steam' = 'Unavailable' (disabled)", pages[0][2][0])
        self.assertIn("2 action 'Join over Steam' = 'Unavailable' (disabled)", pages[0][2][2])
        self.assertNotIn('(disabled)', pages[0][2][1])
        # A session runs (the launcher started this game with one): the first page says so.
        self.assertRegex(pages[0][2][6], r"6 action 'Status' = '(Hosting|Connected) \(P\d\)'")
        # Version 4 can start the game again on a save folder: the multiplayer saves are there.
        self.assertIn("4 action 'Multiplayer saves' = 'None' [", pages[0][2][4])
        # Join's page: every row enabled again, the cursor on its first row.
        self.assertEqual(pages[1][1], 'Join over Local Network')
        self.assertFalse(any('(disabled)' in row for row in pages[1][2][:9]), pages[1])
        self.assertEqual(pages[1][2][-1], 'cursor to row 0')
        # Back: the first page, the cursor on the entry the player came through.
        self.assertEqual(pages[2][1], 'Multiplayer')
        self.assertEqual(pages[2][2][-1], 'cursor to row 3')
        for log in (log_a, log_b):
            self.assertIn("name tags: on (each other player's name over their head, in their colour)", log)
        # Mido walks a circle of radius 8 at height 2: the tag is its name alone, over the head,
        # centred on the point the camera gives (x = 960 + 10 * world x), inside the picture.
        picture = self.drawn(self.root / 'Saria')['pictures'][9.0]
        tags = [line for line in picture if line.endswith(' | Mido')]
        self.assertEqual(len(tags), 1, picture)
        m = re.match(r'text (-?\d+) (-?\d+) (\d+) a=(\d+) \| Mido$', tags[0])
        self.assertTrue(m, tags[0])
        x, y, size, alpha = (int(v) for v in m.groups())
        self.assertTrue(225 <= alpha <= 255, alpha)   # at most 11 units apart: hardly faded
        self.assertTrue(850 <= x <= 1025, x)      # centred on 960 + 10 * x, x within the circle
        self.assertTrue(465 <= y <= 487, y)       # over 540 - 10 * (2 + 1.9), by the text and its plate
        # An older game: no camera, no tags, said once.
        self.assertNotIn('name tags: on', self.finish(*self.player('Zelda', '--seconds', '3', '--v3', exe=HOST))[2])

    def test_version_4_takes_typing_itself_and_restarts_by_the_games_own_restart(self):
        """Interface version 4: for a keyboard the name, address and port rows are the game's own
        text rows (text_max; the password row stays the mod's), for a gamepad they are the mod's;
        and the joiner's game is started again by the interface's restart with NX2NAVI_SAVE_DIR."""
        self.start_server(password='s3cret')
        account = '0123456789abcdef0123456789abcdef'
        host_profile = self.root / 'host-profile'
        own_profile = self.root / 'own-profile'
        d = host_profile / 'save' / account / 'slot_01'
        d.mkdir(parents=True)
        (d / 'progress.sav').write_bytes(b'HOST PROGRESS ' * 2000)
        own = own_profile / 'save' / account
        (own / 'slot_00').mkdir(parents=True)
        (own / 'slot_00' / 'progress.sav').write_bytes(b'my own progress')
        (own_profile / 'account.uid').write_bytes(b'uid')
        (self.root / 'Mido').mkdir()
        (self.root / 'Mido' / 'settings.json').write_text('{"name": "Mido"}', encoding='utf-8')
        a = self.player('Host', '--seconds', '14', '--v3', '--profile', str(host_profile), exe=HOST,
                        password='s3cret')
        time.sleep(1.5)
        b = self.player(
            'Mido', '--seconds', '12', '--v4', '--profile', str(own_profile),
            '--title', '1.0',
            '--menu', '1.4', '3',                                    # Join over Local Network
            '--menu-text', '1.8', '1', ' LocalHost ',                # typed into the game's own row
            '--menu-text', '2.1', '2', '70000',                      # refused: not a port
            '--menu-text', '2.4', '2', str(self.port),
            '--pad', '2.8', 'down',                                  # a gamepad: the rows are ours
            '--pad-key', '3.2', 'down',                              # a keyboard again
            '--menu', '3.6', '3', '--type', '3.8', 's3cret', '--key', '4.1', 'enter',   # the password
            '--menu', '4.5', '6',                                    # Join
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'en-US'})
        code_b, out_b, log_b = self.finish(*b)
        code_a, out_a, log_a = self.finish(*a)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        pages = self.pages(out_b)
        join = [r for _, t, r in pages if t == 'Join over Local Network']
        self.assertTrue(join, out_b)
        self.assertIn("0 text 'Name' = 'Mido' max 64", join[0][0])
        self.assertIn("1 text 'Address' = '' max 253", join[0][1])
        self.assertIn("3 text 'Password' = 'none' [", join[0][3])           # ours: no max
        self.assertTrue(any("1 text 'Address' = '127.0.0.1' max 253" in r[1] for r in join), out_b)
        self.assertTrue(any('The port is a number from 1 to 65535.' in r[2] for r in join), out_b)
        self.assertTrue(any(f"2 text 'Port' = '{self.port}' max 5" in r[2] for r in join), out_b)
        # The gamepad's press: the same rows without the game's typing; the key's: with it again.
        plain = [i for i, r in enumerate(join) if f"2 text 'Port' = '{self.port}' [" in r[2]]
        self.assertTrue(plain, out_b)
        self.assertTrue(any(f"2 text 'Port' = '{self.port}' max 5" in r[2] for r in join[plain[0] + 1:]), out_b)
        self.assertTrue(any("3 text 'Password' = 's3cret_'" in r[3] for r in join), out_b)
        # The host's save, then the game's own restart with the save folder (no window is closed).
        self.assertIn("save: the host's save is slot_00 of the co-op save folder", log_b)
        self.assertIn('the game starts again with NX2NAVI_SAVE_DIR', log_b)
        self.assertIn("restart: asking the game to start again on the co-op save (the plugin interface's "
                      'restart, 4 variables)', log_b)
        m = re.search(r'fake_host: restart asked: (.*)$', out_b, re.M)
        self.assertTrue(m, out_b)
        asked = m.group(1)
        self.assertIn('[NX2NAVI_SAVE_DIR=' + str(self.root / 'Mido' / 'coop-saves' / '@Host') + ']', asked)
        self.assertIn('[OOT_COOP_INI=' + str(self.root / 'Mido' / 'coop.ini') + ']', asked)
        self.assertIn('[OOT_COOP_SAVE_READY=1]', asked)
        self.assertIn('[OOT_COOP_PASSWORD=(set)]', asked)
        self.assertNotIn('s3cret', asked)
        self.assertIn('coop: the game ends (the game starts itself again on the co-op save)', log_b)
        self.assertNotIn('restart: the game starts again as process', log_b)   # not started by hand
        target = self.root / 'Mido' / 'coop-saves' / '@Host' / 'save' / account
        self.assertEqual((target / 'slot_00' / 'progress.sav').read_bytes(), b'HOST PROGRESS ' * 2000)
        self.assertEqual((own / 'slot_00' / 'progress.sav').read_bytes(), b'my own progress')
        self.assertIn('unloaded cleanly', out_b)

    def test_join_refused_says_why_and_can_be_tried_again(self):
        """A wrong password: no session, the page says why, and Join works after it is fixed."""
        self.start_server(password='right')
        own_profile = self.root / 'own-profile'
        (own_profile / 'save' / 'acct').mkdir(parents=True)
        folder = self.root / 'Mido'
        folder.mkdir()
        (folder / 'settings.json').write_text(
            '{"name": "Mido", "host_address": "127.0.0.1", "port": %d, "join_password": "wrong"}' % self.port,
            encoding='utf-8')
        process, folder = self.player(
            'Mido', '--seconds', '14', '--v3', '--profile', str(own_profile),
            '--title', '1.0', '--menu', '1.4', '3', '--menu', '1.8', '6',     # Join: refused
            '--menu', '4.5', '3', '--type', '4.7', 'right', '--key', '5.0', 'enter',
            '--menu', '5.4', '6',                                              # Join again
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'en-US'})
        code, out, log = self.finish(process, folder)
        self.assertEqual(code, 0, out + log)
        pages = self.pages(out)
        self.assertTrue(any("6 action 'Join' = 'Failed'" in r[6] and 'The server refused:' in r[6]
                            for _, _, r in pages), out)
        self.assertIn('coop: server refused: ', log)
        self.assertIn('join: not joined', log)
        # The second try: alone on the server, so there is no host to take a save from.
        self.assertIn('save: we are the first on that server', log)
        self.assertIn('coop: welcomed as player 1', log)
        self.assertTrue(any('The host sent no save' in ' '.join(r) for _, _, r in pages), out)

    # --- Multiplayer saves (interface version 4: the game can be started again on a save folder) ---

    @staticmethod
    def files_of(folder):
        """{relative path: bytes} of every file under a folder."""
        return {str(p.relative_to(folder)): p.read_bytes() for p in sorted(folder.rglob('*')) if p.is_file()}

    def test_multiplayer_saves_made_copied_deleted_and_hosted_on(self):
        """The Host page's Save row: a new multiplayer save by its typed name, a copy of the
        player's own newest slot, a delete asked twice, and Start, which starts the game again
        on the chosen save (the interface's restart with NX2NAVI_SAVE_DIR) instead of playing
        on the player's own. That game then starts the server itself; a second player who joins
        is told the save's name and keeps the host's save under it. The player's own saves are
        never written."""
        account = '0123456789abcdef0123456789abcdef'
        profile = self.root / 'host-profile'
        own = profile / 'save' / account
        for slot, text, age in (('slot_00', b'old solo progress', 900), ('slot_02', b'SOLO PROGRESS ' * 3000, 60)):
            d = own / slot
            d.mkdir(parents=True)
            (d / 'progress.sav').write_bytes(text)
            (d / 'caption.sav').write_bytes(b'\x01\x02HyruleField/HyruleField_Adult\x00 of ' + slot.encode())
            old = time.time() - age
            os.utime(d / 'progress.sav', (old, old))
        (own / 'option.sav').write_bytes(b'my options')
        (own / 'account.sav').write_bytes(b'my account')
        (profile / 'account.uid').write_bytes(b'uid')
        solo_before = self.files_of(profile)
        folder = self.root / 'Saria'
        (folder / 'bin').mkdir(parents=True)
        (folder / 'bin' / SERVER.name).write_bytes(SERVER.read_bytes())
        (folder / 'settings.json').write_text('{\n  "name": "Saria",\n  "port": %d,\n  "kept": [1, 2]\n}' % self.port,
                                              encoding='utf-8')
        process, folder = self.player(
            'Saria', '--seconds', '14', '--v4', '--exit', '--profile', str(profile),
            '--title', '1.0',
            '--menu', '1.4', '1',                      # Host over Local Network
            '--menu', '1.8', '6',                      # Start: no multiplayer save chosen yet
            '--menu', '2.2', '4',                      # the Save row: the list
            '--menu-text', '2.6', '4', 'a/b',          # New save: not a name
            '--menu-text', '3.0', '4', 'Zelda',        # New save "Zelda": made, chosen, back on Host
            '--menu', '3.4', '4',
            '--menu', '3.8', '6',                      # Copy my solo save: "Solo", chosen, back on Host
            '--menu', '4.2', '4',
            '--menu', '4.6', '1',                      # the list: Solo (played), Zelda (new): Zelda's page
            '--menu', '5.0', '6',                      # Delete...
            '--menu', '5.4', '6',                      # ...confirmed: the list
            '--menu', '5.8', '0',                      # Solo's page
            '--menu', '6.2', '0',                      # Play on it: the Host page
            '--menu', '6.6', '6',                      # Start
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'en-US'})
        code, out, log = self.finish(process, folder)
        self.assertEqual(code, 0, out + log)
        pages = self.pages(out)
        what, title, rows = pages[0]
        self.assertEqual((what, title), ('open', 'Multiplayer'))
        self.assertEqual(len(rows), 8)
        self.assertIn("4 action 'Multiplayer saves' = 'None' [Your co-op games, kept apart from your solo"
                      "\\nsaves: create, copy, delete.]", rows[4])
        host = [r for _, t, r in pages if t == 'Host over Local Network']
        self.assertIn("4 action 'Save' = 'Choose' [The multiplayer save this game is played on."
                      "\\nYour solo saves are never written.]", host[0][4])
        self.assertTrue(any("6 action 'Start' = 'Failed' [Choose a multiplayer save first" in r[6] for r in host), out)
        lists = [r for _, t, r in pages if t == 'Multiplayer saves']
        self.assertTrue(lists, out)
        self.assertEqual(lists[0][-1], 'cursor to row 4')        # nothing in the list: on "New save"
        self.assertIn("0 action '-' = '' (disabled)", lists[0][0])
        self.assertIn("3 action 'More saves' = '' (disabled)", lists[0][3])
        self.assertIn("4 text 'New save' = 'Coop' max 64", lists[0][4])
        self.assertEqual(lists[0][5], '5 separator')
        self.assertIn("6 action 'Copy my solo save' = 'Copy'", lists[0][6])
        self.assertTrue(any("A save's name: 1 to 16 characters," in r[4] for r in lists), out)
        self.assertNotIn('menu update refused', out)             # one skeleton: every page change is an update
        # Made and chosen: the Host page shows it; then the copy of the solo save takes its place.
        self.assertTrue(any("4 action 'Save' = 'Zelda'" in r[4] for r in host), out)
        self.assertTrue(any("4 action 'Save' = 'Solo'" in r[4] for r in host), out)
        self.assertIn("menu: multiplayer save 'Zelda' made (a game to begin)", log)
        self.assertIn("menu: multiplayer save 'Solo' made from the newest save of your own", log)
        both = [r for r in lists if "0 action 'Solo' = '" in r[0] and "1 action 'Zelda' = 'New game'" in r[1]]
        self.assertTrue(both, out)
        self.assertIn('[Saved in HyruleField_Adult\\nChosen for hosting.]', both[0][0])
        self.assertIn('[Not begun yet: a new game.\\nYour multiplayer save.]', both[0][1])
        # Zelda's page; Delete asks once more, then the save is out of the list and kept in _deleted.
        zelda = [r for _, t, r in pages if t == 'Zelda']
        self.assertIn("0 action 'Play on it' = 'Choose'", zelda[0][0])
        self.assertIn("1 text 'Name' = 'Zelda' max 64", zelda[0][1])
        self.assertIn("2 action 'Last played' = 'never'", zelda[0][2])
        self.assertIn("4 action 'Origin' = 'Yours'", zelda[0][4])
        self.assertIn("6 action 'Delete' = 'Delete'", zelda[0][6])
        self.assertTrue(any("6 action 'Delete' = 'Confirm' [Delete \"Zelda\"?" in r[6] for r in zelda), out)
        self.assertIn("menu: multiplayer save 'Zelda' taken out of the list", log)
        saves = folder / 'coop-saves'
        self.assertFalse((saves / 'Zelda').exists())
        self.assertEqual((saves / '_deleted' / 'Zelda' / 'save' / account / 'option.sav').read_bytes(), b'my options')
        after = [r for r in lists if "0 action 'Solo' = '" in r[0] and "1 action '-' = '' (disabled)" in r[1]]
        self.assertTrue(after, out)
        # The copy: the newest solo slot under its own name, with the player's settings.
        solo = saves / 'Solo' / 'save' / account
        self.assertEqual((solo / 'slot_02' / 'progress.sav').read_bytes(), b'SOLO PROGRESS ' * 3000)
        self.assertEqual(sorted(p.name for p in solo.iterdir()), ['account.sav', 'option.sav', 'slot_02'])
        solo_page = [r for _, t, r in pages if t == 'Solo']
        self.assertIn("3 action 'Place' = 'HyruleField_Adult'", solo_page[0][3])
        # Start: no server and no session in this game; it is started again on the save.
        self.assertIn("menu: Start chosen (name 'Saria', port %d, multiplayer save 'Solo')" % self.port, log)
        self.assertIn("host: the game starts again on the multiplayer save 'Solo'", log)
        self.assertTrue(any("6 action 'Start' = 'Restarting...' [The game restarts on your multiplayer save:"
                            "\\nthen choose Continue.]" in r[6] for r in host), out)
        m = re.search(r'fake_host: restart asked: (.*)$', out, re.M)
        self.assertTrue(m, out)
        asked = m.group(1)
        self.assertIn('[NX2NAVI_SAVE_DIR=' + str(saves / 'Solo') + ']', asked)
        self.assertIn('[OOT_COOP_INI=' + str(folder / 'coop.ini') + ']', asked)
        self.assertIn('[OOT_COOP_MP_HOST=1]', asked)
        self.assertNotIn('server: started', log)
        self.assertNotIn('welcomed', log)
        ini = (folder / 'coop.ini').read_text(encoding='utf-8')
        self.assertIn('server = 127.0.0.1', ini)
        self.assertIn(f'port = {self.port}', ini)
        self.assertIn('host_save = 0', ini)
        saved = (folder / 'settings.json').read_text(encoding='utf-8')
        self.assertIn('"mp_save": "Solo"', saved)
        self.assertIn('"kept": [1, 2]', saved)
        # The player's own saves: every file as it was.
        self.assertEqual(self.files_of(profile), solo_before)

        # The game the restart starts: on the save's folder, told to host. It starts the server
        # and the session itself; the page's Save row shows the save it runs on.
        (folder / 'coop-nx2navi.log').unlink()
        a = self.player('Saria', '--seconds', '15', '--v4', '--exit', '--profile', str(profile),
                        '--save-root', str(saves / 'Solo'),
                        '--title', '11.5', '--menu', '12.0', '1',
                        exe=HOST, idle=True,
                        env_more={'NX2NAVI_LANG': 'en-US', 'OOT_COOP_INI': str(folder / 'coop.ini'),
                                  'OOT_COOP_MP_HOST': '1',
                                  'OOT_COOP_PASSWORD': DEFAULT_PASSWORD})
        time.sleep(2.5)
        own_b = self.root / 'own-profile' / 'save' / account
        (own_b / 'slot_00').mkdir(parents=True)
        (own_b / 'slot_00' / 'progress.sav').write_bytes(b'my own progress')
        (self.root / 'Mido').mkdir()
        (self.root / 'Mido' / 'settings.json').write_text('{"name": "Mido", "join_password": "%s"}' % DEFAULT_PASSWORD, encoding='utf-8')
        b = self.player(
            'Mido', '--seconds', '10', '--v4', '--profile', str(self.root / 'own-profile'),
            '--title', '1.0', '--menu', '1.4', '3',
            '--menu-text', '1.8', '1', 'localhost', '--menu-text', '2.1', '2', str(self.port),
            '--menu', '2.5', '6',                      # Join
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'en-US'})
        code_b, out_b, log_b = self.finish(*b)
        code_a, out_a, log_a = self.finish(*a)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        self.assertIn('host: this game was started again to host on a multiplayer save', log_a)
        self.assertIn(f'server: started on UDP port {self.port}', log_a)
        self.assertIn('coop: welcomed as player 1', log_a)
        self.assertIn('server: stopped with the game', log_a)
        host = [r for _, t, r in self.pages(out_a) if t == 'Host over Local Network']
        self.assertTrue(host, out_a)
        self.assertIn("4 action 'Save' = 'Solo'", host[0][4])
        # The joiner: the save, then its name; kept as "Solo@Saria" among its multiplayer saves.
        self.assertRegex(log_a, r'save: sending slot_02 \(\d+ bytes, \d+ parts\) to player 2')
        self.assertIn("save: player 2 is told this save's name, 'Solo'", log_a)
        self.assertIn("save: the host's save is named 'Solo'", log_b)
        kept = self.root / 'Mido' / 'coop-saves' / 'Solo@Saria'
        self.assertEqual((kept / 'save' / account / 'slot_00' / 'progress.sav').read_bytes(), b'SOLO PROGRESS ' * 3000)
        m = re.search(r'fake_host: restart asked: (.*)$', out_b, re.M)
        self.assertTrue(m, out_b)
        self.assertIn('[NX2NAVI_SAVE_DIR=' + str(kept) + ']', m.group(1))
        self.assertIn('[OOT_COOP_SAVE_READY=1]', m.group(1))
        self.assertNotIn('OOT_COOP_MP_HOST', m.group(1))
        self.assertEqual((own_b / 'slot_00' / 'progress.sav').read_bytes(), b'my own progress')
        self.assertEqual(self.files_of(profile), solo_before)

    def test_a_host_beginning_a_new_game_has_the_joiner_begin_one_too(self):
        """The host's game runs on a multiplayer save with nothing in it yet (a new game it is
        beginning): a joiner's ask is answered with "a new game" and the save's name, and the
        joiner's game is started again on an empty save folder of its own, named after the
        host's, instead of going on with the player's own saves."""
        account = '0123456789abcdef0123456789abcdef'
        host_mod = self.root / 'Saria'
        (host_mod / 'bin').mkdir(parents=True)
        (host_mod / 'bin' / SERVER.name).write_bytes(SERVER.read_bytes())
        fresh = host_mod / 'coop-saves' / 'Fresh'
        (fresh / 'save' / account).mkdir(parents=True)
        (fresh / 'save' / account / 'option.sav').write_bytes(b"the host's options")
        a = self.player('Saria', '--seconds', '13', '--v4', '--exit', '--profile', str(self.root / 'host-profile'),
                        '--save-root', str(fresh), exe=HOST, env_more={'OOT_COOP_MP_HOST': '1',
                                  'OOT_COOP_PASSWORD': DEFAULT_PASSWORD})
        time.sleep(2.0)
        own = self.root / 'own-profile' / 'save' / account
        (own / 'slot_00').mkdir(parents=True)
        (own / 'slot_00' / 'progress.sav').write_bytes(b'my own progress')
        (own / 'option.sav').write_bytes(b'my options')
        (self.root / 'Mido').mkdir()
        (self.root / 'Mido' / 'settings.json').write_text('{"name": "Mido", "join_password": "%s"}' % DEFAULT_PASSWORD, encoding='utf-8')
        b = self.player(
            'Mido', '--seconds', '9', '--v4', '--profile', str(self.root / 'own-profile'),
            '--title', '1.0', '--menu', '1.4', '3',
            '--menu-text', '1.8', '1', 'localhost', '--menu-text', '2.1', '2', str(self.port),
            '--menu', '2.5', '6',                      # Join
            exe=HOST, idle=True, env_more={'NX2NAVI_LANG': 'en-US'})
        code_b, out_b, log_b = self.finish(*b)
        code_a, out_a, log_a = self.finish(*a)
        self.assertEqual(code_a, 0, out_a + log_a)
        self.assertEqual(code_b, 0, out_b + log_b)
        self.assertIn(f'server: started on UDP port {self.port}', log_a)
        self.assertIn('save: a player asked for our save, but there is none yet', log_a)
        self.assertIn("save: player 2 is told that this save, 'Fresh', has nothing in it yet: a new game to begin",
                      log_a)
        self.assertNotIn('save: sending', log_a)
        self.assertIn("save: the host is beginning a new game on its save 'Fresh'", log_b)
        kept = self.root / 'Mido' / 'coop-saves' / 'Fresh@Saria'
        self.assertIn('save: a new game, as the host\'s, is to begin in the co-op save folder ' + str(kept), log_b)
        self.assertEqual(sorted(p.name for p in (kept / 'save' / account).iterdir()), ['option.sav'])
        self.assertEqual((kept / 'save' / account / 'option.sav').read_bytes(), b'my options')
        pages = self.pages(out_b)
        self.assertTrue(any("6 action 'Join' = 'Restarting...' [The host begins a new game. The game restarts:"
                            "\\nthen start a new game too.]" in r[6] for _, _, r in pages), out_b)
        m = re.search(r'fake_host: restart asked: (.*)$', out_b, re.M)
        self.assertTrue(m, out_b)
        self.assertIn('[NX2NAVI_SAVE_DIR=' + str(kept) + ']', m.group(1))
        self.assertIn('[OOT_COOP_SAVE_READY=1]', m.group(1))
        self.assertEqual((own / 'slot_00' / 'progress.sav').read_bytes(), b'my own progress')
        self.assertEqual(sorted(p.name for p in own.iterdir()), ['option.sav', 'slot_00'])

    def test_fallback_without_the_interface(self):
        """A game whose executable has no nx2navi_plugin_api export (an older NX2navi):
        said once, and co-op runs as before."""
        self.start_server()
        code, out, log = self.finish(*self.player('Saria', '--seconds', '5'))
        self.assertEqual(code, 0, out + log)
        self.assertIn('game found: main at 0x7000000000', log)
        self.assertEqual(log.count('overlay: this game has no plugin interface '
                                   '(no nx2navi_plugin_api export'), 1)
        self.assertNotIn('plugin interface version', log)
        self.assertNotIn('overlay: on', log)
        self.assertEqual(log.count('puppet: this game has no plugin interface'), 1)
        self.assertIn('welcomed as player 1', log)
        self.assertIn('player found in spot00', log)
        self.assertIn('unloaded cleanly', out)


if __name__ == '__main__':
    unittest.main()
