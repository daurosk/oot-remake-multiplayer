#pragma once
// The game's own flag store (gmd::GameDataMgr), read and written over guest
// memory. Layout and functions: docs/flags.md. Bool, Int and UInt flags by name
// hash, and the 64-bit-key flags (chests and other placed actors, keyed by the
// placement's 64-bit hash).
//
// Writes go into the game's own write queues, as the game's setters do, so
// the game applies them on its next GameData update with its own bookkeeping
// (saved copy, save-file mask). Reads see a queued write that is not applied
// yet, as the game's 0x20fd90 does.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <span>

#include "coop/host/flags.hpp"
#include "coop/host/game_memory.hpp"

namespace coop::host {

// Guest memory that can also be written. The emulator implements it.
class WritableGameMemory : public GameMemory {
 public:
  // Copies `data` to guest address `address`. False when any byte cannot be
  // written. Must not throw.
  virtual bool write(std::uint64_t address, std::span<const std::uint8_t> data) = 0;
  // Atomically: when the u32 at `address` equals `expected`, stores
  // `desired`. Returns the value that was there (equal to `expected` on
  // success), nothing when it cannot be accessed. The game's threads use
  // AArch64 CAS on the same words, so this must be atomic against them (or
  // run while the guest is paused).
  virtual std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t address,
                                                            std::uint32_t expected,
                                                            std::uint32_t desired) = 0;
};

// Reads and writes flags in the running game. Holds no state: every call
// follows the manager pointer again, so it is safe across scene changes and
// before the GameData module is up (calls then fail).
class GameFlags {
 public:
  explicit GameFlags(GameMemory& memory) : memory_(memory) {}

  // The flag's value, or nothing when the manager is not up, the hash is not
  // in that table, or memory cannot be read. Hash 0 is never a flag.
  std::optional<bool> get_bool(std::uint32_t hash);
  std::optional<std::int32_t> get_int(std::uint32_t hash);
  // A UInt flag (type 20; 0x57a4e0 reads it, 0x57af30 sets it). A hash only in
  // the game's fallback UInt table (M + 0x800, not read) is not found.
  std::optional<std::uint32_t> get_uint(std::uint32_t hash);
  // An Enum flag (type 6; 0x7e73d0 reads it, 0x3e0980 sets it): the value is
  // the flag_hash of the value's name (PlayerStatus.AssignableItem.Adult:
  // "Bow" is flag_hash("Bow")).
  std::optional<std::uint32_t> get_enum(std::uint32_t hash);
  // A 64-bit-key flag (Bool64bitKey). A key not in the table is nothing,
  // not false: the table holds every key the game knows.
  std::optional<bool> get_key64(std::uint64_t key);
  // Element `index` of a BoolArray flag. Read only.
  std::optional<bool> get_bool_array(std::uint32_t hash, std::uint32_t index);
  // A String64 flag (for example PlayerStatus.EquipWeapon, an actor name).
  // Read only, from the table as 0x3758110 reads it; a write still in the
  // game's queue is not seen until the game applies it. Nothing when the
  // text is not 0-terminated within its buffer or not printable ASCII.
  std::optional<std::string> get_string64(std::uint32_t hash);

  // Queues a write as the game's setter does. False when the flag is
  // unknown, the queue is full, the game applied the queue while this was
  // being written (try again next frame), or memory fails.
  static bool set_bool(WritableGameMemory& memory, std::uint32_t hash, bool value);
  static bool set_int(WritableGameMemory& memory, std::uint32_t hash, std::int32_t value);
  static bool set_uint(WritableGameMemory& memory, std::uint32_t hash, std::uint32_t value);
  static bool set_enum(WritableGameMemory& memory, std::uint32_t hash, std::uint32_t value);
  static bool set_key64(WritableGameMemory& memory, std::uint64_t key, bool value);

  // Test only (tools/autotest warp): writes a String64 flag's text straight
  // into its buffer, 0-terminated, without the game's queue, so the live
  // value changes at once. False when the flag is unknown or the text does
  // not fit (at most 63 bytes, printable ASCII).
  static bool set_string64_now(WritableGameMemory& memory, std::uint32_t hash,
                               std::string_view text);

  // Which typed table (the save type ids, 0 to 25) holds `hash`, for finding
  // a flag's type. Nothing when none does.
  std::optional<int> find_type(std::uint32_t hash);

 private:
  GameMemory& memory_;
};

// FlagStore over the game's tables, for FlagWatcher. A named key with index
// 0: a Bool flag (kind boolean) or else an Int flag (kind int32). Index > 0:
// an element of a BoolArray flag, read only. A key64 key: a Bool64bitKey
// flag (kind boolean) in table 0, the game's one table; any other table is
// unknown.
class GameFlagStore final : public FlagStore {
 public:
  explicit GameFlagStore(WritableGameMemory& memory) : memory_(memory) {}
  std::optional<protocol::FlagEntry> read(FlagKey key) override;
  bool write(const protocol::FlagEntry& entry) override;
  // The queue this flag's writes go to has half its slots taken or more.
  bool busy(FlagKey key) override;

 private:
  WritableGameMemory& memory_;
};

}  // namespace coop::host
