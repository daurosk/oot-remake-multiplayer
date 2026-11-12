# hookshot/

Co-op inside [Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0), the PC port of the same game build. The design and what is proven: [docs/hookshot.md](../docs/hookshot.md); for players: the kit's `Build-Hookshot-Coop.bat` and docs/play-online.md "On Hookshot" (branch hookshot-kit). Hookshot's source is not in this repository and not forked on GitHub. Built games and Hookshot's generated C contain the game: they never leave the player's PC.

| File | What |
|---|---|
| `PIN` | Hookshot's repo and the commit our patch applies to. |
| `patches/0001-coop-bridge.patch` | Our only change to Hookshot: `nx2/coop_hooks.py` (the frame hook after the `mov x0, x20` before each of moduleCalc_'s two calls of the frame function, and the AI hold after main+0x369df8's call), its two lines in `nx2/lift.py`, `host/coop_bridge.c` (loads the module; guest memory, game calls, the log) and `host/hos_coop_api.h`. |
| `patches/0004-coop-title-menu.patch` | The title menu ([docs/hookshot.md](../docs/hookshot.md#the-title-menu)): `nx2/coop_title_hooks.py` (three calls from the game's title screen), the pad to the module first in `host/nn_hid.c`, and `scratch` / x8 for the module's game calls in `host/coop_bridge.c` and `hos_coop_api.h`. |
| `fetch.ps1` | For developers: clones Hookshot beside the main checkout at the pin and applies the patches as a local commit (as `lichii/fetch.ps1`). |
| `src/` | The module, `hookshot_coop.dll`: `hos_coop_api.h` (the bridge's table, the same file the patch adds), `coop_hookshot.cpp` (start, frame, AI hold, stop; it drives `CoopHook` and makes the callers' game calls). |
| `compat/` | Stand-ins for the slice of the emulator's API that `lichii/hook` uses, answered from the bridge's table, so every hook file compiles unchanged for Hookshot: one copy of co-op for both. |
| `CMakeLists.txt` | Target `hookshot_coop` (Windows, MSVC): `lichii/hook`'s files as `core.cmake` lists them, but the emulator's cave and input injection, plus `src/`. |
| `proof.py` | The two-game check on one PC (both games, the server, shots of both pictures, the co-op log lines). |

## A co-op Hookshot game, for development (Windows)

1. `powershell -ExecutionPolicy Bypass -File hookshot\fetch.ps1`.
2. The tools Hookshot's readme lists under "Build from source" (the kit's `Build-Hookshot-Coop.bat` does all of this for players: [setup.md, "Play on Hookshot"](../docs/setup.md#play-on-hookshot-experimental)). In `hookshot-src`: `python -I nx2/make_game.py --nup <your .nup> --out <game folder> --language en-US`. Its log says `co-op hooks: frame hook at ...`.
3. `cmake --build build --config Release --target hookshot_coop` here; `hookshot_coop.dll` beside `hookshot.exe`.
4. The puppet pack: `tools/puppet_pack/build.py --romfs <your .nup> --mod-dir "<game folder>\mods\contents\0400486024DEC000\coop-puppet"`.
5. `coop.ini` beside `hookshot.exe` (as the emulator's: `server = 127.0.0.1`, `name = Saria`) and the `LICHII_COOP_*` switches as in the kit's `coop-options.ini`. `hookshot_log.txt` says `coop: module loaded` and `coop: connected`.

`HOS_COOP=0` keeps the module out for a run.
