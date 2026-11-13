# Third-party notices

The co-op code in this repository is GPL-3.0 ([LICENSE](LICENSE)). This file lists the code by others that it uses or that a release kit (`tools/release/make_kit.ps1` for Windows, `tools/release/make_kit.sh` for Linux, [below](#in-the-linux-kit)) ships, with each one's licence. In a kit, the licence texts are in the `licenses\` folder and `SOURCE.txt` says where the source of that exact build is.

No game files, keys or firmware are part of this repository or of any kit.

## In the repository

| Component | Used for | Licence | Source |
|---|---|---|---|
| ENet 1.3.18 (Lee Salzman) | Network transport (server, emulator hook) | MIT | https://github.com/lsalzman/enet, fetched by CMake at a pinned version |
| doctest | Unit tests only, not shipped | MIT | https://github.com/doctest/doctest, fetched by CMake at a pinned version |
| fmt 12.1.0 (Victor Zverovich and contributors) | The co-op hook's log lines in the Hookshot module (`hookshot_coop.dll`, `hookshot/compat/common/logging.h`) | MIT | https://github.com/fmtlib/fmt, fetched by CMake (`hookshot/CMakeLists.txt`) at a pinned commit |
| Longshot (Team MagicBean and contributors; the continuation of Hookshot by NotRexed and contributors) | `hookshot/patches/` changes it: the patches hold a few context lines of its files; they are applied to Longshot's source at the commit in `hookshot/PIN`, outside this repository | GPL-3.0 | https://git.magicbean.dev/MagicBeanDev/Longshot (history: https://github.com/NotRexed/Hookshot) |
| Monocypher 4.0.2 (Loup Vaillant, Michael Savage, Fabio Scotoni) | Traffic encryption: X25519, BLAKE2b, XChaCha20-Poly1305 (server, emulator hook, Hookshot module) | CC0-1.0 or BSD-2-Clause, the user's choice; we use it under CC0-1.0 | https://github.com/LoupVaillant/Monocypher, fetched by CMake at a pinned commit (cmake/monocypher.cmake) |

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
| This repository's Hookshot module and patches (only in kits with the Hookshot part) | `hookshot\hookshot_coop.dll` (with ENet, Monocypher and fmt built in), `hookshot\patches\`, `Build-Hookshot-Coop.bat`, `Host-Hookshot.bat`, `Join-Hookshot.bat` | GPL-3.0; the patches change, and hold a few context lines of, Longshot (Team MagicBean and contributors, continuing Hookshot by NotRexed and contributors; GPL-3.0, https://git.magicbean.dev/MagicBeanDev/Longshot) | `oot-remake-multiplayer-GPL-3.0.txt` |

## Downloaded on the player's PC by Build-Hookshot-Coop (not in the kit)

`Build-Hookshot-Coop.bat` (`scripts\hookshot-build.ps1`) downloads these into the kit's `hookshot\` folder on the player's own PC and builds the game there. None of them is in this repository or in a kit. The source is fetched at the commit in `hookshot/PIN` and the build tools named in the `tools.lock` of the Longshot release in `tools/release/kit/hookshot/TOOLS.txt`; each download is checked against the SHA-256 that release publishes. The game they build comes from the player's own files and is never shared.

| Component | Where it goes | Licence | Source |
|---|---|---|---|
| Longshot (Team MagicBean and contributors, continuing Hookshot by NotRexed and contributors), with this repository's patches | `hookshot\tools\` (source), then the built game in `hookshot\game\` | GPL-3.0 (its `LICENSE`); parts it vendors under their own licences, listed in its `third_party\*\` folders | https://git.magicbean.dev/MagicBeanDev/Longshot |
| Longshot Builder's tools (once Hookshot Builder's parts): Python with the packages the build uses, the CPM folder (Vulkan and SPIR-V headers, Vulkan Memory Allocator, FFmpeg), wgpu-native, and the prebuilt `third_party` files (a64-kit tools, SDL2, zbic, lld-link and others) | `hookshot\tools\python\`, `cpm\`, `wgpu-native\`, `third_party\` | Their own: PSF for Python and its packages' own licences (in each `*.dist-info`), Apache-2.0 / MIT for the Vulkan headers and wgpu-native, MIT for VMA, LGPL for FFmpeg, and the others as their folders say | Longshot's copies on git.magicbean.dev (the MagicBeanDev organization's packages), first made from the `builder-parts` release of https://github.com/NotRexed/Hookshot; listed in the Longshot release's `tools.lock` |
| llvm-mingw (Martin Storsjö), the compiler that builds the game | `hookshot\tools\llvm-mingw\` | LLVM: Apache-2.0 with LLVM exception; mingw-w64 runtime and headers: their own permissive licences (ZPL, MIT and others), in the package | https://github.com/mstorsjo/llvm-mingw |

## In the Linux kit

The Linux kit (`tools/release/make_kit.sh`) holds the same co-op code, server and puppet pack tool as the Windows kit, with the emulator as one AppImage. Its `licenses/` folder has the same texts, and `licenses/appimage/` has `BUNDLED.txt`, the list of every library and plugin inside the AppImage with where each came from, and the licence text (`<package>.copyright`) of each one (written by `tools/release/appimage_licenses.sh` on the machine that built it).

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
