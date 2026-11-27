# The world keeps running in the pause menu (row 299)

Player report 299 (a suggestion, 0.12.0): in co-op, opening the pause menu should not stop the world, as in an MMO;
the reporter also thinks pausing causes desync. The owner wants it (2026-11-27). Design first, a probe before any
build; then a **host option**, solo never changed.

Status: **phase 0 (probe) run in a game: PASS (2026-11-27).** No option exists yet. Verdict below: option A, with two
things phase 1 must add and prove.

## How the pause works today

### In the game (read from main 1.1.1, build `e0f3ccfaf50065ae`, 2026-11-27)

The game stops its world through a pause manager with **named holds**, not a single flag:

| What | Where | How it was read |
|---|---|---|
| The manager | GOT slot main+0x4cba6d8 -> a global -> the manager | `SeqOneShotRequestPause`'s run (vtable 0x84bd6e08, slot 39 = main+0xd9aec0): `adrp x9, 0x84cda000; ldr x9, [x9, #0x6d8]; ldr x0, [x9]` |
| Its holds | manager +0 u32 count, +8 -> array of 0x20-byte holds sorted by key | the same run's binary search (`add x14, x10, x13, lsl #5; ldr w14, [x14, #8]`) |
| A hold | +8 u32 key (hash of its name: main+0x1739e0(name, 0)), +0x1c u32 how many hold it | Request: `ldadd` +1 on +0x1c; Release (`SeqOneShotReleasePause`, slot 39 = main+0xdad2c0): `cas` -1, never below 0 |
| After a change | manager +0x48, vtable slot 0, called `(sub, key, 1)` on request, `(sub, key, 0)` on release | both runs: `ldr x8, [x0, #0x48]!; ldr x8, [x8]; blr x8` |
| Named in code | Game Over asks `GameOverPause` (string 0x83db8a79) the same way | main+0x30310e0 |
| The UI's own test | manager +0x30..+0x3c, four u32 tested before a screen acts | main+0x2cf094..0x2cf0c4 |
| Per actor | actor +0x30 bits 10 to 15 (six pause kinds); a paused actor goes to state 5 and does not run | docs/objects.md, "Holding a copy's AI" (0xa6740) |

The pause kinds themselves are data (`System/CombinationDataTableData/PauseConfig`, `engine__system__PauseConfig`;
strings in main), and the pause menu's hold is asked by the Sequence graph, so which hold the + and - menus take is
**not known yet**: the probe reads it.

### In co-op

- A player in a menu sends `PLAYER_STATUS` with `in_menu` (and `paused`): `coop/host/src/sampler.cpp` 60 to 65, from
  patch 0018's menu state (`lichii/hook/coop_hook.cpp` 2598 to 2616, also the quick-switch menu).
- The others treat that player as **away** (`coop/protocol/include/coop/protocol/messages.hpp` `status::away`): PvP
  and enemies leave their puppet alone (`coop_dbno_body.cpp` UnseenWhy), counters and switches stop following them
  (`coop_objects.cpp` kCounterAway / kSwitchAway, row 164), shared starts wait (`coop_hook.cpp` 3304), and a boss
  fight they own is handed on after `fight_owner_paused_us`.
- The host's clock stands in its pause and makes the time up after (docs/time.md, "The host's pause").

So a pause today is handled, but every one of these is a hand-over: the paused game's world stood still while the
others' ran on, and coming back means catching up. That is where pause-caused desync can come from (row 164, the
boss hand-overs).

## The probe (phase 0)

`tools/autotest/ingame/test_pause_world.py` (bridge test, game A, the boot's free play), reads only except one step:

1. `state.pause_world` (new, `hookshot/src/coop_pause_world.h`): the holds with a count above 0 and the four words;
   the first read lists every hold to the log (`coop: pause world: probe: hold N key K count C raw ...`).
2. + opens the menu: which holds it took; Link's snapshot (`snapshot_stale`: Longshot takes it only in a running Link
   update) and how many `world` actors moved in 120 frames.
3. `act menu hold release N` (test only): the game's own release of each hold the menu took, done at the next menu
   screen update (patch 0030's `hos_coop_game_screen`, the game's thread): the same reading again, menu still open.
4. The left stick 30 frames under the open menu: does Link walk?
5. + shuts the menu: the holds back to free play's (the menu's own release finds 0 and stays there).

Its `probe: ...` notes answer: which hold is the menu's; with it let go, does the world (NPCs, enemies, Link's update)
run while the menu draws; does the pad reach Link through the menu.

## Options

