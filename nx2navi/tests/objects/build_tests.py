# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds and runs the shared-enemies port's tests (nx2navi/dll/objects) on their own,
without touching nx2navi/build.py's outputs:

    python nx2navi/tests/objects/build_tests.py [--compiler <clang++.exe>] [--no-run]

It reuses nx2navi/build.py's compiler search, its pinned ENet and doctest checkouts and its
compile step (objects go to build/nx2navi/obj/objects_*; the test program is
build/nx2navi/bin/coop_objects_tests.exe). The emulator hook's object files are compiled
unchanged, with nx2navi/dll/objects/shim first on the include path.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NX2NAVI = HERE.parent.parent
sys.path.insert(0, str(NX2NAVI))
import build as nx  # noqa: E402  (nx2navi/build.py, used as a library)

REPO = nx.REPO

# The port of shared enemies, and what it compiles unchanged from the emulator hook.
PORT = ['nx2navi/dll/objects/emu_port.cpp', 'nx2navi/dll/objects/hold.cpp', 'nx2navi/dll/objects/brain.cpp', 'nx2navi/dll/objects/anim_probe.cpp', 'nx2navi/dll/objects/world_probe.cpp',
        'nx2navi/dll/objects/objects.cpp']
# Compiled unchanged from the emulator hook: what the DLL does not have yet...
HOOK = ['lichii/hook/' + n for n in (
    'coop_objects.cpp', 'coop_objects_sync.cpp', 'coop_world.cpp', 'coop_spawn.cpp',
    'coop_projectiles.cpp', 'coop_pickups.cpp', 'coop_ocarina.cpp', 'coop_cheat_items.cpp',
    'coop_flags.cpp', 'coop_boss.cpp', 'coop_boss_game.cpp', 'coop_dungeon.cpp',
    'coop_scrubs.cpp', 'coop_platforms.cpp', 'coop_counters.cpp', 'coop_enemy_shots.cpp', 'coop_rooms.cpp',
    'coop_pvp.cpp', 'coop_puppet_sound.cpp', 'coop_pvp_game.cpp', 'coop_ocarina_sound.cpp', 'coop_colors.cpp',
    'coop_time.cpp', 'coop_time_sync.cpp')]
# ...and what the DLL already links (nx2navi/build.py DLL_MAIN), for the tests only.
HOOK_IN_DLL = ['lichii/hook/' + n for n in ('coop_puppet.cpp', 'coop_pose.cpp', 'coop_camera.cpp')]
TESTS = ['tests/main.cpp', 'nx2navi/tests/objects/fake_game.cpp',
         'nx2navi/tests/objects/port_test.cpp', 'nx2navi/tests/objects/two_games_test.cpp',
         'nx2navi/tests/objects/loopback_test.cpp',
         # The shared code's own cases for the files this port changed or leans on.
         'tests/lichii/objects_test.cpp', 'tests/lichii/boss_test.cpp', 'tests/lichii/world_test.cpp']
EXTRA_INCLUDES = ['nx2navi/dll/objects/shim', 'nx2navi/dll/objects', 'nx2navi/tests/objects']


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--compiler')
    parser.add_argument('--no-run', action='store_true')
    parser.add_argument('--dll-check', action='store_true',
                        help='also link the DLL sources with the port into a scratch DLL '
                             '(build/nx2navi/bin/objects_dll_check.dll) to find clashing symbols')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    parser.add_argument('args', nargs='*', help='passed to the test program (doctest options)')
    options = parser.parse_args()

    cxx, cc = nx.find_compiler(options.compiler)
    nx.say(f'compiler: {cxx}')
    builder = nx.Builder(cxx, cc, max(1, options.jobs))
    enet = nx.fetch('enet', 'enet.cmake')
    doctest = nx.fetch('doctest', 'doctest.cmake')
    builder.include_flags.append(f'-isystem{enet / "include"}')
    extra = [f'-I{REPO / i}' for i in EXTRA_INCLUDES] + [f'-isystem{doctest}']
    # Files outside nx2navi/dll and lichii/hook count for the up-to-date check too.
    builder.headers_changed = max(builder.headers_changed, Path(__file__).stat().st_mtime,
                                  *(p.stat().st_mtime for p in HERE.glob('*.h*')))

    enet_objects = builder.compile_all(
        [p for p in sorted(enet.glob('*.c')) if p.name != 'unix.c'], 'enet')
    monocypher = nx.fetch('monocypher', 'monocypher.cmake')
    builder.include_flags.append(f'-isystem{monocypher / "src"}')
    enet_objects += builder.compile_all([monocypher / 'src' / 'monocypher.c'], 'monocypher')
    coop_objects = builder.compile_all(nx.COOP, 'coop')
    server = builder.compile_all(nx.SERVER_LIB, 'server')
    port = builder.compile_all(PORT + HOOK, 'objects_port', extra)
    port += builder.compile_all(nx.DLL_HOOKED, 'objects_port_hooked', extra + ['-DLICHII_COOP'])
    in_dll = builder.compile_all(HOOK_IN_DLL, 'objects_port', extra)
    tests = builder.compile_all(TESTS, 'objects_tests', extra)
    program = builder.link(nx.BUILD / 'bin' / 'coop_objects_tests.exe',
                           tests + port + in_dll + server + coop_objects + enet_objects)
    if options.dll_check:
        # The DLL's own files as nx2navi/build.py lists them, plus the port.
        dll = builder.compile_all(nx.DLL, 'objects_dll_check', extra)
        builder.link(nx.BUILD / 'bin' / 'objects_dll_check.dll',
                     dll + port + coop_objects + enet_objects, shared=True)
    if options.no_run:
        return
    nx.say('running the shared-enemies tests')
    # Two runs: the port's cases turn townspeople and animals off for the whole process
    # (CoopObjects::ShareFriendly), which the shared code's own cases must not see.
    done = subprocess.run([str(program), '--source-file-exclude=*lichii*', *options.args], env=builder.env)
    if done.returncode != 0:
        sys.exit(done.returncode)
    nx.say("running the shared code's own cases (tests/lichii/objects_test.cpp, boss_test.cpp)")
    done = subprocess.run([str(program), '--source-file=*lichii*', *options.args], env=builder.env)
    sys.exit(done.returncode)


if __name__ == '__main__':
    main()
