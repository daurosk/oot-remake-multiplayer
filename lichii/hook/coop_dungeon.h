// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dungeons the same for every player (docs/dungeon.md). A switch pressed, a
// room cleared, a door opened or the Water Temple's water level changed by
// one player is a named GameData flag the dungeon's room logic writes and
// polls; sharing those flags live makes the other games follow. This file
// holds the list (read from the game's logic graphs and event flows) and adds
// it to the session's shared flags. No emulator code: tested on its own.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "coop/host/flags.hpp"

namespace CoopDungeon {

// One dungeon flag: the Murmur3 hash of its name, whether it is an Int (else
// a Bool), and the name, for logs and tests.
struct Flag {
    std::uint32_t hash;
    bool is_int;
    const char* name;
};

// Every saved (ResetTypeValue 8) Bool and Int flag whose name starts with a
// dungeon's name, as the logic graphs and event flows use them; flags about
// one player alone (where they stand, Navi's hints, the camera) are left out.
std::span<const Flag> Flags();

// Adds the dungeon flags to `shared` when `on`, making the list when there is
// none. Flags already there are kept once. Returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// Merge, on unless LICHII_COOP_DUNGEON=0; logs what it did.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

} // namespace CoopDungeon
