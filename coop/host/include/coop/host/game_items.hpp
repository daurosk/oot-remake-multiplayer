#pragma once

// The player's items as flags in the game's flag store, and how co-op merges
// them (docs/items.md). Caleb's rule: key items are given to everyone,
// rupees and other non-key items stay with each player, bottles are given to
// everyone but what is in them stays with each player.
//
// Items travel as ordinary shared flags (FlagWatcher). Two pieces here:
// - the item table: every item flag with its kind and rule, and the
//   SharedFlags list built from it;
// - ItemFlagStore, a FlagStore over the game's (GameFlagStore) that merges
//   an incoming item value with the one the game holds instead of
//   overwriting it. The host answers a FLAG_SET with what its game holds
//   after the write, so the merged value is what everyone gets.
//
// No game data beyond the flag names and their hashes.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "coop/host/flags.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

enum class ItemRule : std::uint8_t {
  max,         // shared: everyone takes the highest value (owned items, upgrade tiers)
  any,         // shared: true for everyone once true for anyone (IsGet, songs, map, ...)
  mirror,      // shared: the last change is copied (trade items: handing one over takes it)
  count,       // shared: each change (+1 got, -1 used) is added to everyone's value
  per_player,  // never sent (rupees, ammo, life, bottle contents, equipped gear)
};

struct ItemFlag {
  std::string_view name;    // the flag's name in the game
  std::uint32_t hash;       // flag_hash(name)
  protocol::FlagKind kind;  // boolean or int32
  ItemRule rule;
  // False for a rule not turned on yet. All are on now: counts travel as
  // count deltas (docs/protocol.md, "Counts").
  bool on;
};

// Every item flag of docs/items.md, sorted by hash, each hash once.
std::span<const ItemFlag> item_flags();
// The entry for `hash`, or null when it is not an item flag.
const ItemFlag* find_item(std::uint32_t hash);

// The flags to share: every entry that is on and not per player. With
// `with_off`, also the shared ones not on yet (for tests and trials).
SharedFlags item_shared_flags(bool with_off = false);

// What the game should hold after `incoming` arrives for a flag of `rule`
// and `kind` when it holds `local`. Values are raw FlagEntry bits. `base` is,
// for count, the value the sender's change started from (its change is
// incoming - base); without one, count takes `incoming`. per_player keeps
// `local`.
std::uint32_t merge_item(ItemRule rule, protocol::FlagKind kind, std::uint32_t local,
                         std::uint32_t incoming, std::optional<std::uint32_t> base = {});

// What the game gives with an item, besides the item itself: the ammo
// capacity (Int struct 0xf80f8077, one member per ammo) and the ammo it
// starts with. The game writes these in its own get scene; an item that
// arrives from another player has no get scene, so ItemFlagStore writes them
// when it gives a game an item it did not have (issue 128: a shared
// Slingshot came with 0/0 seeds). Each value is a floor: a flag already at
// or above it is left alone. Values follow the original game's get-item
// code: Slingshot and Bow come with 30 and the small bag or quiver, a bag
// upgrade fills the new capacity.
struct ItemGrant {
  std::uint32_t item;       // the item's PouchData.<item>.StockNum hash, or IsGet.<item>
  std::string_view name;    // the granted flag's name in the game
  std::uint32_t hash;       // the granted flag's hash
  protocol::FlagKind kind;  // boolean or int32
  std::int32_t value;       // at least this after the grant (bool: 1 = true)
};

// The grants of the item with StockNum hash `item` (empty for most items).
std::span<const ItemGrant> item_grants(std::uint32_t item);
// Every grant, sorted by item.
std::span<const ItemGrant> all_item_grants();

// A FlagStore that merges item flags on write and passes everything else
// (other world flags, 64-bit keys) through unchanged. Reads are the inner
// store's. Keeps no state beyond counters.
class ItemFlagStore final : public FlagStore {
 public:
  explicit ItemFlagStore(FlagStore& inner) : inner_(inner) {}

  std::optional<protocol::FlagEntry> read(FlagKey key) override { return inner_.read(key); }
  // An item flag: reads what the game holds, merges, and writes only when
  // that changes the value (true, without a write, when it does not). A
  // per-player flag or a kind that does not match the table is refused.
  // When the game's value cannot be read, writes `entry` as it came.
  // A count delta (FlagKind::count_delta) on a count item is added to the
  // game's value, held to 0 and up; it is refused on anything else, or when
  // the game's value cannot be read.
  bool write(const protocol::FlagEntry& entry) override;
  // A named count item (ItemRule::count).
  bool is_count(FlagKey key) override;
  // An item merged from 0 (or less, or false) to 1 or more (true) in the
  // game also raises its grants (item_grants) to their floor, per-player
  // flags included. An item the game holds already gets its capacity grants
  // only (mends a game that got it without them). Only writes through this
  // store do that; the game's own gets are left to the game.

  struct Stats {
    std::uint64_t merged = 0;   // item writes that changed the game's value
    std::uint64_t kept = 0;     // item writes that left the game's value as it was
    std::uint64_t refused = 0;  // item writes with the wrong kind, or per player
    std::uint64_t added = 0;    // count deltas added to the game's value
    std::uint64_t granted = 0;  // grant flags raised (ammo, capacity) with a new item
  };
  const Stats& stats() const { return stats_; }

 private:
  void grant(std::uint32_t item, bool new_item);

  FlagStore& inner_;
  Stats stats_;
};

}  // namespace coop::host
