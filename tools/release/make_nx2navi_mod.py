# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the release files of the NX2navi co-op mod.

    python tools/release/make_nx2navi_mod.py                 build (with tests), then pack
    python tools/release/make_nx2navi_mod.py --no-tests      without the unit tests
    python tools/release/make_nx2navi_mod.py --version 0.2.0 refuse unless mod.json says 0.2.0
    python tools/release/make_nx2navi_mod.py --publish       also make a DRAFT PRE-RELEASE on GitHub

It runs nx2navi/build.py (which builds coop_nx2navi.dll and oot-coop-server.exe and puts the
mod folder together), then writes into build/nx2navi-release/:

    OoT Co-op.nx2mod   the mod folder as one file (NX2navi: docs/mods-launcher.md, "Single-file
                       mods"): a zip with mod.json at its top. Deterministic: sorted names,
                       fixed timestamps and attributes, so the same files give the same bytes.
    SHA256SUMS.txt     `sha256sum -c` format
    notes.md           release notes: version, commit, what works and what does not

Nothing of the game and nothing local goes in. Only an allowlist of files ships (mod.json, the
launcher add-on, README, licence and notices, bin\\*.dll and *.exe, the puppet pack tool's
Python files). Game data (romfs\\, exefs\\), the player's coop.ini, settings.json, logs,
coop_puppet.json, .pdb files, caches and anything not on the list stop the release. The repo's
own content guard (tools/check_content.py) runs on the staged files, and the programs must not
name the build machine's folders (the rule of tools/release/check_paths.ps1 and
cmake/trim_paths.cmake, here in Python).

Options for tests and odd setups: --mod-dir packs another mod folder (a copy, say), --no-build
skips build.py, --out names another output folder. --nx2navi names the NX2navi checkout whose
launcher/mods.py install_archive installs the finished file into a temp folder as a last check
(default: ../NX2navi beside this repo, else the check is skipped).

Python 3.10 or newer, standard library only; --publish needs the GitHub CLI (gh) logged in.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_MOD = REPO / 'build' / 'nx2navi-mod' / 'OoT Co-op'
DEFAULT_OUT = REPO / 'build' / 'nx2navi-release'
ZIP_TIME = (2020, 1, 1, 0, 0, 0)   # a fixed date: the zip's bytes depend on the files only
README = 'nx2navi/README.md'

# What may be in the mod folder. Anything else stops the release.
TOP_FILES = {'mod.json', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.md'}   # + the launcher add-on
REQUIRED = ['mod.json', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.md',
            'bin/coop_nx2navi.dll', 'bin/oot-coop-server.exe', 'tools/puppet_pack/build.py']
# Never, with the reason said. Checked before the allowlist.
FORBIDDEN_DIRS = {'romfs': 'game data: the puppet pack is built on the player\'s PC from their own files',
                  'exefs': 'game data', 'nand': 'game data', 'keys': 'keys',
                  '__pycache__': 'compiled Python names the folder it was compiled in',
                  'logs': 'a log folder'}
FORBIDDEN_NAMES = {'coop.ini': 'the player\'s settings (they hold addresses and passwords)',
                   'settings.json': 'the launcher add-on\'s settings (passwords, as plain text)',
                   'coop_puppet.json': 'made on the player\'s PC beside the puppet pack',
                   'coop-options.ini': 'the player\'s settings', 'prod.keys': 'keys', 'title.keys': 'keys'}
FORBIDDEN_SUFFIXES = {'.log': 'a log', '.pdb': 'symbols hold the build machine\'s folders',
                      '.pyc': 'compiled Python', '.ini': 'local settings', '.keys': 'keys',
                      '.zs': 'game data', '.pack': 'game data', '.sarc': 'game data',
                      '.bfres': 'game data', '.nsp': 'game data', '.xci': 'game data'}


def say(text):
    print(text, flush=True)


def fail(text):
    say('make_nx2navi_mod.py: ' + text)
    sys.exit(1)


def git(*args):
    try:
        done = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True)
    except OSError:
        return None
    return done.stdout.strip() if done.returncode == 0 else None


