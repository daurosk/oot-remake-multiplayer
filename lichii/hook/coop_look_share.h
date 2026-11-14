// SPDX-License-Identifier: GPL-3.0-or-later
// Each player's look of several parts (PLAYER_LOOK, docs/protocol.md "Looks";
// the parts: hookshot/src/coop_look.h). Our own goes out when the player
// changes it and again whenever someone joins or we connect again, so a late
// joiner has it; the others' are kept by player number for the puppets
// (docs/puppet.md, "The Link customizer").
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopLookShare {

using Parts = std::vector<coop::protocol::LookPart>;

// The rules, with no network: what to send and when, what the others wear.
// Not thread-safe (the functions below guard it).
class Share {
public:
    // Our look changed: it goes out at the next chance.
    void Set(Parts parts);
    // An event of the session: a join or a (re)connect sends ours again; a
    // PLAYER_LOOK is kept; a player who left is forgotten. True when this
    // changed what a player wears (for the log and the puppets).
    bool OnEvent(const coop::net::Event& event);
    // Our look to send now (once per change or join); nothing when none is
    // due or none was ever chosen.
    std::optional<coop::protocol::PlayerLook> TakeSend();
    // What player `number` wears: nothing heard yet.
    std::optional<Parts> Of(std::uint8_t number) const;

private:
    std::optional<Parts> ours_;
    bool due_ = false;
    std::map<std::uint8_t, Parts> theirs_;
};

// The hook's one Share. Set: any thread. OnEvent: the network thread, with
// its client (sends at once). Frame: the game's thread, each frame. Of: any.
void Set(Parts parts);
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
void Frame(coop::host::Session* session);
std::optional<Parts> Of(std::uint8_t number);

}  // namespace CoopLookShare
