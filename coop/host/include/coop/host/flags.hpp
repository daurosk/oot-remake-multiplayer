#pragma once

// Shared world flags, the game side (plan stage 1 step 4, docs/host.md).
// What the emulator gives us (FlagStore), the flag name hash, and the list of
// flags that are shared (SharedFlags). No game data here: the list comes from
// a file the user supplies.

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

// The hash of a flag's name in the game: Murmur3 x86 32-bit, seed 0 (the
// game's 0x1739e0 with seed 0). The same function the codec uses.
using protocol::flag_hash;

// One flag: its name hash and, for an array flag, the element (0 for a plain
// flag), or a 64-bit key (chests and other placed actors). The key of a
// FlagEntry.
using protocol::FlagKey;
using protocol::FlagKeyKind;

inline FlagKey key_of(const protocol::FlagEntry& entry) { return entry.key; }

// What the emulator (or later the recomp) implements: read and write one flag
// in the running game by its key. The game's own get and set
// functions are not located yet (docs/host.md); until they are, an
// implementation may go through the flag tables in memory.
class FlagStore {
 public:
  virtual ~FlagStore() = default;

  // The flag's current value, with its kind, or nothing when it cannot be
  // read (unknown hash, table not loaded, unmapped memory). Must not throw.
  virtual std::optional<protocol::FlagEntry> read(FlagKey key) = 0;

  // Writes the flag. False when it could not (unknown hash, a kind that does
  // not match the flag, unmapped memory). Must not throw.
  virtual bool write(const protocol::FlagEntry& entry) = 0;

  // True for a count (small keys, Gold Skulltula tokens, heart pieces): an
  // int32 flag whose changes add up (docs/items.md). A player's watcher sends
  // its change of one as a count delta, and write() then takes a
  // FlagKind::count_delta entry and adds it. Default: no counts.
  virtual bool is_count(FlagKey) { return false; }

  // True for a Bool the game never takes back once set (a dungeon's map and
  // compass, issue 146): FlagWatcher writes it back to 1 when the game's
  // value drops to 0, at any time, not only after a load. Default: none.
  virtual bool is_one_way(FlagKey) { return false; }
};

// The flags this session shares, sorted, each once. Loaded from plain text,
// one flag a line:
//
//   # a comment; blank lines are fine
//   SomeFlagName          a name, hashed with flag_hash, index 0
//   0x15b67d0e            a hash, 0x and 1 to 8 hex digits
//   SomeArrayFlag 3       one element of an array flag
//   SomeArrayFlag 0-7     elements 0 to 7
//   key64:0x2fa4c1d09b7e6a35   a 64-bit-key flag, 1 to 16 hex digits
//   key64:3433081917669272117  the same key in decimal, as Banc files give it
//
// A name is printable ASCII with no spaces. A token of 0x and hex digits only
// is always read as a hash, and one that starts with key64: always as a
// 64-bit key (in the game's one Bool64bitKey table).
//
// Only persistent 64-bit keys belong in the list: ResetTypeValue 24 in the
// game's flag list (chests). Pots and enemies (88, 280) reset on a scene
// change; shared, one player's broken pot would break for everyone
// (docs/flags.md).
class SharedFlags {
 public:
  // Most flags one list may hold; a longer list is refused.
  static constexpr std::size_t kMaxFlags = 1u << 20;

  SharedFlags() = default;
  explicit SharedFlags(std::vector<FlagKey> keys);

  // Nothing on a bad line; `error`, when given, then says which and why.
  static std::optional<SharedFlags> parse(std::string_view text,
                                          std::string* error = nullptr);
  static std::optional<SharedFlags> load(const std::filesystem::path& path,
                                         std::string* error = nullptr);

  bool contains(FlagKey key) const;
  // Adds one key at run time (a one-time pickup seen in the game). False when
  // it was there already or the list is full.
  bool add(FlagKey key);
  const std::vector<FlagKey>& keys() const { return keys_; }
  std::size_t size() const { return keys_.size(); }
  bool empty() const { return keys_.empty(); }

 private:
  std::vector<FlagKey> keys_;  // sorted, unique
};

}  // namespace coop::host
