// SPDX-License-Identifier: GPL-3.0-or-later
//
// Race mode (docs/protocol.md "Race mode", docs/world.md): a host setting.
// On, the players see each other (puppets, name tags), chat and talk, and
// nothing of the world is shared: every flag, item, enemy, boss, door,
// switch, drop, Gold Skulltula, key and save stays in each game, and each
// player plays and finishes on their own.
//
// The setting rides on MOD_SETTINGS (491) under mod id "coop", key "race",
// value "on" or "off" (no entry: off). The list of what race mode keeps in
// each game is here, so the games and the server cannot differ: a game does
// not send such a message and drops one that comes, and the server neither
// relays nor keeps one, nor gives what it kept before to anyone.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::protocol {

inline constexpr std::string_view kRaceModId = "coop";
inline constexpr std::string_view kRaceKey = "race";

// The race setting a host's list carries: nullopt without a coop "race" entry (an older host:
// off), else on for "on" and off for anything else.
std::optional<bool> race_of(const std::vector<ModSetting>& settings);
ModSetting race_setting(bool on);

// Whether race mode keeps this message in each game (world state). False for
// the session, the players' own looks and moves, chat, voice, PvP, down but
// not out, the difficulty, mods and the Lobby. An envelope (LOCAL_SAMPLE,
// LOCAL_MESSAGE) is judged by what it carries.
bool race_private(const Message& message);

}  // namespace coop::protocol
