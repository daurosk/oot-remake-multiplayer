# Co-op on Hookshot (Stage 2: recomp)

[Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0) is a static recompilation of the same game build co-op already supports: main build id `e0f3ccfaf50065ae` (version 1.1.1). The game's AArch64 code is translated to C ahead of time from the player's own dump, compiled for x86-64 Windows and Linux, with Horizon OS and NVN2 written natively and the GPU work replayed on Vulkan. No CPU or GPU emulation.

Co-op runs on it as Stage 2 (see [plan.md](plan.md)), experimental, next to the emulator kit in the release kits. This page is the design, for Hookshot commit `c544828ff2d545829505cd894bb7968c7c2c5fcb` (2026-11-12); the files are in `hookshot/` ([hookshot/README.md](../hookshot/README.md)); what is proven is under Status.

Hookshot's own repository is never forked or committed to from here. Our repository gets only our patches, our code, docs and scripts. Built game files and Hookshot's generated C (both contain the game) are never published.

## Short answer

- Co-op fits Hookshot better than the emulator. The emulator cannot call a game function, so `lichii/hook` writes caves into main and talks to them through a custom SVC. Hookshot calls a game function from host C directly (`a64_lookup(address)(ctx)`), and adds C after any ordinary instruction at build time (`Lifter.after`; not after a call). The SVC cave, the hold cave and their assembly go away.
- Hookshot already does, for its own menu, much of what we need: a hook on Link's update (`hos_oot_is_player`), game function calls by address (`oot_teleport`, the GameDataMgr getters), the current map, cross-map teleports, and a local control socket for tests.
- Addresses: Hookshot loads main at **`0x80020000`** (Eden's layout: rtld at `0x80000000`, then main). Every offset in `CoopGameBuild::MainOffsets` becomes `0x80020000 + offset`. Checked on one function: our `set_time` `0x34522d0` is Hookshot's `0x834722d0` (`hos_main.c` `oot_set_time`). Data slots go through the same offsets; the first runs check them.
- Emulator and Hookshot players can join the same game: same game build, same coordinates and data, same protocol, same server. Nothing on the wire says which runs the game. (In this release a Hookshot game does not show the others: making their Link crashes Hookshot, so it is switched off; see Status.)
- One copy of co-op for both: the module builds every `lichii/hook` file unchanged, with stand-ins for the emulator API they use (`hookshot/compat`). Our patch to Hookshot is about 270 lines in one file.

## How co-op maps onto Hookshot

| Co-op needs | Fritchii today (`lichii/hook`) | Hookshot |
|---|---|---|
| A call once a frame, on the game's calc thread, after the actors ran | Patch the two `bl frame_step` in moduleCalc_ (main+`0x6e6dd8`, rarely; main+`0x6e70f8`, every frame) into a cave at `0x3c46280` that loops on `svc #0xf4` (`coop_svc.h` kHooks) | A lift-time hook after the `mov x0, x20` just before each call (main+`0x6e6dd4` and `0x6e70f4`): `hos_coop_frame(c, site)` (the same mechanism as `nx2/oot_fov.py`; `Lifter.after` does not apply to a call itself). Build id and all four words checked |
| Calling game functions (create actor, play AS, set matrix, equip, malloc, request delete, events ...) | The SVC handler returns one command, the cave makes the call, loops | Direct: save `Ctx`, set x0-x7 and a stack below `sp`, `a64_lookup(addr)(c)`, read x0, restore. `hos_main.c` `oot_gamedata_bool` is the pattern |
| Reading and writing game memory | `EmulatorMemory` over `system.ApplicationMemory()` (`coop_flags.h`), a `coop::host::WritableGameMemory` | The bridge's read/write (bounds-checked against mapped guest memory; writes marked for the GPU mirror like the game's own) behind `hookshot/compat/core/memory.h`, so `EmulatorMemory` is unchanged |
| The AI hold (enemies frozen for a guest) | Patch the `blr x8` at `0x369df8` to the hold cave and table (`coop_hold.s`) | After the next instruction (`ldrh w8, [x21, #0xc4]`, which leaves w0 alone): `w0 = hos_coop_hold(x21, w0)`. The module applies the table `CoopObjects` writes at main+`0x3c46400`, as the hold cave does (with `LICHII_COOP_OBJECTS_AI_HOLD`) |
| Main base, build id | Kernel process and module lookups | Fixed `0x80020000`; Hookshot's `hos_main_build_id` |
| Sockets, network thread | Our `coop/net` thread inside the emulator | The same thread inside the game process. Hookshot has no sockets of its own (it answers the game's socket calls "nothing ready"); ours are plain Winsock/BSD and do not go through the game |
| The puppet pack (a LayeredFS mod) | `user\load\0400486024DEC000\coop-puppet\romfs` | Hookshot reads the same layout from its `mods` folder (`host/mods.c`: any folder holding a `romfs` folder). Built into `<game>\mods\contents\0400486024DEC000\coop-puppet` (`hookshot/compat` points the hook's load folder at `mods\contents`) |
| Co-op window, chat, player list, markers | Qt window in the emulator (`coop_window.cpp`, `qt.cmake`) | Hookshot draws its menus with Dear ImGui over the picture (`gpu/vk/vk_imgui.cpp`, `vk_menu.cpp`). A co-op panel there is later work (for now: none, the log only) |
| Scripted input for tests | `LICHII_COOP_TEST_SCRIPT` into the emulator's HID (`coop_testinput.cpp`) | Hookshot's control bridge ([control.md](https://github.com/NotRexed/Hookshot/blob/main/docs/control.md)): a named pipe (`HOS_CONTROL`), timed presses, `status` (frame, fps, state, map), `wait map`, `teleport`, `shot`, and opt-in guest memory reads (`HOS_CONTROL_MEM=ro`). Client `scripts/hookshot_ctl.py`. Our autotest drives two of them; the test-only steps go to the module through `LICHII_COOP_TEST_COMMANDS` (`tools/autotest/hookshot.py`) |

### Where the co-op module lives

Two parts:

1. **Our patch to Hookshot** (`hookshot/patches/0001-coop-bridge.patch`, for the commit in `hookshot/PIN`):
   - `nx2/coop_hooks.py`: the frame hook and the hold hook, each checked against the build id and the instruction words, else left out with a log line. Called from the lift (`nx2/lift.py`, two lines) next to `oot_occlusion.install`.
   - `host/coop_bridge.c`: on the first frame hook, loads `hookshot_coop.dll` from beside the exe (unless `HOS_COOP=0`) and hands it the C table in `host/hos_coop_api.h`: main's base and build id, the game folder, read, write, a 32-bit compare-and-swap, call (`a64_lookup` with the game's `Ctx` saved and put back whole, arguments in x0-x7, four stack words, s0) and the log. Without the DLL, both hooks return at once.
   - No `nx2/build.py` change: it compiles every `host/*.c`.
2. **The co-op module** (`hookshot_coop.dll`, CMake target `hookshot_coop`, built with MSVC as the emulator's hook is): **all of `lichii/hook`**, the files `lichii/hook/core.cmake` lists, except the emulator's cave (`coop_svc.cpp`); `coop_testinput.cpp` is built without its pad input and plays the autotest driver's test commands (place, give, assign, chat, setflag, setenum, wear, kill) from the file `LICHII_COOP_TEST_COMMANDS` names. `hookshot/compat/` stands in for the slice of the emulator's API those files use (`Core::System`'s memory, process and build id, the loader's module list, `Common::FS` folders, `LOG_*`), answered from the bridge's table. So every co-op feature of a release is in both builds from one copy of the code, and a hook file that reaches for more of the emulator fails the module's CI build. `hookshot/src/coop_hookshot.cpp` drives it: `CoopHook::Start`, `CoopHook::OnFrame` at the every-frame site, then the callers' game calls (`lichii/hook/coop_callers.{h,cpp}`, the same stepper the emulator's `svc #0xf4` handler asks), the AI hold from the same table the emulator's hold cave reads, `CoopHook::Shutdown` at exit.

A DLL keeps the patch to Hookshot small (it changes only when Hookshot moves) and keeps our C++20 out of Hookshot's C build.

## For players: the kit

Nothing of Hookshot or the game is in the kit. The kit's `Build-Hookshot-Coop.bat` (player steps in [setup.md, "Play on Hookshot"](setup.md#play-on-hookshot-experimental)) downloads Hookshot's builder parts and source for `hookshot/PIN` (each checked against its SHA-256), applies `hookshot/patches` without git, runs Hookshot's own `nx2/make_game.py` on the player's game file, and installs `hookshot_coop.dll`, `coop.ini` (beside `hookshot.exe`) and the puppet pack. `Host-Hookshot.bat` and `Join-Hookshot.bat` start it as `Host.bat` and `Join.bat` start the emulator.

What the module expects from that install (`hookshot/compat`): `coop.ini` beside `hookshot.exe`; the puppet pack at `<exe folder>\mods\contents\0400486024DEC000\coop-puppet` (the hook finds packs under `<load folder>\0400486024DEC000\<mod>`, and Hookshot's `host/mods.c` loads that layout as a mod); the `LICHII_COOP_*` switches from `coop-options.ini` in the environment.

## Size

- Our patch to Hookshot: `coop_hooks.py` 70 lines, `coop_bridge.c` 150, `hos_coop_api.h` 50, `lift.py` 2.
- Ours: `hookshot/src` about 250 lines, `hookshot/compat` about 250, `coop_callers` 100, the kit script 120 and the patch applier 90. No hook file changed for Hookshot except `coop_svc` (its stepper moved to `coop_callers`).
- Building Hookshot: the game unpacked (~26 GB), ~1.4 GB of generated C, 25 to 60 minutes the first time.

## Risks

- **Memory.** A Hookshot game takes about 10 GB of RAM; 8 GB of VRAM is recommended. Two games on one PC are fine on the test PC (128 GB of RAM) at the normal preset; players with 16 to 32 GB of RAM should run one game each.
- **Open crashes and GPU limits.** Hookshot needs Vulkan 1.3 with mesh shaders (RTX 20 or newer, RX 6000 or newer); Intel is mostly untested. Open issues there: an intermittent device loss after the first gameplay frames (its design.md, S4), crashes outside Kokiri Forest (Hookshot issues 317, 648), AMD crashes (708, 614).
- **Patch upkeep.** Hookshot moves fast. Our patch touches three new files and two lines of `nx2/lift.py`; moving `hookshot/PIN` (and the kit's list of builder parts with it) is a test like moving the Fritchii pin.
- **Frame timing.** Hookshot runs at its own speed (30 to 60 fps, a high-fps option). The co-op sampler and puppets work from time, not frames; to check with a puppet in view.
- **Addresses.** Code offsets are checked (`set_time`); the data slots are read through the same `CoopGameBuild` offsets from main's base, and the first runs check them (the scene and Link read, puppets made).
- **No co-op window yet.** The emulator's Qt window (panel, chat box, player list, hearts) is not on Hookshot; an ImGui panel in Hookshot's menu is later work.
- **Licences.** Hookshot is GPL-3.0, as we are. Its lifted C contains the game and is never shared; we ship our module, our patch and our scripts, and each player builds Hookshot from their own game.

## Emulator and Hookshot players together

Yes, by design. Both run main `e0f3ccfaf50065ae` with the same co-op code, so positions, actor names, flag hashes and scene names are the same; both send the same `HELLO` with the same protocol version to the same `coop_server`, which compares only the game build. Proof (4) below checks it.

## Proofs

1. Plain Hookshot built from the `.nup` and boots.
2. Hookshot with our patch built; the log says `co-op hooks: frame hook` and `coop: module loaded`.
3. Two Hookshot games in Kokiri Forest, each showing the other's puppet moving (`hookshot/proof.py`, shots in `..\reports\hookshot-shots\`).
4. One Hookshot and one emulator game in one room, each showing the other's puppet.
5. The integrator's autotest list on Hookshot through its control socket, as far as it goes; what fails goes into known issues.

## Credits

Hookshot is by NotRexed and its contributors, GPL-3.0, itself a fork of mkw-recomp and built with a64-kit, suyu's translator and Eden's shader recompiler (see its readme). Co-op only patches and links to it; of its code, only the context lines of our patch (`hookshot/patches`) are in this repository (THIRD_PARTY_NOTICES.md).

## Status

| Step | State |
|---|---|
| The patch (bridge, hooks, API header), `PIN`, `fetch.ps1` | Done; `fetch.ps1` applies it to the pinned source; `coop_bridge.c` passes a compiler's syntax check |
| The module: all of `lichii/hook` through `hookshot/compat` | Written; all 42 hook files and the module pass a syntax check; CI builds it on Windows; `tests/hookshot/module_test.cpp` (the header in the patch is ours, the hook sites are the emulator cave's, the build list) |
| Callers shared by both builds (`coop_callers`) | Done (`tests/lichii/callers_test.cpp`) |
| Kit (`Build-Hookshot-Coop.bat`, `Host-/Join-Hookshot.bat`) | In the release kit; ran end to end on the test PC (download, checks, patch, build, co-op install) |
| Proofs 1 to 5 | 2 done: the patched game builds and loads the module. 3 and 4: making another player's Link (any `CoopPuppet_*`) crashes Hookshot (the game sets a texture matrix on the hair material, which has no material there); the module now refuses those creates, so a Hookshot game does not show the other players yet (no crash in the re-runs; `LICHII_COOP_HOOKSHOT_LINK_PUPPETS=1` re-enables them, and the game then crashes). Emulator players see Hookshot players as Link. <!-- CONFIRM with the proof 3/4 re-runs on hs-crash-b --> |
