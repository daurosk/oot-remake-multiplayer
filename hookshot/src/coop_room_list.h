// SPDX-License-Identifier: GPL-3.0-or-later
// The relay's public room list (server/relay_link.hpp, list_rooms_on_relay; docs/play-online.md) for the pause
// menu's game browser: asked from a thread of its own while the browser shows its Online list, every kEveryMs, and
// kept for the page to read. A room is listed only when its host asked (coop-options.ini list_room = on); the list
// never carries a password, a join code or an address: a room is joined by its name and the password its host gives.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "coop/protocol/official_mark.hpp"

namespace CoopRoomList {

inline constexpr std::uint64_t kEveryMs = 10000;   // asked again this often while shown
inline constexpr std::uint64_t kKeepMs = 30000;    // what was heard is kept this long after the list is left

struct Room {
    std::string room, host;
    int players = 0, max = 0, protocol = 0;   // protocol: the host's version, the public bit off
    bool is_public = false;   // a public game (coop::protocol::kPublicListingBit): joined with no password
    bool official = false;    // the official public game (Proven): shown first, named so
};

// A public listing's version bit (coop::protocol::kPublicListingBit; checked equal in coop_room_list.cpp).
inline constexpr int kPublicListingBit = 0x8000;
// A listed room's proof as the relay passed it on (server/relay_wire.hpp kRoomProofs).
struct Proof {
    std::string room;
    coop::protocol::OfficialProof proof;
};
// Whether a room is the official public game (docs/official-game.md): a public room on the kit's own relay
// (`relay`: official_relay_name of the relay asked; `kits_relay`: of the kit's default relay) with a proof that
// checks against the kit's built-in key for that relay, its name and listing version, made within
// kOfficialProofMaxAgeS of now. A room of any name without one (an old relay, a copy of the name, a proof passed
// on through another relay, a stale or tampered proof) is an ordinary room.
inline bool Proven(const Room& r, const std::vector<Proof>& proofs, std::uint64_t now_s, const std::string& relay,
                   const std::string& kits_relay,
                   const coop::protocol::OfficialPublicKey& key = coop::protocol::kOfficialPublicKey) {
    if (!r.is_public || relay.empty() || relay != kits_relay) return false;
    const auto version = static_cast<std::uint16_t>(r.protocol | kPublicListingBit);
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (const Proof& p : proofs) {
        if (lower(p.room) == lower(r.room) && coop::protocol::official_check(key, relay, r.room, version, p.proof, now_s)) {
            return true;
        }
    }
    return false;
}
// The list's order, the same in every viewer: the official game first, then public games, then the rest (the
// relay's order kept within each); marks each room's `official`.
inline void Order(std::vector<Room>& rooms, const std::vector<Proof>& proofs, std::uint64_t now_s,
                  const std::string& relay, const std::string& kits_relay,
                  const coop::protocol::OfficialPublicKey& key = coop::protocol::kOfficialPublicKey) {
    for (Room& r : rooms) r.official = Proven(r, proofs, now_s, relay, kits_relay, key);
    const auto rank = [](const Room& r) { return r.official ? 0 : r.is_public ? 1 : 2; };
    std::stable_sort(rooms.begin(), rooms.end(), [&](const Room& a, const Room& b) { return rank(a) < rank(b); });
}
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
enum class Viewer { kFindAGame = 0, kLobby = 1, kMenu = 2 };   // the pause tab's browser, the Lobby, co-op's menu
inline constexpr int kViewers = 3;

// The module's part (coop_room_list.cpp): `wanted` while that viewer shows the list; `relay` the kit's relay
// ("host" or "host:port", coop-options.ini relay; "" the kit's own).
void Look(Viewer who, bool wanted, std::uint64_t now_ms, const std::string& relay);
Snapshot Now();
// The list asked again at the next Look (the browser's Refresh), what was heard dropped meanwhile.
void AskNow();

}  // namespace CoopRoomList
