# lichii/

Co-op inside Frizti's emulator (Lichii, now [Fritchii](https://github.com/Frizti/Fritchii), GPL-3.0). The emulator's source is not in this repo and not forked on GitHub: `fetch.ps1` clones it at the pinned commit next to this repo and applies our patches as local commits. Everything we add stays small so a new Fritchii release is easy to take.

| File | What |
|---|---|
| `PIN` | Repo URL, release tag and commit we build against. |
| `fetch.ps1` | Clones or updates the source to the pin and applies `patches/*.patch` in order. |
| `patches/0001-coop-hook.patch` | The hook points, nothing else. |
| `patches/0002-coop-camera.patch` | One call per draw in the Vulkan renderer that hands the game's camera to the hook, for markers. |
| `patches/0003-coop-puppet.patch` | Two calls for the puppets: the code cave written into Ocarina's `main` when it is loaded, and the cave's `svc 0xf4` handed to the hook. |
| `patches/0004-coop-null-write-log.patch` | A debug log line: who writes to guest address 0 (issue 111). |
| `patches/0005-coop-portable-switches.patch` | A portable kit reads only its own `nx2_switches.txt`, never the one in `%APPDATA%\fritchii` or `eden` (v5.8). |
| `patches/0006-coop-save-folder.patch` | A guest's game reads and writes the co-op save folder `<title>-coop` beside its own (issue 167); the hook (`hook/coop_save.cpp`) decides and is told the folder. |
| `hook/` | The co-op code the patch builds into the emulator (`core.cmake` and `qt.cmake` say what), so changing it needs no new patch. How it runs and its settings: [docs/lichii.md](../docs/lichii.md). |

## Fetch and patch

```powershell
powershell -ExecutionPolicy Bypass -File lichii\fetch.ps1
```

This clones `fritchii-src` beside the main checkout of this repo (`-Dest <folder>` picks another place), checks out the pinned commit and puts our patches on a local branch `coop`. It refuses to run if the source has uncommitted changes. Running it again resets `coop` to the pin and applies the patches afresh; build folders and the download cache (`.cache/`) are ignored by the emulator's `.gitignore` and are kept.

## Build (Windows)

Tools:

- Visual Studio 2022 Build Tools with "Desktop development with C++" (MSVC 14.44 was used).
- CMake 3.24 or newer and Ninja on `PATH`.
- `glslangValidator`, for the emulator's shaders. The Vulkan SDK has it; without the SDK, unzip the [glslang release](https://github.com/KhronosGroup/glslang/releases) (`glslang-<version>-windows-x86_64-release.zip`), copy `bin\glslang.exe` to `bin\glslangValidator.exe` and put that `bin` on `PATH`.
- Qt 6.8.3 for `msvc2022_64` with the `qtcharts` module, the same as Fritchii's own Windows CI. The Qt the emulator downloads by itself (6.11.1) is built with a newer MSVC and does not link with Visual Studio 2022 (`__std_rotate` and other STL symbols missing). Without the Qt installer: `uv tool run --from aqtinstall==3.3.0 aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtcharts -O <folder>` (about 2 GB).
- Everything else (SDL3, FFmpeg, OpenSSL, ENet and the rest) is downloaded by the emulator's CMake into `fritchii-src\.cache\cpm` on the first configure.

From a "x64 Native Tools" prompt (or after `vcvarsall.bat x64`), in `fritchii-src`:

```bat
set _CL_=/wd4456 /wd4457 /wd4458 /wd4459
cmake -S . -B build/coop -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF -DYUZU_USE_BUNDLED_QT=OFF "-DCMAKE_PREFIX_PATH=<qt folder>/6.8.3/msvc2022_64" -DLICHII_COOP=ON "-DLICHII_COOP_DIR=C:/path/to/oot-remake-multiplayer" "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=C:/path/to/oot-remake-multiplayer/cmake/trim_paths.cmake"
cmake --build build/coop --target yuzu -j 8
```

The program is `build\coop\bin\lichii.exe`. Leave out the two `LICHII_COOP` options for the plain emulator.

`CMAKE_PROJECT_TOP_LEVEL_INCLUDES` brings in this repo's `cmake/trim_paths.cmake` before the emulator's first `project()`: every source path compiled into the program (the log prints them with each line) is cut down to the part below the source root (`src\core\core.cpp`), and the `.pdb` is named without its folder. Without it the program carries the folders it was built in, and `tools/release/make_kit.ps1` refuses it for a kit. It needs CMake 3.24 and a fresh configure (or a full rebuild).

