# Shared enemies and boss rooms in the NX2navi DLL: wiring it in

**Since 2026-11-13 there is a second build, `build.py --upstream`, in which none of this wiring is used:** every file of `lichii/hook` is built unchanged over `hookshot/compat` and driven by `dll/upstream.cpp` (nx2navi/README.md, "The build that runs upstream's hook unchanged"). Of this folder it keeps `brain.cpp` and `hold.cpp` (the AI hold) and the two probes. What follows describes the older build.

`nx2navi/dll/objects/` ports the emulator hook's shared enemies (docs/objects.md) and boss rooms
(docs/dungeons-boss.md) to NX2navi's plugin interface **version 2**. The hook's own files are compiled against a
small set of stand-in emulator headers (`shim/`). Two switches were added to `lichii/hook/coop_objects_sync.{h,cpp}`
for this port, both off by default there so the emulator build behaves as before: `CoopObjects::ShareFriendly(bool)`
and `CoopObjects::OwnRollsWhenApart(metres)` ("This port's own rules" below); nothing else in `lichii/hook` was
edited.
Since coop-dll 0.7 it is on unless `coop.ini` says `objects = 0` (the default was off until two full two-game
runs held: nx2navi/README.md, "Verified, and not"); with it off, no hook is registered and no game memory is
touched. The settings lines of section 5 below are the first wiring's; `settings.hpp` has today's keys
(`objects_friendly`, `objects_rolls_apart` among them).

| File | What |
|---|---|
| `objects.hpp/.cpp` | The entry: `Init`, `Frame`, `OnEvent`, `OnHostReset`, `StatusText`, `Shutdown`. Registers the frame step hook and runs the hook files' cave callers in it. |
| `emu_port.hpp/.cpp` | What the shim headers forward to: guest memory (`GuestPort`), main's base, the log, `CoopSvc::AddCaller` and the callers' loop, `CoopSpawn::UseDirector`. |
| `shim/common/*.h`, `shim/core/**.h` | `LOG_*` (std::format), `Core::System`, `Core::Memory::Memory`, `Loader::AppLoader`, `Kernel::KProcess`, `Core::FindModules`: only what `coop_objects.cpp`, `coop_boss_game.cpp` and `coop_flags.cpp` use. |
| `hold.hpp/.cpp` | The AI hold on function hooks (replaces the emulator's patch at main+0x369df8). |
| `brain.hpp/.cpp` | Hold mode 3: a held copy's AI update main+0xcb9c0 left out (the default). |
| `scene_gate.hpp` | Which of the DLL's frames reach `Objects::Frame`: a frame without a reading (a hitch of the game's thread, one failed read) is not a scene change. |
| `plugin_api_v2.h` | NX2navi's `runtime/plugin_api.h`, version 2, unchanged (its notice allows the copy). |

Compiled unchanged from `lichii/hook/`: `coop_objects.cpp`, `coop_objects_sync.cpp`, `coop_world.cpp`,
`coop_spawn.cpp` (its emulator-free part: the director), `coop_projectiles.cpp`, `coop_pickups.cpp`,
`coop_ocarina.cpp`, `coop_cheat_items.cpp`, `coop_flags.cpp`, `coop_boss.cpp`, `coop_boss_game.cpp`,
`coop_dungeon.cpp`.

Tests: `python nx2navi/tests/objects/build_tests.py` (20 cases; see that folder). `--dll-check` also links the
DLL's own sources (as `build.py` lists them) with the port into a scratch DLL: the only clash is item 2 below.

## 1. build.py

