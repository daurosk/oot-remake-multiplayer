// SPDX-License-Identifier: GPL-3.0-or-later
//
// World runs in menus, a host setting (row 299, docs/pause-world.md "Phase 1"). Off (the default): a player's pause
// menu stops their world as in solo, and co-op hands their things over as today. On: in a session, the menu's own
// holds are let go while it shows (CoopPauseWorld), so enemies, counters and switches keep going for that player.
//
// As shared floor switches (coop_switch_stand_mode.h): the host's choice is saved in coop-options.ini
// (`pause_world = on`; no line: off) and read when this game starts hosting, so a change in a session takes effect at
// the next Host. It goes to the guests in the host's MOD_SETTINGS (491) as mod id "coop", key "pause_world", value
// "on" or "off"; the server keeps it for late joiners, and a host after a migration sends the setting in force. No
// entry (an older host): off. Outside a session it is never in force (solo and co-op idle stay as they are).
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopPauseWorldMode {

inline constexpr std::string_view kModId = "coop";
inline constexpr std::string_view kKey = "pause_world";

// This player's own choice for its next Host (the menu, coop-options.ini). Any thread.
void SetChoice(bool on);
bool Choice();
// The connection events (CoopModShare, the network thread), as CoopSwitchStandMode's.
void OnWelcomed(bool hosting);
void OnHosting();
void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings);
// Test only (the play-test bridge's `menu world on|off`, on the host): the setting in force now, as if this game had
// hosted with it; the host's next MOD_SETTINGS tells the guests. Lane files reset coop-options.ini at each boot.
void TestSet(bool on);
// The session ended (left, or the connection dropped): nothing in force.
void OnLeft();
// The setting in force for this session (off until told, or our own choice once we host; off outside a session).
// Any thread.
bool InForce();
// The setting a host sends (always, "off" too).
coop::protocol::ModSetting AsSetting();
// The host's entry in `settings`, if it has one.
std::optional<bool> Of(const std::vector<coop::protocol::ModSetting>& settings);

}  // namespace CoopPauseWorldMode
