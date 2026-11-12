# Third-party notices

The co-op code in this repository is GPL-3.0 ([LICENSE](LICENSE)). This file lists the code by others that it uses or that a release kit (`tools/release/make_kit.ps1`) ships, with each one's licence. In a kit, the licence texts are in the `licenses\` folder and `SOURCE.txt` says where the source of that exact build is.

No game files, keys or firmware are part of this repository or of any kit.

## In the repository

| Component | Used for | Licence | Source |
|---|---|---|---|
| ENet 1.3.18 (Lee Salzman) | Network transport (server, emulator hook) | MIT | https://github.com/lsalzman/enet, fetched by CMake at a pinned version |
| doctest | Unit tests only, not shipped | MIT | https://github.com/doctest/doctest, fetched by CMake at a pinned version |
| Monocypher 4.0.2 (Loup Vaillant, Michael Savage, Fabio Scotoni) | Traffic encryption: X25519, BLAKE2b, XChaCha20-Poly1305 (server, emulator hook) | CC0-1.0 or BSD-2-Clause, the user's choice; we use it under CC0-1.0 | https://github.com/LoupVaillant/Monocypher, fetched by CMake at a pinned commit (cmake/monocypher.cmake) |

Code copied in from other projects gets an entry here, with its copyright line and licence text, when it is added.

## In a release kit

| Component | Where in the kit | Licence | Text in `licenses\` |
|---|---|---|---|
| This repository's co-op hook, server, scripts and puppet pack tool | `emulator\lichii.exe` (the hook), `server\`, `scripts\`, `tools\puppet_pack\` | GPL-3.0 | `oot-remake-multiplayer-GPL-3.0.txt` |
| Fritchii (Frizti), based on Eden, yuzu and citra, with this repository's patch | `emulator\` | GPL-3.0-or-later; its parts under the licences in its `LICENSES` folder | `Fritchii-GPL-3.0.txt`, `SPDX-*.txt` |
| Libraries built into the emulator by Fritchii's build (Boost, fmt, SDL3, FFmpeg, OpenSSL, cubeb, libusb, zstd, lz4, zlib, opus, Vulkan headers and memory allocator, sirit, xbyak and others) | `emulator\lichii.exe` | Their own (BSL-1.0, MIT, BSD, Zlib, Apache-2.0, LGPL and others), as listed in the Fritchii source | `SPDX-*.txt` |
| Qt 6.8.3 (The Qt Company) | `emulator\Qt6*.dll`, `emulator\plugins\` | LGPL-3.0 | `SPDX-LGPL-3.0-or-later.txt`, `SPDX-GPL-3.0-or-later.txt`; source at https://download.qt.io/archive/qt/6.8/6.8.3/ |
| DirectX Shader Compiler (Microsoft) | `emulator\dxcompiler.dll`, `emulator\dxil.dll` | dxcompiler: University of Illinois/NCSA; dxil: Microsoft redistributable | https://github.com/microsoft/DirectXShaderCompiler |
| ENet 1.3.18 | `server\coop_server.exe`, `emulator\lichii.exe` | MIT | `ENet-MIT.txt` |
| Monocypher 4.0.2 | `server\coop_server.exe`, `emulator\lichii.exe` | CC0-1.0 (used under it; also offered under BSD-2-Clause) | none needed under CC0-1.0; source at https://github.com/LoupVaillant/Monocypher |
| Microsoft Visual C++ runtime | `emulator\*140*.dll`, `server\*140*.dll` | Microsoft Visual C++ redistributable licence | (redistributable files) |
| Python (only in kits made with `-Python`) | `python\` | PSF License | `python\LICENSE.txt` |
| python-zstandard (only with `-Python`) | `python\Lib\site-packages\zstandard\` | BSD-3-Clause | its `zstandard-*.dist-info` folder |