```python
INCLUDES = [..., 'nx2navi/dll',
            'nx2navi/dll/objects/shim', 'nx2navi/dll/objects']     # add these two, after the others

DLL_MAIN = [...,
            # Shared enemies and boss rooms (nx2navi/dll/objects/INTEGRATION.md).
            'nx2navi/dll/objects/objects.cpp', 'nx2navi/dll/objects/emu_port.cpp',
            'nx2navi/dll/objects/hold.cpp',
            'lichii/hook/coop_objects.cpp', 'lichii/hook/coop_objects_sync.cpp',
            'lichii/hook/coop_world.cpp', 'lichii/hook/coop_spawn.cpp',
            'lichii/hook/coop_projectiles.cpp', 'lichii/hook/coop_pickups.cpp',
            'lichii/hook/coop_ocarina.cpp', 'lichii/hook/coop_cheat_items.cpp',
            'lichii/hook/coop_flags.cpp', 'lichii/hook/coop_boss.cpp',
            'lichii/hook/coop_boss_game.cpp', 'lichii/hook/coop_dungeon.cpp']
```

and, in `main()` under `if options.tests:` after the other test scripts:

```python
        say('running nx2navi/tests/objects/build_tests.py')
        if subprocess.run([sys.executable, '-B', str(HERE / 'tests' / 'objects' / 'build_tests.py'),
                           '--compiler', str(cxx)]).returncode != 0:
            fail('the shared-enemies tests failed')
```

The shim directories hold only `common/...` and `core/...` headers, which no other file of the DLL includes.

## 2. teleport.cpp: drop the `CoopDungeon::IsDungeonScene` stand-in

`coop_boss.cpp` needs `CoopDungeon::NotShared()`, so `lichii/hook/coop_dungeon.cpp` is linked; it compiles
without the emulator (its log is under `#ifdef LICHII_COOP`, off here, as in `tests/CMakeLists.txt`) and defines
the real `IsDungeonScene`. Delete the `namespace CoopDungeon { ... }` block at the top of
`nx2navi/dll/teleport.cpp` (lines 21 to 36). Without that: `multiple definition of
CoopDungeon::IsDungeonScene` (checked with `--dll-check`).

## 3. third_party/plugin_api.h

Replace `nx2navi/dll/third_party/plugin_api.h` with NX2navi's version 2 header (as the runtime agent's note
says), then `objects/plugin_api_v2.h` can become `#include "third_party/plugin_api.h"`. Until then both define
`struct Nx2PluginApi` (v2 only adds members at the end, same include guard, never both in one file), which works
but is two definitions of one type in one DLL.

## 4. CMakeLists.txt

```cmake
add_library(coop_nx2navi SHARED dll/coop_dll.cpp dll/overlay.cpp dll/puppet.cpp ... (as now)
  dll/objects/objects.cpp dll/objects/emu_port.cpp dll/objects/hold.cpp
  ../lichii/hook/coop_objects.cpp ../lichii/hook/coop_objects_sync.cpp ../lichii/hook/coop_world.cpp
  ../lichii/hook/coop_spawn.cpp ../lichii/hook/coop_projectiles.cpp ../lichii/hook/coop_pickups.cpp
  ../lichii/hook/coop_ocarina.cpp ../lichii/hook/coop_cheat_items.cpp ../lichii/hook/coop_flags.cpp
  ../lichii/hook/coop_boss.cpp ../lichii/hook/coop_boss_game.cpp ../lichii/hook/coop_dungeon.cpp)
target_include_directories(coop_nx2navi PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dll/objects/shim" "${CMAKE_CURRENT_SOURCE_DIR}/dll/objects")
```

## 5. settings.hpp / settings.cpp

```cpp
    // Shared enemies and boss rooms (objects/objects.hpp); off unless the file says 1.
    bool objects = false;
    bool objects_boss = true;     // objects_boss = 0: boss rooms not shared
    int objects_ai_hold = 0;      // objects_ai_hold = 1 or 2: hold the copies' brains (test switch)
    bool objects_world = true;    // objects_world = 0: no field-grass game calls
```

Keys `objects`, `objects_boss`, `objects_world` (0/1) and `objects_ai_hold` (0, 1 or 2), parsed like `puppet`.

## 6. coop_dll.cpp

