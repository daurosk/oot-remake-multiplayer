// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chests outside the dungeons (docs/items.md, "Chests"). A chest's opened
// state is the 64-bit key of its placement; the dungeons' chests are shared
// by coop_dungeon, these by this list. Opened in one game, the key is written
// in every game (one way: it never goes back to 0), and a game that has the
// chest loaded removes it (FlagWatcher::take_written_for_others,
// CoopObjects::TakenForOthers): it cannot be opened a second time, and it is
// open when the room loads again. LICHII_COOP_CHESTS=0 turns this list off.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "coop/host/flags.hpp"

namespace CoopChests {

struct Key {
    std::uint64_t key;
    const char* area;   // the Banc folder
    const char* actor;  // the placement's Gyaml
};

// The chest keys outside the dungeons, sorted by area, actor and key.
std::span<const Key> Keys();

// Adds the keys to `shared` (made when empty). Returns how many were new.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// Merge with On(), logged.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

// False when LICHII_COOP_CHESTS=0.
bool On();

} // namespace CoopChests
