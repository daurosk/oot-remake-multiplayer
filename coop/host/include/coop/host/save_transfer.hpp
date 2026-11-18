#pragma once

// The host's save, sent to the guests (issue 167; docs/flags.md, "The host's
// save file"; docs/protocol.md, "Save transfer").
//
// The game keeps each save slot as files in its save folder
// (nand\user\save\0000000000000000\<user>\<title>\slot_NN\progress.sav and
// caption.sav). The host packs its newest slot, splits it into SAVE_PARTs with
// a CRC-32 of the whole, and a guest puts the parts back together and writes
// the slot into its co-op save folder (<title>-coop beside the game's own,
// lichii/patches/0006-coop-save-folder.patch), never into its own saves.
//
// Plain code: no ENet, no emulator. The hook (lichii/hook/coop_save.cpp) feeds
// it the network's messages and the file times.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host::save {

// The suffix of the co-op save folder's name: the game's own folder
// "0400486024DEC000" becomes "0400486024DEC000-coop" for a guest.
inline constexpr std::string_view kCoopSuffix = "-coop";

// The files of one save slot that are sent: the progress (flags, items,
// position) and the caption (the load menu's picture and text). option.sav
// and account.sav are the player's own settings and are never sent.
inline constexpr std::string_view kSlotFiles[] = {"progress.sav", "caption.sav"};
bool slot_file(std::string_view name);

struct SaveFile {
  std::string name;  // one of kSlotFiles
  std::vector<std::uint8_t> bytes;
  bool operator==(const SaveFile&) const = default;
};
struct SaveSlot {
  std::vector<SaveFile> files;  // progress.sav first; caption.sav when there is one
  bool operator==(const SaveSlot&) const = default;
};

// The packed form: "OOTSAVE1", u8 file count, then per file u8 name length,
// the name, u32 size (little endian) and the bytes. pack() returns empty for a
// slot that unpack() would refuse.
std::vector<std::uint8_t> pack(const SaveSlot& slot);
// Nothing for anything but a packed slot: wrong magic, a name not in
// kSlotFiles or twice, sizes past the end, bytes after the last file, no
// progress.sav, or more than protocol::kMaxSaveBytes.
std::optional<SaveSlot> unpack(std::span<const std::uint8_t> bytes);
// "The host has no save yet" (a new game, nothing on disk): "OOTSAVE1" and a file count of 0, sent as an ordinary
// transfer (no new message type). unpack() refuses it, so an older guest logs "not a save slot" and waits on.
std::vector<std::uint8_t> pack_no_save();
bool is_no_save(std::span<const std::uint8_t> bytes);

// CRC-32 (the zlib one: polynomial 0xEDB88320, start and end inverted).
std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// The SAVE_PARTs of a packed slot; empty when it is empty or larger than
// protocol::kMaxSaveBytes. player_number stays 0 (self).
std::vector<protocol::SavePart> split(std::span<const std::uint8_t> packed,
                                      std::uint32_t transfer_id, protocol::SaveReason reason,
                                      std::uint8_t to_player);

// A guest puts the parts of one transfer back together. A part of another
// transfer id (a newer save from the host) starts again; a part whose header
// differs from the transfer's first part, or from another player than the
// host, is refused. Parts may come twice or out of order.
class Assembler {
 public:
  enum class Result {
    partial,   // kept; more to come
    complete,  // take() has the packed slot, its CRC checked
    refused,   // not from the host, or a header that does not match
    bad_crc,   // every part in, but the CRC differs: the transfer is dropped
  };
  Result add(const protocol::SavePart& part, std::uint8_t host_number);
  // The packed slot after `complete`, once.
  std::vector<std::uint8_t> take();
  protocol::SaveReason reason() const { return reason_; }
  std::uint32_t transfer_id() const { return id_; }
  // Parts in of the current transfer, and how many it has.
  std::size_t received() const { return have_count_; }
  std::size_t count() const { return have_.size(); }

 private:
  bool started_ = false;
  std::uint32_t id_ = 0;
  std::uint8_t from_ = 0;
  protocol::SaveReason reason_ = protocol::SaveReason::join;
  std::uint32_t total_ = 0;
  std::uint32_t crc_ = 0;
  std::vector<std::uint8_t> bytes_;
  std::vector<bool> have_;
  std::size_t have_count_ = 0;
  std::vector<std::uint8_t> done_;
};