`_CL_` is needed with MSVC (found at `fritchii-v4.1`; `fritchii-v5`, `fritchii-v5.5`, `fritchii-v5.6`, `fritchii-v5.7` and `fritchii-v5.8` keep the same flags in `src/CMakeLists.txt`, and was built with it): the emulator turns the four "declaration hides another" warnings (C4456 to C4459) into errors, and `src/common/frizti_settings.cpp`, `src/video_core/engines/kepler_compute.cpp`, `maxwell_3d.cpp` and `renderer_vulkan/vk_rasterizer.cpp` trip them. MSVC appends `_CL_` after every other flag, so this switches those four off without changing the emulator's source.

## What the patch touches

With `LICHII_COOP` off (the default) the patch changes nothing in the build.

| File | Change |
|---|---|
| `CMakeLists.txt` | The `LICHII_COOP` option and `LICHII_COOP_DIR`. Stops the configure when `ENABLE_UPDATE_CHECKER` is on as well (see "The emulator's own updater"). When on, adds this repo with `add_subdirectory` (no tests, no server, no test tool) before `src/`. |
| `src/core/CMakeLists.txt` | When on, includes `hook/core.cmake`: the hook and its settings built into `core`, linked with our `coop`, `coop_net`, `coop_host`, `coop_presence` and `coop_overlay`. |
| `src/yuzu/CMakeLists.txt` | When on, includes `hook/qt.cmake`: the co-op panel (`hook/coop_window.cpp`) built into the Qt frontend. |
| `src/yuzu/main_window.cpp` | Starts co-op and its panel once the render window exists (`CoopWindow::Attach`), and stops it when the window closes (`CoopHook::Shutdown`). |
| `src/core/hle/service/nx2_gsv.h` | In `Composite` (the Switch 2 present path, the one Ocarina uses), calls `CoopHook::OnFrame` after the speed limiter and the frame-time stats. |
| `src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp` | The same in `nvdisp_disp0::Composite` (the Switch 1 present path). |
| `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | (0002) After each draw's pipeline is configured, calls `CoopCamera::Track` (`hook/coop_camera_track.h`), which looks for the game's camera buffer as the DLSS path does, without DLSS. `video_core` sees the hook folder and `LICHII_COOP` because `core.cmake` makes both public on `core`, which `video_core` links. |
| `src/core/file_sys/patch_manager.cpp` | (0003) In `PatchNSO`, after the built-in patches, calls `CoopSvc::PatchMain`, which writes the puppet code cave into Ocarina's `main` (checked against the original words; only with `LICHII_COOP_PUPPET=1` or `LICHII_COOP_DEBUG_PUPPET=1`). |
| `src/core/hle/kernel/svc.cpp` | (0003) `Kernel::Svc::Call` asks `CoopSvc::Call` first: it handles `svc 0xf4` from the cave and returns false for every other call, which goes on as before. |
| `src/core/arm/dynarmic/arm_dynarmic_64.cpp` | (0004, only under `LICHII_COOP`) In `MemoryWrite`, a write to guest address 0 (an object built at a null pointer after a failed allocation) logs `Write to address 0 ... at pc ... lr ... sp ...; return addresses: ...` from the frame-pointer chain, the first 16 times in a run. The emulator's own `Unmapped Write64` line has no pc. The JIT syncs guest registers at block boundaries, so pc (and possibly lr and x29) can be the block's start or a little stale; the return addresses from the stack are the useful part. Only `MemoryWrite` is hooked: a 128-bit (q register) or exclusive store to address 0 is not logged. |
| `src/yuzu/main.cpp` | (0005) `LoadNx2Switches`: with a `user\` folder beside the exe (a portable kit) only the exe folder's `nx2_switches.txt` is read, then the switches are set and it returns before the `%APPDATA%` file. |

Our top-level `CMakeLists.txt` reuses the emulator's own `enet` target (the same ENet 1.3.18) when one exists, so there is one ENet in the program.

### Why there

`OnFrame` has to run once per emulated frame, in the emulator, not in the game. Both `Composite` functions run once for each frame the game presents, on the thread that handles the present, and they already end the frame for the emulator's own bookkeeping (speed limiting, `PerfStats::EndSystemFrame`). Calling the hook there ties it to the game's real frame rate (30 or 60), not to the host's display or the GPU thread, and it needs no game address. The renderer's `RendererFrameEndNotify` was not used: it runs on the GPU thread, after the frame has been drawn, which is later and on a thread that should not wait on co-op work.

`coop: loaded (ENet x.y.z)` in the emulator's log at start proves the co-op code is linked; `coop: on` or `coop: off` follows with the settings it found.

## A new Fritchii release

1. Change `PIN` to the new tag and commit.
2. Run `fetch.ps1`. If a patch no longer applies, apply it by hand on the new commit, commit, and replace that file with `git format-patch -1 --zero-commit --no-signature --stdout` of its commit.
3. Build with `LICHII_COOP=ON` and check the log for `coop: loaded`.

The patches stay at 11 files and about 110 lines; everything else is in `hook/`.

Moves so far: `fritchii-v5` to `fritchii-v5.5` (2026-11-09, 37 commits, mostly the Vulkan renderer, shaders and settings; all three patches applied as they were), then `fritchii-v5.6` (2026-11-10, 108 commits after v5.5), then `fritchii-v5.7` (2026-11-10, below).

For v5.6, 0001 and 0003 applied as they were, at new line numbers. 0002 needed one change: v5.6 no longer puts `TrackOcarinaCamera` inside `#ifdef EGLANTINE_STREAMLINE` in `RasterizerVulkan::PrepareDraw`, so our `CoopCamera::Track` call now sits just before it, with no `#ifdef EGLANTINE_STREAMLINE` around it. `nx2_gsv.h`, `patch_manager.cpp` and `main_window.cpp` changed a lot in v5.6, but the lines our calls sit beside are the same. `svc.cpp`, `nvdisp_disp0.cpp` and the kernel did not change.

