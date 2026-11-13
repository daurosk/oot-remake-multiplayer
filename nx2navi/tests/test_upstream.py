# SPDX-License-Identifier: GPL-3.0-or-later
"""The build that runs upstream's hook unchanged (nx2navi/dll/upstream.hpp, build.py --upstream): what it builds.

    python nx2navi/tests/test_upstream.py

No compiler and no game: it checks the list. Every file lichii/hook/core.cmake names is built, but the three this
port answers itself, and each of those three has its answer in dll/upstream.cpp, so a hook file upstream adds
joins this build by itself and one it drops is noticed here.
"""
import importlib.util
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
spec = importlib.util.spec_from_file_location('nx2navi_build', HERE.parent / 'build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class UpstreamBuild(unittest.TestCase):
    def test_every_hook_file_but_three(self):
        listed = set(re.findall(r'coop_[a-z0-9_]+\.cpp', (REPO / 'lichii/hook/core.cmake').read_text('utf-8')))
        built = {Path(s).name for s in build.hook_sources()}
        self.assertEqual(listed - built, {'coop_svc.cpp', 'coop_teleport_game.cpp', 'coop_save.cpp'})
        self.assertEqual(built - listed, set())
        self.assertGreaterEqual(len(built), 46)
        for source in build.hook_sources() + build.UP_OWN:
            self.assertTrue((REPO / source).is_file(), source)

    def test_the_three_are_answered_by_the_port(self):
        text = (REPO / 'nx2navi/dll/upstream.cpp').read_text('utf-8')
        for name in ('namespace CoopSvc {', 'namespace CoopTeleport {', 'namespace CoopSave {',
                     'namespace CoopRuntime {'):
            self.assertIn(name, text)
        # Every function coop_hook.cpp calls of the two modules.
        for call in ('bool OnChat(', 'void OnEvent(', 'void Frame(', 'TakeLines()', 'void OnFrame(',
                     'TakeNotice()', 'std::string Refusal()'):
            self.assertIn(call, text)

    def test_compat_comes_first_and_no_stand_in_headers(self):
        self.assertEqual(build.UP_INCLUDES[0], 'hookshot/compat')
        self.assertNotIn('nx2navi/dll/objects/shim', build.UP_INCLUDES)

    def test_an_enemy_copy_gets_time_to_be_made(self):
        # Run ka1 (two games on one PC): the game took over 3 s to make the first Stalchild copies after a load,
        # the hook's wait ended ("create timed out") and the second player saw no Stalchild at all. The hook's
        # wait stays 3 s for the emulator; this build gives its own.
        header = (REPO / 'lichii/hook/coop_spawn.h').read_text('utf-8')
        self.assertIn('kCreateTimeoutMs = LICHII_COOP_SPAWN_CREATE_TIMEOUT_MS;', header)
        self.assertIn('#define LICHII_COOP_SPAWN_CREATE_TIMEOUT_MS 3000', header)
        source = (HERE.parent / 'build.py').read_text('utf-8')
        wait = re.search(r"-DLICHII_COOP_SPAWN_CREATE_TIMEOUT_MS=(\d+)", source)
        self.assertIsNotNone(wait)
        self.assertGreaterEqual(int(wait.group(1)), 15000)

    def test_a_guests_clock_is_set_again_when_minutes_apart(self):
        # Run td1 (two games): the joiner's load took 7.4 s longer, its clock stayed 892 in-game seconds (the
        # sun in one picture, the moon in the other at dusk) behind the host's, because the hook only sets a
        # clock again when it is an hour apart and its nudges are never written in this game (the time flags
        # stay 4 after a set). The hook's own numbers stay for the emulator; this build narrows them.
        header = (REPO / 'lichii/hook/coop_time_sync.h').read_text('utf-8')
        self.assertIn('kResyncSeconds = LICHII_COOP_TIME_RESYNC_SECONDS;', header)
        self.assertIn("#define LICHII_COOP_TIME_RESYNC_SECONDS 3'600", header)
        self.assertIn("#define LICHII_COOP_TIME_SET_EVERY_US 60'000'000", header)
        source = (HERE.parent / 'build.py').read_text('utf-8')
        apart = re.search(r"-DLICHII_COOP_TIME_RESYNC_SECONDS=(\d+)", source)
        every = re.search(r"-DLICHII_COOP_TIME_SET_EVERY_US=(\d+)", source)
        self.assertIsNotNone(apart)
        self.assertIsNotNone(every)
        self.assertLessEqual(int(apart.group(1)), 240)   # two real seconds
        self.assertGreater(int(apart.group(1)), 120)     # above what a set leaves and the join's own rule
        self.assertLessEqual(int(every.group(1)), 20000000)

    def test_the_actor_list_line_is_not_logged_every_frame(self):
        text = (REPO / 'nx2navi/dll/upstream.cpp').read_text('utf-8')
        self.assertIn('"objects: list head "', text)

    def test_the_old_port_is_still_listed(self):
        self.assertIn('nx2navi/dll/coop_dll.cpp', build.DLL_MAIN)
        self.assertIn('nx2navi/dll/progress.cpp', build.DLL_MAIN)


if __name__ == '__main__':
    unittest.main()