// At most this many parts wait in the host's outbox (four whole slots); a send
// that would go past it is dropped (the hook logs it).
inline constexpr std::size_t kMaxOutboxParts = 4 * protocol::kMaxSaveParts;

// What the host sends, and when (on the network thread under the hook's lock).
// A guest's ask is answered AT ONCE with the newest slot on disk, also right
// after the host's own save load: a guest never waits on the host's next door
// for a save. With no slot on disk the asker is told so at once
// (Send::no_save). After the host's own save load (on_host_load) the newest
// slot on disk may not be the loaded one (an older manual save loaded, a newer
// autosave on disk), so when the game writes a save after the load (the game
// saves at every door) every guest gets that one too (host_load).
//
// A save asked for (player reports 107, 129, 132: the root's case 1, the file
// on disk is as old as the host's last door): while the host plays a file,
// an ask opens a ROUND (on_ask with `fresh_after`): the hook asks the host's
// game for its own autosave once (take_save_request), and the ask is answered
// with the first save written after it (Send::fresh: the newest slot, the
// game now). The game may refuse (its own gate: autosave off in a cutscene or
// a boss room, its cooldown right after a door, a player state it does not
// save in); after kFreshSaveUs the ask is answered with the save on disk as
// today (Send::fresh false: the host's last save, and the log says so).
// Asks during a round join it: one save, a send to each.
class Planner {
 public:
  using Time = std::filesystem::file_time_type;
  struct Send {
    std::uint8_t to_player = 0;  // 0: every guest (host_load)
    protocol::SaveReason reason = protocol::SaveReason::join;
    bool no_save = false;  // the host has no save on disk: send pack_no_save()
    bool fresh = false;    // written after the ask (a round): the newest slot is the host's game now
    bool operator==(const Send&) const = default;
  };
  // How long an ask waits for the save it asked the host's game for (well under the guest's re-ask,
  // kGuestReaskUs; the game writes a slot in about a second once it saves).
  static constexpr std::uint64_t kFreshSaveUs = 6'000'000;
  // A repeated ask within kReaskUs of the last one taken is ignored, so a
  // guest cannot make the host send its save over and over; after that it is
  // taken again (a guest whose ask or answer was lost asks again: GuestSession
  // reask_due; or a rejoin under the same number before PLAYER_LEFT came).
  // `now_us`: a steady clock in microseconds. Returns whether it was taken.
  static constexpr std::uint64_t kReaskUs = 10'000'000;
  // `fresh_after`: the host plays a file (not the title, not a new game never saved), the folder's clock now: the
  // ask opens a round (or joins the open one) and waits for a save written after it. Without it the ask is
  // answered at the next look, as before.
  bool on_ask(std::uint8_t player, std::uint64_t now_us, std::optional<Time> fresh_after = std::nullopt) {
    if (const auto it = answered_.find(player); it != answered_.end() && now_us - it->second < kReaskUs) {
      return false;
    }
    answered_[player] = now_us;
    asks_.insert(player);
    if (fresh_after && !round_) {
      round_ = Round{*fresh_after, now_us, true};
    }
    return true;
  }
  // Once a round: whether the host's game is to be asked for its autosave now (the hook makes the call).
  bool take_save_request() {
    if (!round_ || !round_->request) return false;
    round_->request = false;
    return true;
  }
  // An ask waits for the save of a round.
  bool round_open() const { return round_.has_value(); }
  // `slot_known`: the host knows the slot it loaded (PlayedSlot): that slot on disk is the save just loaded, so it
  // goes at once; otherwise the send waits for the game's next save (then the newest slot is the one played).
  void on_host_load(Time now, bool slot_known = false) {
    reload_ = true;
    if (slot_known) {
      loaded_at_.reset();
    } else {
      loaded_at_ = now;
    }
  }
  // A player left: its ask goes, and a player with its number may ask again.
  void forget(std::uint8_t player) {
    asks_.erase(player);
    answered_.erase(player);
    fresh_owed_.erase(player);
    if (asks_.empty()) round_.reset();   // (nobody waits for its save any more)
  }
  // A Send that could not go out (the outbox full): owed again at the next look. A player who left meanwhile is
  // not asked for again.
  void retry(const Send& send) {
    if (send.to_player == 0) {
      reload_ = true;  // (loaded_at_ is gone: the next look sends)
    } else if (answered_.count(send.to_player) != 0) {
      asks_.insert(send.to_player);
      if (send.fresh) fresh_owed_.insert(send.to_player);   // (the save of its round, not an older one)
    }
  }
  // Nothing owed: no ask and no reload.
  bool idle() const { return !reload_ && asks_.empty(); }
  // Whether the send to every guest waits for the game's next save after the host's load.
  bool waiting() const { return reload_ && loaded_at_.has_value(); }
  // What to send now, given the newest slot's write time (nothing when the
  // host has no save yet: then each ask gets a no_save Send). Each Send is
  // handed out once. `now_us`: the clock of on_ask (a round's time limit).
  std::vector<Send> due(std::optional<Time> newest_write, std::uint64_t now_us = 0);

