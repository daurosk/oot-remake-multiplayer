// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared floor switches, a host setting (docs/dungeon.md, the held floor switch row). On (the default), another
// player standing on our step switch holds it down in our game (CoopLiftRide::TheStander). Off, each game's step
// switches answer only to its own Link and blocks.
//
// As the race mode (coop_race_mode.h): the host's choice is saved in coop-options.ini (`floor_switches = off`; no
// line: on) and read when this game starts hosting, so a change in a session takes effect at the next Host. It goes
// to the guests in the host's MOD_SETTINGS (491) as mod id "coop", key "floor_switches", value "on" or "off"; the
// server keeps it for late joiners, and a host after a migration sends the setting in force. No entry (an older
// host): on. Race mode turns the stander off whatever this says.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopSwitchStandMode {

inline constexpr std::string_view kModId = "coop";
inline constexpr std::string_view kKey = "floor_switches";

// This player's own choice for its next Host (the menu, coop-options.ini). Any thread.
void SetChoice(bool on);
bool Choice();
// The connection events (CoopModShare, the network thread), as CoopRaceMode's.
void OnWelcomed(bool hosting);
void OnHosting();
void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings);
// The setting in force for this session (on until told, or our own choice once we host). Any thread.
bool InForce();
// The setting a host sends (always, "on" too).
coop::protocol::ModSetting AsSetting();
// The host's entry in `settings`, if it has one.
std::optional<bool> Of(const std::vector<coop::protocol::ModSetting>& settings);

}  // namespace CoopSwitchStandMode
