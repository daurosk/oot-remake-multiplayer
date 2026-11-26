// SPDX-License-Identifier: GPL-3.0-or-later
// Games on this network, the sockets (coop_lan.h is the datagram and the list): a hosting game's announcement sent
// every CoopLan::kEveryMs to each local network's broadcast address, 255.255.255.255 and 127.0.0.1 (another game window
// on this PC), port CoopLan::kPort; and a listener on that port, open only while a page looks for games (and a few
// seconds after), shared with the other game windows on this PC (SO_REUSEADDR). Any thread; nothing blocks.
#pragma once

#include <cstdint>
#include <vector>

#include "coop_lan.h"

namespace CoopLanNet {

// While this game hosts: call every frame with its game; it is sent when due.
void Announce(const CoopLan::Game& game, std::uint64_t now_ms);
// While a page looks for games: call every frame with on (the listener opens, what came is read); off a while, it
// closes and the list empties.
void Look(bool on, std::uint64_t now_ms);
// The games heard lately (CoopLan::List::Now).
std::vector<CoopLan::Heard> Games(std::uint64_t now_ms);
// What was heard forgotten (the browser's Refresh): the games still announcing come back within 2 s.
void Forget();

}  // namespace CoopLanNet
