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

**Dropped (Caleb, 2026-11-09): "we dont need warp to partner".** Nothing below is built; it stays as research only.

### How the game changes scene

All checked in the RomFS graphs (`Sequence/*.ainb`, parsed node by node) and the flag list (`GameData/GameDataList.Product.110.byml.zs`, a big-endian BYML):

1. `Main.module` root runs `_LocalModule_cf31571495c4.module` (node 15), a native selector with eight cases: GameOver, ResetGimmick, MinusMenu, PlusMenu, Event, AssignableItemShortCut, **BancChange**, None. The BancChange case leads to `ChangeBancOnMain.module`. The `.ainb` of that local module is empty: the choice is made in C++. Its class name string is at 0x3d40b05, its name getter 0x33044e0, vtable from 0x4bd0158; slot +0xe0 (0xd59ee0) looks up the eight case names and stores the child indices at `this` +0x23 to +0x2a (BancChange at +0x28). **What makes it pick BancChange was not found** (no vtable slot reads those bytes directly; the base class code that does was not traced).
2. `ChangeBancOnMain.module` reads the String64 flag `Sequence_ReservedNextBanc` (hash 0xee27bf71), builds `Work/Banc/GameBancParam/<name>.game__banc__GameBancParam.gyml` and runs `ChangeBanc.module` with it. It also takes `OverrideTraverseStartPosName` from a GameData string. The event variant (`ChangeBancOnEvent.module`) also reads `Sequence_ReservedNextPosition` (Vector3 0x52046b85), `Sequence_ReservedNextStartPosName` (String64 0x0b570ab2), `Sequence_ReservedNextCancelSleepRot` (Vector3 0x1662143e), `Sequence_ReservedDungeonRoomID` (Int 0x050249d2), `Sequence_IsSameBancItemWarp` (Bool 0xdd257ad8). All six have save file index -1 (never saved).
3. During an event, `Event.module` takes the enum `Sequence_ControlInEvent` (0x8c1bbe2f; values Idle, ResetGimmick, BloodyMoon, **ChangeBancOnEvent**, MemoryShortageBloodyMoon, ResetAIScheduleNpc) to run `ChangeBancOnEvent.module`. This is the event flows' `EventReserveBancChange` path.

The String64 table, from its getter 0xcb0ea0 (called with 0xee27bf71 from 0x32998e0): `M` = `*(main+0x4e842a8)`, type 16, table at M + 0x610 (count), +0x618 entries (0x108 bytes each), +0x620 hash slots, +0x62c slot count; the string pointer is entry +0x68 and its capacity entry +0x70. A second table at M + 0x680 is searched when the hash is missing. Checked.

No code in `main` writes `Sequence_ReservedNextBanc` by its hash: the writers are graph nodes (`SeqOneShotSetNonStructGameDataString64` and the like) that hash the name at run time.

### Ways to build it, best first

1. **Same as a loading zone.** Find what a door or exit sets (the C++ request that makes step 1 pick BancChange, plus the reserved flags), and do the same from the emulator between frames. Next step: one run with a hook that logs, every frame, the String64 and Vector3 `Sequence_Reserved*` flags (table layout above) and the selector object's state, while Caleb walks through one loading zone. Then trace the writer of whatever changed (`QueryBancChangeEventRequestArg` 0x29bf380 and the strings `ChangeBancName`, `NextBancParamNameAdult/Child`, `game__MapChangeBancInfoParam` are the leads).
2. **Through the event path.** Set `Sequence_ReservedNextBanc`, `...NextPosition` (the partner's position) and `Sequence_ControlInEvent = ChangeBancOnEvent` while an event runs. Needs an event to be running, so not usable from free play without starting one.
3. **Call 0x3af4d20** (takes the Banc module, a name and a position, claims load slot 0). No graph uses it, so the unload and the rest of `ChangeBanc.module` would not run; not safe.

The warp target is the partner's `PLAYER_SAMPLE` scene and position. A trigger would be a chat command (`/join <player>`) in `coop_chat.*` (not this task's file; needs the coordinator) or a hotkey in a new `lichii/hook/coop_scene.*`.
