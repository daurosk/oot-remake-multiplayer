#pragma once
// The game's flag writes as events and through its own setters, for a game
// whose functions can be wrapped and called (Hookshot's world hooks,
// docs/hookshot.md "World hooks"; layout and functions in docs/flags.md).
//
// - Events: every flag write waits in one of the game's write queues until
//   the applier (main + 0xb99980) takes it. queued_flag_writes() reads the
//   queues as they are just before the applier runs: the game's own list of
//   what it is about to write, with no diff of the tables. GameFlagEvents
//   carries them from the game's thread to the flag watcher.
// - Writes: GameFlagCalls holds the writes co-op wants until the game's calc
//   thread hands each to the game's own setter (GameFlags::use_calls), so
//   nothing of co-op writes into the game's queues.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "coop/host/bounded_queue.hpp"
#include "coop/host/flags.hpp"
#include "coop/host/game_memory.hpp"

namespace coop::host {

// The flag types co-op reads and writes. The values are the game's save type
// ids (docs/flags.md); key64 is the Bool64bitKey table, which has none.
enum class GameFlagType : std::uint8_t { boolean = 0, int32 = 2, enum32 = 6, uint32 = 20, key64 = 0xff };

// One write: `id` is the name hash, or the 64-bit key; `value` as the game's
// setter takes it (0 or 1 for a Bool and a key, the value's bits otherwise).
struct GameFlagWrite {
  GameFlagType type = GameFlagType::boolean;
  std::uint64_t id = 0;
  std::uint32_t value = 0;
  bool operator==(const GameFlagWrite&) const = default;
};

// The watcher's key of a write: a named flag (element 0) or a 64-bit key.
inline FlagKey key_of(const GameFlagWrite& w) {
  return w.type == GameFlagType::key64 ? FlagKey::key64_flag(w.id)
                                       : FlagKey(static_cast<std::uint32_t>(w.id), 0);
}

// The setter of each type, as an offset in main: (manager, value, hash or
// key), read from the instructions (docs/flags.md, "Functions").
constexpr std::uint64_t setter_offset(GameFlagType type) {
  switch (type) {
    case GameFlagType::boolean: return 0x682b20;
    case GameFlagType::int32: return 0x163b70;
    case GameFlagType::enum32: return 0x3e0980;
    case GameFlagType::uint32: return 0x57af30;
    case GameFlagType::key64: return 0x3761720;
  }
  return 0;
}

// The writes waiting in the queues of the manager at `manager` (0: the one
// main points to), oldest first per queue, each given to `sink`: Bool, Int,
// Enum, UInt, then the 64-bit keys (a slot's record index turned back into
// its key). Returns how many. Reads only; a slot another thread is still
// writing may come with its old value or be left out (an empty slot is
// skipped), so this is a list of flags to look at, not a log to replay.
std::size_t queued_flag_writes(GameMemory& memory, std::uint64_t manager,
                               const std::function<void(const GameFlagWrite&)>& sink);

// From the game's threads to the watcher's: a fixed ring, no lock, no
// allocation (bounded_queue.hpp). A push into a full ring is dropped and
// remembered (take_overflow), and the reader then looks at every flag.
class GameFlagEvents {
 public:
  static constexpr std::size_t kSlots = 4096;  // a power of two

  // Any thread, several at once.
  bool push(const GameFlagWrite& write);
  // False when empty.
  bool pop(GameFlagWrite& out) { return ring_.pop(out); }
  // True once after pushes were dropped.
  bool take_overflow() { return overflow_.exchange(false); }
  std::uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
  std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  void clear();

 private:
  BoundedQueue<GameFlagWrite, kSlots> ring_;
  std::atomic<bool> overflow_{false};
  std::atomic<std::uint64_t> pushed_{0}, dropped_{0};
};

// Writes waiting for the game's own setters. put() and pending() from any
// thread (the watcher's); run() on the game's calc thread.
class GameFlagCalls {
 public:
  // Most writes held; more are refused (the caller tries again later, as
  // with a full queue).
  static constexpr std::size_t kMost = 1u << 16;

  // Takes a write. One already waiting for the same flag gets the new value
  // and keeps its place. False when full.
  bool put(const GameFlagWrite& write);
  // The value waiting for this flag, if any: what a read must see until the
  // setter has run (after that it is in the game's queue).
  std::optional<std::uint32_t> pending(GameFlagType type, std::uint64_t id) const;
  // Up to `most` writes, oldest first, each to `call` (the game's setter).
  // `call` returns false when the game cannot take it now (its queue has no
  // room): that write and the rest stay for the next run. Returns how many
  // were handed over.
  std::size_t run(std::size_t most, const std::function<bool(const GameFlagWrite&)>& call);
  std::size_t size() const;
  bool full() const { return size() >= kMost; }
  void clear();
  std::uint64_t handed() const { return handed_.load(std::memory_order_relaxed); }

 private:
  using Id = std::pair<GameFlagType, std::uint64_t>;
  mutable std::mutex mutex_;
  std::map<Id, std::uint32_t> values_;
  std::deque<Id> order_;
  std::atomic<std::uint64_t> handed_{0};
};

}  // namespace coop::host
