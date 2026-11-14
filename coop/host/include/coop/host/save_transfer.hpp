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
class Planner {
 public:
  using Time = std::filesystem::file_time_type;
  struct Send {
    std::uint8_t to_player = 0;  // 0: every guest (host_load)
    protocol::SaveReason reason = protocol::SaveReason::join;
    bool no_save = false;  // the host has no save on disk: send pack_no_save()
    bool operator==(const Send&) const = default;
  };
  // A repeated ask within kReaskUs of the last one taken is ignored, so a
  // guest cannot make the host send its save over and over; after that it is
  // taken again (a guest whose ask or answer was lost asks again: GuestSession
  // reask_due; or a rejoin under the same number before PLAYER_LEFT came).
  // `now_us`: a steady clock in microseconds. Returns whether it was taken.
  static constexpr std::uint64_t kReaskUs = 10'000'000;
  bool on_ask(std::uint8_t player, std::uint64_t now_us) {
    if (const auto it = answered_.find(player); it != answered_.end() && now_us - it->second < kReaskUs) {
      return false;
    }
    answered_[player] = now_us;
    asks_.insert(player);
    return true;
  }
  void on_host_load(Time now) {
    reload_ = true;
    loaded_at_ = now;
  }
  // A player left: its ask goes, and a player with its number may ask again.
  void forget(std::uint8_t player) {
    asks_.erase(player);
    answered_.erase(player);
  }
  // A Send that could not go out (the outbox full): owed again at the next look. A player who left meanwhile is
  // not asked for again.
  void retry(const Send& send) {
    if (send.to_player == 0) {
      reload_ = true;  // (loaded_at_ is gone: the next look sends)
    } else if (answered_.count(send.to_player) != 0) {
      asks_.insert(send.to_player);
    }
  }
  // Nothing owed: no ask and no reload.
  bool idle() const { return !reload_ && asks_.empty(); }
  // Whether the send to every guest waits for the game's next save after the host's load.
  bool waiting() const { return reload_ && loaded_at_.has_value(); }
  // What to send now, given the newest slot's write time (nothing when the
  // host has no save yet: then each ask gets a no_save Send). Each Send is
  // handed out once.
  std::vector<Send> due(std::optional<Time> newest_write);

 private:
  std::set<std::uint8_t> asks_;
  std::map<std::uint8_t, std::uint64_t> answered_;  // asks taken (when), until the player leaves
  bool reload_ = false;
  std::optional<Time> loaded_at_;  // the host's last load, until a save after it
};

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
//   plays from the co-op folder, the host from its own. A game that already has a save loaded (Link in the game) keeps
//   its own: a switch then would mix two saves. `env` is not used there.
// - Where it is fixed when the game starts (the emulator): `env`, LICHII_COOP_HOST_SAVE when set (play.ps1: 1 for a
//   join, by address or invite; 0 for a host; or coop-options.ini), else its own.
enum class FolderChoice { wait, own, coop };
FolderChoice folder_choice(std::optional<bool> env, bool can_switch, bool connected, bool is_host, bool in_game);
// Where the folder can be switched, the choice is per SESSION (the title menu: Join, back to the title, then Host in
// the same run plays from its own saves again): a choice made goes when the session that made it is over (`over`: a
// new client, or the client idle, stopped or refused; a reconnect is not), but never with a save loaded (Link in
// the game: no switch then).
bool decide_again(bool decided, bool over, bool in_game);

// A guest's pending switch to the co-op folder (Hookshot), each frame: `go` only on the title's menu with the host's
// save in and no file loaded (never during a load, where a re-mount changes the folder under the game's read: the
// 10.1 crash; never under a game being played). Else `keep`: also while Link is in the game (the player loaded
// their own file or a new game, maybe too early): the save still comes in and is switched to when they are back on
// the title's menu, so "load a save after Success" holds. The hook holds it while the player's own pick on the
// title loads (CoopSave::CancelPendingSwitch, ResumePendingSwitch).
enum class PendingStep { keep, go };
PendingStep pending_step(bool slot_in, bool title_up, bool in_game);

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
};

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

// A guest's chat lines about the host's save (the owner's wording). The player loads the save themselves (nothing
// presses the title's buttons); these lines say when:
// - kLoading once per transfer, when its first part comes;
// - kSuccessTitle / kSuccessPlaying once it is written whole and the game plays from the co-op folder (at the title,
//   or in the game: a join during play, or the host loaded another save);
// - kTooEarly once per transfer if the player loads a save (Link comes into the game) after kLoading and before
//   Success.
class GuestNotices {
 public:
  static constexpr std::string_view kLoading = "Host save loading, don't load a save.";
  static constexpr std::string_view kSuccessTitle = "Success, load any save.";
  static constexpr std::string_view kSuccessPlaying = "Success, load any save for changes to occur.";
  static constexpr std::string_view kTooEarly =
      "That save loaded before the host's save was in: go back to the title and load a save after Success.";
  static constexpr std::string_view kGaveUp = "The host's save did not come: play on, or join again to get it.";
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