 private:
  struct Round {
    Time since;               // the folder's clock at the ask that opened it
    std::uint64_t start_us;   // on_ask's clock then
    bool request;             // take_save_request not taken yet
  };
  std::optional<Round> round_;
  std::set<std::uint8_t> fresh_owed_;   // a fresh Send that could not go out (retry)
  std::set<std::uint8_t> asks_;
  std::map<std::uint8_t, std::uint64_t> answered_;  // asks taken (when), until the player leaves
  bool reload_ = false;
  std::optional<Time> loaded_at_;  // the host's last load, until a save after it
};

// The slot the host's game plays (player report 120). The host used to answer every join with its newest slot on
// disk by write time, which is another file whenever the player loaded an older slot (or saved another one earlier):
// the guest got a save the host was not playing. Hookshot's patch 0025 reports each open of a slot's progress.sav
// on the account save; patch 0024 (when there) brackets each save the game loads (title, System > Load, Game Over's
// Try Again). Only definite signs set the slot:
// - a write: the game saves into the slot it plays;
// - a read inside a load window: the save being loaded (a load from temporary storage, Try Again's, opens none:
//   the slot stays the one played). Each open and each window mark carries patch 0025's count of opens at that
//   moment, so the order holds however the marks (game thread) and the polled opens (once a frame) arrive: a read
//   counted after the window's start is inside it, one counted before is not;
// - a read with no load windows on this runtime (no patch 0024): the last read when the load is seen.
// The title's menu (a new pick follows) forgets the slot; after it, a load with no read in its window leaves it
// unknown, and a game entered with no load since the title is a new game (nothing saved yet).
class PlayedSlot {
 public:
  using Path = std::filesystem::path;
  enum class Source : std::uint8_t { none, write, load_read, last_read };
  // An open of `slot_dir`'s progress.sav (`write`: opened for writing); `opens`: the count with it.
  void on_open(const Path& slot_dir, bool write, unsigned opens) {
    if (write) {
      set(slot_dir, Source::write);
    } else if (in_window_ && opens != window_opens_) {
      window_read_ = slot_dir;
    } else {
      last_read_ = slot_dir;
    }
  }
  // Patch 0024's load window: a save load starts (`start`) or the save is in, with the opens as they were then
  // (`opens`, the last one's `slot_dir` and `write`).
  void on_load_window(bool start, unsigned opens, const Path& slot_dir, bool write) {
    windows_ = true;
    if (start) {
      in_window_ = true;
      window_opens_ = opens;
      window_read_.reset();
      return;
    }
    in_window_ = false;
    loaded_ = true;
    if (opens != window_opens_ && !write && !slot_dir.empty()) {
      set(slot_dir, Source::load_read);   // the game read a slot between the two marks: the one it loaded
    } else if (window_read_) {
      set(*window_read_, Source::load_read);
    }
    window_read_.reset();
  }
  // The host's save load as the flag watcher sees it (no window on this runtime: the last read is the one loaded).
  void on_load_seen() {
    loaded_ = true;
    if (!windows_ && last_read_) set(*last_read_, Source::last_read);
  }
  // The title's menu is up: the player picks again (a file, or New Game).
  void on_title() {
    slot_.reset();
    source_ = Source::none;
    loaded_ = false;
    last_read_.reset();
    in_window_ = false;   // (the game's own start begins a load with no save in after it: closed here)
    window_read_.reset();
  }
  const std::optional<Path>& slot() const { return slot_; }
  Source source() const { return source_; }
  // In the game with no load since the title, on a runtime that shows loads: a new game, nothing saved yet.
  bool new_game(bool in_game, bool title_seen) const {
    return windows_ && title_seen && in_game && !loaded_ && !slot_;
  }

 private:
  void set(const Path& p, Source s) {
    slot_ = p;
    source_ = s;
  }
  std::optional<Path> slot_;
  Source source_ = Source::none;
  std::optional<Path> last_read_;
  std::optional<Path> window_read_;
  bool in_window_ = false;
  unsigned window_opens_ = 0;  // the opens at the window's start
  bool windows_ = false;  // patch 0024's windows came at least once
  bool loaded_ = false;   // a load since the title
};

