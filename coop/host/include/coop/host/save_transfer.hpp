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
// A guest's ask is answered with the newest slot. After the host's own save
// load (on_host_load) the newest slot on disk is not yet the loaded one (an
// older manual save loaded, a newer autosave on disk), so nothing goes out
// until the game writes a save after the load (the game saves at every door);
// then every guest gets it (host_load), the asks waiting included.
class Planner {
 public:
  using Time = std::filesystem::file_time_type;
  struct Send {
    std::uint8_t to_player = 0;  // 0: every guest (host_load)
    protocol::SaveReason reason = protocol::SaveReason::join;
    bool operator==(const Send&) const = default;
  };
  // A player's ask is answered once while it stays: a repeated ask (an honest
  // game asks once a run) is ignored, so a guest cannot make the host send
  // its save over and over. Returns whether the ask was taken.
  bool on_ask(std::uint8_t player) {
    if (answered_.count(player) != 0) return false;
    answered_.insert(player);
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
  // Nothing owed: no ask and no reload.
  bool idle() const { return !reload_ && asks_.empty(); }
  // Whether a send waits for the game's next save after the host's load.
  bool waiting() const { return (reload_ || !asks_.empty()) && loaded_at_.has_value(); }
  // What to send now, given the newest slot's write time (nothing when the
  // host has no save yet). Each Send is handed out once.
  std::vector<Send> due(std::optional<Time> newest_write);

 private:
  std::set<std::uint8_t> asks_;
  std::set<std::uint8_t> answered_;  // asks taken, until the player leaves
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
// Writes the host's slot into a co-op save folder: into slot_00 and every
// other slot_NN already there (so whichever slot the load menu offers is the
// host's), each file written to a .tmp and renamed over. The player's own
// option.sav and account.sav are copied in from `own_dir` when the co-op
// folder has none yet (copied, never moved). Refuses any folder that is not
// a co-op folder.
WriteResult write_coop_save(const std::filesystem::path& coop_dir, const SaveSlot& slot,
                            const std::filesystem::path& own_dir);

// Which save folder a game plays from (issue 167). Never from coop.ini's address: an invite join is 127.0.0.1
// (the join helper) and an old address may be a friend's while this player now hosts.
// - `env`: LICHII_COOP_HOST_SAVE when set (play.ps1: 1 for a join, by address or invite; 0 for a host): it decides.
// - Else, where the save folder can be switched while the game runs (Hookshot's patch 0008): the session decides at
//   the first connection that has a host: a guest (not the host) plays from the co-op folder, the host from its own.
//   A game that already has a save loaded (Link in the game) keeps its own: a switch then would mix two saves.
// - Else (the emulator, its folder fixed when the game starts): its own.
enum class FolderChoice { wait, own, coop };
FolderChoice folder_choice(std::optional<bool> env, bool can_switch, bool connected, bool is_host, bool in_game);

}  // namespace coop::host::save
