#pragma once
// Small keys lent to our game only (dungeon-sync-b, player report #75).
//
// A small-key door opens only for the game whose player used a key on its
// lock: the door set's graph (Logic/DungeonDoorSingleWithChainLockSet*)
// unlocks the door on a pulse of the lock's ReceiveDoAttention, and deleting
// the lock in another game sends no pulse. So a lock another player opened
// stays shut in our game, and when our Link waits at it the world sync lends
// our game one key (docs/dungeon.md, "Locked doors"): our player opens the
// lock the game's own way.
//
// The group paid for that door already, so the lent key must never reach the
// group: not the +1, not our player's spend of it, not its take-back. LentKeys
// keeps, per count, how many lent keys our game holds on top of the group's
// count, and LentKeyStore shows the flag watcher the game's count minus them.
// Our game's own drop of a count while it holds lent keys spends a lent key
// first (that is the lock being opened); a gain (a key found) is the game's
// own and shows as usual. A total the group writes is written plus the lent
// keys, so a lent key is not lost to another player's spend.
//
// lend(), give_back(), forget() and take_spent() may be called from any
// thread; frame() and the store run on the game's thread, before the watcher.
// Reads see a write still in the game's queue (game_flags.hpp), so a change
// is counted the frame it is made.
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "coop/host/flags.hpp"

namespace coop::host {

class LentKeys {
 public:
  // One key to lend for this count (the dungeon's SmallKeyNum flag).
  void lend(FlagKey count);
  // One lent key of this count not used: taken out of our game again.
  void give_back(FlagKey count);
  // A save load or a new session: the counts are the save's or the group's
  // now. Lent keys are forgotten, nothing is written.
  void forget();
  // The lent keys our game spent since the last call, one entry each.
  std::vector<FlagKey> take_spent();

  // Game thread, once a frame, before the flag watcher's frame: writes what
  // lend() and give_back() asked into `game` (the item store, below the
  // watcher), and sees our game's own spends. Log lines for what it did.
  std::vector<std::string> frame(FlagStore& game);

  // Lent keys our game holds for this count now.
  std::int32_t lent(FlagKey count) const;
  static constexpr std::int32_t kMaxLent = 8;  // per count: a bound, never reached by WorldSync

 private:
  friend class LentKeyStore;
  struct Count {
    std::int32_t lent = 0;
    std::optional<std::int32_t> seen;  // the game's own value, last read or written
  };
  // The game's value read now: our game's own drop spends lent keys first.
  // Returns what the watcher sees.
  std::int32_t observe(FlagKey key, std::int32_t game);
  void note_written(FlagKey key, std::int32_t game);

  mutable std::mutex mutex_;
  std::map<FlagKey, Count> counts_;
  std::vector<FlagKey> asked_lends_;
  std::vector<FlagKey> asked_backs_;
  std::vector<FlagKey> spent_;
};

// The flag watcher's view of `inner`: counts with lent keys show without
// them. Everything else passes through.
class LentKeyStore final : public FlagStore {
 public:
  LentKeyStore(FlagStore& inner, LentKeys& lent) : inner_(inner), lent_(lent) {}

  std::optional<protocol::FlagEntry> read(FlagKey key) override;
  bool write(const protocol::FlagEntry& entry) override;
  bool write_exact(const protocol::FlagEntry& entry) override;
  bool write_load_back(const protocol::FlagEntry& entry) override;
  bool is_count(FlagKey key) override { return inner_.is_count(key); }
  bool is_max(FlagKey key) override { return inner_.is_max(key); }
  bool holds(FlagKey key) override { return inner_.holds(key); }
  bool busy(FlagKey key) override { return inner_.busy(key); }
  bool is_one_way(FlagKey key) override { return inner_.is_one_way(key); }

 private:
  FlagStore& inner_;
  LentKeys& lent_;
};

}  // namespace coop::host
