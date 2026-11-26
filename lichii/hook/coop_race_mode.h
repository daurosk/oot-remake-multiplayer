// SPDX-License-Identifier: GPL-3.0-or-later
//
// Race mode, a host setting (docs/world.md "Race mode"; coop/protocol/race.hpp has
// what it keeps in each game). On, the players see each other, chat and talk,
// and play the world each on their own: no flag, item, enemy, boss, door,
// switch, drop, Gold Skulltula, key or save is shared, and a guest keeps its
// own saves. PvP works as without race mode (the host's PvP row).
//
// As the Gold Skulltula mode (coop_gold_mode.h): the host's choice is saved in
// coop-options.ini (`race = on`; no line: off) and read when this game starts
// hosting, so a change in a session takes effect at the next Host. It goes to
// the guests in the host's MOD_SETTINGS (491) as mod id "coop", key "race",
// value "on" or "off"; the server keeps it for late joiners, and a host after a
// migration sends the mode in force. The gate itself is in the client and the
// server (coop::net::Client::race, coop::server::Session): a session that was
// in race mode stays so until it ends.
#pragma once

#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopRaceMode {

// This player's own choice for its next Host (the menu, coop-options.ini). Any thread.
void SetChoice(bool on);
bool Choice();
// The connection events (CoopModShare, the network thread), as CoopGoldMode's.
void OnWelcomed(bool hosting);
void OnHosting();
void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings);
// The mode in force for this session (off until told, or our own choice once we host). Any thread.
bool InForce();
// The setting a host sends (always: "off" too, so a guest knows the host has the setting).
coop::protocol::ModSetting AsSetting();

}  // namespace CoopRaceMode
