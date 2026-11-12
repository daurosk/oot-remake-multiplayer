# Third-party notices

The co-op code in this repository is GPL-3.0 ([LICENSE](LICENSE)). This file lists the code by others that it uses or that a release kit (`tools/release/make_kit.ps1`) ships, with each one's licence. In a kit, the licence texts are in the `licenses\` folder and `SOURCE.txt` says where the source of that exact build is.

No game files, keys or firmware are part of this repository or of any kit.

## In the repository

| Component | Used for | Licence | Source |
|---|---|---|---|
| ENet 1.3.18 (Lee Salzman) | Network transport (server, emulator hook) | MIT | https://github.com/lsalzman/enet, fetched by CMake at a pinned version |
| doctest | Unit tests only, not shipped | MIT | https://github.com/doctest/doctest, fetched by CMake at a pinned version |
| fmt 12.1.0 (Victor Zverovich and contributors) | The co-op hook's log lines in the Hookshot module (`hookshot_coop.dll`, `hookshot/compat/common/logging.h`) | MIT | https://github.com/fmtlib/fmt, fetched by CMake (`hookshot/CMakeLists.txt`) at a pinned commit |
| Hookshot (NotRexed and contributors) | `hookshot/patches/` changes it: the patch holds a few context lines of its `nx2/lift.py`; it is applied to Hookshot's source at the commit in `hookshot/PIN`, outside this repository | GPL-3.0 | https://github.com/NotRexed/Hookshot |

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
| Python (only in kits made with `-Python`) | `python\` | PSF License | `python\LICENSE.txt` |
| python-zstandard (only with `-Python`) | `python\Lib\site-packages\zstandard\` | BSD-3-Clause | its `zstandard-*.dist-info` folder |
| This repository's Hookshot module and patches (only in kits with the Hookshot part) | `hookshot\hookshot_coop.dll` (with ENet and fmt built in), `hookshot\patches\`, `Build-Hookshot-Coop.bat`, `Host-Hookshot.bat`, `Join-Hookshot.bat` | GPL-3.0; the patches change, and hold a few context lines of, Hookshot (NotRexed and contributors, GPL-3.0, https://github.com/NotRexed/Hookshot) | `oot-remake-multiplayer-GPL-3.0.txt` |

## Downloaded on the player's PC by Build-Hookshot-Coop (not in the kit)

`Build-Hookshot-Coop.bat` (`scripts\hookshot-build.ps1`) downloads these into the kit's `hookshot\` folder on the player's own PC and builds the game there. None of them is in this repository or in a kit. The source is fetched at the commit in `hookshot/PIN` and the build tools from the Hookshot release in `tools/release/kit/hookshot/TOOLS.txt`; each download is checked against the SHA-256 that release publishes. The game they build comes from the player's own files and is never shared.

| Component | Where it goes | Licence | Source |
|---|---|---|---|
| Hookshot (NotRexed and contributors), with this repository's patches | `hookshot\tools\` (source), then the built game in `hookshot\game\` | GPL-3.0 (its `LICENSE`); parts it vendors under their own licences, listed in its `third_party\*\` folders | https://github.com/NotRexed/Hookshot |
| Hookshot Builder's tool parts: Python with the packages the build uses, the CPM folder (Vulkan and SPIR-V headers, Vulkan Memory Allocator, FFmpeg), wgpu-native, and the prebuilt `third_party` files (a64-kit tools, SDL2, zbic, lld-link and others) | `hookshot\tools\python\`, `cpm\`, `wgpu-native\`, `third_party\`, `work\` | Their own: PSF for Python and its packages' own licences (in each `*.dist-info`), Apache-2.0 / MIT for the Vulkan headers and wgpu-native, MIT for VMA, LGPL for FFmpeg, and the others as their folders say | The `builder-parts` release of https://github.com/NotRexed/Hookshot (listed in that release's `hookshot-setup.txt`) |
| llvm-mingw (Martin Storsjö), the compiler that builds the game | `hookshot\tools\llvm-mingw\` | LLVM: Apache-2.0 with LLVM exception; mingw-w64 runtime and headers: their own permissive licences (ZPL, MIT and others), in the package | https://github.com/mstorsjo/llvm-mingw |
