# Guide for coding agents

You are an AI coding agent and someone has handed you this repository. This page tells you what the project is, the rules you must never break, what to do with what the user gives you, and how to turn it into a pull request. Read it all before you change anything. Humans: the same rules are in [CONTRIBUTING.md](CONTRIBUTING.md).

## The hard rule (read first)

**Nothing that came out of the game goes into the repository, an issue or a pull request.** No game files, keys, firmware, saves, extracted data, shaders, shader caches, audio, movies, screenshots of the game's files, or builds that contain any of them. Only information about the game goes in: addresses, offsets, hashes, short masked instruction signatures made by `tools/game_scan` ([docs/game-builds.md](docs/game-builds.md)), and code we wrote.

- Never ask the user for game files, keys or firmware, and never accept them. If they paste or attach one, do not read it into the repository; tell them it cannot be used.
- Logs: a zip from `scripts\collect-logs.cmd` is already masked. Still, never paste addresses, IP addresses, user names or passwords from a log into an issue, a commit or a pull request. Quote only the lines you need, masked.
- `tools/check_content.py` enforces this. It must pass. Never bypass it or the git hook (`--no-verify`).

## Licence rules

- The project is GPL-3.0. Your contribution is GPL-3.0.
- Copy code only from projects whose licence is compatible (MIT, BSD, ISC, zlib, Apache-2.0, CC0, GPL-3.0). Keep its header, credit it next to the code and add an entry to [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Projects without a licence file (Ship of Harkinian and its Anchor server) and closed projects (the TOTK Online mod): study how they behave if useful, copy nothing, not even small pieces.
- Never add secrets, tokens, passwords or personal data.

## What the project is

Online co-op for the Ocarina of Time remake on PC. Up to 16 players see each other as Link, share progress and fight the same enemies.

- **Today:** co-op runs on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot), a native PC recompilation of the game (the continuation of Hookshot). Releases are Longshot only. The emulator (Fritchii) build still exists in the code, but its releases ended.
- **Direction for 1.0:** co-op ships as an add-on module for the recomp (`hookshot_coop.dll`). Patches to the recomp stay minimal and generic (a hook layer: frame calls, input, drawing, memory); every co-op feature lives in our module (`hookshot/src` plus the shared `lichii/hook` code). Prefer a change in the module over a new patch.
- One design, one wire protocol, one server for every build.

## Repo map

| Path | What it is |
|---|---|
| `coop/` | Shared library: `protocol/` (messages and codec), `net/` (ENet transport, clock), `host/` (game memory readers, flags, saves), `presence/`, `overlay/`, `voice/` |
| `server/` | `coop_server`: relays and decides, never runs the game (sessions, relay, UPnP, hole punching) |
| `lichii/hook/` | The co-op game code: puppets, objects, flags, dungeons, chat commands. Built into both the emulator and the Longshot module. File list in `lichii/hook/core.cmake` |
| `lichii/` (rest) | Emulator pin (`PIN`), fetch scripts and patches for Fritchii |
| `hookshot/src/` | The Longshot module: frame driver, menus, chat box, drawing, input |
| `hookshot/patches/`, `hookshot/PIN` | Our small patches to Longshot and the commit they apply to |
| `hookshot/compat/` | Stand-ins for the emulator API that `lichii/hook` uses |
| `tests/` | doctest unit tests, one folder per area (`tests/lichii/` for `lichii/hook`, `tests/protocol/`, ...) |
| `tools/` | `check_content.py`, `autotest/` (two-game tests, maintainer only), `release/` (kit scripts and their tests), `puppet_pack/`, `netsim/`, `game_scan/`, `coop_test/` |
| `docs/` | Design and player docs (below) |

Where things are written down:

- Wire protocol, message types, channels and version history: [docs/protocol.md](docs/protocol.md); code in `coop/protocol/`.
- What is shared between games and what is not, per actor class: [docs/sync-coverage.md](docs/sync-coverage.md).
- Area design notes: `docs/objects.md`, `flags.md`, `items.md`, `dungeon.md`, `dungeons-boss.md`, `world.md`, `spawns.md`, `puppet.md`, `time.md`, `voice.md`, `scenes.md`.
- Game addresses: next to the code that uses them (`lichii/hook/*.h`, `coop_sites.h`, `coop_game_scan_patterns.inc`) and in the area docs. Game builds and signatures: [docs/game-builds.md](docs/game-builds.md).
- Longshot module and patches: [docs/hookshot.md](docs/hookshot.md). Plan: [docs/plan.md](docs/plan.md). Known issues: [docs/known-issues.md](docs/known-issues.md). Tests: [docs/testing.md](docs/testing.md).

## What to do with what the user gives you

First look for an existing issue: `gh issue list --state all --search "<key words>"` (or the issues page). Ask the user only for what you cannot get yourself: the co-op version (`VERSION.txt` in their kit), the game version, who hosted, how many players, the steps. Never ask for game files.