// The slot to send: `played` when it is a slot_NN folder of `game_dir` with a progress.sav, else the newest
// (newest_slot). `played_used` says which.
std::optional<std::filesystem::path> slot_to_send(const std::filesystem::path& game_dir,
                                                  const std::optional<std::filesystem::path>& played,
                                                  bool* played_used = nullptr);

// The newest slot_NN folder of a game save folder (by progress.sav's write
// time), or nothing.
std::optional<std::filesystem::path> newest_slot(const std::filesystem::path& game_dir);
std::optional<std::filesystem::file_time_type> newest_write(const std::filesystem::path& game_dir);
// A slot folder's files (progress.sav must be there).
std::optional<SaveSlot> read_slot(const std::filesystem::path& slot_dir);

// Whether a folder is a co-op save folder (its name ends in kCoopSuffix).
bool is_coop_dir(const std::filesystem::path& dir);
// The co-op folder of a game save folder, and back.
std::filesystem::path coop_dir_of(const std::filesystem::path& own_dir);
std::filesystem::path own_dir_of(const std::filesystem::path& coop_dir);

// The first multiplayer saves (the hook's solo/multiplayer save sets, coop_save ChooseSaveSet): `from` copied whole
// to `to`. The copy goes to `to` + ".part" and is renamed to `to` only once it is complete, so a failed copy never
// leaves a half `to` that would be played from ever after (a ".part" left by an earlier try is removed first). `from`
// is only read. False when `to` already exists (nothing done), when `from` is not a folder, or on a failure (`to` then
// absent, `error` says why).
bool copy_dir_whole(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error);

enum class WriteResult {
  written,
  not_coop,  // refused: the folder is not a co-op save folder (never a player's own saves)
  failed,    // a file could not be written
};
// Writes the host's slot into a co-op save folder: into slot_00, every other
// slot_NN already there and every slot_NN of `own_dir` (so whichever slot the
// load menu offers is the host's). Each slot is written whole into
// slot_NN.part (a name the game and newest_slot never use) and then renamed to
// slot_NN, so a half-written slot is never loadable. The player's own
// option.sav and account.sav are copied in from `own_dir` when the co-op
// folder has none yet (copied, never moved). Refuses any folder that is not
// a co-op folder.
WriteResult write_coop_save(const std::filesystem::path& coop_dir, const SaveSlot& slot,
                            const std::filesystem::path& own_dir);

// Which save folder a game plays from (issue 167). Never from coop.ini's address: an invite join is 127.0.0.1
// (the join helper) and an old address may be a friend's while this player now hosts.
// - Where the save folder can be switched while the game runs (Hookshot's patch 0008): the session decides at the
//   first connection that has a host, whatever the start was (the title menu can Host or Join after any start): a guest
//   plays from the co-op folder, the host from its own. A guest that already has a save loaded (Link in the game: a
//   Join from the pause menu, a rejoin during play) is `coop_later`: it asks for the host's save all the same, but
//   its folder is not touched under the game being played (that would mix two saves): the save is written and
//   switched to on the title's menu (pending_step), and the chat tells the player to go there. Before, such a guest
//   kept its own saves and never asked, so it had only the shared flags and nothing of the host's unshared state
//   (player reports 107 and 120). `env` is not used there.
// - Where it is fixed when the game starts (the emulator): `env`, LICHII_COOP_HOST_SAVE when set (play.ps1: 1 for a
//   join, by address or invite; 0 for a host; or coop-options.ini), else its own.
enum class FolderChoice { wait, own, coop, coop_later };
FolderChoice folder_choice(std::optional<bool> env, bool can_switch, bool connected, bool is_host, bool in_game);
// Where the folder can be switched, the choice is per SESSION (the title menu: Join, back to the title, then Host in
// the same run plays from its own saves again): a choice made goes when the session that made it is over (`over`: a
// new client, or the client idle, stopped or refused; a reconnect is not), but never with a save loaded (Link in
// the game: no switch then).
bool decide_again(bool decided, bool over, bool in_game);
// A choice kept through a session's end because a save was loaded (decide_again says no), and now a NEW session is
// connected while Link is still in the game (a rejoin from the pause menu, or the player now hosts): that session
// chooses again at once, without touching the folder (folder_choice's `own` for the host, `coop_later` for a guest).
// `ended`: the session the choice was made for was seen over since.
bool rejoined_in_game(bool decided, bool ended, bool connected, bool in_game);

