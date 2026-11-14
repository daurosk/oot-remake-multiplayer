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
#include <functional>
#include <string>
#include <string_view>

namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopSave {

// Whether this game uses the co-op save folder. Where the folder is fixed when
// the game starts (the emulator): LICHII_COOP_HOST_SAVE=1 (play.ps1 sets it
// for a join, by address or invite code), else its own saves. Never from
// coop.ini's address (save::folder_choice). Where it can be switched while
// the game runs (SetSwitch: Hookshot), the session decides it in OnFrame.
bool UseCoopFolder();

// Hookshot (patch 0008): the folder the account save is mounted from now ("" none yet), and a re-mount of it
// from another folder (true when done). With them, a guest switches to <folder>-coop when it joins.
void SetSwitch(std::function<std::string()> root, std::function<bool(const std::string&)> remount);

// Each frame: whether Link is in the game (a loaded save): no switch then.
void NoteInGame(bool in_game);

// The host's save has been written whole into this game's co-op folder and (Hookshot) the game plays from it now:
// a title menu may load it. Before that a guest's game stays on its own folder.
bool HostSaveReady();

// The host's save is on its way: parts came in the last 10 s, or it is in and waits for the title's menu to be
// written and switched to. (A title waiting for it counts no wait time then.)
bool HostSaveComing();

// The host said it has no save yet (a new game): nothing to wait for.
bool HostHasNoSave();

// Hookshot: a session has chosen its save folder and is still connected (false before that, and once it ended:
// the host dropped, the link went, Leave). A title waiting for the host's save stops when it goes off.
bool SessionLive();

// Hookshot's title module, each pad poll: whether the title's menu is up. A guest switches to the host's save only
// then (never while a file loads).
void NoteTitle(bool title_up);

// A guest's switch to the host's save that has not happened yet waits (the player picked a file on the title):
// never while that file loads. Once Link is in the game the hold ends; the switch still waits (never under a game
// being played) and goes when the player is back on the title's menu with no file loaded.
void CancelPendingSwitch(std::string_view why);
// The player is back on the title's menu with no file loaded (they backed out of their pick): the waiting switch
// goes again (the host's save, once in, is written and switched to on the title's menu).
void ResumePendingSwitch();

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