# ---------------------------------------------------------------- what ships

def refusal(rel, launcher):
    """None when the file (a posix path inside the mod folder) may ship, else why not."""
    parts = rel.split('/')
    name = parts[-1].lower()
    for part in parts[:-1]:
        if part.lower() in FORBIDDEN_DIRS:
            return f'is inside {part}\\: {FORBIDDEN_DIRS[part.lower()]}'
    if any(p.startswith('.') for p in parts):
        return 'is hidden'
    if name in FORBIDDEN_NAMES:
        return FORBIDDEN_NAMES[name]
    suffix = os.path.splitext(name)[1]
    if suffix in FORBIDDEN_SUFFIXES:
        return FORBIDDEN_SUFFIXES[suffix]
    if len(parts) == 1 and (rel in TOP_FILES or rel == launcher):
        return None
    if len(parts) == 2 and parts[0] == 'bin' and suffix in ('.dll', '.exe'):
        return None
    if len(parts) == 3 and parts[:2] == ['tools', 'puppet_pack'] and suffix == '.py' \
            and not name.startswith('test') and not name.endswith('_test.py') and 'place_test' not in name:
        return None
    return 'is not on the list of files the mod ships'


def read_manifest(mod_dir):
    path = mod_dir / 'mod.json'
    if not path.is_file():
        fail(f'{path} not found: build the mod first (nx2navi/build.py) or name its folder with --mod-dir')
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        fail(f'mod.json cannot be read: {e}')
    name, version = data.get('name'), data.get('version')
    if not isinstance(name, str) or not name or re.search(r'[|/\\:*?"<>]|^\.|^\s|\s$', name):
        fail(f'mod.json: "name" is not usable as a file name: {name!r}')
    if not isinstance(version, str) or not re.fullmatch(r'\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?', version):
        fail(f'mod.json: "version" must look like 1.2.3, not {version!r}')
    launcher = data.get('launcher') or ''
    if not isinstance(launcher, str) or '/' in launcher or '\\' in launcher:
        fail('mod.json: "launcher" must be a file name at the top of the mod folder')
    return data, name, version, launcher


def collect(mod_dir, launcher):
    """The shippable files as sorted (posix path, Path). Fails on anything that must not go out."""
    refused, files, bad_dirs = [], [], []
    for path in sorted(mod_dir.rglob('*'), key=lambda p: p.relative_to(mod_dir).as_posix()):
        rel = path.relative_to(mod_dir).as_posix()
        if any(rel.startswith(d) for d in bad_dirs):
            continue   # said once, for the folder
        if path.is_symlink():
            refused.append(f'{rel}: is a link')
        elif path.is_dir():
            part = rel.split('/')[-1].lower()
            if part in FORBIDDEN_DIRS or part.startswith('.'):
                refused.append(f'{rel}/: {FORBIDDEN_DIRS.get(part, "is hidden")}')
                bad_dirs.append(rel + '/')
        else:
            why = refusal(rel, launcher)
            if why:
                refused.append(f'{rel}: {why}')
            else:
                files.append((rel, path))
    if refused:
        more = f'\n  ... and {len(refused) - 40} more' if len(refused) > 40 else ''
        fail('the mod folder holds files that must not ship:\n  ' + '\n  '.join(refused[:40]) + more)
    have = {rel for rel, _ in files}
    missing = [r for r in REQUIRED + ([launcher] if launcher else []) if r not in have]
    if missing:
        fail('the mod folder lacks: ' + ', '.join(missing))
    return files


# ---------------------------------------------------------------- the repo's checks

