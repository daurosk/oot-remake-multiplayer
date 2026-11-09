# lichii/

Co-op inside Frizti's emulator (Lichii, now [Fritchii](https://github.com/Frizti/Fritchii), GPL-3.0). The emulator's source is not in this repo and not forked on GitHub: `fetch.ps1` clones it at the pinned commit next to this repo and applies our patches as local commits. Everything we add stays small so a new Fritchii release is easy to take.

| File | What |
|---|---|
| `PIN` | Repo URL, release tag and commit we build against. |
| `fetch.ps1` | Clones or updates the source to the pin and applies `patches/*.patch` in order. |
| `patches/0001-coop-hook.patch` | The hook points, nothing else. |
| `patches/0002-coop-camera.patch` | One call per draw in the Vulkan renderer that hands the game's camera to the hook, for markers. |
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
cmake -S . -B build/coop -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF -DYUZU_USE_BUNDLED_QT=OFF "-DCMAKE_PREFIX_PATH=<qt folder>/6.8.3/msvc2022_64" -DLICHII_COOP=ON "-DLICHII_COOP_DIR=C:/path/to/oot-remake-multiplayer"
cmake --build build/coop --target yuzu -j 8
```

The program is `build\coop\bin\lichii.exe`. Leave out the two `LICHII_COOP` options for the plain emulator.

`_CL_` is needed with MSVC at `fritchii-v4.1`: the emulator turns the four "declaration hides another" warnings (C4456 to C4459) into errors, and `src/common/frizti_settings.cpp`, `src/video_core/engines/kepler_compute.cpp`, `maxwell_3d.cpp` and `renderer_vulkan/vk_rasterizer.cpp` trip them. MSVC appends `_CL_` after every other flag, so this switches those four off without changing the emulator's source.

## What the patch touches

With `LICHII_COOP` off (the default) the patch changes nothing in the build.

| File | Change |
|---|---|
| `CMakeLists.txt` | The `LICHII_COOP` option and `LICHII_COOP_DIR`. When on, adds this repo with `add_subdirectory` (no tests, no server, no test tool) before `src/`. |
| `src/core/CMakeLists.txt` | When on, includes `hook/core.cmake`: the hook and its settings built into `core`, linked with our `coop`, `coop_net`, `coop_host`, `coop_presence` and `coop_overlay`. |
| `src/yuzu/CMakeLists.txt` | When on, includes `hook/qt.cmake`: the co-op panel (`hook/coop_window.cpp`) built into the Qt frontend. |
| `src/yuzu/main_window.cpp` | Starts co-op and its panel once the render window exists (`CoopWindow::Attach`), and stops it when the window closes (`CoopHook::Shutdown`). |
| `src/core/hle/service/nx2_gsv.h` | In `Composite` (the Switch 2 present path, the one Ocarina uses), calls `CoopHook::OnFrame` after the speed limiter and the frame-time stats. |
| `src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp` | The same in `nvdisp_disp0::Composite` (the Switch 1 present path). |
| `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | (0002) After each draw's pipeline is configured, calls `CoopCamera::Track` (`hook/coop_camera_track.h`), which looks for the game's camera buffer as the DLSS path does, without DLSS. `video_core` sees the hook folder and `LICHII_COOP` because `core.cmake` makes both public on `core`, which `video_core` links. |

Our top-level `CMakeLists.txt` reuses the emulator's own `enet` target (the same ENet 1.3.18) when one exists, so there is one ENet in the program.

### Why there

`OnFrame` has to run once per emulated frame, in the emulator, not in the game. Both `Composite` functions run once for each frame the game presents, on the thread that handles the present, and they already end the frame for the emulator's own bookkeeping (speed limiting, `PerfStats::EndSystemFrame`). Calling the hook there ties it to the game's real frame rate (30 or 60), not to the host's display or the GPU thread, and it needs no game address. The renderer's `RendererFrameEndNotify` was not used: it runs on the GPU thread, after the frame has been drawn, which is later and on a thread that should not wait on co-op work.

`coop: loaded (ENet x.y.z)` in the emulator's log at start proves the co-op code is linked; `coop: on` or `coop: off` follows with the settings it found.

## A new Fritchii release

1. Change `PIN` to the new tag and commit.
2. Run `fetch.ps1`. If a patch no longer applies, apply it by hand on the new commit, commit, and replace that file with `git format-patch -1 --zero-commit --no-signature --stdout` of its commit.
3. Build with `LICHII_COOP=ON` and check the log for `coop: loaded`.

The patches stay at 7 files and about 50 lines; everything else is in `hook/`.
