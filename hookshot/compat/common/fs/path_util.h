// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot/compat: the emulator's folders, as a Hookshot game folder has them.
// ConfigDir: the game folder (coop.ini beside longshot.exe). LoadDir:
// mods\contents, so LoadDir\0400486024DEC000\<mod>\romfs is a mod Hookshot
// loads (its host/mods.c takes that deeper layout too).
#pragma once
#include <filesystem>

#include "coop_runtime.h"

namespace Common::FS {
enum class EdenPath { ConfigDir, LoadDir };
inline std::filesystem::path GetEdenPath(EdenPath which) {
    const auto game = CoopRuntime::GameDir();
    return which == EdenPath::LoadDir ? game / "mods" / "contents" : game;
}
}  // namespace Common::FS
