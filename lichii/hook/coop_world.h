// SPDX-License-Identifier: GPL-3.0-or-later
//
// World objects shared like enemies (docs/world.md): the breakables. Pots,
// crates, liftable and breakable rocks, grass, signs, cracked walls and the
// rest have the same Life and Death components as enemies (their packs'
// LifeRef, DeathRef and DestructionRef), so the enemy sharing in
// coop_objects_sync.cpp carries them: one owner, a break on a copy goes to
// the owner as DAMAGE, and the owner's life 0 reaches every copy, where the
// game's own death breaks it. No emulator code.

#pragma once

#include <string_view>

namespace CoopWorld {

// True for the breakable world objects (by actor name).
bool IsBreakable(std::string_view name);

} // namespace CoopWorld
