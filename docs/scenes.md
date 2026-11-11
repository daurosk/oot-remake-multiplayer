# Scenes

Plan stage 1 step 3: show only players in your scene, hold or hide puppets while a cutscene runs, and a "join partner" warp. Game: main of build e0f3ccfaf50065ae (1.1.1). Offsets are into the loaded module (text 0x0, rodata 0x3c47000, data 0x484d000), read from `dump/main.plain` loaded as an NSO image. **Checked** means read from the instructions or the RomFS. Nothing on this page has been run yet.

## Same-scene filter and cutscene state (done)

`coop/host/include/coop/host/scene.hpp`, header only, tests in `tests/host/scene_test.cpp`:

- `same_scene(a, b)`: both scenes known, non-empty and equal. Unknown is never the same scene.
- `in_cutscene(flags)`: bit 2 of a `PLAYER_STATUS` flags byte.
- `visibility(local_scene, local_hides, remote_scene, remote_flags, remote_anim)` gives `hide`, `park`, `hold` or `show`:
  - other or unknown scene: `hide`;
  - our own cutscene running (`local_event_hides`: not while we play the Ocarina): `hide` (puppets are not actors the event knows, so they must not walk into its shot);
  - they ride a horse (`activity_of(remote_anim)`): `park` (puppet out of sight, marker kept);
  - their cutscene running, unless they play the Ocarina: `hold` (freeze at the last shown pose);
  - otherwise `show`.
  The cases this covers (title, menus, loading, Epona, Ocarina, crawlspace) are listed in [puppet.md](puppet.md#cases-issue-86).

The inputs already exist. The sender reads the scene (Banc +0x40, file name) and "event running" (event manager `*(main+0x4e94840)` +0x68 not zero, the game's `IsEventRunning` at 0xbcc100) in `coop/host` (`PlayerReader`, [host.md](host.md)) and sends the event bit as `PLAYER_STATUS` bit 2. The receiver keeps scene and status per player in `coop::presence::Presence` (`scene(n)`, `status(n)`, [presence.md](presence.md)). The local side is `PlayerReading::scene` and `PlayerReading::event_running` of this frame's reading.

One line each for the callers:

- Puppet (`coop_puppet.*`): `switch (coop::host::visibility(local.scene, local.event_running, presence.scene(n), presence.status(n)))`: `hide` despawns or hides the puppet, `hold` stops updating its transform and animation, `show` drives it as now.
- Overlay markers (`coop_window.*`): `if (coop::host::visibility(local.scene, local.event_running, presence.scene(n), presence.status(n)) == coop::host::Visibility::hide) continue;` (draw a held marker with its last pose).

## Join-partner warp (dropped)

**Dropped (Caleb, 2026-11-09): "we dont need warp to partner"; back as `/teleport <player>` for test8** (docs/setup.md "Chat commands", `lichii/hook/coop_teleport*`). It uses `JOIN_PARTNER` as below and the boss rooms' two moves ([dungeons-boss.md](dungeons-boss.md)): the same scene moves Link in place; another scene asks Main's free-play scene change ("Free-play scene change" below), then moves Link next to the player.

### How the game changes scene

All checked in the RomFS graphs (`Sequence/*.ainb`, parsed node by node) and the flag list (`GameData/GameDataList.Product.110.byml.zs`, a big-endian BYML):

1. `Main.module` root runs `_LocalModule_cf31571495c4.module` (node 15), a native selector with eight cases: GameOver, ResetGimmick, MinusMenu, PlusMenu, Event, AssignableItemShortCut, **BancChange**, None. The BancChange case leads to `ChangeBancOnMain.module`. The `.ainb` of that local module is empty: the choice is made in C++. Its class name string is at 0x3d40b05, its name getter 0x33044e0, vtable from 0x4bd0158; slot +0xe0 (0xd59ee0) looks up the eight case names and stores the child indices at `this` +0x23 to +0x2a (BancChange at +0x28; the name 0x3db48e8 is loaded right before that store). The choice is slot +0x138, 0x3303ca0: see "Free-play scene change" below.
2. `ChangeBancOnMain.module` reads the String64 flag `Sequence_ReservedNextBanc` (hash 0xee27bf71), builds `Work/Banc/GameBancParam/<name>.game__banc__GameBancParam.gyml` and runs `ChangeBanc.module` with it. It also takes `OverrideTraverseStartPosName` from a GameData string. The event variant (`ChangeBancOnEvent.module`) also reads `Sequence_ReservedNextPosition` (Vector3 0x52046b85), `Sequence_ReservedNextStartPosName` (String64 0x0b570ab2), `Sequence_ReservedNextCancelSleepRot` (Vector3 0x1662143e), `Sequence_ReservedDungeonRoomID` (Int 0x050249d2), `Sequence_IsSameBancItemWarp` (Bool 0xdd257ad8). All six have save file index -1 (never saved).
3. During an event, `Event.module` takes the enum `Sequence_ControlInEvent` (0x8c1bbe2f; values Idle, ResetGimmick, BloodyMoon, **ChangeBancOnEvent**, MemoryShortageBloodyMoon, ResetAIScheduleNpc) to run `ChangeBancOnEvent.module`. This is the event flows' `EventReserveBancChange` path.

The String64 table, from its getter 0xcb0ea0 (called with 0xee27bf71 from 0x32998e0): `M` = `*(main+0x4e842a8)`, type 16, table at M + 0x610 (count), +0x618 entries (0x108 bytes each), +0x620 hash slots, +0x62c slot count; the string pointer is entry +0x68 and its capacity entry +0x70. A second table at M + 0x680 is searched when the hash is missing. Checked.

No code in `main` writes `Sequence_ReservedNextBanc` by its hash: the writers are graph nodes (`SeqOneShotSetNonStructGameDataString64` and the like) that hash the name at run time.

### Free-play scene change (found 2026-11-11, by reading; not yet run)

The selector's choice function 0x3303ca0 (vtable slot +0x138) checks, in order, the GameOver and ResetGimmick triggers, menus, the Event case and the item shortcut, and then, at 0x3304154, the **Bool flag 0x3fc35576** in the GameDataMgr (`*(main+0x4e842a8)`, the Bool table at +0x20/+0x2c, entry +0x16 value; then the BoolArray table at +0x80): if it is 1 it picks child +0x28, **BancChange**, which runs `ChangeBancOnMain.module`. That graph reads `Sequence_ReservedNextBanc`, loads `Work/Banc/GameBancParam/<it>.game__banc__GameBancParam.gyml` through `ChangeBanc.module` (the same module the event path runs: BeforeChangeBanc sets `Sequence_IsLoadingBanc`, unload, load, AfterChangeBanc) and sets a String64 (the request, emptied). It reads no start name or position: Link lands at the scene's own start.

- Flag 0x3fc35576: Bool, default false, reset type 0 (never reset by the game), save index -1. Its name is in no file of the game (main, every `.ainb`, every actor pack, Event, AI, Logic, Component, scanned with Murmur3 seed 0), and its hash is in `main` only at the two reads in 0x3303ca0, so **nothing in the shipped game sets it**: a leftover debug switch. A door or exit does not use it.
- What a loading zone does instead (read from the graphs and node names, not traced in C++): it is an event. The exit actors start an event (`FieldAndDungeonEntranceEventRequestActor`, `QueryBancChangeEventRequestArg`); the event flow's `EventReserveBancChange` sets `Sequence_ReservedNextBanc` / `NextStartPosName` / `NextPosition` and `Sequence_ControlInEvent = ChangeBancOnEvent`, and `EventToMain` / `ChangeBancOnEvent.module` change scene when the event ends. That is why the bare request was taken only during a talk.
- So /teleport sets the request and the switch (`lichii/hook/coop_teleport_game.cpp`): `Sequence_ReservedNextStartPosName` = "" (the event path would read a stale one), `Sequence_ReservedNextBanc` = the scene (written straight into the table), then Bool 0x3fc35576 = 1 through the game's write queue. Once the game has taken it (`Sequence_IsLoadingBanc` up, the scene unreadable, or the request emptied) the Bool goes back to 0, so Main does not pick BancChange again with an empty request; on arrival or give-up the request and the start name are emptied too.
- Not proven: that ChangeBancOnMain loads cleanly from free play (it is the debug path), and the load fade it shows. Check: `/teleport` between two scenes in free play (reports/teleport-anywhere.md has the steps); the log shows `asked the scene change to X: scene set, switch on`, then `scene change taken: switch off`, then `scene change cleared`.

Probe: `LICHII_COOP_SCENE_PROBE=1` (`lichii/hook/coop_scene_probe.*`) logs a line `coop: scene probe: frame N scene=... event=... switch=... other=... loading=... control=... banc=... start=... starter=... boot=...` each time one of them changes. Walk through one door or exit with it on to see what a loading zone sets (expected: event=1, then banc/start set and control=ChangeBancOnEvent, then loading=1, switch=0 throughout); a free-play /teleport should show switch=1 with banc set, then loading=1, then switch=0.

### Ways to build it, best first

1. **Same as a loading zone.** (Superseded: a loading zone is an event, and Main's own free-play switch is used instead; see above.) Find what a door or exit sets (the C++ request that makes step 1 pick BancChange, plus the reserved flags), and do the same from the emulator between frames. Next step: one run with a hook that logs, every frame, the String64 and Vector3 `Sequence_Reserved*` flags (table layout above) and the selector object's state, while Caleb walks through one loading zone. Then trace the writer of whatever changed (`QueryBancChangeEventRequestArg` 0x29bf380 and the strings `ChangeBancName`, `NextBancParamNameAdult/Child`, `game__MapChangeBancInfoParam` are the leads).
2. **Through the event path.** Set `Sequence_ReservedNextBanc`, `...NextPosition` (the partner's position) and `Sequence_ControlInEvent = ChangeBancOnEvent` while an event runs. Needs an event to be running, so not usable from free play without starting one.
3. **Call 0x3af4d20** (takes the Banc module, a name and a position, claims load slot 0). No graph uses it, so the unload and the rest of `ChangeBanc.module` would not run; not safe.

The warp target is the partner's `PLAYER_SAMPLE` scene and position. A trigger would be a chat command (`/join <player>`) in `coop_chat.*` (not this task's file; needs the coordinator) or a hotkey in a new `lichii/hook/coop_scene.*`.
