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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/ticks.hpp"
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

// Gold Skulltula tokens (issue 89): PouchData.Item_Token_Gold.StockNum, a
// count raised only by a Skulltula's token. Taking the token sets the 64-bit
// key of that Skulltula's placement (Enemy_Skulltula_Gold) in the same scan.
inline constexpr std::uint32_t kGoldTokenCount = 0x04735978;
// The placement keys of all 100 Gold Skulltulas (the Enemy_Skulltula_Gold
// placements of every map, Child and Adult the same key; each key is in the
// game's key list), sorted. The hook shares them all from the start and
// guards the token count with them (FlagWatcher::guard_count).
std::span<const std::uint64_t> gold_skulltula_keys();

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

// Strength (issue 125). What Link can lift is PlayerStatus.LiftGrade, an
// Enum (None, Low, Middle, High), not the bracelet or gauntlet flags. A
// bomb is lifted out of the bag, so with None every pulled bomb is deleted
// at once. The game sets the grade when Link gets the item itself; a
// strength item shared by another player (or written into a save) leaves
// it at None. Hashes are flag_hash of the names.
namespace lift {
constexpr std::uint32_t kGrade = 0x59979321;  // PlayerStatus.LiftGrade
constexpr std::uint32_t kNone = 0xb6eede09, kLow = 0xd9e40095, kMiddle = 0xb6a1ccdf,
                        kHigh = 0x165af889;
constexpr std::uint32_t kGoron = 0xd1317941;   // PouchData.Armor_Child_Gauntlet_Goron.StockNum
constexpr std::uint32_t kSilver = 0x0012b8c0;  // PouchData.Armor_Adult_Gauntlet_Silver.StockNum
constexpr std::uint32_t kGold = 0x40ae1183;    // PouchData.Armor_Adult_Gauntlet_Gold.StockNum
}  // namespace lift

// The grade the held strength items give (Goron's Bracelet Low, Silver
// Gauntlets Middle, Golden Gauntlets High), when it is above `current`.
// Nothing to write otherwise, or when `current` is not one of the four.
std::optional<std::uint32_t> lift_grade_raise(std::int32_t goron, std::int32_t silver,
                                              std::int32_t gold, std::uint32_t current);

// Issue 190 (docs/items.md, "The ocarina"): D-pad Left takes out the ocarina
// named in PlayerStatus.EquipOcarina (String64, an actor name, "" by
// default). The game writes it when Link gets an ocarina himself; an ocarina
// shared by another player leaves it "", so nothing comes out. Every save of
// a player who got it from Saria holds "Item_Ocarina_Fairy".
namespace ocarina {
constexpr std::uint32_t kEquip = 0xb72cd3fc;  // PlayerStatus.EquipOcarina
constexpr std::uint32_t kFairy = 0xb7df60f1;  // PouchData.Item_Ocarina_Fairy.StockNum
constexpr std::uint32_t kTime = 0x54e2a3d1;   // PouchData.Item_Ocarina_Time.StockNum
constexpr std::string_view kFairyName = "Item_Ocarina_Fairy";
constexpr std::string_view kTimeName = "Item_Ocarina_Time";
// The Bools a real get sets beside the count (tools/autotest/save_flags.py
// NO_OCARINA clears the same ones for a player who never met Saria). A
// shared ocarina brought only the count (ocarina-given, bigrun 2026-11-12:
// B's EquipOcarina was mended, but D-pad Left still took nothing out).
constexpr std::uint32_t kIsGetFairy = 0x42bd296f;   // IsGet.Item_Ocarina_Fairy
constexpr std::uint32_t kIsGetTime = 0x1da2c511;    // IsGet.Item_Ocarina_Time
constexpr std::uint32_t kSlotA = 0xaacdef1f;        // 0x541947d0.Ocarina
constexpr std::uint32_t kSlotB = 0xda41879d;        // 0x57f52eb8.Ocarina
constexpr std::uint32_t kGotFairy = 0x137df1ac;     // 0x41f3e410.Item_Ocarina_Fairy
constexpr std::uint32_t kGotTime = 0xac6a85e3;      // 0x41f3e410.Item_Ocarina_Time
}  // namespace ocarina