// A guest's pending switch to the co-op folder (Hookshot), each frame: `go` only on the title's menu with the host's
// save in and no file loaded (never during a load, where a re-mount changes the folder under the game's read: the
// 10.1 crash; never under a game being played). Else `keep`: also while Link is in the game (the player loaded
// their own file or a new game, maybe too early): the save still comes in and is switched to when they are back on
// the title's menu, so "load a save after Success" holds. The hook holds it while the player's own pick on the
// title loads (CoopSave::CancelPendingSwitch, ResumePendingSwitch).
enum class PendingStep { keep, go };
PendingStep pending_step(bool slot_in, bool title_up, bool in_game);
// A guest that asked without a switch (a join with a file loaded, Resync from host: AskAtTitle) while its game
// already plays from the co-op folder (it played an earlier host's save) holds the OLDER host's save there. Once it
// is on the title's menu with no file loaded and the new save not in yet, the title would offer that older one: the
// game goes back to its own folder first and the co-op folder is emptied, as on any join (folder_choice's `coop`),
// and the new save is then written and switched to as always. `on_pending_dir`: the folder mounted is the pending
// co-op folder.
bool old_coop_first(bool pending, bool slot_in, bool title_up, bool in_game, bool on_pending_dir);

// A guest's save state for ONE session (CoopSave): the ask sent, the parts coming in, a slot that came before the
// save folder was open, whether the host's save is written, the "stuck" note said. A new session starts from a
// fresh one (GuestSession{}), so a second Join in the same run asks again and gets the save again.
struct GuestSession {
  Assembler assembler;
  std::optional<SaveSlot> unwritten;
  bool asked = false;
  bool ready = false;
  bool no_save = false;   // the host said it has no save yet (pack_no_save)
  bool said_stuck = false;
  std::uint64_t last_part_us = 0;
  std::uint64_t asked_us = 0;   // when the last ask went out
  int asks = 0;                 // asks sent this session
  bool gave_up = false;         // the last ask got nothing whole: the guest stopped asking (give_up_due)
  std::uint32_t transfer = 0;   // the transfer id of the last part that came (note_part)
};

// A part of the host's save came (transfer `transfer_id`) at `now_us`. A new transfer (the host loaded another
// save, or answered again) starts fresh: gave_up ends, so it can give up (and say so) again if it stalls too.
void note_part(GuestSession& s, std::uint32_t transfer_id, std::uint64_t now_us);

// The host's save came but could not be written or switched to: the ask is over (nothing is on its way), so the
// request state is cleared. Without this "asked && !ready" read as "coming" for the rest of the session (Resync
// then said "already on its way" for good). A later ask (Resync from host) starts a fresh GuestSession.
void note_write_failed(GuestSession& s);

// Is the host's save on its way: a slot waiting for the title, a transfer with a part in the last `stuck_us`, or an
// ask still waiting for its answer (not given up, not refused, not failed).
bool save_coming(const GuestSession& s, std::uint64_t now_us, std::uint64_t stuck_us);

// A guest asks again kGuestReaskUs after its last ask when nothing at all came (the ask or the whole answer was
// lost, or the host refused it as too soon), or when a transfer stopped part way and no part came for
// kGuestReaskUs (a part lost: the host sends the whole save again, under a new transfer id); at most
// kGuestMaxAsks times a session. Not while parts come in, once the save (or "no save") is in, or once the slot
// waits to be written.
inline constexpr std::uint64_t kGuestReaskUs = 15'000'000;
inline constexpr int kGuestMaxAsks = 4;
bool reask_due(const GuestSession& s, std::uint64_t now_us);
// Once a session: the kGuestMaxAsks-th ask got nothing whole either (it would be due again, but the asks are used
// up). The guest stops waiting: Loading ends (GuestNotices::on_gave_up) and the player plays on.
bool give_up_due(const GuestSession& s, std::uint64_t now_us);

