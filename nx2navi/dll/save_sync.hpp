// SPDX-License-Identifier: GPL-3.0-or-later
// Only the host loads a save: a joiner gets the host's newest save slot before
// it loads anything (README, "The host's save for a joiner"). The packing, the
// parts and the host's rules are the emulator build's own
// (coop/host/save_transfer.hpp, the protocol's SAVE_ASK and SAVE_PART); this is
// what lichii/hook/coop_save.cpp does there, for the port:
//
// - the host's side runs inside its session: a guest's SAVE_ASK is answered
//   with the newest slot of the game's save folder (the plugin interface's
//   save_dir), a part a frame; when that folder is a multiplayer save's
//   (mp_saves.hpp), the save's name follows as one more small transfer
//   (NamePacket), which a guest of an older build never sees: it has left by
//   then;
// - the joiner's side runs BEFORE its session, on a client of its own (Fetch):
//   it joins under the player's name, asks, puts the parts together, says
//   goodbye. The slot is then written into a save folder of its own among
//   the player's multiplayer saves, named after the host's save and the host
//   (WriteCoopProfile), never into the player's own saves, and the game is
//   started again on that folder (coop_dll.cpp), since the game opens its
//   save folder once, at its start.
//
// No game code. Fetch and the host's side need the network; WriteCoopProfile
// is plain file work, tested in tests/nx2navi_test.cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/save_transfer.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopNx2navi::SaveSync {

void SetLog(void (*log)(const char* line));

// --- The host, inside its session ---

// Network thread, from the session's on_event: SAVE_ASK and PLAYER_LEFT.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
// The DLL's frame thread, every frame. `save_dir`: the game's committed save
// folder (Nx2GameInfo::save_dir), empty while not known.
void Frame(coop::host::Session& session, bool connected, const std::filesystem::path& save_dir);

// --- The joiner, before its session ---

enum class FetchResult : std::uint8_t {
    fetched,    // `slot` is the host's save
    new_game,   // the host plays on a multiplayer save with nothing in it yet (`save_name`): a game to begin
    no_save,    // connected, but no save came: the host has none, we are the first in, or no answer to the ask
    refused,    // the server refused us: `reason`
    no_answer,  // nobody answered at that address
    stopped,    // the DLL is stopping
};
struct FetchOptions {
    std::string server;
    std::uint16_t port = 27510;
    std::string name;
    std::string password;
    std::string build_id;       // this DLL's
    std::string game_build_id;
    std::string game_version;
    std::uint32_t connect_ms = 12000;  // no WELCOME by then: no_answer
    std::uint32_t save_ms = 15000;     // no whole save this long after the ask: no_save
    std::uint32_t name_ms = 1500;      // how long the save's name is waited for after the save
};
struct Fetched {
    FetchResult result = FetchResult::no_answer;
    std::string reason;  // refused: the server's words
    std::optional<coop::host::save::SaveSlot> slot;
    std::uint8_t host = 0;  // the host's player number
    std::string host_name;  // the host's player name, "" when the server did not tell
    std::string save_name;  // the name of the host's multiplayer save, "" when the host told none
};

// The save's name as the host sends it after the save: a transfer of its own
// whose bytes are "OOTSNAM1" and the name. NameOf: the name of such a packet
// ("" when the bytes are not one, or the name is not one a save can have).
std::vector<std::uint8_t> NamePacket(std::string_view name);
std::optional<std::string> NameOf(std::span<const std::uint8_t> packet);
// What a host on a multiplayer save with no slot yet (a new game it is
// beginning) answers an ask with, in place of a save: "OOTSNEW1" and the
// save's name. The guest then begins a new game too, on an empty save folder
// of its own, never on the player's own saves. A guest of an older build takes
// it for a damaged save and goes on as it did when no save came.
std::vector<std::uint8_t> NewGamePacket(std::string_view name);
std::optional<std::string> NewGameOf(std::span<const std::uint8_t> packet);
// Joins, asks the host for its save, leaves. `progress(true)` once the server
// welcomed us (the save is then awaited). Blocks; `stop` ends it early.
Fetched Fetch(const FetchOptions& options, const std::atomic<bool>& stop,
              const std::function<void(bool welcomed)>& progress);

// Makes `coop_profile` a folder the game can start on with the host's slot as
// its only save, either as its save folder (NX2NAVI_SAVE_DIR: save\<account>
// in it) or as a whole profile (NX2NAVI_PROFILE_DIR: account.uid too): the
// player's own account id and settings (account.uid, account.sav, option.sav:
// copied, never moved) and the slot as slot_00. A save already there (an
// earlier session's) is kept once, in save-previous. Refuses to write into the
// player's own profile. False with `error` set on failure.
bool WriteCoopProfile(const std::filesystem::path& own_profile,
                      const std::filesystem::path& own_save_dir,
                      const std::filesystem::path& coop_profile,
                      const coop::host::save::SaveSlot& slot, std::string& error);
// The same folder with no slot in it: the game started on it has nothing to
// continue and begins a new game (for a host that is beginning one). A save
// already there is kept once in save-previous, as above.
bool WriteEmptyCoopProfile(const std::filesystem::path& own_profile,
                           const std::filesystem::path& own_save_dir,
                           const std::filesystem::path& coop_profile, std::string& error);

}  // namespace CoopNx2navi::SaveSync
