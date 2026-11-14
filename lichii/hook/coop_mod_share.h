// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mods in a co-op session (docs/mods.md "Mods in co-op", docs/protocol.md
// "Mods"): each game sends the mods it runs (MOD_LIST, ids, versions and
// hashes), a guest whose mods differ from the host's is warned (and the host
// is told which guest differs), and the host's "shared" mod settings
// (MOD_SETTINGS: a randomizer's seed) become every guest's.
//
// The game's mod loader (hookshot/src/coop_mods_runtime.cpp) gives its part
// through a Provider; without one (the emulator) nothing is sent and lists
// that come are only compared with an empty list of our own.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopModShare {

// How two lists differ, in plain English, one line per mod (at most `max`, then "and N more"). Empty: the same. The
// order of the mods is not compared (it decides which mod wins a file, but a different order of mods that share no
// file plays the same; the Mods tab shows it).
std::vector<std::string> Compare(const coop::protocol::ModList& host, const coop::protocol::ModList& mine,
                                 std::size_t max = 6);

struct Provider {
    // This game's list once its mods are hashed (any thread, often: cheap); nullopt until then.
    std::function<std::optional<coop::protocol::ModList>()> mine;
    // This game's own values of its shared settings (the present thread).
    std::function<std::vector<coop::protocol::ModSetting>()> shared;
    // The host's values for a guest's game; nullopt when this game hosts, is alone or left (the present thread).
    std::function<void(std::optional<std::vector<coop::protocol::ModSetting>>)> host_values;
    // The mods' own chat lines since the last call (the randomizer switched: restart the game), alone or in co-op
    // (the present thread).
    std::function<std::vector<std::string>()> notes;
};
void SetProvider(Provider provider);

// Network thread, from the session's on_event (the network lock is held).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
// Present thread, once per frame: sends our list once per connection, the host's shared settings when they change,
// hands the host's values on, and compares. `session` null when not connected.
void Frame(coop::host::Session* session);
// The chat lines since the last call.
std::vector<std::string> TakeLines();

// For the Mods tab.
struct View {
    bool connected = false;
    bool hosting = false;
    bool host_list_known = false;
    std::vector<std::string> differences;  // this game's against the host's (guest), empty when the same
    std::vector<std::string> guests_differ;  // host: "Player 2 (Zelda): 1 mod differs"
};
View Now();

}  // namespace CoopModShare
