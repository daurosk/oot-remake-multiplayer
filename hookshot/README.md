# hookshot/

Co-op inside [Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0), the static recompilation of the same game build. The plan and what is proven: [docs/hookshot.md](../docs/hookshot.md). Hookshot's source is not in this repository and not forked on GitHub: `fetch.ps1` clones it at the pinned commit next to this repo and applies our patch as a local commit. Built games and Hookshot's generated C contain the game: they never leave the player's PC.

| File | What |
|---|---|
| `PIN` | Repo URL and the Hookshot commit our patch applies to. |
| `fetch.ps1` | Clones or updates `hookshot-src` beside the main checkout to the pin and applies `patches/*.patch` on a local branch `coop`. |
| `patches/0001-coop-bridge.patch` | `nx2/coop_hooks.py` (the frame hook after the `mov x0, x20` before each of moduleCalc_'s two calls of the frame function, and the AI hold after main+0x369df8's call), its two lines in `nx2/lift.py`, `host/coop_bridge.c` (loads the module, guest memory, game calls, log) and `host/hos_coop_api.h`. Hookshot compiles every `host/*.c`, so its build needs no change. |
| `src/hos_coop_api.h` | The C table between the bridge and the module; the patch carries the same file (`tests/hookshot/module_test.cpp` checks). |
| `src/coop_hookshot.cpp` | The module, `hookshot_coop.dll`: settings, the network thread, our Link read and sent, puppets planned and their game calls made (lichii/hook's `coop_puppet`, `coop_pose`, `coop_callers`). Built by this repository's CMake (`COOP_BUILD_HOOKSHOT`, on by default). |

## Make a co-op Hookshot game folder (Windows)

1. `powershell -ExecutionPolicy Bypass -File hookshot\fetch.ps1` (once, and after a pin change).
2. Build the game from your own `.nup` with Hookshot's own script, from `hookshot-src` (its readme, "Build from source", lists the tools: Python 3 with its `requirements.txt`, llvm-mingw, the builder's `tools` folder): `python -I nx2/make_game.py --nup <your .nup> --out <game folder> --language en-US`. The log says `co-op hooks: frame hook at ...`.
3. Build the module: `cmake -S . -B build` and `cmake --build build --config Release --target hookshot_coop` in this repository; copy `hookshot_coop.dll` next to `hookshot.exe` (the game folder's `bin\` when it has one).
4. The puppet pack into the game's mods: `<repo venv python> tools/puppet_pack/build.py --romfs <your .nup> --mod-dir "<game folder>\mods\contents 0486024DEC000\coop-puppet"` (the hook looks for the pack there, as in an emulator's load folder).
5. `coop.ini` beside the game (as the emulator's: `server = 127.0.0.1`, `name = Saria`), or `LICHII_COOP_SERVER` / `LICHII_COOP_NAME`. Start `coop_server` and the game. `hookshot_log.txt` says `coop: module loaded` and `coop: connected`.

`HOS_COOP=0` keeps the module out for a run.
