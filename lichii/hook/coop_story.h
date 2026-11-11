// SPDX-License-Identifier: GPL-3.0-or-later
//
// The story the same for every player (issue 173, docs/flags.md "Story and
// cutscenes"). A story scene one player has seen (Saria at the forest exit,
// the owl in Hyrule Field) is a saved `HavePlayedEvent.<scene>` flag; the
// world's story gates (the invisible wall at the forest exit, Mido at the
// Deku Tree path) are named flags that the overworld's placements test
// (`Presence.FlagName` in Banc/<area>/*.bcett.byml). Sharing them makes a
// scene seen once count for everyone and opens a gate in every game, and,
// like every shared flag, the host's save decides (FLAG_SNAPSHOT for a player
// who joins, FLAG_RESET when the host loads a save). No emulator code: tested
// on its own.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "coop/host/flags.hpp"

namespace CoopStory {

enum class Kind : std::uint8_t {
    scene,  // a story scene or story event has played (HavePlayedEvent.)
    owl,    // a talk with the owl (Kaepora Gaebora)
    world,  // a story gate placements test: an invisible wall, a blocker, a water plane
};

// One story flag: the Murmur3 hash of its name, what it is, and the name, for
// logs and tests. All are saved (ResetTypeValue 8) Bools.
struct Flag {
    std::uint32_t hash;
    Kind kind;
    const char* name;
};

// Every story flag shared, sorted by hash.
std::span<const Flag> Flags();

// The flag of `hash`, or nullptr.
const Flag* Find(std::uint32_t hash);

// Adds the story flags to `shared` when `on`, making the list when there is
// none. Flags already there are kept once. Returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// False when LICHII_COOP_STORY=0.
bool On();

// Merge, on unless LICHII_COOP_STORY=0; logs what it did.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

} // namespace CoopStory
