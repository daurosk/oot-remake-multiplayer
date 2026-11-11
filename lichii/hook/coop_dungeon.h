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
#include <string_view>
#include <optional>
#include <span>
#include <string_view>

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

// Room switch flags that are NOT saved (reset types 9 and 40: the room's own
// state, back on a reload) but open or start something for everyone in the
// room: a switch on, a light switch hit, a gate, a room's provisional clear.
// Shared one way (FlagWatcher::share_one_way: a 1 crosses, the game's own
// reset to 0 does not, no write-back after a load). The hook calls it for
// each (test7, "temple actions", docs/dungeon.md). Left out:
// ForestTemple_Room00_AppearWolfosSwitch, a sensor of where one player is.
// Found by hashing the dungeon logic graphs' strings against GameDataList,
// all reset types; the rest of the 31 non-saved hits are one player's own
// (where they stand, Navi) or a one-way ride, and stay out.
std::span<const Flag> LiveFlags();

// Issue 129: a shared flag that marks a room gimmick done (its played
// event: the Deku Tree ladder fell, a shutter opened) arrives in a game whose
// room is already loaded; the room does not play the result, and its own
// switch no longer starts it (the event counts as played): a softlock. Such
// flags are held while our Link is in that dungeon and written when we are
// out of it or loading (the room then loads with them). Meanwhile our own
// switch still plays the event. Also the Forest Temple corridor's state
// flags (its twist must never turn under a player). Returns the dungeon
// scene the flag belongs to ("" for a gimmick event of no one dungeon: held
// in any dungeon), or nothing for a flag that is not held.
std::optional<std::string_view> HeldIn(std::uint32_t hash);

// The played event a held flag stands for ("HavePlayedEvent.<event>" ->
// "<event>"), or "" when the flag is not one (issue 129, live events).
std::string_view EventOf(std::uint32_t hash);

// A dungeon's scene (the Banc names of Keys()).
bool IsDungeonScene(std::string_view scene);
// Named flags of the dungeons that are never shared: where one player is in a
// boss fight (the boss door shut behind them), not what the group did (issue 148).
std::span<const std::string_view> NotShared();

// One placed thing whose "done" is a 64-bit-key flag keyed by its placement
// hash (a chest opened, a silver rupee taken, a room's enemies beaten, a wall
// blown open): the key, the dungeon (Banc folder) and the actor, for logs.
struct Key {
    std::uint64_t key;
    const char* dungeon;
    const char* actor;
};

// Every such saved (ResetTypeValue 24) key placed in a dungeon. They are
// one-time: once 1 in any game, 1 in all (shared as pickups, docs/dungeon.md).
std::span<const Key> Keys();

// Adds the dungeon flags and keys to `shared` when `on`, making the list when there is
// none. Flags already there are kept once. Returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// Merge, on unless LICHII_COOP_DUNGEON=0; logs what it did.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

// False when LICHII_COOP_DUNGEON=0 (then the keys are not shared either).
bool On();

} // namespace CoopDungeon
