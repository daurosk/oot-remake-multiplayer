# Game builds

Co-op works with one build of the game: main build id `e0f3ccfaf50065ae` (whole id `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`), display version 1.1.1. This page lists what ties co-op to that build, what a player with another build sees, and what it takes to support another build (issue 97).

## What a player with another build sees

The hook reads the running game's build id when the game starts (`coop_hook.cpp`, `Read`) and looks it up in the build table (`lichii/hook/coop_game_build.h`).

- Known build: co-op reads and sends as usual. The log says `coop: game build e0f3ccfaf50065ae, main at 0x...`.
- Any other build: co-op stays off for that game. The panel says `Co-op off: game build <id> not supported, needs version 1.1.1`. The log says once per game start: `coop: game build <id> is not supported; co-op needs version 1.1.1 (build e0f3ccfaf50065ae). Install that game update ...`.
- The loader patch (`CoopSvc::PatchMain`) checks main's whole build id and the instruction words at the hook sites, and skips the patch with a log line (`Skipping "coop puppet": build id ... is not ...`) when they differ. So another build never gets code written into it.
- The object, drop and spawn readers (`coop_objects.cpp`, `coop_drops.cpp`, `coop_spawn.cpp`) each compare the build id themselves and stay off silently; the hook's line above already says why.

The fix for a player is to install the 1.1.1 update (File > Install Files to NAND, see [setup](setup.md)).

The server does not compare game builds that it could tell apart: every client sends the table's primary id in `HELLO` (`docs/protocol.md`), not the running game's, because `HELLO` goes out when the emulator starts, before any game. Two players on different builds therefore join the same room, and the one on the other build sees the panel line above.

## What is tied to the build

Counted on `integrate-p4`, 2026-11-09.

| Kind | Where | Count | Changes with a new build? |
|------|-------|-------|---------------------------|
| Build checks | `coop_hook.cpp` (table), `coop_objects.cpp`, `coop_drops.cpp`, `coop_spawn.cpp` (8-byte arrays), `coop_svc.cpp` (`kMainBuild`, 40 hex) | 5 | The id itself, yes |
| Global pointer slots in main | actor manager, game flags manager, GameUI, Scene, Banc, event manager, WorldMgr, foliage module | 8 | Yes, almost surely |
| Functions in main the hook calls | create actor, take/drop create, malloc, request delete, play AS, set AS frame, sync AS slot, equip, equip change state, set matrix, set time, cut foliage | 13 | Yes, almost surely |
| Data in main | actor vtable, the GOT slot of the create default | 2 | Yes |
| Code patches | two `bl frame_step` sites in moduleCalc_, the AI hold's `blr x8` site, the two caves and the hold table in main's zero padding before rodata | 8 | Yes; the caves (`coop_svc.s`, `coop_hold.s`) are linked at fixed addresses and must be re-linked |
| Offsets inside game objects | actor fields (+0x2c, +0x31c, +0x328, +0x358, +0x270 ...), AS controller, EquipmentUser, GameUI fields, Scene tables, flag tables | about 60 | Maybe not. Only changes if the game's classes changed; must be checked |
| Game data hashes | flag keys (`coop_dungeon.cpp`, `coop/host/src/game_items.cpp`, `game_flags.cpp`), actor names | about 1100 | No: Murmur3 hashes of names from the romfs; only new or removed flags change |

All the main offsets are in one place now: `CoopGameBuild::MainOffsets` in `coop_game_build.h`, one entry per build, each field naming the constant it mirrors. The readers still use their own copies (`coop_puppet.h` `addr`, `coop_world.h`, `coop_fairy.h`, `coop_svc.h`, `coop_hold.h`, `coop/host/src/addresses.hpp`, and file-local ones in `coop_objects.cpp`, `coop_time.cpp`, `coop_flags.cpp`, `coop_world.cpp`, `game_flags.cpp`, `coop_pose.cpp`). `tests/lichii/game_build_test.cpp` checks the table against every one of those it can see, so moving a reader onto the table is a plain swap.

Duplicates found: the actor manager slot is in three files, malloc in three, the game flags slot in two, the Scene slot in two (one a bare number in `coop_pose.cpp`), the vtable, set matrix, request delete, play AS and set AS frame in two each.

## Supporting another build

### How to fill an entry: table, filled offline

Two ways to find the addresses:

- **Signature scan at run time**: the hook searches main for byte patterns around each function and slot when the game starts. Works on builds nobody has looked at, but needs a pattern per address (about 31), each pattern must survive a recompile (ADRP/ADD pairs and branch offsets differ), a wrong match calls a wrong function in the game, and the caves still have to be re-linked per build, so a scan alone cannot make the code patches work.
- **Per-build table, filled offline** (recommended): someone with that build runs a tool over its main once, checks the result, and adds one `Build` entry. The hook only looks up the entry. A build without an entry stays refused with the clear message above.

Recommended: the table, with an offline tool that does the matching. The game has had few updates, so few entries are needed, and each entry is checked by a person and by a test before anyone plays on it.

### Steps for one new build

1. Get the build's main (from your own game; never add game files to the repo) and run `..\integration`'s `tools/nso_index.py` on it for the string, reference and call index.
2. Find each `MainOffsets` field. Most have a string or a call chain written next to them in `docs/puppet.md`, `docs/objects.md`, `docs/flags.md`, `docs/time.md`, `docs/world.md` and `addresses.hpp` (for example `ExecuteWmSetTime` for `set_time`, the AI node names for the AS calls). A tool that takes the 1.1.1 function's bytes with the relocations masked and finds the same function in the new main would do most of it; this tool does not exist yet.
3. Check the object offsets (the "maybe not" row): read the functions those offsets came from in the new build and compare.
4. Re-link the caves at the new addresses (`coop_svc.s`, `coop_hold.s` say how) and give `CoopSvc`/`CoopHold` per-build cave bytes and hook words.
5. Add the `Build` entry and a test that the entry's hook words decode to its `frame_step`, as the 1.1.1 test does.
6. Test the new build in game with two players on that build. Players must still share one build: the protocol carries actor names and flag hashes that may differ between versions, so mixed builds in one room are not supported.

### What has to change in the code first (one-time)

| Step | Work | Effort |
|------|------|--------|
| Readers take their offsets from the build entry the hook found (pass `const MainOffsets&` where `main_base` is passed now) | `coop_puppet`, `coop_objects`, `coop_spawn`, `coop_drops`, `coop_fairy`, `coop_time`, `coop_world`, `coop_flags`, `coop_pose`, `coop/host` (`PlayerReader`, `game_flags`) | 1 day, touches most hook files: do it in a quiet chunk, not beside other branches |
| One build check: the four readers and the loader patch use `CoopGameBuild::Find` | `coop_objects.cpp`, `coop_drops.cpp`, `coop_spawn.cpp`, `coop_svc.cpp` | 1 hour |
| Caves per build | `coop_svc.h/.s`, `coop_hold.h/.s` | half a day |
| HELLO sends the running build, so the server keeps different builds apart | `coop_hook.cpp`, client config, server session; HELLO goes before the game, so it needs a new message or a reconnect when the game starts | half a day, protocol change |
| Matching tool (step 2 above) | `tools/` | 1 to 2 days |

Then each new build: half a day to a day with the tool, plus one in-game test.

Not needed until someone owns another build: no other build id is known to us yet. The first step is to learn which builds exist (the game's update history) and which ones players have.