What in v5.6 matters to co-op:
- **High refresh rate (Ctrl+U in OoT):** the game draws more than 60 frames a second at normal speed. `OnFrame` runs once per present (up to 2.75 times as often). Since issue 106 nothing on the present thread counts presents: scans, checks and logs run on the steady clock (`coop_every.h`), the hold flags' "no Link" debounce is 0.5 s, and `PLAYER_SAMPLE` is sent at most once per 14 ms (`LICHII_COOP_DEBUG_SAMPLES=1` logs samples sent and presents a second every 5 s). FlagWatcher and LoadMend count 1/60 s ticks of real time (`coop/host/ticks.hpp`). Per-game-frame budgets (`kMaxCallsPerFrame`, `kCutsPerFrame`) count the game's own frames and stay.
- **About 1 GB less RAM:** memory the game cleared to zero is given back to Windows. The hook reads guest memory through the emulator's own memory calls, so this should be invisible to it. Not checked in a game yet.
- **"Preparing shaders" runs once more** (about 15 minutes) on the first start of a v5.6 build, in every kit and every test folder. Autotest runs need a longer first timeout.
- **Mods that patch the same spot as Fritchii's built-in patches now win over them.** Our cave in `main` (0003) is written after the built-in patches, in `PatchNSO`, as before.
- Fritchii now offers its own update when it starts (v4 to v5.5 users). That is the updater below, and a co-op build does not have it.

Then `fritchii-v5.7` (2026-11-10). 0002, 0003 and 0004 applied as they were, at new line numbers. 0001 needed one change: v5.7 adds an `nx_tools` block (.nx archives) at the end of `src/yuzu/CMakeLists.txt`, right where our `qt.cmake` include sits; both blocks are kept, ours after it. The updater is still only built with `ENABLE_UPDATE_CHECKER`, so 0001's refusal still covers it.

What in v5.7 matters to co-op:
- **Its high refresh code (Ctrl+U) moved to main+0x3C46E00..0x3C46F10**, at the free end of the page, because the Hyrule Cheat Menu mod uses the spot after Dark's routine. Before (v5.6) it lived at main+0x3C46280, where our cave (0003, `CoopGameBuild` `svc_cave`) is, so turning Ctrl+U on while playing wrote over our cave. In v5.7 a live Ctrl+U writes only its call (main+0x6E6CC4), the water hook (main+0x475F8) and main+0x3C46E00.., none of which our code uses (our cave 0x3C46280..0x3C46300, the AI hold 0x3C46300.., its table 0x3C46400..0x3C46610). One thing stays as it was: when the setting is on at start, the built-in patch writes its cave before ours, and our check that main+0x3C46280..0x3C47000 is free fails, so "coop puppet" is skipped (the log says `main+0x3c46e00 is not free`). Keep Ctrl+U off when starting a co-op game; narrowing that check to the bytes we use is a possible follow-up.
- **Stretched or glowing geometry and white areas after area changes fixed** (the GPU kept showing freed memory that was reused). Should make long co-op sessions with many scene changes cleaner.
- **No out-of-memory crash on 16 GB PCs**, less VRAM (about 6 GB instead of up to 10 GB) and more free RAM: two emulators on one PC (the autotests) should fit better.
- **About 5 fps more in fights, less stutter after area changes, shorter hitches on loads and kills** (file reads no longer held to 40 MB/s): loads finish sooner, which shortens the window where our flags and objects wait for a scene.
- **.nx games** (Convert to NX in the game list): another way to keep the game. The puppet pack tool still needs the romfs (`.nup` or a dump folder), not an `.nx`.
- **"Preparing shaders" runs once after the update** (a few minutes; the driver keeps most of its cache). The kit's `Update.bat` moves the old shader cache aside whenever `lichii.exe` changes, so the first start after it prepares them anew.
- Fritchii's tuned settings are applied once more on the first start of v5.7 (its own `qt-config.ini` change; a kit's player settings in `emulator\user\config` are kept by the update).