```cpp
#include "objects/objects.hpp"

// In the start-up, after Puppet::Configure:
        {
            CoopNx2navi::Objects::Options o;
            o.on = settings_.objects;
            o.boss = settings_.objects_boss;
            o.ai_hold = settings_.objects_ai_hold;
            o.world = settings_.objects_world;
            o.debug = Environment(L"OOT_COOP_DEBUG_OBJECTS") == L"1";
            CoopNx2navi::Objects::Init(CoopNx2navi::Overlay::Api(), o, LogLine);  // the api may still be null
            if (!o.on) g_log.Line("objects: off (objects = 0 in coop.ini)");
        }

// session.on_event, beside Puppet::OnEvent:
            CoopNx2navi::Objects::OnEvent(event);

// Frame(), inside `if (trusted_) {`, after PublishPuppets (so the puppet's frame step hook is
// registered first and runs first, as the puppet director is the cave's first caller):
            CoopNx2navi::Objects::FrameInput objects;
            objects.api = CoopNx2navi::Overlay::Api();
            objects.main_base = memory_.main_base();
            objects.session = report.connected ? session_.get() : nullptr;
            objects.scene = reading ? reading->scene : std::nullopt;
            if (reading) objects.position = reading->position;
            objects.event_running = reading && reading->event_running.value_or(false);
            objects.server_now_us = report.server_now_us;
            CoopNx2navi::Objects::Frame(objects);

// Frame(), where report.host_reset is logged:
            CoopNx2navi::Objects::OnHostReset();

// Status(): append to the status line
        line += CoopNx2navi::Objects::StatusText();

// Run(), beside Puppet::Shutdown():
        CoopNx2navi::Objects::Shutdown();
```

`Frame` must be called every frame, also when not connected (it then forgets the shared state, as the
emulator's does with a null session).

## Threads (the emulator's model, kept)

- `Frame`: the DLL's 60 Hz frame thread = the emulator's present thread. It walks the actor list through
  `read` (fast in v2, callable from any thread), decides, sends. Like the emulator it walks while the game runs;
  a walk whose length is not the list's count is dropped. NX2navi's guide warns that a `read` from another
  thread can still fault if the game gives that memory back to the system at that moment.
- The frame step hook (main+0x3b4e300, only the calls returning to main+0x6e70fc and +0x6e6ddc, as
  `puppet.cpp`): the emulator's cave. The callers the hook files add (`CoopSvc::AddCaller`: the spawn
  director 10, the objects Mover 30, field grass 40, the boss mover 50) are asked in order; each call is made
  with `guest_call_regs`, stack arguments included (v2). At most 256 calls a frame. A refused call stops all
  further calls (logged).
- `OnEvent`: the network thread, into the hook files' inboxes.

## The AI hold (`objects_ai_hold`: 3 by default, 0 off)

**Mode 3, the one that works** (`brain.hpp`, `brain.cpp`; nx2navi/README.md, "Shared enemies"): a held copy's AI
update main+0xcb9c0 `(actor, times)` is left out by a function hook, except for a copy's first 45 updates as a
held copy and the 8 after a hit came in. In the real game it takes a copy's own animation choices from 90% of the
Mover's plays to none after the copy's first second and a half, and replayed hits are taken (8 of 8). Modes 1 and
2, below, are the emulator hook's brain skip bit; both ran in the real game and neither stops a copy's AI.

The emulator patched `blr x8` at main+0x369df8 (coop_hold.s). Read from main: that call is
`x0 = [manager+0x950], x1 = BSA component, blr [[x0] + 0x40 or 0x48]`, inside the BSA loop main+0x369b00
(called twice from the frame function at main+0x3b57010/+0x3b57020); the frame function has a third, inlined
copy of the loop with the same call at main+0x3b56d60, which the emulator never patched. Here:

1. main+0x369b00 is hooked; its x1 points at the manager, so each call resolves `[[[x1]+0x950]]+0x40/+0x48`
   and hooks those two functions once (runtime addresses: no list to ask for);
2. those hooks act only when `call->lr` is main+0x369dfc or main+0x3b56d64 and `x1` is a held component:
   `call_original`, then `x0 = (x0 & and) | or` (mode 1 sets bit 0, mode 2 clears it), exactly the cave;
3. if neither function can be hooked, the loop hook rewrites bits 0..3 of `component+0xc4` after the loop.

The held set is what the unchanged Mover computes (`Sync::Holds`: enemy copies the owner's samples place now);
it writes `CoopHold::Table` bytes to main+0x3c46400 each game frame, and the port's guest memory catches that
write instead of storing it. Neither mode stops the copy's own AI (run in the real game: the bit reads as asked
in every held copy-frame and the copies choose their own animation as often as with no hold); the status line
says `hold: N held, ... answers, ... altered`.

## This port's own rules

(coop-dll 0.8, after the upstream update of 2026-11-13: nx2navi/README.md, "Shared enemies", has the measurements.)

- **Every AS slot of an enemy is copied** (`CoopObjects::EnemySlots(true)`, set in `objects.cpp`; `lichii/hook/coop_objects_sync.h`).
  The owner's walk fills `Seen::slots` for every `Enemy_` actor (upstream did for bosses) and sends slot 0 also when it plays
  nothing; `Sync::Plays` wishes every slot; the Mover plays a command when the owner's slot changed to it (`last_wish_`) or the
  copy shows another one, and sets no frame.
- **A copy's first 150 AI updates run** (`brain.hpp` `kWarmFrames`, was 45): a Stalchild takes 2.5 s to rise.
- **The nearest player's game owns an enemy** (`server/`: `SessionConfig::nearest_owner`, `--owner nearest|classic`; the mod's
  server is compiled with `COOP_SERVER_OWNER_NEAREST`, so nearest is its default; the emulator kit's server is unchanged).
- **A player away from the host rolls its own Stalchildren**: now the shared code's own rule (`Sync::RollerFor`, 40 m), which
  replaced this port's `OwnRollsWhenApart`; the `objects_rolls_apart` key is read and ignored.
- **Time of day and drops**: `CoopTime::Frame` / `OnEvent` and `CoopDrops::Frame` / `OnEvent` are called from `objects.cpp`
  (`Options::time`, `Options::drops`); `coop_drops.cpp` is compiled with `-DLICHII_COOP` (its game side is under it).
- **The callers** are added through `CoopCallers::Add` (the shared `coop_callers.h`); the port keeps its own list and loop
  (`emu_port.cpp`), because the shared stepper asks the puppet director first and the port runs that in `puppet.cpp`.
- **Research switches**: `OOT_COOP_PROBE_ANIM=1` (each enemy near our Link twice a second: place, facing, slot names, whether
  its bones moved; and every AS play of a Stalchild, Guay or Tektite), `=2` (also writes each enemy back to one pose every
  frame: the picture still animates, so a pose written at the frame step is not what is drawn), `=4` (which game function
  empties a slot: `main+0x37b6420`, called with an object of the AI's, not the controller).

The older rules below are 0.7's.

- **Townspeople and animals are each game's own** (`objects_friendly = 0`, the default; `ShareFriendly(false)`).
  The hook shares every `Npc_` and `Animal_` actor (Malon, Ingo, the horses, the cows, cuccos, fish) like an
  enemy: one owner, the others' copies placed every frame where the owner's is. Only `Enemy_` copies are held
  from their own AI (`Sync::Holds`), so a shared horse or Malon ran its own AI in the other game and was pulled
  back each frame, and a player could not rely on talking to, riding or milking one another game owned. Holding
  them as enemies are held would be worse: an actor whose AI update is left out does not answer a talk or a
  mount at all. Nothing of theirs has to agree between the games (no life, no kill; what a talk gives is a flag
  or an item, shared or not by other means), so they are left out of sharing: each game runs its own, as without
  the mod. `objects_friendly = 1` puts the hook's rule back.
- **A player away from the host rolls its own Stalchildren** (`objects_rolls_apart = 30`, metres; 0 = off;
  `OwnRollsWhenApart`). The hook lets one game decide a scene's rolled enemies (the lowest player number) and
  deletes every other game's own rolls, so two players side by side get one set. A game rolls around its own
  Link only, so a second player away from the first had no Stalchildren at all (run `n1`: the joiner stood in
  the field at night for 60 s, 25 m from the host, with none). Now a game that does not decide keeps and shares
  a roll of its own that lies further than this from the deciding player's Link; it owns it, so it is the player
  that enemy goes for, and the other games get it as a copy. A fresh roll right where another game's enemy is
  being copied is never shared as a new enemy.
- **A hitch is not a scene change** (`scene_gate.hpp`). The hook forgets all it shares when it is told "no scene".
  The DLL told it so whenever the player could not be read for one frame, which also happens when the game's
  thread makes no frame for a second and the DLL's own thread makes the session's frame instead. Seen in run
  `s1`: after each such frame the host's game shared its Stalchildren again under new ids and the joiner's game
  made a second and a third copy of each. Now such a frame does not reach `Objects::Frame`; "no scene" is passed
  on when the game's own thread could not read the player for half a second (a real load).

## What it gives, and what it does not

- Same enemies, places, life and deaths: yes as far as the emulator build's rules go (one owner per enemy; the
  others' copies placed by the game's own setter every game frame, 100 ms behind the owner, with its animation;
  a hit on a copy goes to the owner as DAMAGE; the owner's kill removes the copies; rolled Stalchild decided by
  one game and copied by the spawn director). The copy's own AI still runs unless the hold works: it may pick
  its own animation for a frame (put back the next) and attack its own game's Link, harmlessly placed.
- Seen in two real games (nx2navi/README.md, "Verified, and not", the runs with coop-dll 0.7): the second
  player's strikes and kills on the first player's enemies (8 of 8 strikes mirrored, 4 kills), the first player's
  on a mini-boss room's two Stalfos (18 of 18), a placed enemy changing hands as two players swapped
  places (9 to 16 s after each swap, five swaps in three runs), and each player's own Stalchildren when they are apart.
- "The enemy attacks the nearest player": it attacks the player of the game that owns it. Ownership moves to
  the nearer player's game only out of a fight, when that player is within 30 m, at least 10 m nearer than the
  owner, and the owner is over 40 m away or that player within 20 m, for 3 s (measured in the tests: 3.4 s);
  or at once when the nearer player hits it; or when the other game's copy chases its Link for 0.5 s. In a
  fight (life changed lately) it stays: measured 25.5 s after the last hit before the distance rule moved it
  (the owner flags in_fight for 12 s and each OBJECT_PRESENT restarts the server's 12 s lock). An enemy between
  two players with less than 10 m between their distances stays with its owner. Hits on a puppet do nothing.
- Bosses: one owner for the boss and its parts (OBJECT_GROUP), pull into the room once a player's intro or
  fight starts, the intro skipped for those still in it when another finished: same room, same moment within
  a network delay. The boss's AI phase is not copied: only place, turn, animation and life follow the owner.
- Not ported: `coop_spawn.cpp`'s projectile sync (bombs, arrows on other screens), `coop_drops.cpp`,
  `coop_time.cpp`, `coop_live_events.cpp`'s cave caller.

## The build that runs upstream's hook (the default since 2026-11-13)

`build.py` builds `lichii/hook` unchanged and `dll/upstream.cpp` drives it; of this folder it uses `brain.cpp`, `hold.cpp`, `anim_probe.cpp` and `world_probe.cpp` only. Two things of that build belong to shared enemies: the spawner's wait for a copy is 20 s there (`-DLICHII_COOP_SPAWN_CREATE_TIMEOUT_MS=20000`; with the hook's own 3 s the second player of two games on one PC got no Stalchild copies at all), and the hook's `objects: list head` line is logged once a minute. `nx2navi/README.md`, "Since the afternoon of 2026-11-13", has the runs.

Since 14:00 of 2026-11-13 two more things of that build touch what this folder is about: the hook's reads no longer go through a kept `map_range` page (a crash in a load, run `wd1`; every read is the interface's own `read`), and the hook's clock rule is narrowed for this build (`-DLICHII_COOP_TIME_RESYNC_SECONDS=180 -DLICHII_COOP_TIME_SET_EVERY_US=15000000`: a guest's clock is set again when it is three in-game minutes from the host's). An enemy that changes owner still plays its own first animation in the new owner's game (a Stalchild: "Disappear"): not changed.