| The user gives you | Do this |
|---|---|
| A crash log, a `collect-logs` zip, an error message | Find the matching issue or draft one from the Crash form (`.github/ISSUE_TEMPLATE/1-crash.yml`). Find the last co-op lines (`coop:`) before the crash and the code that prints them (`grep -rn` in `lichii/hook`, `hookshot/src`, `coop`). Find the cause. Write a unit test that fails for it, fix, run the tests, open a PR. |
| A desync ("X looks different on the other screen", "the boss is not the same for both") | Look the thing up in [docs/sync-coverage.md](docs/sync-coverage.md): is it shared at all, by which path, is it proven. Read its area doc and the messages it uses in [docs/protocol.md](docs/protocol.md). Not shared yet: that is a feature (next rows). Shared but wrong: find where the owner sends it and where the copy applies it, test, fix. Update its sync-coverage row in the same PR. Draft the issue from the Desync form if there is none. |
| A screenshot, a Discord message, a bug description | Turn it into the facts the Bug form asks for (`3-bug.yml`), then the same as a crash: issue, cause, failing test, fix. |
| A feature idea ("add a /wave command") | Check open issues and [docs/plan.md](docs/plan.md). Find the nearest existing feature and copy its shape (for a chat command: `lichii/hook/coop_teleport.{h,cpp}` parses `/teleport`, its test is `tests/lichii/teleport_test.cpp`, and the Commands page lists it in `hookshot/src/coop_pause_menu.cpp`). A new or changed message goes in [docs/protocol.md](docs/protocol.md) in the same PR. Keep it small and on one topic. |
| A typo or a docs fix | Fix it and open a PR. No issue needed. |
| A question about how something works | Answer from the docs and code; change nothing unless asked. |

If the cause needs a game run to find, say so, write down what to check in the game, and stop guessing.

## Commands

Needs CMake 3.24+, a C++20 compiler (Visual Studio 2022 on Windows, GCC or Clang on Linux), Python 3.10+. ENet, doctest and Monocypher are fetched by CMake at pinned commits.

```sh
git config core.hooksPath .githooks        # once: runs check_content before every commit
cmake -S . -B build -DCOOP_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python tools/check_content.py
```

Or all of it at once: `tools/dev-check.sh` (Linux, macOS, Git Bash) or `powershell -File tools/dev-check.ps1` (Windows).

Python tool tests (CI runs these; run the ones for what you touched): `python tools/puppet_pack/tests/test_puppet_pack.py`, `tools/puppet_pack/tests/test_title_menu.py`, `tools/autotest/tests/test_save_flags.py`, `tools/autotest/tests/test_hookshot.py`, `tools/netsim/tests/test_netsim.py`, `tools/release/tests/test_apply_patch.py`, `tools/check_actions_test.py`. Kit script tests: `tools/release/tests/*_test.ps1`. The full list is in `.github/workflows/build.yml`.

A new test file goes in the `add_executable(coop_tests ...)` list in `tests/CMakeLists.txt`, and a new `lichii/hook` source it needs goes there too. Test names must not contain `;`.

**You do not need the game.** You cannot run it and should not try: there are no game files here. The unit tests and CI are your proof. Whatever only the game can show goes in the PR's "Check in game" list; the maintainer checks it in one combined in-game test ([docs/testing.md](docs/testing.md)).

## How to contribute

1. Fork the repository and branch from `main`. Branch name: a short lowercase topic with hyphens (`wave-command`, `boss-door-fix`). Never push to this repository directly.
2. One topic per branch and per pull request. Small diffs. Tests next to the code they test.
3. Message changes: a new message type, a component or a layout change comes with [docs/protocol.md](docs/protocol.md) changed in the same PR. Follow its "Versioning" section: a layout an older reader cannot skip raises the version; new types take a free range (each branch keeps a block of 5, for example "355 to 359 kept"). Type numbers, the protocol version and other shared numbers (settings keys, patch numbers) are claimed by nobody until merged: say in the PR which numbers you took, so the maintainer can renumber if another PR took them first.
4. Changes to `.github/workflows/` pin each action to a full commit hash with its version in a comment (`tools/check_actions.py` checks it).
5. Commit messages: plain sentences, no `feat:` prefixes. Name a source file in the message if you copied code from it.
6. Fill in the PR template (`.github/PULL_REQUEST_TEMPLATE.md`) honestly:
   - **Issues:** `Fixes #N` only for what is fully resolved and proven (in the game, if the issue is about the game). Otherwise `Part of #N`. Since you cannot run the game, a game bug is usually `Part of #N` with the in-game check listed.
   - **Testing:** the unit tests you added and ran, and that CI is green.
   - **Check in game:** short steps the maintainer can follow.
   - **Not yet proven:** everything else.
7. Before you push: build, unit tests and `tools/check_content.py` pass. Never use `--no-verify`, never force-push someone else's branch, never commit build output.
8. Every PR is reviewed before it is merged. Answer review comments by pushing to the same branch.

## Style

- English, plain short sentences, in code comments, docs, commits and PRs. Match the docs' tone: say what happens, not how great it is.
- Follow the surrounding code: C++20, the existing names and file layout. No new dependency without a reason in the PR and an entry in `THIRD_PARTY_NOTICES.md`.
- Change docs when behaviour changes: the area doc, `docs/sync-coverage.md` for what is shared, `docs/known-issues.md` for what players see.
- Do not write release names or old version tags in the repository (`check_content.py` refuses them); give the date or the issue instead.
