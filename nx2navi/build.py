# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the NX2navi mod without CMake: coop_nx2navi.dll and oot-coop-server.exe, then
the ready-to-drop mod folder.

    python nx2navi/build.py            build and assemble build/nx2navi-mod/OoT Co-op/
    python nx2navi/build.py --tests    also build and run the unit tests
    python nx2navi/build.py --compiler C:/path/to/g++.exe

For machines that have a MinGW compiler but no CMake (the NX2navi launcher installs
one). With CMake, `cmake -S . -B build -DCOOP_BUILD_NX2NAVI=ON` builds the same DLL.

ENet and doctest are fetched with git at the commits cmake/enet.cmake and
cmake/doctest.cmake pin, into build/nx2navi/ (ignored by git), and the checkout is
refused unless git says it is that commit. Both programs are linked statically: no
libstdc++, libgcc or winpthread DLL beside them.
"""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
BUILD = REPO / 'build' / 'nx2navi'
MOD_NAME = 'OoT Co-op'
MOD_OUT = REPO / 'build' / 'nx2navi-mod' / MOD_NAME

INCLUDES = ['coop/include', 'coop/protocol/include', 'coop/net/include', 'coop/presence/include',
            'coop/host/include', 'coop/overlay/include', 'server', 'lichii/hook', 'nx2navi/dll',
            # Stand-in emulator headers for the hook files of shared enemies (dll/objects/INTEGRATION.md):
            # only common/... and core/... headers, which nothing else here includes.
            'nx2navi/dll/objects/shim', 'nx2navi/dll/objects']

# The emulator-independent libraries, as coop/CMakeLists.txt lists them.
COOP = ['coop/protocol/src/codec.cpp', 'coop/protocol/src/join_password.cpp',
        'coop/protocol/src/traffic_crypto.cpp',
        'coop/net/src/clock.cpp', 'coop/net/src/client.cpp',
        'coop/presence/src/presence.cpp'] + [
    'coop/host/src/' + n for n in (
        'flags.cpp', 'flag_watcher.cpp', 'game_flags.cpp', 'game_items.cpp', 'notify.cpp',
        'game_memory.cpp', 'player_reader.cpp', 'sampler.cpp', 'save_transfer.cpp', 'session.cpp')]
SERVER_LIB = ['server/' + n for n in (
    'session.cpp', 'world.cpp', 'enet_server.cpp', 'options.cpp',
    # Hosting over the internet (server/CMakeLists.txt): the relay, UPnP, invite codes, hole punching.
    'relay.cpp', 'relay_link.cpp', 'relay_wire.cpp', 'upnp.cpp', 'nat_codes.cpp', 'stun.cpp', 'punch.cpp',
    'join_helper.cpp')]
SERVER = SERVER_LIB + ['server/main.cpp']
DLL_LIB = ['nx2navi/dll/guest_memory.cpp', 'nx2navi/dll/settings.cpp', 'nx2navi/dll/overlay_model.cpp',
           'nx2navi/dll/teleport_auto.cpp', 'nx2navi/dll/menu_model.cpp', 'nx2navi/dll/save_sync.cpp',
           'nx2navi/dll/mp_saves.cpp',
           # Over Steam: the pages and what the two games say around the session (no Windows code).
           'nx2navi/dll/steam_model.cpp', 'nx2navi/dll/steam_menu.cpp',
           'lichii/hook/coop_settings.cpp', 'lichii/hook/coop_colors.cpp']
# The puppet director and its pose and camera helpers are the emulator hook's own files:
# they hold no emulator code (lichii/hook/coop_puppet.h).
DLL_MAIN = ['nx2navi/dll/coop_dll.cpp', 'nx2navi/dll/overlay.cpp', 'nx2navi/dll/puppet.cpp',
            'nx2navi/dll/teleport.cpp', 'nx2navi/dll/title_menu.cpp', 'nx2navi/dll/ai_probe.cpp',
            'nx2navi/dll/steam_net.cpp',
            # Shared progress and world state (dll/progress.hpp) and the hook's lists it shares.
            'nx2navi/dll/progress.cpp', 'lichii/hook/coop_chests.cpp', 'lichii/hook/coop_story.cpp',
            'lichii/hook/coop_quests.cpp', 'lichii/hook/coop_quest.cpp', 'lichii/hook/coop_shared_scenes.cpp',
            'lichii/hook/coop_puppet.cpp', 'lichii/hook/coop_pose.cpp', 'lichii/hook/coop_camera.cpp',
            # Teleport's rules and the event request, also the hook's own and emulator-free.
            'lichii/hook/coop_teleport.cpp', 'lichii/hook/coop_live_events.cpp',
            'lichii/hook/coop_live_places.cpp',
            # Shared enemies and boss rooms (nx2navi/dll/objects/INTEGRATION.md); off unless objects = 1.
            'nx2navi/dll/objects/objects.cpp', 'nx2navi/dll/objects/emu_port.cpp',
            'nx2navi/dll/objects/hold.cpp', 'nx2navi/dll/objects/brain.cpp', 'nx2navi/dll/objects/anim_probe.cpp',
            'nx2navi/dll/objects/world_probe.cpp',
            'lichii/hook/coop_objects.cpp', 'lichii/hook/coop_objects_sync.cpp',
            'lichii/hook/coop_world.cpp', 'lichii/hook/coop_spawn.cpp',
            'lichii/hook/coop_projectiles.cpp', 'lichii/hook/coop_pickups.cpp',
            'lichii/hook/coop_ocarina.cpp', 'lichii/hook/coop_cheat_items.cpp',
            'lichii/hook/coop_flags.cpp', 'lichii/hook/coop_boss.cpp',
            'lichii/hook/coop_boss_game.cpp', 'lichii/hook/coop_dungeon.cpp',
            # What the shared enemies' files call since the upstream update.
            'lichii/hook/coop_scrubs.cpp', 'lichii/hook/coop_platforms.cpp',
            'lichii/hook/coop_counters.cpp', 'lichii/hook/coop_enemy_shots.cpp',
            'lichii/hook/coop_rooms.cpp', 'lichii/hook/coop_pvp.cpp', 'lichii/hook/coop_puppet_sound.cpp',
            'lichii/hook/coop_pvp_game.cpp', 'lichii/hook/coop_ocarina_sound.cpp',
            # The host's time of day in every game (docs/time.md).
            'lichii/hook/coop_time.cpp', 'lichii/hook/coop_time_sync.cpp']
# Shared drops: its game side is under LICHII_COOP (the other hook files here are built without it).
DLL_HOOKED = ['lichii/hook/coop_drops.cpp']
DLL = DLL_LIB + DLL_MAIN
# The build that runs upstream's hook unchanged, as its Hookshot module does (nx2navi/dll/upstream.hpp):
# every file lichii/hook/core.cmake lists, but the emulator's cave and the two modules this port answers
# itself (teleport's game side, the save transfer), over hookshot/compat.
UP_OWN = ['nx2navi/dll/coop_dll.cpp', 'nx2navi/dll/upstream.cpp', 'nx2navi/dll/overlay.cpp',
          'nx2navi/dll/teleport.cpp', 'nx2navi/dll/title_menu.cpp', 'nx2navi/dll/steam_net.cpp',
          'nx2navi/dll/objects/brain.cpp', 'nx2navi/dll/objects/hold.cpp',
          'nx2navi/dll/objects/anim_probe.cpp', 'nx2navi/dll/objects/world_probe.cpp']
UP_NOT = {'coop_svc.cpp', 'coop_teleport_game.cpp', 'coop_save.cpp'}
UP_INCLUDES = ['hookshot/compat', 'coop/include', 'coop/protocol/include', 'coop/net/include',
               'coop/presence/include', 'coop/host/include', 'coop/overlay/include', 'server', 'lichii/hook',
               'nx2navi/dll', 'nx2navi/dll/objects']


def hook_sources():
    text = (REPO / 'lichii' / 'hook' / 'core.cmake').read_text(encoding='utf-8')
    names = []
    for name in re.findall(r'coop_[a-z0-9_]+\.cpp', text):
        if name not in UP_NOT and name not in names:
            names.append(name)
    return ['lichii/hook/' + n for n in names]
# The repo's tests that need no emulator file beyond coop_settings.cpp, and this port's.
TESTS = ['tests/main.cpp', 'tests/smoke_test.cpp',
         'tests/protocol/codec_test.cpp', 'tests/protocol/join_password_test.cpp',
         'tests/net/clock_test.cpp', 'tests/net/client_test.cpp',
         'tests/presence/presence_test.cpp',
         'tests/host/reader_test.cpp', 'tests/host/sampler_test.cpp', 'tests/host/flags_test.cpp',
         'tests/host/flag_watcher_test.cpp', 'tests/host/game_flags_test.cpp',
         'tests/host/game_items_test.cpp', 'tests/host/notify_test.cpp', 'tests/host/session_test.cpp',
         'tests/host/scene_test.cpp',
         'tests/server/session_test.cpp', 'tests/server/options_test.cpp',
         'tests/server/loopback_test.cpp',
         'tests/lichii/settings_test.cpp',
         'nx2navi/tests/nx2navi_test.cpp', 'nx2navi/tests/steam_test.cpp', 'coop/src/version.cpp']


def say(text):
    print(text, flush=True)


def fail(text):
    say('build.py: ' + text)
    sys.exit(1)


def pinned(cmake_file):
    """(repository, commit) from one of the cmake/ fetch files (or another CMake file of the repository)."""
    text = (REPO / cmake_file if '/' in cmake_file else REPO / 'cmake' / cmake_file).read_text(encoding='utf-8')
    repo = re.search(r'GIT_REPOSITORY\s+(\S+)', text)
    commit = re.search(r'GIT_TAG\s+([0-9a-f]{40})', text)
    if not repo or not commit:
        fail(f'cmake/{cmake_file} names no repository and 40-digit commit')
    return repo.group(1), commit.group(1)


def fetch(name, cmake_file):
    """Checks the pinned commit out into build/nx2navi/<name> and returns the folder."""
    url, commit = pinned(cmake_file)
    folder = BUILD / name
    git = shutil.which('git')
    if not git:
        fail('git is needed to fetch ' + name + ' (' + url + ' at ' + commit + ')')

    def run(*args):
        return subprocess.run([git, '-C', str(folder), *args], capture_output=True, text=True)

    if run('rev-parse', 'HEAD').stdout.strip() != commit:
        if folder.exists():
            shutil.rmtree(folder, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
        folder.mkdir(parents=True)
        say(f'fetching {name} at {commit[:12]} from {url}')
        for args in (('init', '-q'), ('remote', 'add', 'origin', url),
                     ('fetch', '-q', '--depth', '1', 'origin', commit),
                     ('-c', 'advice.detachedHead=false', 'checkout', '-q', 'FETCH_HEAD')):
            done = run(*args)
            if done.returncode != 0:
                fail(f'git {" ".join(args)} failed for {name}: {done.stderr.strip()}')
    # git names a commit by the hash of its content, so this is the check.
    head = run('rev-parse', 'HEAD').stdout.strip()
    if head != commit:
        fail(f'{name}: checked out {head}, but cmake/{cmake_file} pins {commit}')
    if run('status', '--porcelain').stdout.strip():
        fail(f'{name}: the checkout in {folder} was changed; delete that folder and build again')
    return folder


def find_compiler(asked):
    """(c++ compiler, c compiler) of one MinGW toolchain."""
    candidates = []
    if asked:
        candidates.append(Path(asked))
    local = os.environ.get('LOCALAPPDATA', '')
    if local:
        ucrt = Path(local) / 'Programs' / 'NX2navi' / 'toolchain' / 'msys64' / 'ucrt64' / 'bin'
        candidates += [ucrt / 'clang++.exe', ucrt / 'g++.exe']
    candidates.append(Path(r'C:\ProgramData\mingw64\mingw64\bin\g++.exe'))
    for name in ('g++', 'clang++'):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for cxx in candidates:
        if not cxx.is_file():
            continue
        cc = cxx.with_name(cxx.name.replace('clang++', 'clang').replace('g++', 'gcc'))
        if cc.is_file():
            return cxx, cc
    fail('no C++ compiler found. Looked for the NX2navi toolchain (MSYS2 UCRT64 clang++ or g++), '
         r'C:\ProgramData\mingw64 and g++/clang++ on PATH; or pass --compiler <path to g++.exe>.')


class Builder:
    def __init__(self, cxx, cc, jobs):
        self.cxx, self.cc, self.jobs = cxx, cc, jobs
        # The compiler's own DLLs are beside it.
        self.env = dict(os.environ)
        self.env['PATH'] = str(cxx.parent) + os.pathsep + self.env.get('PATH', '')
        self.include_flags = [f'-I{REPO / i}' for i in INCLUDES]
        # No build machine folders in the programs (cmake/trim_paths.cmake).
        self.trim = [f'-ffile-prefix-map={REPO.as_posix()}/=', f'-ffile-prefix-map={REPO}\\=']
        self.headers_changed = max(
            (p.stat().st_mtime for root in ('coop', 'server', 'lichii/hook', 'nx2navi/dll', 'tests')
             for pattern in ('*.h*', '*.inc')  # coop_up.inc, the hook's tables: included like headers
             for p in (REPO / root).rglob(pattern)), default=0)
        self.headers_changed = max(self.headers_changed, Path(__file__).stat().st_mtime)

    def run(self, args, what):
        done = subprocess.run([str(a) for a in args], capture_output=True, text=True, env=self.env)
        if done.returncode != 0:
            say(done.stdout + done.stderr)
            fail(what + ' failed')
        if done.stderr.strip() and what.startswith('compiling'):
            say(done.stderr.strip())   # warnings
        return done

    def compile(self, source, group, extra=(), includes=None):
        """One object file; skipped when it is newer than its source and every header."""
        source = Path(source)
        relative = source.relative_to(REPO) if source.is_relative_to(REPO) else Path(source.name)
        obj = BUILD / 'obj' / group / (str(relative).replace('\\', '_').replace('/', '_') + '.o')
        obj.parent.mkdir(parents=True, exist_ok=True)
        if obj.exists() and obj.stat().st_mtime > max(source.stat().st_mtime, self.headers_changed):
            return obj
        if source.suffix == '.c':
            args = [self.cc, '-O2', '-DNDEBUG', '-w']
        else:
            args = [self.cxx, '-std=c++20', '-O2', '-DNDEBUG', '-Wall', '-Wextra', '-Wpedantic']
        self.run([*args, *self.trim, *(includes if includes is not None else self.include_flags), *extra,
                  '-c', source, '-o', obj],
                 f'compiling {relative}')
        return obj

    def compile_all(self, sources, group, extra=(), includes=None):
        paths = [s if Path(s).is_absolute() else REPO / s for s in sources]
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            return list(pool.map(lambda s: self.compile(s, group, extra, includes), paths))

    def link(self, out, objects, shared=False, extra=()):
        out.parent.mkdir(parents=True, exist_ok=True)
        args = [self.cxx, '-o', out, *objects, *extra, '-static', '-static-libgcc',
                '-static-libstdc++', '-s', '-lws2_32', '-lwinmm', '-lcrypt32', '-liphlpapi', '-lbcrypt']
        if shared:
            args.insert(1, '-shared')
        self.run(args, f'linking {out.name}')
        self.check_imports(out)
        say(f'built {out.relative_to(REPO)} ({out.stat().st_size // 1024} KiB)')
        return out

    def check_imports(self, program):
        """Fails when the program needs a compiler runtime DLL at run time."""
        objdump = self.cxx.with_name('objdump.exe')
        if not objdump.is_file():
            say(f'  (no objdump beside the compiler: the imports of {program.name} were not checked)')
            return
        text = self.run([objdump, '-p', program], f'reading the imports of {program.name}').stdout
        dlls = sorted({m.lower() for m in re.findall(r'DLL Name:\s*(\S+)', text)})
        bad = [d for d in dlls if re.match(r'lib(stdc\+\+|gcc|winpthread|c\+\+|unwind)', d)]
        if bad:
            fail(f'{program.name} needs {", ".join(bad)}: it was not linked statically')
        say(f'  imports of {program.name}: {", ".join(dlls)}')


def assemble(dll, server, fmt=None):
    """The folder a player drops into <game>/mods/."""
    if MOD_OUT.exists():
        shutil.rmtree(MOD_OUT)
    (MOD_OUT / 'bin').mkdir(parents=True)
    shutil.copy2(HERE / 'mod' / 'mod.json', MOD_OUT / 'mod.json')
    shutil.copy2(HERE / 'mod' / 'launcher_plugin.py', MOD_OUT / 'launcher_plugin.py')
    shutil.copy2(HERE / 'mod' / 'README.txt', MOD_OUT / 'README.txt')
    shutil.copy2(dll, MOD_OUT / 'bin' / 'coop_nx2navi.dll')
    shutil.copy2(server, MOD_OUT / 'bin' / 'oot-coop-server.exe')
    # The puppet pack tool: the launcher add-on makes the other players' models with it, on
    # the player's PC from their own game files (never shipped: the mod holds no game data).
    tool = MOD_OUT / 'tools' / 'puppet_pack'
    tool.mkdir(parents=True)
    # Every module of the tool (build.py imports them all; a missing one stops the pack).
    # Test helpers stay behind: the release maker (tools/release/make_nx2navi_mod.py) refuses them.
    for source in sorted((REPO / 'tools' / 'puppet_pack').glob('*.py')):
        if source.name.startswith('test') or source.name.endswith('_test.py'):
            continue
        shutil.copy2(source, tool / source.name)
    # The programs are GPL-3.0 and hold ENet (MIT): their licences go with them.
    shutil.copy2(REPO / 'LICENSE', MOD_OUT / 'LICENSE.txt')
    # The repository's notices, then this mod's own (the plugin interface header).
    notices = (REPO / 'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8').rstrip('\n') + '\n\n' + \
        (HERE / 'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
    if fmt is not None:
        # The DLL that runs upstream's hook holds fmt (MIT, header only): its licence text goes with it, from
        # the checkout the build used (the commit hookshot/CMakeLists.txt pins, checked by fetch()).
        licence = (fmt / 'LICENSE').read_text(encoding='utf-8').replace('\r\n', '\n').rstrip('\n')
        notices = (notices.rstrip('\n') + '\n\n## fmt (in `bin\\coop_nx2navi.dll`): its licence\n\n```\n' +
                   licence + '\n```\n')
    (MOD_OUT / 'THIRD_PARTY_NOTICES.md').write_bytes(notices.encode('utf-8'))
    say(f'mod folder: {MOD_OUT}')
    for path in sorted(MOD_OUT.rglob('*')):
        if path.is_file():
            say(f'  {path.relative_to(MOD_OUT)}  {path.stat().st_size} bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--compiler', help='the C++ compiler (g++.exe or clang++.exe of a MinGW toolchain)')
    parser.add_argument('--tests', action='store_true', help='also build and run the unit tests')
    parser.add_argument('--upstream', action='store_true',
                        help='(the default since 2026-11-13; kept so that older command lines still work)')
    parser.add_argument('--own-port', action='store_true',
                        help="the mod folder gets the older DLL, the port's own wiring (coop-dll 0.9), instead of "
                             "the one that runs upstream's hook unchanged (dll/upstream.hpp)")
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    options = parser.parse_args()
    if os.name != 'nt':
        fail('the NX2navi mod is a Windows DLL; build it on Windows')

    cxx, cc = find_compiler(options.compiler)
    say(f'compiler: {cxx}')
    builder = Builder(cxx, cc, max(1, options.jobs))

    enet = fetch('enet', 'enet.cmake')
    builder.include_flags.append(f'-isystem{enet / "include"}')
    enet_objects = builder.compile_all(
        [p for p in sorted(enet.glob('*.c')) if p.name != 'unix.c'], 'enet')
    # Monocypher (cmake/monocypher.cmake): the traffic encryption's primitives, one C file.
    monocypher = fetch('monocypher', 'monocypher.cmake')
    builder.include_flags.append(f'-isystem{monocypher / "src"}')
    enet_objects += builder.compile_all([monocypher / 'src' / 'monocypher.c'], 'monocypher')
    coop_objects = builder.compile_all(COOP, 'coop')

    server_lib = builder.compile_all(SERVER_LIB, 'server')
    # The mod's server gives an enemy to the nearest player's game by default (server/options.hpp;
    # --owner classic puts the shared code's rule back). The tests' server library is built without it.
    server = builder.link(BUILD / 'bin' / 'oot-coop-server.exe',
                          builder.compile_all(['server/main.cpp', 'server/options.cpp'], 'server_mod',
                                              ['-DCOOP_SERVER_OWNER_NEAREST']) +
                          [o for o in server_lib if 'options.cpp' not in o.name] +
                          coop_objects + enet_objects)
    dll_lib = builder.compile_all(DLL_LIB, 'dll')
    dll = builder.link(BUILD / 'bin' / 'coop_nx2navi.dll',
                       builder.compile_all(DLL_MAIN, 'dll') +
                       builder.compile_all(DLL_HOOKED, 'dll_hooked', ['-DLICHII_COOP']) + dll_lib +
                       coop_objects + enet_objects, shared=True)
    fmt = None
    if not options.own_port:
        # fmt (MIT) for the hook's log lines, at the commit hookshot/CMakeLists.txt pins; header-only here.
        fmt = fetch('fmt', 'hookshot/CMakeLists.txt')
        up_includes = [f'-I{REPO / i}' for i in UP_INCLUDES] + [
            f'-isystem{fmt / "include"}', f'-isystem{enet / "include"}', f'-isystem{monocypher / "src"}']
        up_flags = ['-DLICHII_COOP', '-DCOOP_HOOKSHOT', '-DCOOP_UPSTREAM', '-DFMT_HEADER_ONLY',
                    '-DLICHII_COOP_VERSION="nx2navi"', '-DLICHII_COOP_SPAWN_CREATE_TIMEOUT_MS=20000', '-DLICHII_COOP_TIME_RESYNC_SECONDS=180', '-DLICHII_COOP_TIME_SET_EVERY_US=15000000', '-Wno-unused-parameter', '-Wno-missing-field-initializers']
        lib = [o for o in dll_lib if 'coop_settings' not in o.name and 'coop_colors' not in o.name]
        dll = builder.link(BUILD / 'bin' / 'coop_nx2navi_up.dll',
                           builder.compile_all(UP_OWN + hook_sources() + ['coop/overlay/src/overlay.cpp', 'coop/src/version.cpp'],
                                               'dll_up', up_flags, up_includes) + lib +
                           coop_objects + enet_objects, shared=True)
    assemble(dll, server, fmt)

    if options.tests:
        doctest = fetch('doctest', 'doctest.cmake')
        extra = [f'-isystem{doctest}', f'-DCOOP_TEST_SOURCE_DIR="{(REPO / "tests").as_posix()}"']
        tests = builder.link(BUILD / 'bin' / 'coop_nx2navi_tests.exe',
                             builder.compile_all(TESTS, 'tests', extra) + dll_lib + server_lib +
                             coop_objects + enet_objects)
        # The stand-in game process for nx2navi/tests/test_dll.py.
        builder.link(BUILD / 'bin' / 'coop_nx2navi_fake_game.exe',
                     builder.compile_all(['nx2navi/tests/fake_game.cpp'], 'tests'), extra=['-municode'])
        # The same, built as a game with NX2navi's plugin interface: it exports
        # nx2navi_plugin_api and drives the DLL's overlay, key and text callbacks.
        builder.link(BUILD / 'bin' / 'coop_nx2navi_fake_host.exe',
                     builder.compile_all(['nx2navi/tests/fake_game.cpp'], 'tests_host',
                                         ['-DCOOP_FAKE_PLUGIN_HOST']), extra=['-municode'])
        say('running the unit tests')
        done = subprocess.run([str(tests)], env=builder.env)
        if done.returncode != 0:
            fail('the unit tests failed')
        for script in ('test_plugin.py', 'test_upstream.py', 'test_dll.py'):
            say(f'running nx2navi/tests/{script}')
            if subprocess.run([sys.executable, '-B', str(HERE / 'tests' / script)]).returncode != 0:
                fail(f'{script} failed')
        say('running nx2navi/tests/objects/build_tests.py')
        if subprocess.run([sys.executable, '-B', str(HERE / 'tests' / 'objects' / 'build_tests.py'),
                           '--compiler', str(cxx)]).returncode != 0:
            fail('the shared-enemies tests failed')
    say('done')


if __name__ == '__main__':
    main()
