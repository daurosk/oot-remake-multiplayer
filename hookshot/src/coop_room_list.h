// SPDX-License-Identifier: GPL-3.0-or-later
// The relay's public room list (server/relay_link.hpp, list_rooms_on_relay; docs/play-online.md) for the pause
// menu's game browser: asked from a thread of its own while the browser shows its Online list, every kEveryMs, and
// kept for the page to read. A room is listed only when its host asked (coop-options.ini list_room = on); the list
// never carries a password, a join code or an address: a room is joined by its name and the password its host gives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CoopRoomList {

inline constexpr std::uint64_t kEveryMs = 10000;   // asked again this often while shown
inline constexpr std::uint64_t kKeepMs = 30000;    // what was heard is kept this long after the list is left

struct Room {
    std::string room, host;
    int players = 0, max = 0, protocol = 0;   // protocol: the host's version, the public bit off
    bool is_public = false;   // a public game (coop::protocol::kPublicListingBit): joined with no password
};
// A listing's version as a Room has it (the public bit taken off into is_public).
Room FromListing(std::string room, std::string host, int players, int max, int version);

// What a room's cell says: "kokiri (2/4)", "(full)", "(other version)" as the games on this network's rows.
std::string CellText(const Room& r, int my_protocol);
// The line under it, plain words: "Public game, Players 2/4" or "Needs the host's password, Players 2/4" (and
// "Full", "Needs an update" / "Another version"), as the Lobby and Find a game both show it.
std::string DetailText(const Room& r, int my_protocol);
// Whether it can be joined from here (same protocol, room for one more).
bool Joinable(const Room& r, int my_protocol);

struct Snapshot {
    std::vector<Room> rooms;
    std::string state;   // "" asking (nothing heard yet) or rooms listed; else why there are none, in plain words
                         // (kNoGames, kCantReach, ...; shown through CoopI18n::Tr)
};
// The states' words (the owner's): the same in the Lobby and Find a game.
inline constexpr const char* kNoGames = "No public games right now. Host one and choose Public game.";
inline constexpr const char* kCantReach =
    "Can't reach the game list. Check your internet connection, then press Refresh.";
inline constexpr const char* kNoNetwork = "No network here. Check your internet connection, then press Refresh.";
inline constexpr const char* kBadServer =
    "The game list's server in Co-op Settings (Relay) is not right. Clear it to use the kit's own.";

// Who shows the list: each says each frame whether it does; the list is asked while one of them does, and one
// leaving never stops the other's asks.
enum class Viewer { kFindAGame = 0, kLobby = 1 };
inline constexpr int kViewers = 2;

// The module's part (coop_room_list.cpp): `wanted` while that viewer shows the list; `relay` the kit's relay
// ("host" or "host:port", coop-options.ini relay; "" the kit's own).
void Look(Viewer who, bool wanted, std::uint64_t now_ms, const std::string& relay);
Snapshot Now();
// The list asked again at the next Look (the browser's Refresh), what was heard dropped meanwhile.
void AskNow();

}  // namespace CoopRoomList
