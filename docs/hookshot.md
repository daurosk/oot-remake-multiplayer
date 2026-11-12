# Co-op on Hookshot (Stage 2: recomp)

[Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0) is a static recompilation of the same game build co-op already supports: main build id `e0f3ccfaf50065ae` (version 1.1.1). The game's AArch64 code is translated to C ahead of time from the player's own dump, compiled for x86-64 Windows and Linux, with Horizon OS and NVN2 written natively and the GPU work replayed on Vulkan. No CPU or GPU emulation.

This page is the plan for moving co-op onto it as Stage 2 (see [plan.md](plan.md)). It was written from Hookshot commit `c544828ff2d545829505cd894bb7968c7c2c5fcb` (2026-11-12) and our `lichii/hook` on main at `dbf084b`. Phase 2 is under way: the patch, the module and their unit tests are in `hookshot/` ([hookshot/README.md](../hookshot/README.md)); the game has not been built or run with them yet (see Status).

Hookshot's own repository is never forked or committed to from here. Our repository gets only our patches, our code, docs and scripts. Built game files and Hookshot's generated C (both contain the game) are never published.

## Short answer

- Co-op fits Hookshot better than the emulator. The emulator cannot call a game function, so `lichii/hook` writes caves into main and talks to them through a custom SVC. Hookshot calls a game function from host C directly (`a64_lookup(address)(ctx)`), and adds C after any ordinary instruction at build time (`Lifter.after`; not after a call). The SVC cave, the hold cave and their assembly go away.
- Hookshot already does, for its own menu, much of what we need: a hook on Link's update (`hos_oot_is_player`), game function calls by address (`oot_teleport`, the GameDataMgr getters), the current map, cross-map teleports, and a local control socket for tests.
- Addresses: Hookshot loads main at **`0x80020000`** (Eden's layout: rtld at `0x80000000`, then main). Every offset in `CoopGameBuild::MainOffsets` becomes `0x80020000 + offset`. Checked on one function: our `set_time` `0x34522d0` is Hookshot's `0x834722d0` (`hos_main.c` `oot_set_time`). Data slots must be checked one by one in Phase 2 (Hookshot names other singletons than ours, so none of its data addresses could be compared yet).
- Emulator and Hookshot players can play together: same game build, same coordinates and data, same protocol, same server. Nothing on the wire says which runs the game.
- Size: about 300 lines of patches to Hookshot, about 1,500 lines of new co-op code on our side, and a one-time change of about 10 hook files from the emulator's types to an interface both sides implement. The protocol, server, net, session and most of the game logic port unchanged.

## How co-op maps onto Hookshot

| Co-op needs | Fritchii today (`lichii/hook`) | Hookshot |
|---|---|---|
| A call once a frame, on the game's calc thread, after the actors ran | Patch the two `bl frame_step` in moduleCalc_ (main+`0x6e6dd8`, rarely; main+`0x6e70f8`, every frame) into a cave at `0x3c46280` that loops on `svc #0xf4` (`coop_svc.h` kHooks) | A lift-time hook after the `mov x0, x20` just before each call (main+`0x6e6dd4` and `0x6e70f4`): `hos_coop_frame(c, site)` (the same mechanism as `nx2/oot_fov.py`; `Lifter.after` does not apply to a call itself). Build id and all four words checked |
| Calling game functions (create actor, play AS, set matrix, equip, malloc, request delete, events ...) | The SVC handler returns one command, the cave makes the call, loops | Direct: save `Ctx`, set x0-x7 and a stack below `sp`, `a64_lookup(addr)(c)`, read x0, restore. `hos_main.c` `oot_gamedata_bool` is the pattern |
| Reading and writing game memory | `EmulatorMemory` over `system.ApplicationMemory()` (`coop_flags.h`), a `coop::host::WritableGameMemory` | The same interface over `hos_guest_base()` + guest address (or `recomp_load*/store*`). Writes the GPU reads go through Hookshot's write-watch like the game's own |
| The AI hold (enemies frozen for a guest) | Patch the `blr x8` at `0x369df8` to the hold cave and table (`coop_hold.s`) | After the next instruction (`ldrh w8, [x21, #0xc4]`, which leaves w0 alone): `w0 = hos_coop_hold(x21, w0)`. The module returns the bits unchanged until shared enemies come over |
| Main base, build id | Kernel process and module lookups | Fixed `0x80020000`; Hookshot's `hos_main_build_id` |
| Sockets, network thread | Our `coop/net` thread inside the emulator | The same thread inside the game process. Hookshot has no sockets of its own (it answers the game's socket calls "nothing ready"); ours are plain Winsock/BSD and do not go through the game |
| The puppet pack (a LayeredFS mod) | `user\load\0400486024DEC000\coop-puppet\romfs` | Hookshot reads the same layout from its `mods` folder (`host/mods.c`: any folder holding a `romfs` folder). `tools/puppet_pack/build.py --mod-dir <game folder>\mods` |
| Co-op window, chat, player list, markers | Qt window in the emulator (`coop_window.cpp`, `qt.cmake`) | Hookshot draws its menus with Dear ImGui over the picture (`gpu/vk/vk_imgui.cpp`, `vk_menu.cpp`). A co-op panel there is later work; the proof needs none |
| Scripted input for tests | `LICHII_COOP_TEST_SCRIPT` into the emulator's HID (`coop_testinput.cpp`) | Hookshot's control bridge ([control.md](https://github.com/NotRexed/Hookshot/blob/main/docs/control.md)): a named pipe (`HOS_CONTROL`), timed presses, `status` (frame, fps, state, map), `wait map`, `teleport`, `shot`, and opt-in guest memory reads (`HOS_CONTROL_MEM=ro`). Client `scripts/hookshot_ctl.py`. Our autotest drives two of them |

### Where the co-op module lives

Two parts:

1. **Our patch set to Hookshot** (`hookshot/patches/*.patch`, applied by `hookshot/fetch.ps1` to the commit in `hookshot/PIN`, as `lichii/` does for Fritchii):
   - `nx2/coop_hooks.py`: the frame hook and the hold hook, each checked against the build id and the instruction words, else left out with a log line. Called from the lift (`nx2/lift.py`, two lines) next to `oot_occlusion.install`.
   - `host/coop_bridge.c`: on the first frame hook, loads `hookshot_coop.dll` from beside the exe (unless `HOS_COOP=0`) and hands it the C table in `host/hos_coop_api.h`: main's base and build id, the game folder, read, write, a 32-bit compare-and-swap, call (`a64_lookup` with the game's `Ctx` saved and put back whole, arguments in x0-x7, four stack words, s0) and the log. Without the DLL, both hooks return at once.
   - No `nx2/build.py` change: it compiles every `host/*.c`.
2. **Our co-op DLL** (`hookshot/coop/` in our repo, built by our CMake with Hookshot's llvm-mingw clang): `coop/` (protocol, net, session, host readers) as it is, the game logic from `lichii/hook` that has no emulator types, and a Hookshot adapter in place of `coop_hook.cpp`'s `Core::System` parts.

A DLL keeps the patch set to Hookshot small (it changes only when Hookshot moves) and keeps our C++20 out of Hookshot's C build. The emulator's types reach only 10 hook files today (`coop_hook`, `coop_objects`, `coop_spawn`, `coop_drops`, `coop_testinput`, `coop_teleport_game`, `coop_boss_game`, `coop_time`, `coop_svc`, `coop_scene_probe`), and nearly all of that is `system.ApplicationMemory()` (23 uses) and the module and build id lookups. The puppet director already works through its own `CoopPuppet::Guest` read/write interface and emits `Call`s, which on Hookshot become direct calls instead of cave commands.

## What ports as it is, what is rewritten

| Part | On Hookshot |
|---|---|
| `coop/protocol`, `coop/net`, `server/` | As it is. No server change; one server for both kinds of player |
| `coop/host` (session, sampler, player reader, flags, items, save transfer) | As it is, given a `WritableGameMemory` |
| `coop_puppet`, `coop_pose`, `coop_world`, `coop_objects_sync`, `coop_dungeon`, `coop_flags`, `coop_chests`, `coop_fairy`, `coop_story`, `coop_time_sync`, `coop_ocarina`, `coop_projectiles`, `coop_live_events`, `coop_boss` | As they are (no emulator types). Addresses through `CoopGameBuild` with Hookshot's main base |
| `coop_hook.cpp` (frame driver, build check, settings start) | Rewritten as the Hookshot adapter (`hookshot/coop/coop_hookshot.cpp`): same order of work per frame, memory and calls through the bridge |
| `coop_svc.{cpp,s}`, `coop_hold.{h,s}` (caves, SVC 0xf4, loader patch) | Gone: replaced by the lift hooks and direct calls |
| `coop_objects`, `coop_spawn`, `coop_drops`, `coop_teleport_game`, `coop_boss_game`, `coop_time`, `coop_scene_probe` | Small change: take the memory interface and the main base instead of `Core::System`. Done once, in `lichii/hook`, so both builds share one copy |
| `coop_testinput` | Not used: the control bridge does it |
| `coop_window`, `coop_cheats_window`, `qt.cmake`, the overlay | Not in the proof. Later an ImGui panel in Hookshot's menu (a patch to `vk_menu.cpp`), or a separate small window |

## Size

- Patches to Hookshot (done): `coop_hooks.py` 70 lines, `coop_bridge.c` 150, `hos_coop_api.h` 50, `lift.py` 2; one patch file.
- New on our side: the adapter ~600 lines, the DLL's C table and call wrapper ~300, `hookshot/fetch.ps1` and a build script ~300, the autotest driver for two Hookshot games ~300.
- Changed on our side: the 10 files above, mostly a type swap.
- Building Hookshot: the player's dump extracted (~26 GB), ~1.4 GB of generated C, about 25 to 45 minutes for a first full build, a few minutes after. Hookshot's own builder does the extract, lift and build from the `.nup`. This PC had 261 GB free on 2026-11-12.

## Risks

- **Two games on one PC.** A Hookshot run takes about 10 GB of RAM and 8 GB of VRAM is recommended. Two at once need the lower quality preset (`RX_VK_LOW_SPEC=1` or the Minimum/Low preset). Hookshot's own `run-vk.ps1` refuses a second run; we start `hookshot.exe` directly, each from its own game folder (saves and logs beside it).
- **Open crashes and GPU limits.** Hookshot needs Vulkan 1.3 with mesh shaders (RTX 20 or newer, RX 6000 or newer); Intel is mostly untested. Open issues: an intermittent device loss after the first gameplay frames (design.md, S4), crashes outside Kokiri Forest (Hookshot issues 317, 648), AMD crashes (708, 614). Kokiri Forest is its best-tested place, which suits the proof.
- **Patch upkeep.** Hookshot moves fast (many commits a day). Our patches touch new files plus two small edits (`build.py`, the lift's install list); `hookshot/PIN` names the commit they apply to, and moving the pin is a test like moving the Fritchii pin.
- **Frame timing.** Hookshot runs the game at its own speed (30 to 60 fps, a high-fps option of its own). Our sampler and puppet already work from time, not frames, but the high-fps option is to check with a puppet in view.
- **Untested addresses.** Only code offsets are checked against Hookshot so far. Each data slot in `CoopGameBuild::MainOffsets` is checked once in Phase 2 (read through the control bridge and compared with what the emulator reads at the same place).
- **Licences.** Hookshot is GPL-3.0, as we are. Its lifted C contains the game and is never shared; we ship our DLL, our patches and our scripts, and a player builds Hookshot from their own dump.

## Emulator and Hookshot players together

Yes, by design. Both run main `e0f3ccfaf50065ae`, so positions, actor names, flag hashes and scene names are the same; both send the same `HELLO` with the same protocol version to the same `coop_server`. The server cannot tell them apart and does not need to. To check in Phase 2 once one Hookshot game works: one Fritchii kit and one Hookshot game in the same room.

## Phase 2 plan

1. `hookshot/PIN` (repo, commit), `hookshot/fetch.ps1` (clone to `..\hookshot-src`, apply `hookshot/patches/*.patch` on a local branch, never push), `hookshot/README.md`.
2. Build plain Hookshot from source with the `.nup` (no patches) and boot it once into Kokiri Forest: the baseline.
3. The patch set: frame hook + bridge, with a test DLL that only logs once a second. Then the adapter with the presence and puppet path only (send our pose, spawn and move the remote puppet).
4. The two-game proof: two Hookshot games, two game folders, one `coop_server`, both in Kokiri Forest, each showing the other's puppet moving. Screenshots in `..\reports\hookshot-shots\`.

## Credits

Hookshot is by NotRexed and its contributors, GPL-3.0, itself a fork of mkw-recomp and built with a64-kit, suyu's translator and Eden's shader recompiler (see its readme). Co-op only patches and links to it; none of its code is in this repository.

## Status

| Step | State |
|---|---|
| `hookshot/PIN`, `fetch.ps1`, the patch (bridge, hooks, API header) | Done; `fetch.ps1` applies the patch to the pin. `coop_bridge.c` passes a compiler's syntax check; not yet built inside Hookshot |
| The module (`hookshot/src`, CMake target `hookshot_coop`): presence and puppets | Written; built and unit-tested by CI (`tests/hookshot/module_test.cpp`: the API header in the patch is ours, the hook sites are the emulator cave's, build and settings checks) |
| Callers shared by both builds (`lichii/hook/coop_callers.{h,cpp}`) | Done: the emulator's `svc #0xf4` handler and the module ask the same stepper (`tests/lichii/callers_test.cpp`) |
| Data slots of `CoopGameBuild::MainOffsets` checked on Hookshot | Not yet (the first run reads Link through them: `coop: scene ...` in the log is the first check) |
| Hookshot built from the `.nup` with the patch | Not yet: needs Hookshot's toolchain (llvm-mingw and its builder's `tools` folder) and a build turn |
| Two Hookshot games in Kokiri Forest showing each other's puppet | Not yet |
