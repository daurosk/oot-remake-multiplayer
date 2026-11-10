// SPDX-License-Identifier: GPL-3.0-or-later
//
// One-time pickups (docs/items.md, "One-time pickups"): a Gold Skulltula, its
// token, a heart container, a heart piece. Each is gone for good once taken,
// and the game remembers that in a 64-bit-key flag keyed by the actor's
// placement hash. No list names those keys, so the object walk notes the
// hash of every such actor it sees loaded, and the hook hands the new ones to
// the flag watcher (coop::host::Session::share_pickup): from then on the key
// turning 1 in any game is written in all, and each game deletes its own
// copy of the actor (coop_objects_sync.cpp, "taken"). No emulator code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace CoopPickups {

// True for the one-time pickups (by actor name).
bool IsPickup(std::string_view name);

// Any thread: an actor the walk saw. Kept when it is a pickup with a
// placement hash (not 0, not all ones) that was not noted before.
void Note(std::string_view name, std::uint64_t hash);

// Any thread: the noted keys from the `from`-th on, in the order noted. The
// list only grows until Reset.
std::vector<std::uint64_t> Since(std::size_t from);

// Forgets every key (a new game process).
void Reset();

} // namespace CoopPickups