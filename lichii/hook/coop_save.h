#pragma once

// The host's save for the guests (issue 167; docs/flags.md, "The host's save
// file"; coop/host/save_transfer.hpp).
//
// A guest's game reads and writes a co-op save folder, <title>-coop beside its
// own (lichii/patches/0006-coop-save-folder.patch calls SaveDirectory), so the
// host's save never goes over the player's own. On joining it asks the host
// for its save; the host sends its newest slot, and again to everyone after
// its own save load (once the game has saved after it).

#include <cstdint>
#include <string>

namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopSave {

// Whether this game uses the co-op save folder, decided once (the first call):
// LICHII_COOP_HOST_SAVE=1 or 0 if set; else on when coop.ini names a server
// that is not this PC (Join.bat), off for 127.0.0.1 or no server (Host.bat,
// single player).
bool UseCoopFolder();

// The emulator's save folder path for a user's account save (relative to the
// NAND, "/user/save/0000000000000000/<user>/<title>"): with the co-op folder
// on, the same path with "-coop" added. Any thread (the game's file service).
std::string SaveDirectory(const std::string& path, bool account_save);

// The real folder the emulator opened for the game's account save (the
// redirected one on a guest). Any thread.
void NoteSaveDir(const std::string& real_path);

// Network thread, under the session's lock: SAVE_ASK (host) and SAVE_PART
// (guest), PLAYER_LEFT (a waiting ask goes), and a new connection (a guest asks).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Emulator thread, each frame, with the client under the session's lock:
// `host_reset` is FrameReport::host_reset (on the host: its own save load).
// Sends what the host owes, a part a frame.
void OnFrame(coop::net::Client& client, bool host_reset);

// One line for the log and chat when something happened (a save written, a
// transfer refused), or "".
std::string TakeNotice();

}  // namespace CoopSave