| | What | For | Against |
|---|---|---|---|
| A | Let go of the menu's own hold while the menu is open (the game's own release, module only, at each menu screen update; only the hold the menu took, only in a session with the host's option on) | No game patch; the game's own path; solo untouched (needs a session) | Things that assume a paused world while a menu shows: Link's input, the menu's own Link model, sounds; must be measured |
| B | A wrap on the request (patch 0005's table: one row), dropping the menu's request | Never paused at all | A game rebuild; same assumptions as A |
| C | Keep the pause, shorten the hand-overs (longer grace for an `in_menu` player) | Smallest change | Not what the row asks: the paused player's world still stands |

## Risks to measure before building

- **Input**: with the world running, Link's update may read the pad the menu is using (step 4). If so, Link's input is
  held off while the menu shows (patch 0017's input-off wrap, `docs/native-player.md`).
- **Being hit**: a menu player's Link stands in a running world. Today the others' enemies ignore an away player; with
  this option the player is not away (MMO rule: you can be hit). The option's text says so.
- **Status**: with the option on, a menu player sends `in_menu` but not `paused`, so counters, switches and boss
  ownership keep following them (no hand-over: the desync this row is about).
- **Timers and events**: the world's timers run (a heat or water timer, a minigame). An event (cutscene) keeps its own
  pause: only the menu's hold is let go, never another's.
- **Solo and co-op idle**: unchanged (no session, or the option off: nothing is released).

## Verdict

The probe ran once on lane 2 (`pause-world-a01fc8e2-2`, 2026-11-27 23:21 UTC, run tree a01fc8e21 = integrate-0121
53d40ab3e + this branch c4dd61e1f; summary.json `test_pause_world_probe` PASS, 166 s; game A, child Link in Kakariko
Village, the boot's free play). Its readings:

| Step | Holds (slot: key, count) | The four UI words | Link's snapshot | `world` actors moved / seen |
|---|---|---|---|---|
| Free play | none | 0, 0, 0, 0 | fresh | 0 / 19 |
| + menu open | 16: 0xba597383, 1 | 2134548244, 747643567, 75423, 0 | stale (Link's update stopped) | 1 / 23 |
| Menu open, hold 16 let go | none | 0, 0, 0, 0 | fresh (Link's update runs) | 3 / 26 |
| Stick 30 frames under the open menu | none | - | Link walked (-118.9, 96.68, 58.7) -> (-117.39, 96.82, 58.34) | - |
| + menu shut | none (equal to free play's) | 0, 0, 0, 0 | fresh | 2 / 26 |

The release line: `coop: pause world: test release 16 key 0xba597383: count 1 -> 0, notify 0x80b1ba60 called`; the
menu's own release at shut found 0 and stayed there.

What this settles:

- **The + menu takes one hold**, key 0xba597383, and only that one. Letting it go with the game's own release restarts
  Link's update while the menu stays open, and shutting the menu leaves the holds as in free play. **Option A works
  with no game patch**: release only that key, only while the menu is open, only in a session with the host's option on.
- **The pad reaches Link through the menu** (risk "Input": confirmed). Option A must hold Link's input off while the menu
  shows (patch 0017's input-off wrap), and a test must show Link stands still under the menu.

What it does not settle (phase 1 must prove each in a game):

- **The UI words go back to 0 when the hold is let go** (they read the manager's +0x30..+0x3c, which screens test before
  they act). The menu may then stop taking input or act as if shut. Phase 1 must show the menu still opens pages, moves
  its cursor and shuts, with screenshots on each game (user-facing).
- **Other actors**: Kakariko's free play moved 0 of 19 actors, so "3 moved" with the hold let go is weak proof that NPCs
  and enemies run. Phase 1 measures in a scene with moving enemies.
- The - menu (and the quick-switch menu) were not read: the probe only opened +.

Next (phase 1, the host option): option A in the module behind the host's option, Link's input off while the menu
shows, `in_menu` without `paused` sent; tests: the menu works (shots), Link stands, enemies move under the menu, solo
and co-op idle unchanged (nothing released).

## Phase 1: the host option (design, B #158 GO 2026-11-27)

Branch `pause-world-299-p1` off integrate-0121. Default **off**: no session, or the option off, nothing is released and
nothing changes (solo, co-op idle and today's co-op pause stay exactly as they are).

1. **The setting** (`lichii/hook/coop_pause_world_mode.{h,cpp}`, made as `coop_switch_stand_mode.h`): the host's choice
   in coop-options.ini (`pause_world = on`; no line: off), sent in the host's `MOD_SETTINGS` (491) as mod id `coop`, key
   `pause_world`, value `on`/`off` (docs/protocol.md "co-op's own host settings": no new type, no version, so no clash
   with row 298's protocol 19); an older host sends none: off. In force from the next Host, as Shared floor switches.
   Wired where CoopSwitchStandMode is: `coop_mod_share.cpp` (OnWelcomed, OnHosting, OnHostSettings, the host's list
   and its key filter), `coop_pause.cpp` (SessionNow, Host), `coop_pause_files.cpp`, `coop_title_menu.{h,cpp}`
   Read/Write.
2. **The row** on the Host settings page (`coop_pause_menu.cpp` HostRuleLines, after Shared floor switches, the same
   `Toggle` + `ruled_help`; `Act::kPauseWorld`, Change as kFloorSwitches): "World runs in menus", help: "On: in a game you
   host, the world keeps going while a player is in the pause menu (enemies can still hit them). Used at your next
   Host." The game's own panes and font (the existing Toggle row), no new pane.
3. **The release** (`coop_pause_world_module.cpp` OnScreen, the game's thread): while the setting is in force, in a
   session, and a game menu is up (`CoopMapPage::MenuState()->menu_open || CoopPauseTab::GameMenuUp()`, as row 199),
   release each held hold whose key is in **one table** of menu holds (`coop_pause_world.h kMenuHolds`; +: 0xba597383
   from the phase 0 run; the - menu's and the quick-switch menu's keys are read first by the phase 1 test's probe
   step, never guessed) with the game's own release (the probe's Release). Only those keys: an event's or Game Over's
   hold is never touched. The menu's own release at shut finds 0 and stays there (cas, never below 0).
4. **Root cause of "the UI words go to 0"**: the words at manager +0x30 are what the notify sub keeps for the holds in
   force, and screens test them (main+0x2cf094..0x2cf0c4) before acting. Before building 3, read that test in main: if it
   only asks "is a pause in force" for screens that are not the menu's own, nothing to do; if the menu's own screens
   need it, the release keeps the menu's bits (set them back after the notify, from the reading before) instead of
   leaving 0. The phase 1 test proves the menu still acts: with the option on, L/R moves the - menu's pages
   (`menu.map_page` / `menu.minus.tab`), the cursor moves, B/+ shuts it.
5. **Link's input** off while a menu is up and its hold was let go (patch 0017's input-off wrap, docs/native-player.md
   "Its own input"): the phase 0 run showed the stick walks Link under the menu.
6. **Status**: with the setting in force a menu player sends `in_menu` but not `paused`/away, so enemies, counters,
   switches and boss ownership keep following them (no hand-over: the desync row 299 is about).
7. **The test** (`tools/autotest/ingame/test_pause_world.py`, a second test): a scene with moving enemies (not
   Kakariko's free play, where 0 of 19 actors moved: weak) for "the world runs"; option off: the menu holds as today;
   option on: the menu's holds let go, enemies moved, Link stands under the stick, the menu still pages and shuts;
   framebuffer shots on each game of the Host settings page with the new row and of the open menu with the world
   running (user-facing), compared to stored refs (NO-REF until the owner approves).

### Phase 1 reading: the UI words and the menu's holds (main 1.1.1, build `e0f3ccfaf50065ae`)

- **The four words are a 128-bit set**, one bit per hold in force. A screen tests a hold this way: if any word is not 0,
  it looks the key up in the manager's +0x40 hash table (key -> bit number), then tests that bit. The test at
  main+0x2cf094..0x2cf0c4 (in main+0x2cf010) tests only keys 0x7bf055fc and 0x0778e566, not the menu's.
- **The menu's holds**: one screen object keeps one of three holds by its state (+0x384), from the table at
  main+0x3f16804 {1: 0xb584333e, 2: 0xba597383, 3: 0x90ddf88b}. Each change releases the old state's key and asks the
  new one (Request main+0xafb980, `ldadd` +1; Release main+0xafba70, `cas` -1 never below 0): state 1 at main+0x33eb440,
  1 -> 2 at main+0x2d6880 (0xba597383, the key phase 0 saw while the menu was open), -> 3 at main+0x33ec974, 3 -> 0 at
  main+0x2d6840.
- **Nothing tests those three bits**: each key appears in code only at those Request/Release calls (an immediate
  scan of main's text for `mov`/`movk` pairs), never in a bit test. So the release may leave the menu's bits at 0;
  item 4 needs no code. The test still proves the menu acts with the hold let go (pages, cursor, shut).
- So `kMenuHolds` is that table's three keys, all from code (none guessed). The test's probe step logs which of them
  the - menu and the quick-switch menu take; a menu that takes a key not in the table stays paused as today.