// The Lobby tab's "Resync from host" (player report 120): a guest asks for the host's save again at any time. The
// same path as on joining (SAVE_ASK, asked again until it is whole, GuestNotices in the chat); the save is written
// and switched to only on the title's menu (pending_step), so a press during a cutscene, a load or play never
// touches the running game. Refused for the host, without a session, where the save folder cannot switch (the
// emulator: its folder is fixed at the start), and while a transfer is still coming in.
enum class ResyncCheck : std::uint8_t { go, not_guest, no_switch, coming };
ResyncCheck resync_check(bool connected_guest, bool can_switch, bool coming);
// Back on the title (the player's Quit, Game Over's Quit Game, any return), a guest who played a file in this session
// asks for the host's save again by itself, the same ask as Resync from host (AskAtTitle): the next Load or Continue is
// the host's save, never the file it played before (its own copy of the world). Unlike the button it never explains
// itself: when it does not go, nothing is said. It goes only when all hold:
struct TitleResync {
  bool title_came = false;         // the title's menu has just come up
  bool connected_guest = false;    // a connected guest of a live session (CanResync)
  bool played_in_session = false;  // a file was played in this session (on joining the save is asked anyway)
  bool on = true;                  // LICHII_COOP_TITLE_RESYNC is not 0
  bool can_switch = false;         // this game's save folder can switch (not the emulator's fixed one)
  bool plays_hosts_save = false;   // the guest plays from the co-op folder (not LICHII_COOP_TEST_OWN_SAVE, not a
                                   // failed folder switch): a guest that keeps its own saves keeps them
  bool ask_waiting = false;        // an ask already waits for the title or a save is coming (a join with a file
                                   // loaded, the button, a transfer under way)
};
bool title_resync_due(const TitleResync& t);

// A guest's chat lines about the host's save (the owner's wording). The player loads the save themselves (nothing
// presses the title's buttons); these lines say when:
// - kLoading once per transfer, when its first part comes;
// - kSuccessTitle / kSuccessPlaying once it is written whole and the game plays from the co-op folder (at the title,
//   or in the game: a join during play, or the host loaded another save);
// - kTooEarly once per transfer if the player loads a save (Link comes into the game) after kLoading and before
//   Success.
class GuestNotices {
 public:
  // `in_game`: Link is in the game when these lines start (a join or a Resync during play): that file was loaded
  // before any host's save was asked for, so it is never "too early".
  explicit GuestNotices(bool in_game = false) : was_in_game_(in_game) {}
  static constexpr std::string_view kLoading = "Host save loading, don't load a save.";
  static constexpr std::string_view kSuccessTitle = "Success, load any save.";
  static constexpr std::string_view kSuccessPlaying = "Success, load any save for changes to occur.";
  static constexpr std::string_view kTooEarly =
      "That save loaded before the host's save was in: go back to the title and load a save after Success.";
  static constexpr std::string_view kGaveUp = "The host's save did not come: play on, or join again to get it.";
  // A guest that joined with a file loaded (FolderChoice::coop_later).
  static constexpr std::string_view kJoinedPlaying =
      "You joined with a file loaded: the host's save is coming. To play from the host's progress, go back to the "
      "title and load a save once the chat says Success.";
  // A guest back on the title after playing in the session (save::title_resync_due): the host's save is asked for.
  static constexpr std::string_view kTitleAsked = "The host's save is coming: load a save once the chat says Success.";
  // A part of transfer `transfer_id` (of `count` parts) came: kLoading when it is a new transfer of a save, else "".
  // A "no save" answer is one part (pack_no_save: a few bytes); a save slot is about 170 KB, many parts.
  std::string on_part(std::uint32_t transfer_id, std::size_t count);
  // The host answered "no save", or the transfer ended without a save to play (damaged, not a slot, the write or
  // the folder switch failed): no Loading any more (the caller says what happened), no kTooEarly.
  void on_no_save() { loading_ = false; }
  void on_failed() { loading_ = false; }
  // The guest stopped asking (give_up_due): no Loading any more, no kTooEarly; kGaveUp.
  std::string on_gave_up();
  // The save is written and played from: kSuccessTitle when Link is not in the game, else kSuccessPlaying.
  std::string on_ready(bool in_game);
  // Each frame: Link in the game or not. kTooEarly when Link came in while the save was still loading.
  std::string on_in_game(bool in_game);
  bool loading() const { return loading_; }

 private:
  bool started_ = false;
  std::uint32_t transfer_ = 0;
  bool loading_ = false;
  bool said_early_ = false;
  bool was_in_game_ = false;
};

// The slot_NN folders of a co-op save folder taken away, so it holds nothing until the host's save comes (a guest
// switching to it must not find an older host's save and load that). Refuses a folder that is not a co-op one.
// Returns the folders removed, or -1.
int clear_coop_slots(const std::filesystem::path& coop_dir);

}  // namespace coop::host::save
