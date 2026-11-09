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
// flag). The key of a FlagEntry.
struct FlagKey {
  std::uint32_t hash = 0;
  std::uint16_t index = 0;
  auto operator<=>(const FlagKey&) const = default;
};

inline FlagKey key_of(const protocol::FlagEntry& entry) { return {entry.hash, entry.index}; }

// What the emulator (or later the recomp) implements: read and write one flag
// in the running game by its hash and index. The game's own get and set
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
};

// The flags this session shares, sorted, each once. Loaded from plain text,
// one flag a line:
//
//   # a comment; blank lines are fine
//   SomeFlagName          a name, hashed with flag_hash, index 0
//   0x15b67d0e            a hash, 0x and 1 to 8 hex digits
//   SomeArrayFlag 3       one element of an array flag
//   SomeArrayFlag 0-7     elements 0 to 7
//
// A name is printable ASCII with no spaces. A token of 0x and hex digits only
// is always read as a hash.
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
  const std::vector<FlagKey>& keys() const { return keys_; }
  std::size_t size() const { return keys_.size(); }
  bool empty() const { return keys_.empty(); }

 private:
  std::vector<FlagKey> keys_;  // sorted, unique
};

}  // namespace coop::host