// The ocarina to equip for the held ones (Ocarina of Time over the Fairy
// Ocarina), when `current` is "" or the Fairy Ocarina and a better one is
// held. Nothing otherwise: never lowered, and a name we do not know is left
// alone.
std::optional<std::string_view> ocarina_equip_mend(std::int32_t fairy, std::int32_t time,
                                                   std::string_view current);

// The ocarina Bools to raise for the held ones (ocarina::kIsGet*, kSlot*,
// kGot*): those `get` reads as false (not true, not unreadable). Only ever
// raised; nothing for an ocarina not held.
std::vector<std::uint32_t> ocarina_bools_mend(std::int32_t fairy, std::int32_t time,
                                              const std::function<std::optional<bool>(std::uint32_t)>& get);

// When to mend (ItemFlagStore::mend), kept by the hook across frames: for
// the first frames after Link comes into the game (a save or a scene
// loaded: no Link, then Link), once a frame. The load may still be writing
// the save's values in the first frames, and a mend is only floors, so it
// is run for a while rather than once. It also keeps the items that came
// while there was no Link: they get their whole grant (ammo too) in the
// mend frames (issues 128, 138).
class LoadMend {
 public:
  static constexpr unsigned kFrames = 90;  // 1.5 s: 1/60 s frames of set_clock's clock

  // Issue 106: frame() is called once per presented frame (up to 165 a
  // second with high refresh); with a clock kFrames counts real 1/60 s.
  // Without one each call is one frame.
  void set_clock(GameTicks::Clock clock) { ticks_.set_clock(std::move(clock)); }

  // Once per emulated frame, with whether Link is in the game. True: mend
  // this frame.
  bool frame(bool link) {
    const unsigned step = ticks_.step();
    if (link && !link_) left_ = kFrames;
    link_ = link;
    if (!link_) return false;
    if (left_ == 0) {
      pending_.clear();  // their mend frames are over
      return false;
    }
    left_ -= (std::min)(left_, step);
    return true;
  }
  // Link is in the game (as of the last frame()).
  bool ready() const { return link_; }
  // An item new to the game came while not ready (ItemFlagStore).
  void note_new(std::uint32_t item) {
    if (std::find(pending_.begin(), pending_.end(), item) == pending_.end()) pending_.push_back(item);
  }
  bool pending(std::uint32_t item) const {
    return std::find(pending_.begin(), pending_.end(), item) != pending_.end();
  }

 private:
  bool link_ = false;
  unsigned left_ = 0;
  GameTicks ticks_;
  std::vector<std::uint32_t> pending_;
};

// A FlagStore that merges item flags on write and passes everything else
// (other world flags, 64-bit keys) through unchanged. Reads are the inner
// store's. Keeps no state beyond counters.
class ItemFlagStore final : public FlagStore {
 public:
  // With `load`: grants only while Link is in the game (load->ready()).
  // Before that (the title screen, a save or scene loading) items are
  // merged but nothing is granted: the load would put the capacities back
  // to the save's (issue 138). An item new to the game then is noted in
  // `load` and gets its whole grant at the mend. Without `load`: always.
  explicit ItemFlagStore(FlagStore& inner, LoadMend* load = nullptr)
      : inner_(inner), load_(load), ready_(load == nullptr || load->ready()) {}

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
  // A named int32 item merged by the highest value (ItemRule::max).
  bool is_max(FlagKey key) override;
  // DungeonInfo.<dungeon>.IsGetMap and .IsGetCompass (issue 146).
  bool is_one_way(FlagKey key) override;
  bool busy(FlagKey key) override { return inner_.busy(key); }
  // The value as it is (no max, OR or mirror merging; per-player flags still
  // refused): the host's save decides (docs/flags.md, "Saves").
  bool write_exact(const protocol::FlagEntry& entry) override;
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

  // Every item the game holds gets its capacity grants; one that came while
  // there was no Link (LoadMend::pending) gets its whole grant, ammo too.
  // Run in LoadMend's frames after a load: the load puts back the save's
  // capacities, which a save from before the fix has at 0. Returns how many
  // flags it raised.
  std::size_t mend();

 private:
  void grant(std::uint32_t item, bool new_item);

  bool exact_ = false;  // write_exact: take the value as it is
  FlagStore& inner_;
  LoadMend* load_ = nullptr;
  bool ready_ = true;
  Stats stats_;
};

}  // namespace coop::host
