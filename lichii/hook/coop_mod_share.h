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

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

// How a mod's hash was made, marked in its last three bytes ('S', 'Q', the way's number). Way 2 (0.11.13, row 274):
// only what decides the mod's world: its game files, with the files the kit rewrites on each PC read from the mod's own
// kept copies, and its size tables left out (they hold file sizes, and the kit, OOTRR and the button prompts merge the
// other mods' entries into them on each PC). Unmarked: way 1 (every file as it was on that PC). Hashes made two ways
// are not compared: the higher way is the newer kit.
inline constexpr std::uint8_t kHashWay = 2;
void MarkHashWay(coop::protocol::ModHash& hash);
std::uint8_t HashWay(const coop::protocol::ModHash& hash);

// How two lists differ, in plain English, one line per mod (at most `max`, then "and N more"). Empty: the same. The
// order of the mods is not compared (it decides which mod wins a file, but a different order of mods that share no
// file plays the same; the Mods tab shows it).
std::vector<std::string> Compare(const coop::protocol::ModList& host, const coop::protocol::ModList& mine,
                                 std::size_t max = 6);

// A randomizer seed's mod id (the folder OOTRR builds, randomizer-<seed word>; its id is the folder's name in lower
// case).
bool IsSeedId(std::string_view id);
// The host's randomizer seed that a player's list lacks (#59: guests whose game could not build it played the base
// game, another world, and nobody knew): its seed word in capitals; empty when the host plays no seed or the player
// has it.
std::string SeedMissing(const coop::protocol::ModList& host, const coop::protocol::ModList& theirs);
// The host's seed that a player has with other files (both hashes known and made the same way, HashWay): built by
// another OOTRR (a player on another co-op kit: the same seed word is another world); its seed word, else empty.
std::string SeedFilesOther(const coop::protocol::ModList& host, const coop::protocol::ModList& theirs);
// The host's seed that a player has hashed another way (one of the two kits is older, HashWay): its seed word in
// `seed`, and > 0 when the player's kit is the newer one (the host's is older), < 0 when the player's is older; 0: not
// so (`seed` untouched).
int SeedKitOther(const coop::protocol::ModList& host, const coop::protocol::ModList& theirs, std::string* seed);
// The chat lines for it: to the host and the other players (about player `number`, `name` may be empty), to the
// player itself, and when the player has the seed after all.
std::string SeedlessLine(std::uint8_t number, std::string_view name, std::string_view seed);
std::string SeedlessMineLine(std::string_view seed);
std::string SeedOtherLine(std::uint8_t number, std::string_view name, std::string_view seed);
std::string SeedOtherMineLine(std::string_view seed);
// SeedKitOther's lines: about player `number` (`newer`: its kit is newer than the host's), and to the player itself
// (`newer`: its kit is the newer one, so the host's is older).
std::string SeedKitLine(std::uint8_t number, std::string_view name, std::string_view seed, bool newer);
std::string SeedKitMineLine(std::string_view seed, bool newer);
std::string SeedBackLine(std::uint8_t number, std::string_view name);

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