Then `fritchii-v5.8` (2026-11-10). 0001-0004 applied as they were (refreshed at new line numbers). One new patch, **0005-coop-portable-switches**: Fritchii reads `nx2_switches.txt` beside the exe and then the one in `%APPDATA%\fritchii` (v5.8; `%APPDATA%\eden` before, and v5.7 did the same there). With a portable kit (`user\` beside `lichii.exe`) 0005 reads only the kit's own file, so a player's normal Fritchii install never changes a kit's switches.

What in v5.8 matters to co-op:
- **Its own data folder, `%APPDATA%\fritchii`, and a first-launch import** that copies settings, saves, keys and mods from `%APPDATA%\eden` or `%APPDATA%\lichii` (`src/frontend_common/profile_import.cpp`, `src/yuzu/user_data_migration.cpp`). A kit never runs it: `NeedsLegacyProfileImport()` returns false when `Common::FS::IsPortableMode()`, which is true when `user\` exists beside the exe (`src/common/fs/path_util.cpp`, the same check that makes the kit portable). In portable mode the data path is the kit's `user\`. Nothing else in the emulator reads `%APPDATA%`, apart from the switches file (0005) and a log line about the graphics driver's own cache folder (`%LOCALAPPDATA%` NVIDIA GLCache or AMD VkCache, sizes only). The import's state folder would sit beside the data folder (`user.migration`), but it is never made in portable mode.
- **Water ripples at any frame rate**: with Dark's Dynamic FPS on (the default) v5.8 also writes the water hook (main+0x475F8) and its routine at main+0x3C46E00..0x3C46F10, the spots Ctrl+U used in v5.7. Ours are main+0x3C46280..0x3C46610 and the calls at main+0x6E6DD8, 0x6E70F8 and 0x369DF8 (`CoopSites`), so nothing collides, with Ctrl+U on or off, and `ChangedSite` (no-mods) does not count Fritchii's own built-ins.
- **"Preparing shaders" only when a release changes how shaders are translated.** The kit's `Update.bat` still moves the old shader cache aside whenever `lichii.exe` changes (issue 117: a cache from another build crashed it), so the first start after an update prepares them anyway. Kept as it is: Fritchii's check is about its own translation, not about the co-op build.
- DLSS quality modes, faster texture uploads, the settings-during-load freeze fixed; Fritchii's tuned settings are applied once more on the first start.

Then `fritchii-v5.8.4` (2026-11-11). 0001-0005 applied as they were. Its 12 commits (settings revision 12, Saves backups, a reverted read-back experiment) do not touch the hook, the patch sites (Dynamic FPS main+0x3C46200, high refresh and water ripples main+0x3C46E00 and 0x475F8, all as in v5.8) or portable mode.

### The emulator's own updater

Frizti's release builds check for a new Fritchii when they start and can replace themselves (`frontend_common/update_checker.cpp`, `update_installer.h`, `yuzu/updater/`, the Help menu's "Check for Updates"). All of it is compiled only with `ENABLE_UPDATE_CHECKER`, which is off by default and which Frizti's release CI turns on. A co-op build must not have it: it would replace our patched `lichii.exe` with a plain Fritchii. So 0001 stops the configure with "LICHII_COOP needs ENABLE_UPDATE_CHECKER=OFF" when both are on, and `tools/release/make_kit.ps1` refuses an emulator that has the updater's text in it ("Check for Updates", "update-old"). Players update a kit with its own `Update.bat`.
