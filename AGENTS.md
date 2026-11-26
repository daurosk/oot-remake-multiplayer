# Guide for coding agents

You are an AI coding agent and someone has handed you this repository. This page tells you what the project is, the rules you must never break, what to do with what the user gives you, and how to turn it into a pull request. Read it all before you change anything. Humans: the same rules are in [CONTRIBUTING.md](CONTRIBUTING.md).

## The hard rule (read first)

**Nothing that came out of the game goes into the repository, an issue or a pull request.** No game files, keys, firmware, saves, extracted data, shaders, shader caches, audio, movies, screenshots of the game's files, or builds that contain any of them. Only information about the game goes in: addresses, offsets, hashes, short masked instruction signatures made by `tools/game_scan` ([docs/game-builds.md](docs/game-builds.md)), and code we wrote.

- Never ask the user for game files, keys or firmware, and never accept them. If they paste or attach one, do not read it into the repository; tell them it cannot be used.
- Logs: a zip from `scripts\collect-logs.cmd` is already masked, but it goes through the `/bugreport` command on the Squadala! Discord (https://discord.gg/GwtRKAqng5), never as an attachment on a public issue or pull request ([docs/setup.md](docs/setup.md)). Still, never paste addresses, IP addresses, user names or passwords from a log into an issue, a commit or a pull request. Quote only the lines you need, masked.
- `tools/check_content.py` enforces this. It must pass. Never bypass it or the git hook (`--no-verify`).

## The UI rule

**Every co-op screen, menu, tab, notice, name tag and HUD piece is built from the game's own UI: its Layout panes, buttons, textures, fonts, icons and sounds, through the game's own UI system.** No hand-drawn UI: no rectangles, lines, bars, circles, boxes or panels drawn by our code (Dear ImGui or any other drawing), and no pictures we made. The game's assets are read from the player's own game at run time (never committed; see the hard rule above).

- Everything fits on screen: no row hidden below a scroll, no text cut off, nothing off the edge. When a list is longer than the screen, it pages (the game's own page buttons), and Back is always in view.
- It looks and behaves like the game's own menus: the game's focus frame, move and decide sounds, A to pick, B to go back one step.
- A pull request that adds a drawn shape or a screen that does not fit is sent back in review. Name in the pull request the game pane or texture each new UI piece uses.

## How we fix and build things

These are the project's standing engineering rules. A review sends a change back for breaking one.

- **Root cause, every related case.** No band-aids. Find the real cause and fix it there, then search the code for the same pattern (other bosses, enemies, scenes, flags, platforms, code paths) and fix every case that shares it, not only the reported one. In the PR say what the root cause is, which related cases you found, and how each is covered. A symptom patch, or a missed related case, is sent back.
- **Solo must work.** Anything that is not inherently multiplayer must work with co-op idle (no host, no join): settings, Customize, the Mods tab and mod loading, solo difficulty options, story and save fixes. Test and say in the PR that the idle path still works.
- **Native to the game.** Do it with the game's own functions, actors, flags, animations and assets, through the hook layer and our module. No poking game memory from outside, no copying state, poses or pixels to fake an effect. Name the game functions you used in the PR. Keep patches to the recomp small and generic; features live in the module.
- **A remote action must look like the real one.** When another player hits an enemy, opens a door or lifts a platform, your screen shows what a local action shows through the game's own path: the enemy flinches, the hit flash, sparks and sound play, the health bar changes. A remote hit that is silent or invisible is a bug.
- **Screenshot proof for anything visible.** A log line only proves a message was sent. For anything you can see (platforms, doors, actors, items, HUD, other players, menus), list the visible checks under "Check in game" in the PR; the maintainer's in-game run takes the screenshots on every game concerned. Do not call it proven from log lines.
- **Nothing sent to Longshot.** Our builds never send crash reports, telemetry or performance statistics to Longshot or anyone else, not even opt-in. Players report problems through the Squadala! Discord `/bugreport` command and `scripts\collect-logs.cmd` ([docs/problem-reports.md](docs/problem-reports.md)).
- **Discord Rich Presence is ours.** When the mod is loaded it always shows the Squadala name, icon and our own status (solo and co-op), replacing the game's own presence ([docs/presence.md](docs/presence.md)).
- **Everything shipped must work together.** A change is checked against what else ships with it: other contributors' features, public and private games, Character Select, colour options, with and without the mod on the other side. Where two pieces clash, add a fix on top; do not drop either.
- **Honest reports.** Say what you ran and what you did not. Never write "fixed" for what only a game run could show.

## Licence rules

- The project is GPL-3.0. Your contribution is GPL-3.0.
- Copy code only from projects whose licence is compatible (MIT, BSD, ISC, zlib, Apache-2.0, CC0, GPL-3.0). Keep its header, credit it next to the code and add an entry to [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Projects without a licence file (Ship of Harkinian and its Anchor server) and closed projects (the TOTK Online mod): study how they behave if useful, copy nothing, not even small pieces.
- Never add secrets, tokens, passwords or personal data.

## What the project is

Online co-op for the Ocarina of Time remake on PC. Up to 16 players see each other as Link, share progress and fight the same enemies.

- **Today:** co-op runs on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot), a native PC recompilation of the game by Team MagicBean, whose changes are being merged into [Hookshot](https://git.notrexed.dev/Hookshot/Hookshot) (NotRexed and contributors). Releases are Longshot only. The emulator (Fritchii) build still exists in the code, but its releases ended.
- **Direction for 1.0:** co-op ships as an add-on module for the recomp (`hookshot_coop.dll`). Patches to the recomp stay minimal and generic (a hook layer: frame calls, input, drawing, memory); every co-op feature lives in our module (`hookshot/src` plus the shared `lichii/hook` code). Prefer a change in the module over a new patch.
- One design, one wire protocol, one server for every build.

## Repo map

| Path | What it is |
|---|---|
| `coop/` | Shared library: `protocol/` (messages and codec), `net/` (ENet transport, clock), `host/` (game memory readers, flags, saves), `presence/`, `overlay/`, `voice/` |
| `server/` | `coop_server`: relays and decides, never runs the game (sessions, relay, UPnP, hole punching) |
| `lichii/hook/` | The co-op game code: puppets, objects, flags, dungeons, chat commands. Built into the Longshot module (it was also built into the retired emulator). File list in `lichii/hook/core.cmake` |
| `lichii/` (rest) | Retired (0.11.12): emulator pin (`PIN`), fetch scripts and patches for Fritchii; not built or shipped, kept for reference |
| `hookshot/src/` | The Longshot module: frame driver, menus, chat box, drawing, input |
| `hookshot/patches/`, `hookshot/PIN` | Our small patches to Longshot and the commit they apply to |
| `hookshot/compat/` | Stand-ins for the emulator API that `lichii/hook` uses |
| `tests/` | doctest unit tests, one folder per area (`tests/lichii/` for `lichii/hook`, `tests/protocol/`, ...) |
| `tools/` | `check_content.py`, `autotest/` (two-game tests, maintainer only), `playtest/` (shared play-test scripts and game data), `release/` (kit scripts and their tests), `puppet_pack/`, `netsim/`, `game_scan/`, `coop_test/` |
| `docs/` | Design and player docs (below) |

Where things are written down. **Read [docs/README.md](docs/README.md) first** (one line per doc: what it answers and when to read it), then only the one doc for your task. **Look an address up in [docs/addresses.md](docs/addresses.md) before you search the code or the game for it** (every address and offset the code names, with its comment, generated by `tools/address_index.py`).

- Wire protocol, message types, channels and version history: [docs/protocol.md](docs/protocol.md); code in `coop/protocol/`.
- What is shared between games and what is not, per actor class: [docs/sync-coverage.md](docs/sync-coverage.md).
- Area design notes: `docs/objects.md`, `flags.md`, `items.md`, `dungeon.md`, `dungeons-boss.md`, `world.md`, `spawns.md`, `puppet.md`, `time.md`, `voice.md`, `scenes.md`.
- Game addresses: [docs/addresses.md](docs/addresses.md) first; the code that uses them (`lichii/hook/*.h`, `coop_sites.h`, `coop_game_scan_patterns.inc`) and the area docs hold the layouts and the reasoning. Game builds and signatures: [docs/game-builds.md](docs/game-builds.md).
- What is done, open and waited on: [docs/agent-state.md](docs/agent-state.md). What the test-kit work learned about the game (offsets, behaviour found in runs): [docs/agent-handoff.md](docs/agent-handoff.md).
- Longshot module and patches: [docs/hookshot.md](docs/hookshot.md). Plan: [docs/plan.md](docs/plan.md). Known issues: [docs/known-issues.md](docs/known-issues.md). Tests: [docs/testing.md](docs/testing.md); live two-game play-testing: [docs/playtest.md](docs/playtest.md).
- Do not read a long doc whole: search it (`grep -n`) and read the range. When you find something new about the game, write it in the area doc (and the address in the code next to its constant), so the next agent does not look it up again.

## What to do with what the user gives you

First look for an existing issue: `gh issue list --state all --search "<key words>"` (or the issues page). Ask the user only for what you cannot get yourself: the co-op version (`VERSION.txt` in their kit), the game version, who hosted, how many players, the steps. Never ask for game files.

| The user gives you | Do this |
|---|---|
| A crash log, a `collect-logs` zip, an error message | Find the matching issue or draft one from the Crash form (`.github/ISSUE_TEMPLATE/1-crash.yml`); tell the user to attach the zip to `/bugreport` on the Squadala! Discord, not to the issue. Find the last co-op lines (`coop:`) before the crash and the code that prints them (`grep -rn` in `lichii/hook`, `hookshot/src`, `coop`). Find the cause. Write a unit test that fails for it, fix, run the tests, open a PR. |
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

Python tool tests (CI runs these; run the ones for what you touched): `python tools/puppet_pack/tests/test_puppet_pack.py`, `tools/puppet_pack/tests/test_title_menu.py`, `tools/autotest/tests/test_save_flags.py`, `tools/autotest/tests/test_hookshot.py`, `tools/netsim/tests/test_netsim.py`, `tools/release/tests/test_apply_patch.py`, `tools/check_actions_test.py`. Kit script tests: `tools/release/tests/*_test.ps1`. The full list is in `.github/workflows/build.yml` and `content-guard.yml`.

A new test file goes in the `add_executable(coop_tests ...)` list in `tests/CMakeLists.txt`, and a new `lichii/hook` source it needs goes there too. Test names must not contain `;`.

**You do not need the game.** You cannot run it and should not try: there are no game files here. The unit tests and CI are your proof. Whatever only the game can show goes in the PR's "Check in game" list; the maintainer checks it in one combined in-game test ([docs/testing.md](docs/testing.md)).

### How tests and CI run

Full details: [docs/testing.md](docs/testing.md) (CI tiers, automatic tests). In short:

- **Unit tests first.** A bug fix starts with a test that fails for the cause, then the fix. Test names must not contain `;` (CMake configure fails). Run the CMake configure and the tests before you push.
- **CI tiers.** The `Build` workflow runs a small tier on a push to your branch (the `linux (gcc)` job, Python tool tests, Content guard) and the full tier (Windows MSVC, gcc, clang, ASan, TSan, Python 3.10 and 3.12, Longshot Linux) on a pull request into `main`. Do not push needlessly: one push per batch of commits.
- **The Linux gcc job builds the tests with `-Werror`.** Fix warnings, do not silence them. On a branch, let that job finish (2 to 3 minutes) before you call the work green.
- **Changes under `lichii/hook/` or `hookshot/src/`** also need the `linux (gcc)` job's strict compile of the Hookshot module green (step "Compile the hook code": g++-13, `COOP_HOOK_STRICT`, warnings as errors; it runs on every push). It catches errors the plain test build does not (for example string truncation warnings). The Fritchii emulator is retired (0.11.12): no job builds it.
- **Green means finished.** A review OK, or "ready", is only valid on a run that has finished green and ran the tests your change touches. Quote the run id and job. "It will be OK once CI is green" is not an OK.
- **Two-game tests** (`tools/autotest/` scenarios, maintainer only) need the game and run one at a time; the maintainer runs them for your "Check in game" list. Test listeners bind `127.0.0.1` only.

## Reviews

- Every change is reviewed by someone who did not write it, before it merges. Nothing merges unreviewed, however small.
- A review checks the root cause and related cases, the solo path, native implementation, UI rules, screenshot proof for visible changes, protocol and docs updates, and the content rules above.
- A review returns CHANGES for: a symptom patch, a missed related case, a hand-drawn UI shape or a screen that does not fit, a visible test with no screenshot, a remote hit that shows nothing, anything that only works in a session.
- If you are the author, you do not review your own change. Answer review comments by pushing to the same branch.

## Pull requests from outside contributors

- Contributors' commits are merged as authored: `git merge --no-ff` of the PR head, never squash, rebase-rewrite or cherry-pick under another name. What review finds is fixed by the maintainers in a separate commit on top; contributors are not asked to redo the work, and the merge comment thanks them and names what was added.
- A contributor's own commit trailers (including AI co-author lines) are kept as authored. Our own commits carry none.
- When a release build starts, the maintainer sets a cutoff time. Commits pushed to a PR after it go into the next update (nobody chases a moving PR head); the PR comment says so.
- Every merged contributor is listed in the README's Contributors section.

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
9. Your commits reach `main` as you wrote them, with your name on them: an outside PR is merged with `git merge --no-ff` of its head (GitHub shows it merged), never squashed, rebased, cherry-picked under another name or redone by the maintainer. A change the maintainer needs goes on top as a separate commit. That is how you show up in the repository's contributors list and in the README.

## Style

- English, plain short sentences, in code comments, docs, commits and PRs. Match the docs' tone: say what happens, not how great it is.
- Follow the surrounding code: C++20, the existing names and file layout. No new dependency without a reason in the PR and an entry in `THIRD_PARTY_NOTICES.md`.
- Change docs when behaviour changes: the area doc, `docs/sync-coverage.md` for what is shared, `docs/known-issues.md` for what players see.
- Do not write release names or old version tags in the repository (`check_content.py` refuses them); give the date or the issue instead.

## Play-testing the game (agent tester)

This section is only for a session that was handed **running test games** with the play-test bridge on and an MCP
server per game (`oot-p1`, `oot-p2`, ...; set up by the maintainer: [docs/playtest.md](docs/playtest.md), "Playing
through the bridge"). Everything above still holds. A coding session never starts games itself. The order to do things
in (start, drive, read, shoot, replay) and the shared scripts and data folders (`tools/playtest/`) are in
[docs/playtest-agents.md](docs/playtest-agents.md).

- You control each game through its own tools (`get_state`, `get_world`, `get_actor`, `walk_to`, `press`, `step`, `warp`,
  `flag`, `give`, `kill`, `go_to_actor`, `attack`, `checkpoint`, `desync_check`, `check_logs`, `session_status`, `log`,
  `screenshot`, and more: the list is in [docs/playtest.md](docs/playtest.md)). Every reply has the game's frame count.
- **The games are paused between your calls.** Nothing happens unless you act; a call runs only the frames it asks
  for. Call `get_state` on every game first: a game whose tools you never called still runs.
- Use `warp` (and `flag`, `give`) to set a test up. `warp` goes by Longshot's teleport in free play (a map of its
  list, no start position) and by the game's own request during an event (that one takes an entrance); for another
  map or an entrance in free play, start the session with a boot script that warps. Do not walk across Hyrule to
  reach it.
- Call `checkpoint` with the same tag on **every** game after each action that should sync. Let the other games run
  a few frames first (`step 30`): a held game receives nothing.
- After each scenario run `desync_check` (or `python -I tools/autotest/desync_check.py <log dir> --clients`).
- Anything visible (a door, a chest lid, a puppet) is proven by a `screenshot`, never by flags alone.
- A bug report needs: the scenario, the steps (the tool calls), what was expected, what happened, the first tag where
  the games differ with each game's frame, and the shots.
- The scenarios are in [tests/scenarios.md](tests/scenarios.md). Run them in order and write the results to the
  report file you were given.
- **Never edit the mod's source during a test run.** Report; do not fix. Fixing is a separate session, so a test run
  cannot quietly hide what it was meant to catch.
