#pragma once

// The host's save for the guests (issue 167; docs/flags.md, "The host's save
// file"; coop/host/save_transfer.hpp).
//
// A guest's game reads and writes a co-op save folder, <title>-coop beside its
// own (lichii/patches/0006-coop-save-folder.patch calls SaveDirectory), so the
// host's save never goes over the player's own. On joining it asks the host
// for its save; the host sends its newest slot, and again to everyone after
// its own save load (once the game has saved after it).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
// Rows 239/240/241 (coop_char_slots.h FixSavePick): puts a slot's progress.sav right before the game can read it (a
// Character Select pick whose model is not here becomes Link); returns a line for the log, "" when nothing changed.
// Run on the host's save a guest writes, and on every slot of the folder the game plays from each time the title comes
// up. Unset: nothing.
using SaveFixFn = std::function<std::string(std::vector<std::uint8_t>& progress)>;
void SetSaveFix(SaveFixFn fix);
// Player report 120, Hookshot's patch 0025 (hos_coop_fs_save_slot_opened): the count of opens of a slot's
// progress.sav on the account save, the last one's slot folder and whether it was a write, so the host sends the slot
// its game plays (save::PlayedSlot). Unset (the emulator, an older Hookshot): the newest slot on disk, as before.
using SlotOpenedFn = unsigned (*)(char* slot_dir, std::size_t cap, int* write);
void SetSlotOpens(SlotOpenedFn opened);
// Each frame, also without a session: takes the new opens and the load windows (cheap when there are none).
void PollSlotOpens();
// The host's save as it is NOW for a guest that asks (case 1 of the join's state): Hookshot says at start that it can
// ask our game for its own autosave (SetSaveRequests(true)); then a guest's ask while our file is played waits for
// that save (save::Planner's round), and Hookshot's game-thread step takes the request once (TakeSaveRequest) and
// makes the game's call (the EventTriggerRequestAutoSave node's own). Unset (the emulator): the save on disk at once.
void SetSaveRequests(bool on);
// Which ask the game-thread step takes (once): a guest's (the send waits for that save) or the test commands'
// `autosave` (AskOwnAutoSave); none when no file is played.
enum class SaveRequest { none, guests, test };
SaveRequest TakeSaveRequest();
// The test commands' `autosave` (rejoin-host-273-2): our game asked for its own autosave with the same call, at the
// game-thread step. False when it cannot be: no file played, or a runtime with no such call (the emulator).
bool AskOwnAutoSave();
// rejoin-host-273-2: the title's slot fixes (rows 239/240/241) written into the folder the game already mounted
// (Longshot 3.1 plays a work copy made at the mount): RestageDue says the folder is to be mounted again, and the
// title's step calls RestageSaveFolder when the game is not reading its save list (the list is then re-read).
bool RestageDue();
void RestageSaveFolder();
// The Lobby tab's "Resync from host" (a guest; save::resync_check): taken at the next OnFrame, which asks the host
// again; the chat says how it goes. CanResync: the row is shown (a connected guest on a switchable folder).
void RequestResync();
bool CanResync();

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
// Row 63, Gold Skulltulas Not shared, a guest: the game plays from the co-op folder whose host's save was written with
// our race (coop_gold_mode.h), so the Skulltulas it shows are ours and the hook keeps the race from them.
bool PlaysRaceSave();

// Hookshot's title module, each pad poll: whether the title's menu is up. A guest switches to the host's save only
// then (never while a file loads).
void NoteTitle(bool title_up);
// What NoteTitle last said (false on the emulator, which has no title module).
bool TitleUp();
// Player report 124: true once after each switch of the save folder (every switch: a join's host's save, Resync from
// host, back to the player's own folder for a new session). Hookshot's title step then has the game re-read its save
// list (its caption manager: Continue and Load), as the game itself does at boot.
bool TakeSaveListStale();

// Hookshot's save-load wraps (patch 0024, hos_coop_save_load.h), from the game's thread: the game's own sequence
// nodes start loading a save (start) or say the save is in (!start). Every save the game loads: the title, the System
// menu's Load, Game Over's Try Again. The emulator never calls these.
void NoteGameSaveLoad(bool start);
// Game Over saved its last state (its Try Again or its Quit Game: SeqExecuteAutoSaveMgrLastGameOverSave). A save load
// after it with no title menu between (NoteTitle) is Try Again.
void NoteGameOverSave();
// Whether this Hookshot has all four wraps (its hello's bits); false on the emulator.
void SetNativeSaveLoads(bool on);
bool NativeSaveLoads();
// How many save loads the game has started since boot (NoteGameSaveLoad(true): the title's Continue/Load, the System
// menu's Load, Game Over's Try Again, the host's save), never reset: a reader keeps the last count it saw.
std::uint64_t SaveLoadStarts();
// The starts and loads since the last call (the session's flag watcher takes them).
// Only fresh ones: a start older than kGameSaveLoadFreshMs (the game's own start loads a save before any session) or
// a Game Over mark older than kGameOverFreshMs is dropped.
struct GameSaveLoads {
    unsigned starts = 0;
    unsigned loads = 0;
    bool retry = false;  // the save in is Game Over's Try Again (NoteGameOverSave, no title since)
};
inline constexpr long long kGameSaveLoadFreshMs = 30'000;
inline constexpr long long kGameOverFreshMs = 120'000;
GameSaveLoads TakeGameSaveLoads();
// True once after each save load (the solo map keeper drops its baseline; with or without a session).
bool TakeKeeperSaveLoad();

// (Navi) Solo or multiplayer saves: the Multiplayer tab's Settings page, "Saves". The multiplayer saves are their own
// folder beside the solo ones (<account>-mp; a guest's copy of the host's save is then <account>-mp-coop), first made
// as a copy of the solo saves. Switched on the title's menu with no file loaded and no session, and kept in
// coop-saves.ini beside coop.ini for the next start (put in force on the first title). Hookshot only.
// What it did, in plain words, for the menu's note.
std::string ChooseSaveSet(bool multiplayer);
// The saves chosen (as kept: in force from the title's menu on).
bool MultiplayerSaves();

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
