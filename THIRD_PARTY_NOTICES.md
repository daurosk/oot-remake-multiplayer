# Third-party notices

The co-op code in this repository is GPL-3.0 ([LICENSE](LICENSE)). This file lists the code by others that it uses or that a release kit (`tools/release/make_kit_hookshot.ps1` for Windows, `tools/release/make_kit_linux.sh` for Linux) ships. **From 0.11.12 no kit ships the Fritchii emulator** (Fritchii, Qt, its libraries, the AppImage): the emulator rows below describe the older emulator kits only, with each one's licence. In a kit, the licence texts are in the `licenses\` folder and `SOURCE.txt` says where the source of that exact build is.

No game files, keys or firmware are part of this repository or of any kit.

## In the repository

| Component | Used for | Licence | Source |
|---|---|---|---|
| ENet 1.3.18 (Lee Salzman) | Network transport (server, emulator hook) | MIT | https://github.com/lsalzman/enet, fetched by CMake at a pinned version |
| doctest | Unit tests only, not shipped | MIT | https://github.com/doctest/doctest, fetched by CMake at a pinned version |
| fmt 12.1.0 (Victor Zverovich and contributors) | The co-op hook's log lines in the Hookshot module (`hookshot_coop.dll`, `hookshot/compat/common/logging.h`) | MIT | https://github.com/fmtlib/fmt, fetched by CMake (`hookshot/CMakeLists.txt`) at a pinned commit |
| Longshot (Team MagicBean and contributors; its changes are being merged into Hookshot by NotRexed and contributors) | `hookshot/patches/` changes it: the patches hold a few context lines of its files; they are applied to Longshot's source at the commit in `hookshot/PIN`, outside this repository. `tools/hookshot/menu_pads_behind_test.py` is adapted from its `tests/test_pads_after_menu.py` (patch 0039's test, run on the patched source) | GPL-3.0 | https://git.magicbean.dev/MagicBeanDev/Longshot (Hookshot: https://git.notrexed.dev/Hookshot/Hookshot) |
| Longshot's ExeFS patch builder (`nx2/exefs_patches.py`: HumanBean's draft as merged in Longshot 3653c9c5, #134; put onto v3.0.0 by snaildev99) | `hookshot/patches/0034-mods-exefs-patches-at-build.patch` (and 0035 on top): mods' code patches written into the game's code when it is built. Its parse of the three patch formats follows valentderah/totk-pc-recomp `tools/exefs_patches.py`, "Copyright (c) 2026 Valiantsin Dzerakh", MIT; the MIT text is in the file's own head inside the patch | GPL-3.0 (as Longshot), with that part MIT | https://git.magicbean.dev/MagicBeanDev/Longshot (commit 3653c9c5), https://github.com/valentderah/totk-pc-recomp |
| OoT Character Select v0.2.1 (Copyright (c) 2026 snaildev99), its page, picker, Customize and four-tab header builder | Co-op's own Characters tab, Link only (`tools/puppet_pack/charsel/`): `make_data.py` runs the mod's `mods/build.py` (header_four_tabs, characters_page, character_models, picker_page) and `mods/customize.py` (the Customize screen and its camera glide) as a library for Link alone; the code patch it makes (`charsel-native.ips`) is that code's output. Character tab and Customize layout: Snail (snaildev99), OoT Character Select, used with his permission. No file of the game is in this repository | MIT (text below) | https://github.com/snaildev99/OoT-Character-Select (tag v0.2.1) |
| OoT Character Select v0.2.1 (Copyright (c) 2026 snaildev99), its sequence helpers and its page's recipe | Co-op's own - menu screen CoopMenu_00 (`tools/puppet_pack/coopmenu/make_data.py`, `tools/puppet_pack/coop_menu.py`): `make_data.py` runs the mod's `mods/build.py` sequence helpers (`Graph`, `vanilla`, the `t_*` nodes, `_plugs`, `TRIG`) as a library to edit the Map page's sequence, as his `characters_page` edits Key Items'; `coop_menu.py` follows his page's recipe (a copy of Key Items' layout under the page's name, four copies in the layout archive, the layout category, the screens table row, a copy of Key Items' messages). No file of the game is in this repository (`coopmenu/data/map.cdelta` holds copy and insert operations against the player's own file) | MIT (text below) | https://github.com/snaildev99/OoT-Character-Select (tag v0.2.1) |
| OoT Character Select (Copyright (c) 2026 snaildev99), `tools/bwav.py` | `tools/puppet_pack/bwav.py`: the BARS archive and BWAV clip layouts, the DSP ADPCM decoder, the encoder (`fit_coefs`, `encode_adpcm`) and `bars_build`, adapted (bytes in and out, clips of any length). It reads and writes the player's own sound files on their PC; no sound is in this repository | MIT (text below) | https://github.com/snaildev99/OoT-Character-Select |
| OoT Character Select v0.2.1 (Copyright (c) 2026 snaildev99), its colour tables | The Customize colours on puppets (`lichii/hook/coop_char_colours.cpp`): the numbers of `HAIR_COLOURS`, `brow_colour` of `HAIR_TEXTURE` and `EYE_COLOURS` in its `mods/customize.py`, rounded to 255ths as the mod stores them; the material and parameter names the mod's palettes use. The code is ours; nothing of the mod's code patch or of its game-derived files is in this repository | MIT (text below) | https://github.com/snaildev99/OoT-Character-Select (tag v0.2.1) |
| OoT Character Select (Copyright (c) 2026 snaildev99), its hair recolour and BC7 block reader (commit 785d8d0d) | Link's hair in the player's colour (`hookshot/src/coop_look_paint.cpp`, `HairRecolour` and `Bc7Map`): the hair rule of `hair_fn` in `mods/customize.py` and the BC7 end-point read and write of `bc7_blocks` in `tools/texrecolor.py`, adapted to work on the end colours of each block in C++. It paints the player's own hair texture on their PC; no file of the game is in this repository | MIT (text below) | https://github.com/snaildev99/OoT-Character-Select (commit 785d8d0d) |
| CityHash64 1.1 (Copyright (c) 2011 Google, Inc.) | The game's pane ids for the pack's copied UI parts (`tools/puppet_pack/ui_pack.py` `city_hash64`, written in Python from city.cc) | MIT | https://github.com/google/cityhash |
| Monocypher 4.0.2 (Loup Vaillant, Michael Savage, Fabio Scotoni) | Traffic encryption: X25519, BLAKE2b, XChaCha20-Poly1305 (server, emulator hook, Hookshot module); its optional Ed25519 + SHA-512 (the official public game's signed mark) | CC0-1.0 or BSD-2-Clause, the user's choice; we use it under CC0-1.0 | https://github.com/LoupVaillant/Monocypher, fetched by CMake at a pinned commit (cmake/monocypher.cmake) |

Code copied in from other projects gets an entry here, with its copyright line and licence text, when it is added.

## In a release kit

| Component | Where in the kit | Licence | Text in `licenses\` |
|---|---|---|---|
| This repository's co-op hook, server, scripts and puppet pack tool | `emulator\lichii.exe` (the hook), `server\`, `scripts\`, `tools\puppet_pack\` | GPL-3.0 | `oot-remake-multiplayer-GPL-3.0.txt` |
| Fritchii (Frizti), based on Eden, yuzu and citra, with this repository's patch | `emulator\` | GPL-3.0-or-later; its parts under the licences in its `LICENSES` folder | `Fritchii-GPL-3.0.txt`, `SPDX-*.txt` |
| Libraries built into the emulator by Fritchii's build (Boost, fmt, SDL3, FFmpeg, OpenSSL, cubeb, libusb, zstd, lz4, zlib, opus, Vulkan headers and memory allocator, sirit, xbyak and others) | `emulator\lichii.exe` | Their own (BSL-1.0, MIT, BSD, Zlib, Apache-2.0, LGPL and others), as listed in the Fritchii source | `SPDX-*.txt` |
| Qt 6.8.3 (The Qt Company) | `emulator\Qt6*.dll`, `emulator\plugins\` | LGPL-3.0 | `SPDX-LGPL-3.0-or-later.txt`, `SPDX-GPL-3.0-or-later.txt`; source at https://download.qt.io/archive/qt/6.8/6.8.3/ |
| DirectX Shader Compiler (Microsoft) | `emulator\dxcompiler.dll`, `emulator\dxil.dll` | dxcompiler: University of Illinois/NCSA; dxil: Microsoft redistributable | https://github.com/microsoft/DirectXShaderCompiler |
| ENet 1.3.18 | `server\coop_server.exe`, `emulator\lichii.exe`, `hookshot\hookshot_coop.dll` | MIT | `ENet-MIT.txt` |
| fmt 12.1.0 (Victor Zverovich and contributors) (only in kits with the Hookshot part) | `hookshot\hookshot_coop.dll` | MIT | `fmt-MIT.txt` |
| Microsoft Visual C++ runtime | `emulator\*140*.dll`, `server\*140*.dll`, `hookshot\*140*.dll` | Microsoft Visual C++ redistributable licence | (redistributable files) |
| Monocypher 4.0.2 | `server\coop_server.exe`, `emulator\lichii.exe`, `hookshot\hookshot_coop.dll` | CC0-1.0 (used under it; also offered under BSD-2-Clause) | none needed under CC0-1.0; source at https://github.com/LoupVaillant/Monocypher |
| Python (only in kits made with `-Python`) | `python\` | PSF License | `python\LICENSE.txt` |
| python-zstandard (only with `-Python`) | `python\Lib\site-packages\zstandard\` | BSD-3-Clause | its `zstandard-*.dist-info` folder |
| This repository's Hookshot module and patches (only in kits with the Hookshot part) | `hookshot\hookshot_coop.dll` (with ENet, Monocypher and fmt built in), `hookshot\patches\`, `Start Squadala.bat` and `scripts\` (the Longshot kit; `Build-Hookshot-Coop.bat`, `Host-Hookshot.bat`, `Join-Hookshot.bat` in the older emulator kits) | GPL-3.0; the patches change, and hold a few context lines of, Longshot (Team MagicBean and contributors, GPL-3.0; its changes are being merged into Hookshot by NotRexed and contributors; https://git.magicbean.dev/MagicBeanDev/Longshot) | `oot-remake-multiplayer-GPL-3.0.txt` |

## Downloaded on the player's PC by the kit's game build (not in the kit)

The kit's game build (`scripts\hookshot-build.ps1`, run by `Start Squadala.bat`; `Build-Hookshot-Coop.bat` in the older emulator kits; `scripts/build.sh` in the Linux kit) downloads these into the kit's `hookshot\` folder on the player's own PC and builds the game there. None of them is in this repository or in a kit. The source is fetched at the commit in `hookshot/PIN` and the build tools named in the `tools.lock` of the Longshot release in `tools/release/kit/hookshot/TOOLS.txt`; each download is checked against the SHA-256 that release publishes. The game they build comes from the player's own files and is never shared.

| Component | Where it goes | Licence | Source |
|---|---|---|---|
| Longshot (Team MagicBean and contributors), with this repository's patches | `hookshot\tools\` (source), then the built game in `hookshot\game\` | GPL-3.0 (its `LICENSE`); parts it vendors under their own licences, listed in its `third_party\*\` folders | https://git.magicbean.dev/MagicBeanDev/Longshot |
| Longshot Builder's tools (once Hookshot Builder's parts): Python with the packages the build uses, the CPM folder (Vulkan and SPIR-V headers, Vulkan Memory Allocator, FFmpeg) and the prebuilt `third_party` files (a64-kit tools, SDL2, zbic, lld-link and others) | `hookshot\tools\python\`, `cpm\`, `third_party\` | Their own: PSF for Python and its packages' own licences (in each `*.dist-info`), Apache-2.0 / MIT for the Vulkan headers, MIT for VMA, LGPL for FFmpeg, and the others as their folders say | Longshot's copies on git.magicbean.dev (the MagicBeanDev organization's packages), first made from the `builder-parts` release of Hookshot (NotRexed and contributors, then on GitHub, now https://git.notrexed.dev/Hookshot/Hookshot); listed in the Longshot release's `tools.lock` (Longshot rc.4 and older also wgpu-native, for the WebGPU renderer rc.5 dropped) |
| NVIDIA DLSS SDK (NGX), only in a kit whose player opted in (`NVIDIA DLSS.bat` / `--dlss`) and typed yes to NVIDIA's license, which the kit shows first. Fetched by Longshot's own `scripts/ngx_sdk.py` at its pinned NVIDIA commit, every file SHA-256 checked; a copy the Longshot Builder or Launcher already fetched on that PC (or `-DlssSdk`) is copied in first, so only files that differ are downloaded. Never in this repository, a kit, a release zip or CI (the license allows no redistribution as a stand-alone SDK and no use that puts it under an open-source license: it is combined with the game only on the player's own PC, and the kit tells them never to share that game folder) | `hookshot\work\ngx-sdk\` (Linux `data/work/ngx-sdk/`); its runtimes `nvngx_dlss.dll`, `nvngx_dlssg.dll` in the game's `ngx\` | NVIDIA RTX SDKs license (proprietary; its `LICENSE.txt` is saved beside the files) | https://github.com/NVIDIA/DLSS |
| AMD FSR 4 runtime (FSR SDK v2.4.0, FSR 4.1.2: `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`), only in a kit whose player opted in (`AMD FSR 4.bat`, #326) and typed yes to AMD's license, which the kit shows first. Fetched by Longshot's own `scripts/fetch.py fsr` (`make_game.py --fsr --fetch-fsr-runtime`) at its pinned AMD commit, every file SHA-256 checked. Never in this repository, a kit, a release zip or CI. Windows only: the Linux kit fetches nothing (`--fsr4` builds the renderer with FSR 4; the player puts AMD's library beside the game) | `hookshot\work\ffx-sdk\`; the two DLLs and `LICENSE-AMD-FSR-SDK.md` in the game folder | AMD FSR SDK license (proprietary; saved as `LICENSE-AMD-FSR-SDK.md` beside the files) | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| Intel XeSS library (XeSS 2.0.2.68: `libxess.dll`), only in a kit whose player opted in (`Intel XeSS.bat`, #326) and typed yes to Intel's license, which the kit shows first. Fetched by Longshot's own `scripts/fetch.py xess` at its pinned Intel commit, every file SHA-256 checked. Never in this repository, a kit, a release zip or CI. Windows only | `hookshot\work\xess-sdk\`; `libxess.dll`, `LICENSE-INTEL-XESS-SDK.txt` and `THIRD-PARTY-INTEL-XESS-SDK.txt` beside longshot.exe | Intel Simplified Software License (proprietary; saved as `LICENSE-INTEL-XESS-SDK.txt` beside the file) | https://github.com/intel/xess |
| llvm-mingw (Martin Storsjö), the compiler that builds the game | `hookshot\tools\llvm-mingw\` | LLVM: Apache-2.0 with LLVM exception; mingw-w64 runtime and headers: their own permissive licences (ZPL, MIT and others), in the package | https://github.com/mstorsjo/llvm-mingw |
| OOTRR, the randomizer (REALAZK), the release in `tools/release/kit/hookshot/OOTRR.txt` (0.8.2): its Windows portable zip, SHA-256 checked against that release's `SHA256SUMS.txt`. Linux kit: its `linux-x86_64.tar.gz` the same way, into `data/game/tools/ootrr/` by `scripts/build.sh` | `hookshot\game\tools\ootrr\` (`ootrr.exe`, with its own `LICENSE` and `NOTICE` beside it) | GPL-3.0-or-later since 0.5.0 (releases up to 0.4.0: MIT); its `NOTICE` credits the OoT Randomizer logic it ports (MIT) and its fonts | https://github.com/ootrando/ootrr (REALAZK/ootrr redirects there; the source of each release is its tag) |

## In the Linux kit

The older Linux emulator kit (made by `make_kit.sh`, retired and removed in 0.11.12) held the same co-op code, server and puppet pack tool as the Windows kit, with the emulator as one AppImage. Its `licenses/` folder has the same texts, and `licenses/appimage/` has `BUNDLED.txt`, the list of every library and plugin inside the AppImage with where each came from, and the licence text (`<package>.copyright`) of each one (written by `tools/release/appimage_licenses.sh` on the machine that built it).

| Component | Where in the kit | Licence | Text |
|---|---|---|---|
| This repository's co-op hook, server, scripts and puppet pack tool | `emulator/lichii.AppImage` (the hook), `server/coop_server`, `*.sh`, `scripts/`, `tools/puppet_pack/` | GPL-3.0 | `licenses/oot-remake-multiplayer-GPL-3.0.txt` |
| Fritchii (Frizti), based on Eden, yuzu and citra, with this repository's patch, and the libraries its build puts into the program (as in the Windows kit) | `emulator/lichii.AppImage` (`usr/bin/lichii`) | GPL-3.0-or-later; its parts under the licences in its `LICENSES` folder | `licenses/Fritchii-GPL-3.0.txt`, `licenses/SPDX-*.txt` |
| Qt 6.8.3 (The Qt Company): its libraries, its plugins (platforms such as xcb and wayland, image formats, styles, TLS) and the ICU it ships | inside `emulator/lichii.AppImage` (`usr/lib/libQt6*`, `usr/plugins/`) | LGPL-3.0 (ICU: Unicode-3.0) | `licenses/SPDX-LGPL-3.0-or-later.txt`, `licenses/appimage/ICU-Unicode-3.0.txt`, `licenses/SPDX-GPL-3.0-or-later.txt`; source at https://download.qt.io/archive/qt/6.8/6.8.3/ |
| System libraries the emulator and Qt need that a Linux system may lack (from Ubuntu 24.04 packages: for example Boost context and fiber, FFmpeg, Opus, libusb, libva, libvdpau, the XCB and xkbcommon libraries) | inside `emulator/lichii.AppImage` (`usr/lib/`) | Their own (BSL-1.0, LGPL-2.1, BSD, MIT and others); `BUNDLED.txt` names each file's package | `licenses/appimage/<package>.copyright`; source of each Ubuntu package at https://launchpad.net/ubuntu/+source/<package> |
| The AppImage start program (AppRun) and the type 2 AppImage runtime, from linuxdeploy | `emulator/lichii.AppImage` | MIT | https://github.com/linuxdeploy/linuxdeploy, https://github.com/AppImage/type2-runtime |
| ENet 1.3.18 | `server/coop_server`, `emulator/lichii.AppImage` | MIT | `licenses/ENet-MIT.txt` |
| Monocypher 4.0.2 | `server/coop_server`, `emulator/lichii.AppImage` | CC0-1.0 (used under it; also offered under BSD-2-Clause) | none needed under CC0-1.0; source at https://github.com/LoupVaillant/Monocypher |

The C library, graphics drivers and other libraries every desktop Linux has (glibc, libGL and Vulkan loaders, X11, Wayland, fontconfig and others on linuxdeploy's exclude list) are not in the AppImage: the player's own system provides them.

## Tools that build the Linux kit (not shipped as programs)

| Tool | Used for | Licence | Source |
|---|---|---|---|
| linuxdeploy 1-alpha-20251107-1 | Makes the AppImage (`lichii/build-linux.sh --appimage`; the download is checked against a SHA-256 kept in the script) | MIT | https://github.com/linuxdeploy/linuxdeploy |
| linuxdeploy-plugin-qt 1-alpha-20250213-1 | Puts Qt and its plugins into the AppImage (also SHA-256 checked) | MIT | https://github.com/linuxdeploy/linuxdeploy-plugin-qt |
| Qt 6.8.3 for Linux, installed by `jurplel/install-qt-action` (aqtinstall) | The Qt the emulator is built with and that goes into the AppImage | LGPL-3.0 (the action and aqtinstall: MIT) | https://download.qt.io/archive/qt/6.8/6.8.3/, https://github.com/jurplel/install-qt-action |

### OoT Character Select (MIT)

```
MIT License

Copyright (c) 2026 snaildev99

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