def load_check_content():
    spec = importlib.util.spec_from_file_location('check_content', REPO / 'tools' / 'check_content.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_content_guard(stage, files):
    """tools/check_content.py's rules (game file types, key names, content folders, release
    names in text) on the staged files. Its 2 MB limit is for the repo's sources, not for the
    programs, so it is not applied under bin\\."""
    guard = load_check_content()
    bad = []
    for rel, _ in files:
        why = guard.problem(str(stage), rel)
        if why and not (rel.startswith('bin/') and why == 'is larger than 2 MB'):
            bad.append(f'{rel}: {why}')
        bad += [f'{rel}: {line}' for line in guard.release_names(str(stage), rel)]
    if bad:
        fail('tools/check_content.py rejects the staged files:\n  ' + '\n  '.join(bad))
    say('content guard (tools/check_content.py rules): passed')


def path_needles():
    """The build machine's folders that must not be in anything shipped: the user folder without
    its drive, with either slash (check_paths.ps1's Get-UserPathNeedles), and this checkout."""
    needles = set()
    homes = [os.environ.get('USERPROFILE'), str(Path.home()), os.environ.get('HOME')]
    for home in homes:
        if not home:
            continue
        path = re.sub(r'^[A-Za-z]:', '', home.replace('/', '\\').rstrip('\\'))
        path = re.sub(r'^\\([A-Za-z])\\', r'\\', path)   # /c/Users/x of a Unix shell
        if len(path) >= 3 and re.match(r'^\\[^\\]', path):
            needles.update({path, path.replace('\\', '/')})
    needles.update({str(REPO), REPO.as_posix()})
    return sorted(needles)


def find_paths(files, needles):
    """Files holding a needle as one-byte text or as UTF-16, any letter case."""
    hits = []
    for rel, path in files:
        data = path.read_bytes().lower()
        for needle in needles:
            n = needle.lower()
            wide = ''.join(c + '\0' for c in n).encode('latin-1', 'replace')
            if n.encode('utf-8', 'replace') in data or wide in data:
                hits.append((rel, needle))
                break
    return hits


def run_path_check(files):
    needles = path_needles()
    hits = find_paths(files, needles)
    if hits:
        fail("the build machine's folders appear in shipped files: " +
             ', '.join(f'{rel} (has {n})' for rel, n in hits[:8]) +
             '. Build with trimmed paths (nx2navi/build.py passes -ffile-prefix-map; cmake/trim_paths.cmake).')
    say(f'build paths: none of {len(needles)} build-machine folder forms in {len(files)} files')


# ---------------------------------------------------------------- the files

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def write_archive(files, out):
    """A deterministic zip: names sorted, timestamps and attributes fixed."""
    tmp = out.with_name(out.name + '.tmp')
    try:
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for rel, path in sorted(files):
                info = zipfile.ZipInfo(rel, ZIP_TIME)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                z.writestr(info, path.read_bytes(), compresslevel=9)
        os.replace(tmp, out)
    finally:
        tmp.unlink(missing_ok=True)


def commit_line():
    sha = git('rev-parse', 'HEAD')
    if not sha:
        return 'unknown (not a git checkout)', None, False
    dirty = bool(git('status', '--porcelain', '--untracked-files=no'))
    return sha + (' (with uncommitted changes)' if dirty else ''), sha, dirty


def notes_text(name, version, commit, archive, digest, size):
    return f"""# {name} {version} for NX2navi (pre-release)

Co-op as a mod of the NX2navi PC port. Source commit: `{commit}`.

**Not game data:** the file holds no game files. The puppets' models are made on your PC, from your own
game files, the first time you host or join with "Show Other Players" on.

## Install

Download `{archive}`. In the NX2navi launcher with the mod loader: Mods, Install a Mod, pick the file.
Or put it into `<game folder>\\mods` and the launcher installs it the next time it reads the folder.
Then use the new "Multiplayer" row on the home screen. Check the download with `SHA256SUMS.txt`
(`{archive}`: `{digest}`, {size:,} bytes).

## What works

- Host and Join from the launcher; the session, your presence (scene, position, pose, gear) sent to
  the others, with a join password; reconnects.
- With every NX2navi build: the others' joins, leaves, chat and scenes in the mod's log file.
- With an NX2navi build that has its plugin interface: an in-game panel, notices, a chat box, and the
  other players as a second Link in your scene.

## What does not work

Markers and name tags over the other players, their Navi, shared enemies, objects and world state,
and shared progress (experimental, off) are missing or unverified. The mod was tested with two
games on one PC and not on two PCs; the real launcher's menu was not run by hand. The full list
is in [`{README}`]({README}) in the source.

## Licence

The mod's programs are GPL-3.0 (`LICENSE.txt`) and contain ENet (MIT); `THIRD_PARTY_NOTICES.md` has
the notices, including NX2navi's plugin interface header, copied under the permission in its
own notice. Source for this build is the commit above.
"""


def write_release(files, mod_dir, name, version, out):
    out.mkdir(parents=True, exist_ok=True)
    for old in out.iterdir():
        if old.is_file():
            old.unlink()
    archive = out / f'{name}.nx2mod'
    write_archive(files, archive)
    digest, size = sha256(archive), archive.stat().st_size
    commit, _, _ = commit_line()
    notes = out / 'notes.md'
    notes.write_text(notes_text(name, version, commit, archive.name, digest, size), encoding='utf-8', newline='\n')
    sums = out / 'SHA256SUMS.txt'
    sums.write_text(''.join(f'{sha256(p)}  {p.name}\n' for p in (archive, notes)), encoding='ascii', newline='\n')
    say(f'wrote {archive} ({size:,} bytes, sha256 {digest})')
    return archive, notes, sums


# ---------------------------------------------------------------- the install check

def find_nx2navi(asked):
    candidates = [Path(asked)] if asked else [Path(os.environ[v]) for v in ('NX2NAVI_DIR',) if os.environ.get(v)]
    if not asked:
        candidates.append(REPO.parent / 'NX2navi')
    for c in candidates:
        if (c / 'launcher' / 'mods.py').is_file():
            return c
    if asked:
        fail(f'--nx2navi: no launcher/mods.py in {asked}')
    return None


INSTALL_SCRIPT = r'''
import json, sys
sys.path.insert(0, sys.argv[1])
from launcher import mods
dest = mods.install_archive(sys.argv[2], sys.argv[3])
print(json.dumps({"dest": str(dest)}))
'''


def run_install_check(nx2navi, archive, files):
    """Installs the archive with NX2navi's own mods.install_archive and compares what it wrote."""
    with tempfile.TemporaryDirectory(prefix='nx2mod-check-') as tmp:
        done = subprocess.run([sys.executable, '-I', '-B', '-c', INSTALL_SCRIPT, str(nx2navi), str(archive), tmp],
                              capture_output=True, text=True)
        if done.returncode != 0:
            fail('NX2navi could not install the archive:\n' + (done.stdout + done.stderr).strip())
        dest = Path(json.loads(done.stdout.strip().splitlines()[-1])['dest'])
        got = {p.relative_to(dest).as_posix(): sha256(p) for p in dest.rglob('*') if p.is_file()}
        want = {rel: sha256(path) for rel, path in files}
        if got != want:
            diff = sorted(set(got) ^ set(want)) or sorted(k for k in want if got.get(k) != want[k])
            fail('the installed files differ from the archive: ' + ', '.join(diff[:10]))
        if dest.name != archive.stem:
            fail(f'installed into {dest.name!r}, not {archive.stem!r}')
        say(f'install check: mods.install_archive put {len(got)} files, identical, into <mods>\\{dest.name}')


# ---------------------------------------------------------------- publishing

def publish(version, archive, notes, sums):
    tag = f'nx2navi-v{version}'
    _, sha, dirty = commit_line()
    if not sha or dirty:
        fail('--publish needs a clean checkout at a commit (the release names it); commit first')
    if git('branch', '-r', '--contains', sha) == '':
        say('warning: this commit is on no remote branch; gh creates the tag on GitHub, which needs it pushed')
    if not shutil.which('gh'):
        fail('--publish needs the GitHub CLI (gh), logged in')
    # Draft and pre-release: nothing is public until someone presses Publish on GitHub.
    cmd = ['gh', 'release', 'create', tag, str(archive), str(sums), '--draft', '--prerelease',
           '--target', sha, '--title', f'OoT Co-op for NX2navi {version} (pre-release)',
           '--notes-file', str(notes)]
    say('running: ' + ' '.join(cmd))
    if subprocess.run(cmd, cwd=REPO).returncode != 0:
        fail('gh release create failed')
    say(f'draft pre-release {tag} created; review it on GitHub and publish it there')


# ---------------------------------------------------------------- main

def run_build(options):
    cmd = [sys.executable, str(REPO / 'nx2navi' / 'build.py')]
    if not options.no_tests:
        cmd.append('--tests')
    if options.compiler:
        cmd += ['--compiler', options.compiler]
    if options.jobs:
        cmd += ['--jobs', str(options.jobs)]
    say('running: ' + ' '.join(cmd))
    if subprocess.run(cmd, cwd=REPO).returncode != 0:
        fail('nx2navi/build.py failed; no release was made')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--no-tests', action='store_true', help='build without running the unit tests')
    parser.add_argument('--compiler', help='passed to build.py: the C++ compiler (g++.exe or clang++.exe)')
    parser.add_argument('--jobs', type=int, help='passed to build.py')
    parser.add_argument('--version', help='the version the release must have; fails when mod.json says another')
    parser.add_argument('--publish', action='store_true',
                        help='create a draft pre-release nx2navi-v<version> with gh (never done by default)')
    parser.add_argument('--no-build', action='store_true', help='pack the existing mod folder, do not run build.py')
    parser.add_argument('--mod-dir', type=Path, help=f'the mod folder to pack (default {DEFAULT_MOD})')
    parser.add_argument('--out', type=Path, help=f'output folder (default {DEFAULT_OUT})')
    parser.add_argument('--nx2navi', help='NX2navi checkout for the install check (default ../NX2navi)')
    options = parser.parse_args()
    if sys.version_info < (3, 10):
        fail('Python 3.10 or newer is needed')
    if options.mod_dir and not options.no_build:
        fail('--mod-dir packs a folder as it is: add --no-build')

    mod_dir = (options.mod_dir or DEFAULT_MOD).resolve()
    out = (options.out or DEFAULT_OUT).resolve()
    if not options.no_build:
        run_build(options)
    if mod_dir == out or out.is_relative_to(mod_dir):
        fail('the output folder must not be inside the mod folder')

    data, name, version, launcher = read_manifest(mod_dir)
    if options.version and options.version != version:
        fail(f'--version {options.version} but mod.json says {version}; change mod.json (nx2navi/mod/mod.json) '
             'and build again, the release does not rewrite it')
    say(f'{name} {version}, from {mod_dir}')
    files = collect(mod_dir, launcher)

    # The checks run on a staged copy: what is checked is what ships.
    with tempfile.TemporaryDirectory(prefix='nx2mod-stage-') as tmp:
        stage = Path(tmp)
        for rel, path in files:
            (stage / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, stage / rel)
        staged = [(rel, stage / rel) for rel, _ in files]
        run_content_guard(stage, staged)
        run_path_check(staged)
        archive, notes, sums = write_release(staged, mod_dir, name, version, out)

    for rel, _ in files:
        say(f'  {rel}')
    nx2navi = find_nx2navi(options.nx2navi)
    if nx2navi:
        run_install_check(nx2navi, archive, files)
    else:
        say('install check: skipped (no NX2navi checkout at ../NX2navi; --nx2navi names one)')
    if options.publish:
        publish(version, archive, notes, sums)
    say('done')


if __name__ == '__main__':
    main()
