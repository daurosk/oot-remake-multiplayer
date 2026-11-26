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
#include <filesystem>
#include <optional>
#include <string>
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

// Forgets every key (a new game process), the one-time list too.
void Reset();

// Row 219 (#84): the one-time placements of the game as it is served, listed by the kit's start
// (tools/puppet_pack/merge_tables.py one_time_keys) in <mods>/kOneTimeFile. Learning them from the walk alone
// missed every placement no game had loaded since its start: one taken before a save + reload never loads again,
// so no game shared its key and another player's copy was taken again; and a randomizer seed's shuffled pots,
// crates, flying pots and beehives (made saved-once by the seed) were never shared at all.
//   coop-one-time-keys 1
//   game <FNV-1a 64 of the game's own saved values list>
//   served <path under the mods folder> <FNV-1a 64 of it>   (or "served - -": the game's own list)
//   pickup <hex>   a placed one-time pickup (rupee, heart, heart piece, Gold Skulltula, token, ...)
//   holder <hex>   a placement the served list saves once and the game's own list resets (a seed's pot)
inline constexpr const char* kOneTimeFile = "coop-one-time-keys.txt";

struct OneTime {
    std::vector<std::uint64_t> pickups;
    std::vector<std::uint64_t> holders;
    std::string served;      // the served list's path under the mods folder; empty: the game's own
    std::string served_fnv;  // its FNV-1a 64, 16 lower-case hex
};

// The list's text; nothing on a bad line (`why` says which).
std::optional<OneTime> ParseOneTime(std::string_view text, std::string* why = nullptr);

// FNV-1a 64 of bytes, 16 lower-case hex (merge_tables.py fnv64_bytes).
std::string Fnv64Hex(std::string_view bytes);

// Reads `file` (UTF-8 path). Its holders are dropped (`note` says so) when the served list is no longer the one it
// was made from (a seed built or taken out since the kit's start): kept, a pot the served game resets would break
// for everyone. Nothing when the file is missing or bad (`note` says which).
std::optional<OneTime> LoadOneTime(const std::string& file, std::string& note);

// Any thread: the list the walk's removal and the world sync go by (SetOneTime at a session's start).
void SetOneTime(const OneTime& list);
// A listed placement (pickup or holder): a copy still loaded here goes when another game took it.
bool IsOneTime(std::uint64_t hash);
// A seed's one-time holder: its item is given once, so another player's break deletes ours (no second drop).
bool IsOneTimeHolder(std::uint64_t hash);

} // namespace CoopPickups