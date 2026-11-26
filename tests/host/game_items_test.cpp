#include <doctest/doctest.h>

#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/game_items.hpp"
#include "coop/protocol/codec.hpp"
#include "fake_memory.hpp"

namespace {

using coop::host::find_item;
using coop::host::FlagKey;
using coop::host::FlagRole;
using coop::host::FlagWatcher;
using coop::host::GameFlags;
using coop::host::GameFlagStore;
using coop::host::item_flags;
using coop::host::item_shared_flags;
using coop::host::ItemFlagStore;
using coop::host::ItemRule;
using coop::host::merge_item;
using coop::host::test::FakeMemory;
namespace protocol = coop::protocol;
using K = protocol::FlagKind;

// Item flags used below (docs/items.md).
constexpr std::uint32_t kHookshot = 0x8573d2cd;        // PouchData.Item_Hookshot.StockNum, max
constexpr std::uint32_t kHookshotGet = 0xff98ed58;     // IsGet.Item_Hookshot, OR
constexpr std::uint32_t kPocketEgg = 0x07f10aaf;       // PouchData.Item_PocketEgg.StockNum, mirror
constexpr std::uint32_t kRupees = 0xa77921d7;          // PlayerStatus.CurrentRupee, per player
constexpr std::uint32_t kBottleA = 0xd53ce3a7;         // PouchData.BottleA.StockNum, OR
constexpr std::uint32_t kBottleAValid = 0xf39ce410;    // BottleInfo.BottleA.IsValid, OR
constexpr std::uint32_t kBottleAItem = 0x7d48df76;     // BottleInfo.BottleA.BottleItem, per player
constexpr std::uint32_t kForestKeys = 0x90237092;      // DungeonInfo.ForestTemple.SmallKeyNum, count
constexpr std::uint32_t kGoldTokens = 0x04735978;      // PouchData.Item_Token_Gold.StockNum, count
constexpr std::uint32_t kHeartPieces = 0x51ec97b3;     // PouchData.Obj_Heart_Piece.StockNum, count
constexpr std::uint32_t kMaxLife = 0xfbe01da1;         // PlayerStatus.MaxLife, max
constexpr std::uint32_t kDekuShield = 0x7ca64f23;      // PouchData.Weapon_Shield_Deku.StockNum, max
constexpr std::uint32_t kWorldBool = 0x11, kWorldInt = 0x22;  // not items
constexpr std::uint32_t kDekuFloors = 0x5c6c21c1;  // DungeonInfo.InsideTheDekuTree.EnteredFloorBit, bits
constexpr std::uint32_t kWorldUInt = 0x33;         // a UInt that is not an item

// The GameDataMgr layout of docs/flags.md, as in game_flags_test.cpp, with
// item hashes in its Bool and Int tables.
class Writable final : public coop::host::WritableGameMemory {
 public:
  std::uint64_t main_base() const override { return mem.main_base(); }
  bool read(std::uint64_t a, std::span<std::uint8_t> out) override { return mem.read(a, out); }
  bool write(std::uint64_t a, std::span<const std::uint8_t> data) override {
    std::vector<std::uint8_t> probe(data.size());
    if (!mem.read(a, probe)) return false;
    mem.put(a, data.data(), data.size());
    return true;
  }
  std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t a, std::uint32_t expected,
                                                    std::uint32_t desired) override {
    const auto v = coop::host::read_u32(mem, a);
    if (v && *v == expected) mem.put_u32(a, desired);
    return v;
  }
  FakeMemory mem;
};

constexpr std::uint64_t kMgr = 0x8000000000ull;
constexpr std::uint64_t kBoolEntries = 0x8100000000ull, kBoolSlots = 0x8110000000ull,
                        kBoolQueue = 0x8120000000ull;
constexpr std::uint64_t kUIntEntries = 0x8500000000ull, kUIntSlots = 0x8510000000ull,
                        kUIntQueue = 0x8520000000ull;
constexpr std::uint64_t kIntEntries = 0x8200000000ull, kIntSlots = 0x8210000000ull,
                        kIntQueue = 0x8220000000ull;
constexpr std::uint32_t kCap = 17, kQueueCap = 16;
constexpr std::uint64_t kBoolStride = 0x18, kBoolValue = 0x16, kIntStride = 0x28, kIntValue = 0x1c;

void zero(FakeMemory& m, std::uint64_t a, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) m.put_u8(a + i, 0);
}

void table(FakeMemory& m, std::uint64_t base, std::uint64_t entries, std::uint64_t slots,
           std::uint64_t stride, const std::vector<std::uint32_t>& hashes) {
  m.put_u32(kMgr + base, static_cast<std::uint32_t>(hashes.size()));
  m.put_u64(kMgr + base + 8, entries);
  m.put_u64(kMgr + base + 0x10, slots);
  m.put_u32(kMgr + base + 0x1c, kCap);
  zero(m, slots, 8 * kCap);
  zero(m, entries, stride * hashes.size());
  std::uint32_t index = 0;
  for (const auto h : hashes) {
    std::uint32_t s = h % kCap;
    while (*coop::host::read_u32(m, slots + 8 * s) != 0) s = (s + 1) % kCap;
    m.put_u32(slots + 8 * s, h);
    m.put_u32(slots + 8 * s + 4, index++);
  }
}

void queue(FakeMemory& m, std::uint64_t base, std::uint64_t buf, std::uint64_t slot) {
  m.put_u32(kMgr + base + 0x20, kQueueCap);
  m.put_u64(kMgr + base + 0x28, buf);
  m.put_u32(kMgr + base + 0x30, 0x30000000);
  zero(m, buf, slot * kQueueCap);
}

const std::vector<std::uint32_t> kBools{kHookshotGet, kBottleAValid, kWorldBool};
const std::vector<std::uint32_t> kUInts{kDekuFloors, kWorldUInt};
const std::vector<std::uint32_t> kInts{kHookshot,   kPocketEgg,  kRupees, kBottleA,
                                       kBottleAItem, kForestKeys, kWorldInt, kMaxLife,
                                       kDekuShield};

std::size_t at(const std::vector<std::uint32_t>& v, std::uint32_t h) {
  for (std::size_t i = 0; i < v.size(); ++i)
    if (v[i] == h) return i;
  FAIL("not in the fake table");
  return 0;
}

Writable game() {
  Writable w;
  auto& m = w.mem;
  m.put_u64(FakeMemory::kBase + 0x4e842a8, kMgr);
  table(m, 0x10, kBoolEntries, kBoolSlots, kBoolStride, kBools);
  queue(m, 0x10, kBoolQueue, 8);
  table(m, 0xd0, kIntEntries, kIntSlots, kIntStride, kInts);
  queue(m, 0xd0, kIntQueue, 12);
  table(m, 0x790, kUIntEntries, kUIntSlots, kIntStride, kUInts);  // UInt: the Int layout 0x6c0 on
  queue(m, 0x790, kUIntQueue, 12);
  return w;
}

// Sets a value as the game holds it after its own update (not queued).
void hold_bool(Writable& w, std::uint32_t h, bool v) {
  w.mem.put_u8(kBoolEntries + kBoolStride * at(kBools, h) + kBoolValue, v ? 1 : 0);
}
void hold_int(Writable& w, std::uint32_t h, std::int32_t v) {
  w.mem.put_u32(kIntEntries + kIntStride * at(kInts, h) + kIntValue, static_cast<std::uint32_t>(v));
}
void hold_uint(Writable& w, std::uint32_t h, std::uint32_t v) {
  w.mem.put_u32(kUIntEntries + kIntStride * at(kUInts, h) + kIntValue, v);
}

// The game's applier: the queued Int writes go into the table and the queue
// is emptied (its round moves on), as the game does each frame. Without it a
// read keeps returning the newest queued value.
void apply_ints(Writable& w) {
  const std::uint64_t word_at = kMgr + 0xd0 + 0x30;
  const std::uint32_t word = *coop::host::read_u32(w.mem, word_at);
  for (std::uint32_t k = 0; k < (word & 0xfffff); ++k) {
    const std::uint32_t value = *coop::host::read_u32(w.mem, kIntQueue + 12 * k);
    const std::uint32_t hash = *coop::host::read_u32(w.mem, kIntQueue + 12 * k + 4);
    w.mem.put_u32(kIntEntries + kIntStride * at(kInts, hash) + kIntValue, value);
  }
  w.mem.put_u32(word_at, (((word >> 28) + 1) & 0xf) << 28);
}

protocol::FlagEntry b(std::uint32_t h, bool v) { return {h, 0, K::boolean, v ? 1u : 0u}; }
protocol::FlagEntry i(std::uint32_t h, std::int32_t v) {
  return {h, 0, K::int32, static_cast<std::uint32_t>(v)};
}
std::uint32_t bits(std::int32_t v) { return static_cast<std::uint32_t>(v); }

}  // namespace

TEST_CASE("game items: every name hashes to its hash, sorted, each once") {
  const auto items = item_flags();
  CHECK(items.size() == 318);  // IsGet.Item_Bottle and IsGet.Item_Bottle_Letter (issue 58), 14 dungeons' EnteredFloorBit (issue 146)
  std::uint32_t last = 0;
  for (const auto& item : items) {
    CAPTURE(item.name);
    CHECK(protocol::flag_hash(item.name) == item.hash);
    CHECK(item.hash > last);
    last = item.hash;
    CHECK(find_item(item.hash) == &item);
    // Owned counts are Int, IsGet and the dungeon items Bool.
    if (item.name.starts_with("PouchData.")) CHECK(item.kind == K::int32);
    const bool floors = item.name.ends_with(".EnteredFloorBit");
    if (floors) CHECK((item.name.starts_with("DungeonInfo.") && item.kind == K::uint32 && item.rule == ItemRule::bits));
    const bool dungeon_bool = item.name.starts_with("DungeonInfo.") && !item.name.ends_with(".SmallKeyNum") && !floors;
    if (item.name.starts_with("IsGet.") || dungeon_bool)
      CHECK((item.kind == K::boolean && item.rule == ItemRule::any));
    CHECK(item.on);  // every rule is on, counts included
  }
  CHECK(find_item(0) == nullptr);
  CHECK(find_item(kWorldInt) == nullptr);
}

TEST_CASE("game items: Alex's rule, item by item") {
  struct Want {
    std::uint32_t hash;
    ItemRule rule;
    bool on;
  };
  for (const auto& w : {
           Want{kHookshot, ItemRule::max, true},        // key item: shared
           Want{kHookshotGet, ItemRule::any, true},
           Want{kPocketEgg, ItemRule::mirror, true},    // trade item
           // Issue 171: Zelda's Letter on the max rule: showing it takes it
           // from the shower only (a max-rule drop is kept, never sent).
           Want{0x6be5c3b0, ItemRule::max, true},
           Want{kRupees, ItemRule::per_player, true},   // rupees stay with the player
           Want{0xdb1bcd4f, ItemRule::per_player, true},  // bombs (ammo)
           Want{0xbd3ca2c6, ItemRule::max, true},       // the bomb bag tier is shared
           Want{kBottleA, ItemRule::any, true},         // the bottle is shared
           Want{kBottleAValid, ItemRule::any, true},
           Want{kBottleAItem, ItemRule::per_player, true},  // what is in it is not
           Want{kForestKeys, ItemRule::count, true},
           Want{kGoldTokens, ItemRule::count, true},
           Want{kHeartPieces, ItemRule::count, true},
           Want{kMaxLife, ItemRule::max, true},
           Want{0x31ab5580, ItemRule::per_player, true},  // Life
           Want{0x01c20002, ItemRule::any, true},         // Zelda's Lullaby
       }) {
    CAPTURE(w.hash);
    const auto* item = find_item(w.hash);
    REQUIRE(item);
    CHECK(item->rule == w.rule);
    CHECK(item->on == w.on);
  }
}

TEST_CASE("game items: the shared list") {
  const auto shared = item_shared_flags();
  CHECK(shared.contains(FlagKey{kHookshot, 0}));
  CHECK(shared.contains(FlagKey{kBottleAValid, 0}));
  CHECK(shared.contains(FlagKey{kPocketEgg, 0}));
  CHECK_FALSE(shared.contains(FlagKey{kRupees, 0}));
  CHECK_FALSE(shared.contains(FlagKey{kBottleAItem, 0}));
  CHECK(shared.contains(FlagKey{kForestKeys, 0}));  // counts travel as deltas
  CHECK(shared.contains(FlagKey{kMaxLife, 0}));
  CHECK(shared.size() == 318 - 15);  // 15 per player (the Deku Shield and the FairyBless items are shared)
  const auto all = item_shared_flags(true);
  CHECK(all.contains(FlagKey{kForestKeys, 0}));
  CHECK(all.contains(FlagKey{kMaxLife, 0}));
  CHECK_FALSE(all.contains(FlagKey{kRupees, 0}));
  CHECK(all.size() == 318 - 15);  // the Deku Shield is shared again (issue 130)
}

TEST_CASE("game items: merge rules") {
  CHECK(merge_item(ItemRule::max, K::int32, 0, 1) == 1);
  CHECK(merge_item(ItemRule::max, K::int32, 2, 1) == 2);
  CHECK(merge_item(ItemRule::max, K::int32, bits(-1), 0) == 0);  // signed
  CHECK(merge_item(ItemRule::any, K::boolean, 0, 1) == 1);
  CHECK(merge_item(ItemRule::any, K::boolean, 1, 0) == 1);
  CHECK(merge_item(ItemRule::any, K::int32, 1, 0) == 1);  // a bottle stays owned
  CHECK(merge_item(ItemRule::mirror, K::int32, 1, 0) == 0);  // the egg was handed over
  CHECK(merge_item(ItemRule::max, K::int32, 1, 0) == 1);     // the letter shown elsewhere: ours stays
  CHECK(merge_item(ItemRule::mirror, K::int32, 0, 1) == 1);
  CHECK(merge_item(ItemRule::per_player, K::int32, 50, 99) == 50);
  // Count: the sender went from 2 to 3 keys (+1) while this game spent one (2 -> 1).
  CHECK(merge_item(ItemRule::count, K::int32, 1, 3, 2u) == 2);
  CHECK(merge_item(ItemRule::count, K::int32, 3, 2, 3u) == 2);  // -1 used
  CHECK(merge_item(ItemRule::count, K::int32, 5, 7) == 7);      // no base: as it came
  CHECK(merge_item(ItemRule::count, K::int32, 0x7fffffff, 1, 0u) == 0x7fffffffu);  // clamped
  // Bits: the union; nothing set is ever cleared (issue 146, a dungeon's floors).
  CHECK(merge_item(ItemRule::bits, K::uint32, 12, 31) == 31u);
  CHECK(merge_item(ItemRule::bits, K::uint32, 31, 12) == 31u);
  CHECK(merge_item(ItemRule::bits, K::uint32, 0b0110, 0b1001) == 0b1111u);
  CHECK(merge_item(ItemRule::bits, K::uint32, 0x80000000u, 0) == 0x80000000u);
}

TEST_CASE("game items: the store merges item writes in the game's flags") {
  auto w = game();
  GameFlagStore game_store(w);
  ItemFlagStore store(game_store);
  GameFlags flags(w);
  hold_int(w, kHookshot, 1);
  hold_int(w, kPocketEgg, 1);
  hold_int(w, kRupees, 50);
  hold_bool(w, kHookshotGet, true);

  // Max: a lower value leaves the game's as it is, and writes nothing.
  CHECK(store.write(i(kHookshot, 0)));
  CHECK(flags.get_int(kHookshot) == 1);
  CHECK(store.stats().kept == 1);
  CHECK(coop::host::read_u32(w.mem, kIntQueue + 4) == 0u);  // nothing queued
  // OR: false does not take a got item away; true on a new one is written.
  CHECK(store.write(b(kHookshotGet, false)));
  CHECK(flags.get_bool(kHookshotGet) == true);
  CHECK(store.write(b(kBottleAValid, true)));
  CHECK(flags.get_bool(kBottleAValid) == true);
  CHECK(store.write(i(kBottleA, 1)));
  CHECK(flags.get_int(kBottleA) == 1);
  // Mirror: the egg handed over goes away here too.
  CHECK(store.write(i(kPocketEgg, 0)));
  CHECK(flags.get_int(kPocketEgg) == 0);
  CHECK(store.stats().merged == 3);
  // Per player: refused, the game's rupees and bottle contents stay.
  CHECK_FALSE(store.write(i(kRupees, 999)));
  CHECK_FALSE(store.write({kBottleAItem, 0, K::uint32, 5}));
  CHECK(flags.get_int(kRupees) == 50);
  // The wrong kind for an item is refused.
  CHECK_FALSE(store.write(b(kHookshot, true)));
  CHECK(store.stats().refused == 3);
  // Not an item: passed through as it came.
  CHECK(store.write(i(kWorldInt, 7)));
  CHECK(flags.get_int(kWorldInt) == 7);
  CHECK(store.write(b(kWorldBool, true)));
  CHECK(flags.get_bool(kWorldBool) == true);
  // An item the game does not have: the inner store refuses it.
  CHECK_FALSE(store.write(i(kGoldTokens, 1)));
  // Reads are the game's.
  CHECK(store.read(FlagKey{kRupees, 0}) == i(kRupees, 50));
}

TEST_CASE("game items: through FlagWatcher, the host answers with the merged value") {
  auto w = game();
  GameFlagStore game_store(w);
  ItemFlagStore store(game_store);
  hold_int(w, kHookshot, 1);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  host.frame(store);
  // A player without the Hookshot reports 0, and gets a pocket egg; its
  // rupees are not shared at all.
  CHECK(host.receive(protocol::FlagSet{2, {i(kHookshot, 0), i(kPocketEgg, 1), i(kRupees, 5)}}));
  const auto out = host.frame(store);
  REQUIRE(out.size() == 1);
  protocol::Message m;
  REQUIRE(protocol::decode(out[0].bytes, m) == protocol::Error::none);
  const auto* state = std::get_if<protocol::FlagState>(&m);
  REQUIRE(state);
  CHECK(state->flags == std::vector{i(kPocketEgg, 1), i(kHookshot, 1)});
  CHECK(host.stats().not_shared == 1);
  CHECK(host.frame(store).empty());

  // A player's game takes the host's answer the same way: a key item it has
  // and the host's message lacks is not taken away.
  auto pw = game();
  GameFlagStore player_game(pw);
  ItemFlagStore player_store(player_game);
  hold_int(pw, kHookshot, 1);
  hold_bool(pw, kBottleAValid, true);
  FlagWatcher player(item_shared_flags(), FlagRole::player);
  player.frame(player_store);
  CHECK(player.receive(protocol::FlagState{1, {i(kHookshot, 0), b(kBottleAValid, false),
                                               i(kPocketEgg, 1)}}));
  player.frame(player_store);
  GameFlags pf(pw);
  CHECK(pf.get_int(kHookshot) == 1);
  CHECK(pf.get_bool(kBottleAValid) == true);
  CHECK(pf.get_int(kPocketEgg) == 1);
}

namespace {

protocol::FlagEntry delta(std::uint32_t h, std::int32_t change) {
  return {h, 0, K::count_delta, static_cast<std::uint32_t>(change)};
}

// The decoded messages of one frame.
std::vector<protocol::Message> decoded(const std::vector<coop::host::Outgoing>& out) {
  std::vector<protocol::Message> messages;
  for (const auto& o : out) {
    protocol::Message m;
    REQUIRE(protocol::decode(o.bytes, m) == protocol::Error::none);
    messages.push_back(std::move(m));
  }
  return messages;
}

// Hands a frame's messages to another watcher, as the server would.
void deliver(FlagWatcher& to, const std::vector<coop::host::Outgoing>& out) {
  for (const auto& o : out) CHECK(to.receive(o.bytes));
}

std::int32_t keys(Writable& w) { return *GameFlags(w).get_int(kForestKeys); }

}  // namespace

TEST_CASE("game items: the store adds count deltas") {
  auto w = game();
  GameFlagStore game_store(w);
  ItemFlagStore store(game_store);
  hold_int(w, kForestKeys, 2);
  CHECK(store.is_count(FlagKey{kForestKeys, 0}));
  CHECK(store.is_count(FlagKey{kHeartPieces, 0}));
  CHECK(store.is_count(FlagKey{kGoldTokens, 0}));
  CHECK_FALSE(store.is_count(FlagKey{kHookshot, 0}));
  CHECK_FALSE(store.is_count(FlagKey{kMaxLife, 0}));  // max, not a count
  CHECK_FALSE(store.is_count(FlagKey{kForestKeys, 1}));
  CHECK_FALSE(store.is_count(FlagKey::key64_flag(kForestKeys)));

  CHECK(store.write(delta(kForestKeys, 1)));
  CHECK(keys(w) == 3);
  CHECK(store.write(delta(kForestKeys, -1)));
  CHECK(keys(w) == 2);
  CHECK(store.write(delta(kForestKeys, -5)));  // never below 0
  CHECK(keys(w) == 0);
  CHECK(store.stats().added == 3);
  // A total is written as it came: the host's value is the truth.
  CHECK(store.write(i(kForestKeys, 4)));
  CHECK(keys(w) == 4);
  // A delta on anything but a count, or a count the game lacks, is refused; a max-rule int (the Hookshot's stock)
  // takes a gain only (row 218, heart containers), never a lowering.
  CHECK_FALSE(store.write(delta(kHookshot, -1)));
  CHECK_FALSE(store.write(delta(kWorldInt, 1)));
  CHECK_FALSE(store.write(delta(kGoldTokens, 1)));  // not in this fake game
  CHECK(store.stats().refused == 3);
  CHECK(keys(w) == 4);
}

TEST_CASE("game items: a key our own game spent is told apart from one written in for another") {
  // Locked doors (docs/dungeon.md): a lock's second key goes back only against
  // a drop of our own game's count, never a total the watcher wrote in.
  auto hw = game(), aw = game(), bw = game();
  GameFlagStore hg(hw), ag(aw), bg(bw);
  ItemFlagStore hs(hg), as(ag), bs(bg);
  for (auto* w : {&hw, &aw, &bw}) hold_int(*w, kForestKeys, 2);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  FlagWatcher a(item_shared_flags(), FlagRole::player);
  FlagWatcher b(item_shared_flags(), FlagRole::player);
  host.frame(hs);
  a.frame(as);
  b.frame(bs);
  // A opens a door.
  hold_int(aw, kForestKeys, 1);
  const auto from_a = a.frame(as);
  CHECK(a.take_own_count_drops() == std::vector{FlagKey{kForestKeys, 0}});
  CHECK(a.take_own_count_drops().empty());  // taken once
  deliver(host, from_a);
  const auto answer = host.frame(hs);
  CHECK(keys(hw) == 1);
  deliver(b, answer);
  b.frame(bs);
  CHECK(keys(bw) == 1);
  // The host and B only took A's spend in: no drop of their own.
  host.frame(hs);
  b.frame(bs);
  CHECK(host.take_own_count_drops().empty());
  CHECK(b.take_own_count_drops().empty());
  // A key picked up is no drop; the host's own door is one. (The games apply
  // the totals written in first, as each frame does.)
  apply_ints(hw);
  apply_ints(bw);
  host.frame(hs);
  b.frame(bs);
  CHECK(host.take_own_count_drops().empty());
  CHECK(b.take_own_count_drops().empty());
  hold_int(bw, kForestKeys, 2);
  b.frame(bs);
  CHECK(b.take_own_count_drops().empty());
  hold_int(hw, kForestKeys, 0);
  host.frame(hs);
  CHECK(host.take_own_count_drops() == std::vector{FlagKey{kForestKeys, 0}});
}

TEST_CASE("game items: two players gaining keys at once lose no increment") {
  // Host H and players A and B, each with 2 Forest Temple keys.
  auto hw = game(), aw = game(), bw = game();
  GameFlagStore hg(hw), ag(aw), bg(bw);
  ItemFlagStore hs(hg), as(ag), bs(bg);
  for (auto* w : {&hw, &aw, &bw}) hold_int(*w, kForestKeys, 2);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  FlagWatcher a(item_shared_flags(), FlagRole::player);
  FlagWatcher b(item_shared_flags(), FlagRole::player);
  CHECK(host.frame(hs).empty());  // first frames are the baseline
  CHECK(a.frame(as).empty());
  CHECK(b.frame(bs).empty());

  // In the same frame: A and B each pick up a key, the host opens a door.
  hold_int(aw, kForestKeys, 3);
  hold_int(bw, kForestKeys, 3);
  hold_int(hw, kForestKeys, 1);
  const auto from_a = a.frame(as);
  const auto from_b = b.frame(bs);
  const auto host_own = host.frame(hs);
  // The players send their change, the host its value.
  REQUIRE(decoded(from_a).size() == 1);
  CHECK(std::get<protocol::FlagSet>(decoded(from_a)[0]).flags ==
        std::vector{delta(kForestKeys, 1)});
  CHECK(std::get<protocol::FlagState>(decoded(host_own)[0]).flags ==
        std::vector{i(kForestKeys, 1)});
  CHECK(a.pending() == 0);  // a count waits for no answer of its own

  // The host's own FLAG_STATE reaches the players first; A picks up another
  // key before its next frame.
  deliver(a, host_own);
  deliver(b, host_own);
  hold_int(aw, kForestKeys, 4);
  deliver(host, from_a);
  deliver(host, from_b);
  const auto answer = host.frame(hs);
  REQUIRE(decoded(answer).size() == 1);
  CHECK(std::get<protocol::FlagState>(decoded(answer)[0]).flags ==
        std::vector{i(kForestKeys, 3)});  // 2 - 1 + 1 + 1
  CHECK(keys(hw) == 3);

  // A takes the host's 1, then its 3, each on top of the key it has not
  // sent yet, and sends that key as +1.
  deliver(a, answer);
  deliver(b, answer);
  const auto from_a2 = a.frame(as);
  CHECK(keys(aw) == 4);
  REQUIRE(decoded(from_a2).size() == 1);
  CHECK(std::get<protocol::FlagSet>(decoded(from_a2)[0]).flags ==
        std::vector{delta(kForestKeys, 1)});
  CHECK(b.frame(bs).empty());
  CHECK(keys(bw) == 3);

  deliver(host, from_a2);
  const auto answer2 = host.frame(hs);
  deliver(a, answer2);
  deliver(b, answer2);
  CHECK(a.frame(as).empty());
  CHECK(b.frame(bs).empty());
  CHECK(host.frame(hs).empty());
  // 2 - 1 (door) + 1 (A) + 1 (B) + 1 (A again): everyone holds 4.
  CHECK(keys(hw) == 4);
  CHECK(keys(aw) == 4);
  CHECK(keys(bw) == 4);
}

TEST_CASE("game items: the host adds deltas its client hands over") {
  // The live path: the host's net client does not answer a delta itself; it
  // gives it to the game in its flag_state event (a FLAG_STATE with the
  // deltas), and the host's watcher sends the total.
  auto hw = game();
  GameFlagStore hg(hw);
  ItemFlagStore hs(hg);
  hold_int(hw, kForestKeys, 2);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  host.frame(hs);
  hold_int(hw, kForestKeys, 1);  // the host spends one this frame too
  CHECK(host.receive(protocol::Message{
      protocol::FlagState{1, {delta(kForestKeys, 1), i(kHookshot, 1)}}}));
  const auto out = decoded(host.frame(hs));
  CHECK(keys(hw) == 2);
  CHECK(host.stats().deltas_added == 1);
  REQUIRE(out.size() == 1);
  // Its own spend and the player's gain in one total.
  CHECK(std::get<protocol::FlagState>(out[0]).flags == std::vector{i(kForestKeys, 2)});

  // A player never takes a delta.
  auto pw = game();
  GameFlagStore pg(pw);
  ItemFlagStore ps(pg);
  hold_int(pw, kForestKeys, 2);
  FlagWatcher player(item_shared_flags(), FlagRole::player);
  player.frame(ps);
  CHECK(player.receive(protocol::Message{protocol::FlagState{1, {delta(kForestKeys, 1)}}}));
  CHECK(player.frame(ps).empty());
  CHECK(keys(pw) == 2);
}

TEST_CASE("game items: a late joiner gets the host's items, not its own (the host's save decides, #96)") {
  // The host has played on: the Hookshot, 3 Forest Temple keys, a heart
  // container. The joiner comes in with only a bottle the host lacks. The
  // server hands the joiner the host's table as FLAG_SNAPSHOT; here the
  // host's own snapshot stands in for the server's copy of it.
  for (const bool joiner_first_frame : {true, false}) {
    CAPTURE(joiner_first_frame);
    auto hw = game(), jw = game();
    GameFlagStore hg(hw), jg(jw);
    ItemFlagStore hs(hg), js(jg);
    hold_int(hw, kHookshot, 1);
    hold_bool(hw, kHookshotGet, true);
    hold_int(hw, kForestKeys, 3);
    hold_int(hw, kMaxLife, 16);
    hold_int(hw, kRupees, 50);
    hold_int(jw, kMaxLife, 12);
    hold_int(jw, kBottleA, 1);
    hold_bool(jw, kBottleAValid, true);
    hold_int(jw, kRupees, 10);
    FlagWatcher host(item_shared_flags(), FlagRole::host);
    FlagWatcher joiner(item_shared_flags(), FlagRole::player);
    CHECK(host.frame(hs).empty());
    // The joiner's game may or may not have run a frame before the snapshot.
    if (joiner_first_frame) CHECK(joiner.frame(js).empty());

    host.request_snapshot();
    const auto snapshot = host.frame(hs);
    REQUIRE_FALSE(snapshot.empty());
    CHECK(std::holds_alternative<protocol::FlagSnapshot>(decoded(snapshot)[0]));
    deliver(joiner, snapshot);
    const auto from_joiner = joiner.frame(js);

    GameFlags jf(jw);
    CHECK(jf.get_int(kHookshot) == 1);
    CHECK(jf.get_bool(kHookshotGet) == true);
    CHECK(jf.get_int(kForestKeys) == 3);   // the host's total
    CHECK(jf.get_int(kMaxLife) == 16);
    // The host's save decides (docs/flags.md, "Saves"; player report #96: a
    // guest's older save filled the host's new file): the joiner's own
    // bottle, which the host's save lacks, is not kept and not sent.
    CHECK(jf.get_int(kBottleA) == 0);
    CHECK(jf.get_bool(kBottleAValid) == false);
    CHECK(jf.get_int(kRupees) == 10);      // per player, not in the table
    CHECK(from_joiner.empty());

    GameFlags hf(hw);
    CHECK(hf.get_int(kBottleA) == 0);
    CHECK(hf.get_bool(kBottleAValid) == false);
    CHECK(hf.get_int(kRupees) == 50);
    CHECK(joiner.frame(js).empty());
    CHECK(host.frame(hs).empty());
  }
}

namespace {

// A flag store over a map, for the grants: holds Int and Bool flags by hash.
class MapStore final : public coop::host::FlagStore {
 public:
  std::optional<protocol::FlagEntry> read(FlagKey key) override {
    const auto it = flags.find(key.hash);
    if (it == flags.end()) return std::nullopt;
    return protocol::FlagEntry{key, it->second.first, it->second.second};
  }
  bool write(const protocol::FlagEntry& entry) override {
    const auto it = flags.find(entry.key.hash);
    if (it == flags.end() || it->second.first != entry.kind) return false;
    it->second.second = entry.value;
    ++writes;
    return true;
  }
  void hold(std::uint32_t hash, K kind, std::int32_t value) {
    flags[hash] = {kind, static_cast<std::uint32_t>(value)};
  }
  std::int32_t value(std::uint32_t hash) const {
    return static_cast<std::int32_t>(flags.at(hash).second);
  }
  std::map<std::uint32_t, std::pair<K, std::uint32_t>> flags;
  int writes = 0;
};

constexpr std::uint32_t kSlingshot = 0x80d34ce9;     // PouchData.Weapon_Range_SlingShot.StockNum
constexpr std::uint32_t kBulletBagM = 0x9a30a9e5;    // PouchData.Item_BulletBag_Medium.StockNum
constexpr std::uint32_t kBow = 0x938d56bf;           // PouchData.Weapon_Range_Bow.StockNum
constexpr std::uint32_t kBombBagS = 0xb0a1bf51;      // PouchData.Item_BombBag_Small.StockNum
constexpr std::uint32_t kSeeds = 0x5fadf7fc;         // PouchData.Item_DekuSeed.StockNum, per player
constexpr std::uint32_t kSeedsGet = 0x29888e1e;      // IsGet.Item_DekuSeed
constexpr std::uint32_t kSeedCap = 0xae8cc742;       // 0xf80f8077.DekuSeed
constexpr std::uint32_t kArrows = 0x518ec74b, kArrowCap = 0x418db791;
constexpr std::uint32_t kBombs = 0xdb1bcd4f, kBombCap = 0xe753508b;

// A game holding every grant flag at 0 and the given items.
MapStore game_with_grant_flags() {
  MapStore store;
  for (const auto& g : coop::host::all_item_grants()) store.hold(g.hash, g.kind, 0);
  for (const auto& item : item_flags()) {
    if (item.kind == K::int32) store.hold(item.hash, K::int32, 0);
  }
  return store;
}

protocol::FlagEntry owned(std::uint32_t hash, std::int32_t n = 1) {
  return protocol::FlagEntry{FlagKey{hash, 0}, K::int32, static_cast<std::uint32_t>(n)};
}

}  // namespace

TEST_CASE("item grants: the table is sorted and names its flags") {
  const auto grants = coop::host::all_item_grants();
  REQUIRE(!grants.empty());
  std::uint32_t last = 0;
  for (const auto& g : grants) {
    CAPTURE(g.name);
    CHECK(g.item >= last);
    last = g.item;
    // Every granting item is an owned item shared by max, or an IsGet flag
    // (shared by OR) that grants a capacity only.
    const auto* item = find_item(g.item);
    REQUIRE(item != nullptr);
    if (item->name.starts_with("IsGet.")) {
      CHECK(item->rule == ItemRule::any);
      CHECK(item->kind == K::boolean);
      CHECK(g.name.starts_with("0xf80f8077."));
    } else {
      CHECK(item->rule == ItemRule::max);
      CHECK(item->kind == K::int32);
    }
    CHECK(g.value > 0);
    if (g.name.starts_with("0xf80f8077.")) {
      CHECK(g.kind == K::int32);  // a capacity: the struct's name is not known
      CHECK(find_item(g.hash) == nullptr);
    } else {
      // Ammo counts and MP are per-player items, IsGet flags are shared
      // items; MPMax (a meter's grant) is shared by max.
      CHECK(protocol::flag_hash(g.name) == g.hash);
      const auto* granted = find_item(g.hash);
      REQUIRE(granted != nullptr);
      CHECK(granted->kind == g.kind);
      if (g.name == "PlayerStatus.MPMax") {
        CHECK(granted->rule == ItemRule::max);
      } else {
        CHECK(granted->rule == (g.kind == K::int32 ? ItemRule::per_player : ItemRule::any));
      }
    }
    CHECK(!coop::host::item_grants(g.item).empty());
  }
  CHECK(coop::host::item_grants(0x8573d2cd).empty());  // the Hookshot needs no ammo
}

TEST_CASE("item grants: the six capacity hashes are the ones in GameDataList") {
  // The children of struct 0xf80f8077, read from GameDataList; the unit
  // tests cannot hash them (the struct's name is not known), so pin them.
  const std::map<std::string_view, std::uint32_t> want{
      {"0xf80f8077.Rupee", 0xe0010a50},    {"0xf80f8077.DekuSeed", 0xae8cc742},
      {"0xf80f8077.Bomb", 0xe753508b},     {"0xf80f8077.Arrow", 0x418db791},
      {"0xf80f8077.DekuStick", 0x457616b0}, {"0xf80f8077.DekuNut", 0x735b00f7},
  };
  std::set<std::string_view> seen;
  for (const auto& g : coop::host::all_item_grants()) {
    if (!g.name.starts_with("0xf80f8077.")) continue;
    CAPTURE(g.name);
    REQUIRE(want.count(g.name) == 1);
    CHECK(g.hash == want.at(g.name));
    seen.insert(g.name);
  }
  CHECK(seen.size() == want.size());  // every capacity is granted by something
}

TEST_CASE("item grants: a flag whose value cannot be read is not written") {
  // A store that cannot read the seed flags but would take a write.
  class Blind final : public coop::host::FlagStore {
   public:
    std::optional<protocol::FlagEntry> read(FlagKey key) override {
      if (key.hash == kSlingshot) return protocol::FlagEntry{key, K::int32, slingshot};
      return std::nullopt;
    }
    bool write(const protocol::FlagEntry& entry) override {
      if (entry.key.hash == kSlingshot) slingshot = entry.value;
      else ++others;
      return true;
    }
    std::uint32_t slingshot = 0;
    int others = 0;
  } game;
  ItemFlagStore store(game);
  CHECK(store.write(owned(kSlingshot)));
  CHECK(game.slingshot == 1u);
  CHECK(game.others == 0);
  CHECK(store.stats().granted == 0);
}

TEST_CASE("item grants: a shared Slingshot comes with its bag and 30 seeds (issue 128)") {
  MapStore game = game_with_grant_flags();
  game.hold(kSeedsGet, K::boolean, 0);
  ItemFlagStore store(game);
  CHECK(store.write(owned(kSlingshot)));
  CHECK(game.value(kSlingshot) == 1);
  CHECK(game.value(kSeedCap) == 30);
  CHECK(game.value(kSeeds) == 30);
  CHECK(game.value(kSeedsGet) == 1);
  CHECK(store.stats().granted == 3);

  // The same item again, or the chest opened later through the share: no
  // second grant, the seeds used meanwhile stay used.
  game.hold(kSeeds, K::int32, 12);
  CHECK(store.write(owned(kSlingshot)));
  CHECK(game.value(kSeeds) == 12);
  CHECK(store.stats().granted == 3);
}

TEST_CASE("item grants: only raise, never lower") {
  MapStore game = game_with_grant_flags();
  // A game with a bigger bag and more seeds already (an odd save).
  game.hold(kSeedCap, K::int32, 50);
  game.hold(kSeeds, K::int32, 45);
  ItemFlagStore store(game);
  CHECK(store.write(owned(kSlingshot)));
  CHECK(game.value(kSeedCap) == 50);
  CHECK(game.value(kSeeds) == 45);
}

TEST_CASE("item grants: a bag upgrade raises the capacity and fills it") {
  MapStore game = game_with_grant_flags();
  game.hold(kSlingshot, K::int32, 1);
  game.hold(kSeedCap, K::int32, 30);
  game.hold(kSeeds, K::int32, 7);
  ItemFlagStore store(game);
  CHECK(store.write(owned(kBulletBagM)));
  CHECK(game.value(kSeedCap) == 40);
  CHECK(game.value(kSeeds) == 40);
}

TEST_CASE("item grants: Bow and bomb bag follow the same rule") {
  MapStore game = game_with_grant_flags();
  ItemFlagStore store(game);
  CHECK(store.write(owned(kBow)));
  CHECK(game.value(kArrowCap) == 30);
  CHECK(game.value(kArrows) == 30);
  CHECK(store.write(owned(kBombBagS)));
  CHECK(game.value(kBombCap) == 20);
  CHECK(game.value(kBombs) == 20);
}

TEST_CASE("item grants: a game that owns the item gets nothing extra") {
  MapStore game = game_with_grant_flags();
  game.hold(kSlingshot, K::int32, 1);  // its own chest: the game gave the seeds itself
  game.hold(kSeedCap, K::int32, 30);
  game.hold(kSeeds, K::int32, 4);      // and some were used since
  ItemFlagStore store(game);
  CHECK(store.write(owned(kSlingshot)));
  CHECK(game.value(kSeeds) == 4);
  CHECK(game.value(kSeedCap) == 30);
  CHECK(store.stats().granted == 0);
  CHECK(store.stats().kept == 1);
}

TEST_CASE("item grants: an item held with room for 0 gets its capacity, not ammo (older saves)") {
  MapStore game = game_with_grant_flags();
  game.hold(kSlingshot, K::int32, 1);  // shared by an older build: Slingshot with 0/0 seeds
  ItemFlagStore store(game);
  CHECK(store.write(owned(kSlingshot)));  // the snapshot on join, or any later echo
  CHECK(game.value(kSeedCap) == 30);
  CHECK(game.value(kSeeds) == 0);         // seeds are picked up now; none given
  CHECK(store.stats().granted == 1);
  CHECK(store.stats().kept == 1);
}

TEST_CASE("item grants: the other player's first stick gives room for sticks (issue 138)") {
  constexpr std::uint32_t kSticksGet = 0xc4e21ac2, kSticks = 0xb9494411, kStickCap = 0x457616b0;
  constexpr std::uint32_t kNutsGet = 0x00c2ee37, kNuts = 0xf30d9f54, kNutCap = 0x735b00f7;
  MapStore game = game_with_grant_flags();
  ItemFlagStore store(game);
  // Only IsGet crosses for a stick picked up elsewhere (the count is per player).
  CHECK(store.write(b(kSticksGet, true)));
  CHECK(game.value(kSticksGet) == 1);
  CHECK(game.value(kStickCap) == 10);
  CHECK(game.value(kSticks) == 0);
  CHECK(store.write(b(kNutsGet, true)));
  CHECK(game.value(kNutCap) == 20);
  CHECK(game.value(kNuts) == 0);
  // A bigger bag already: left alone. IsGet false: nothing.
  game.hold(kStickCap, K::int32, 30);
  CHECK(store.write(b(kSticksGet, true)));
  CHECK(game.value(kStickCap) == 30);
  MapStore fresh = game_with_grant_flags();
  ItemFlagStore other(fresh);
  CHECK(other.write(b(kSticksGet, false)));
  CHECK(fresh.value(kStickCap) == 0);
}

TEST_CASE("item grants: a flag the game cannot write is skipped, the item still arrives") {
  MapStore game;
  game.hold(kSlingshot, K::int32, 0);  // no capacity, seed or IsGet flags in this game
  ItemFlagStore store(game);
  CHECK(store.write(owned(kSlingshot)));
  CHECK(game.value(kSlingshot) == 1);
  CHECK(store.stats().granted == 0);
}

// Issue 130 (Alex, 2026-11-12: "buying deku shield was not sync'd"). A Deku
// Shield one player buys (Kokiri Shop) or finds reaches every player:
// PouchData.Weapon_Shield_Deku.StockNum is max, IsGet.Weapon_Shield_Deku any.
// A shield lost in one game (burnt, a Like Like) is a max drop: it stays in
// that game and is not sent (the MaxLife echo rule), so that player buys a
// new one, as in the one-player game, and the others keep theirs.
TEST_CASE("game items: a Deku Shield one player buys reaches every player") {
  constexpr std::uint32_t kDekuShieldGet = 0x5217da1e;
  REQUIRE(find_item(kDekuShield) != nullptr);
  CHECK(find_item(kDekuShield)->rule == ItemRule::max);
  REQUIRE(find_item(kDekuShieldGet) != nullptr);
  CHECK(find_item(kDekuShieldGet)->rule == ItemRule::any);
  CHECK(item_shared_flags().contains(FlagKey{kDekuShield, 0}));
  CHECK(item_shared_flags().contains(FlagKey{kDekuShieldGet, 0}));
  CHECK(item_shared_flags().contains(FlagKey{0x08befd3a, 0}));  // Hylian
  CHECK(item_shared_flags().contains(FlagKey{0xf58e0e2f, 0}));  // Mirror

  auto hw = game();
  GameFlagStore host_game(hw);
  ItemFlagStore host_store(host_game);
  auto pw = game();
  GameFlagStore player_game(pw);
  ItemFlagStore player_store(player_game);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  FlagWatcher player(item_shared_flags(), FlagRole::player);
  host.frame(host_store);
  player.frame(player_store);

  // The host buys one: it reaches the player's pouch.
  hold_int(hw, kDekuShield, 1);
  deliver(player, host.frame(host_store));
  player.frame(player_store);
  apply_ints(pw);
  CHECK(*GameFlags(pw).get_int(kDekuShield) == 1);
  player.frame(player_store);

  // The player's shield burns: gone there only, nothing sent, the host keeps its own.
  hold_int(pw, kDekuShield, 0);
  CHECK(player.frame(player_store).empty());
  CHECK(*GameFlags(pw).get_int(kDekuShield) == 0);
  CHECK(*GameFlags(hw).get_int(kDekuShield) == 1);
  CHECK(player.stats().max_drops_kept >= 1);

  // The player buys a new one: 0 -> 1 goes out; the host already has one.
  apply_ints(pw);
  hold_int(pw, kDekuShield, 1);
  const auto bought = player.frame(player_store);
  CHECK_FALSE(bought.empty());
  deliver(host, bought);
  host.frame(host_store);
  CHECK(*GameFlags(hw).get_int(kDekuShield) == 1);  // never 2
}

TEST_CASE("game items: a Deku Shield the player buys first reaches the host") {
  auto hw = game();
  GameFlagStore host_game(hw);
  ItemFlagStore host_store(host_game);
  auto pw = game();
  GameFlagStore player_game(pw);
  ItemFlagStore player_store(player_game);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  FlagWatcher player(item_shared_flags(), FlagRole::player);
  host.frame(host_store);
  player.frame(player_store);

  hold_int(pw, kDekuShield, 1);
  deliver(host, player.frame(player_store));
  host.frame(host_store);
  apply_ints(hw);
  CHECK(*GameFlags(hw).get_int(kDekuShield) == 1);
}

TEST_CASE("item grants: nothing is granted on the title screen, a loaded save is mended (issue 138)") {
  constexpr std::uint32_t kSticksGet = 0xc4e21ac2, kSticks = 0xb9494411, kStickCap = 0x457616b0;
  MapStore game = game_with_grant_flags();
  coop::host::LoadMend load;

  // Title screen: the join snapshot brings the Slingshot and a stick pickup.
  CHECK_FALSE(load.frame(false));
  {
    ItemFlagStore store(game, &load);
    CHECK(store.write(owned(kSlingshot)));
    CHECK(store.write(b(kSticksGet, true)));
    CHECK(store.stats().granted == 0);
  }
  CHECK(game.value(kSlingshot) == 1);
  CHECK(game.value(kSeedCap) == 0);
  CHECK(game.value(kStickCap) == 0);
  CHECK(game.value(kSticks) == 0);

  // The save loads with its own values (room for 0), then Link appears.
  CHECK_FALSE(load.frame(false));
  CHECK(load.frame(true));
  {
    ItemFlagStore store(game, &load);
    CHECK(store.mend() == 4);  // seed room, seeds, IsGet seeds, stick room
  }
  CHECK(game.value(kSeedCap) == 30);
  CHECK(game.value(kStickCap) == 10);
  CHECK(game.value(kSeeds) == 30);  // the Slingshot came on the title: its whole grant now
  CHECK(game.value(kSticks) == 0);  // IsGet gives room only

  // The load writes its 0 again in the next frame: the mend runs again.
  game.hold(kSeedCap, K::int32, 0);
  CHECK(load.frame(true));
  {
    ItemFlagStore store(game, &load);
    CHECK(store.mend() == 1);
  }
  CHECK(game.value(kSeedCap) == 30);
  // After the mend frames, an item held from before gets room only.
  while (load.frame(true)) {
  }
  CHECK_FALSE(load.pending(kSlingshot));
  game.hold(kSeedCap, K::int32, 0);
  game.hold(kSeeds, K::int32, 3);
  CHECK_FALSE(load.frame(false));
  CHECK(load.frame(true));
  {
    ItemFlagStore store(game, &load);
    store.mend();
  }
  CHECK(game.value(kSeedCap) == 30);
  CHECK(game.value(kSeeds) == 3);

}

TEST_CASE("item grants: the mend runs kFrames after each load") {
  coop::host::LoadMend load;
  CHECK_FALSE(load.frame(false));
  unsigned frames = 0;
  while (load.frame(true)) ++frames;
  CHECK(frames == coop::host::LoadMend::kFrames);
  CHECK_FALSE(load.frame(true));
  CHECK_FALSE(load.frame(false));  // a scene load
  CHECK(load.frame(true));
}

TEST_CASE("item grants: an item that comes during a scene load gets its whole grant after it") {
  MapStore game = game_with_grant_flags();
  coop::host::LoadMend load;
  load.frame(true);
  while (load.frame(true)) {
  }
  CHECK_FALSE(load.frame(false));  // a door: no Link for a moment
  {
    ItemFlagStore store(game, &load);
    CHECK(store.write(owned(kBow)));  // the other player opened the Bow chest
    CHECK(store.write(owned(kBombBagS)));
  }
  CHECK(game.value(kBow) == 1);
  CHECK(game.value(kArrows) == 0);
  CHECK(load.frame(true));
  {
    ItemFlagStore store(game, &load);
    store.mend();
  }
  CHECK(game.value(kArrowCap) == 30);
  CHECK(game.value(kArrows) == 30);
  CHECK(game.value(kBombCap) == 20);
  CHECK(game.value(kBombs) == 20);
}

TEST_CASE("item grants: a pending item's grant leaves a bigger bag and more ammo alone") {
  MapStore game = game_with_grant_flags();
  coop::host::LoadMend load;
  CHECK_FALSE(load.frame(false));
  {
    ItemFlagStore store(game, &load);
    CHECK(store.write(owned(kBow)));
  }
  game.hold(kArrowCap, K::int32, 50);  // the save loads with a big quiver
  game.hold(kArrows, K::int32, 45);
  CHECK(load.frame(true));
  {
    ItemFlagStore store(game, &load);
    CHECK(store.mend() == 1);  // IsGet.Item_Arrow_Normal only
  }
  CHECK(game.value(kArrowCap) == 50);
  CHECK(game.value(kArrows) == 45);
}

TEST_CASE("item grants: a mend leaves a bigger bag and items not held alone") {
  MapStore game = game_with_grant_flags();
  game.hold(kSlingshot, K::int32, 1);
  game.hold(kSeedCap, K::int32, 50);
  ItemFlagStore store(game);
  CHECK(store.mend() == 0);
  CHECK(game.value(kSeedCap) == 50);
  CHECK(game.value(kArrowCap) == 0);  // no Bow
  CHECK(game.value(kBombCap) == 0);   // no bomb bag
}

TEST_CASE("item grants: the mend lasts 1.5 s at 165 presents a second (issue 106)") {
  coop::host::LoadMend load;
  std::uint64_t now = 5'000'000;
  load.set_clock([&] { return now; });
  CHECK_FALSE(load.frame(false));
  now += 6'061;
  const std::uint64_t in = now;
  std::uint64_t last_mend = 0;
  for (int i = 0; i < 165 * 3; ++i) {
    if (load.frame(true)) last_mend = now;
    now += 6'061;
  }
  CHECK(last_mend - in >= 1'450'000);
  CHECK(last_mend - in <= 1'550'000);
}

TEST_CASE("game items: a held strength item raises the lift grade (issue 125)") {
  namespace lift = coop::host::lift;
  using coop::host::lift_grade_raise;
  // The hashes are the names' (GameDataList: PlayerStatus.LiftGrade, enum
  // None/Low/Middle/High; real bomb saves hold the bracelet and Low).
  CHECK(protocol::flag_hash("PlayerStatus.LiftGrade") == lift::kGrade);
  CHECK(protocol::flag_hash("None") == lift::kNone);
  CHECK(protocol::flag_hash("Low") == lift::kLow);
  CHECK(protocol::flag_hash("Middle") == lift::kMiddle);
  CHECK(protocol::flag_hash("High") == lift::kHigh);
  CHECK(protocol::flag_hash("PouchData.Armor_Child_Gauntlet_Goron.StockNum") == lift::kGoron);
  CHECK(protocol::flag_hash("PouchData.Armor_Adult_Gauntlet_Silver.StockNum") == lift::kSilver);
  CHECK(protocol::flag_hash("PouchData.Armor_Adult_Gauntlet_Gold.StockNum") == lift::kGold);
  // The three are shared items, so a shared bracelet reaches this check.
  for (const std::uint32_t h : {lift::kGoron, lift::kSilver, lift::kGold}) {
    REQUIRE(find_item(h) != nullptr);
    CHECK(find_item(h)->rule == ItemRule::max);
  }

  // A bracelet from another player: None -> Low (the test saves' case).
  CHECK(lift_grade_raise(1, 0, 0, lift::kNone) == lift::kLow);
  CHECK(lift_grade_raise(1, 1, 0, lift::kLow) == lift::kMiddle);
  CHECK(lift_grade_raise(1, 1, 1, lift::kNone) == lift::kHigh);
  CHECK(lift_grade_raise(0, 0, 1, lift::kMiddle) == lift::kHigh);
  // Nothing held, or the grade already there or above: nothing written.
  CHECK_FALSE(lift_grade_raise(0, 0, 0, lift::kNone));
  CHECK_FALSE(lift_grade_raise(1, 0, 0, lift::kLow));
  CHECK_FALSE(lift_grade_raise(1, 0, 0, lift::kHigh));  // never lowered
  // A value that is not a grade (memory not what we think): left alone.
  CHECK_FALSE(lift_grade_raise(1, 0, 0, 0));
}

TEST_CASE("game items: a held ocarina is equipped (issue 190)") {
  namespace oc = coop::host::ocarina;
  using coop::host::ocarina_equip_mend;
  CHECK(protocol::flag_hash("PlayerStatus.EquipOcarina") == oc::kEquip);
  CHECK(protocol::flag_hash("PouchData.Item_Ocarina_Fairy.StockNum") == oc::kFairy);
  CHECK(protocol::flag_hash("PouchData.Item_Ocarina_Time.StockNum") == oc::kTime);
  // Both are shared items, so an ocarina from another player reaches this.
  for (const std::uint32_t h : {oc::kFairy, oc::kTime}) {
    REQUIRE(find_item(h) != nullptr);
    CHECK(find_item(h)->rule == ItemRule::max);
  }

  // The Fairy Ocarina from another player: "" -> the Fairy Ocarina.
  CHECK(ocarina_equip_mend(1, 0, "") == oc::kFairyName);
  // The Ocarina of Time, given or shared: it replaces the Fairy Ocarina.
  CHECK(ocarina_equip_mend(1, 1, "") == oc::kTimeName);
  CHECK(ocarina_equip_mend(1, 1, oc::kFairyName) == oc::kTimeName);
  CHECK(ocarina_equip_mend(0, 1, "") == oc::kTimeName);
  // Already equipped, or no ocarina: nothing written.
  CHECK_FALSE(ocarina_equip_mend(1, 0, oc::kFairyName));
  CHECK_FALSE(ocarina_equip_mend(1, 1, oc::kTimeName));
  CHECK_FALSE(ocarina_equip_mend(0, 0, ""));
  // Never lowered, and a name we do not know is left alone.
  CHECK_FALSE(ocarina_equip_mend(1, 0, oc::kTimeName));
  CHECK_FALSE(ocarina_equip_mend(1, 1, "Item_Something_Else"));
}

TEST_CASE("ocarina_bools_mend: a shared ocarina gets the Bools a real get sets (issue 190)") {
  using coop::host::ocarina_bools_mend;
  namespace oc = coop::host::ocarina;
  std::map<std::uint32_t, std::optional<bool>> flags;
  const auto get = [&](std::uint32_t h) -> std::optional<bool> {
    const auto it = flags.find(h);
    return it == flags.end() ? std::optional<bool>(false) : it->second;
  };
  CHECK(ocarina_bools_mend(0, 0, get).empty());  // none held: nothing
  auto want = ocarina_bools_mend(1, 0, get);     // the Fairy Ocarina from another player
  CHECK(want == std::vector<std::uint32_t>{oc::kIsGetFairy, oc::kGotFairy, oc::kSlotA, oc::kSlotB, oc::kPrepared});
  flags[oc::kIsGetFairy] = true;                 // already set: left out
  flags[oc::kSlotB] = std::nullopt;              // unreadable: left alone
  want = ocarina_bools_mend(1, 0, get);
  CHECK(want == std::vector<std::uint32_t>{oc::kGotFairy, oc::kSlotA, oc::kPrepared});
  want = ocarina_bools_mend(0, 1, get);          // the Ocarina of Time
  CHECK(want == std::vector<std::uint32_t>{oc::kIsGetTime, oc::kGotTime, oc::kSlotA, oc::kPrepared});
  // Playable at once (playtest 11: a shared ocarina played only after a reload).
  CHECK(oc::kPrepared == protocol::flag_hash("HavePreparedToPlayOcarina"));
}

TEST_CASE("OcarinaEquip: the game equips a shared ocarina from its flags once (rows 93, 103)") {
  using coop::host::OcarinaEquip;
  namespace oc = coop::host::ocarina;
  const auto settle = [](OcarinaEquip& e, const std::optional<std::string_view>& live, bool link, bool busy) {
    for (int i = 0; i < OcarinaEquip::kSettleFrames; ++i) e.arm(live, link, busy);
  };
  OcarinaEquip e;
  CHECK_FALSE(e.step(false));  // nothing wanted: no call
  e.want("Item_Ocarina_Fairy");
  CHECK(e.waiting());
  // The live name not there yet ("" while the String64 waits), unread, or another name: no call.
  settle(e, std::string_view(""), true, false);
  CHECK_FALSE(e.step(false));
  settle(e, std::nullopt, true, false);
  CHECK_FALSE(e.step(false));
  // Read for fewer than kSettleFrames frames: not yet (a queued write reads so before the applier wrote it).
  for (int i = 0; i + 1 < OcarinaEquip::kSettleFrames; ++i) e.arm("Item_Ocarina_Fairy", true, false);
  CHECK_FALSE(e.step(false));
  // An event, menu or load, or no Link: not now, and the want stays.
  e.arm("Item_Ocarina_Fairy", true, true);
  CHECK_FALSE(e.step(false));
  e.arm("Item_Ocarina_Fairy", false, false);
  CHECK_FALSE(e.step(false));
  CHECK(e.waiting());
  // Wanted again with the same name: its count goes on (the mend may say so again).
  e.want("Item_Ocarina_Fairy");
  e.arm("Item_Ocarina_Fairy", true, false);
  // Ready: slot 13, then 16 (as the game's own refreshes), then done with a note.
  auto slot = e.step(false);
  REQUIRE(slot);
  CHECK(*slot == 13);
  slot = e.step(true);
  REQUIRE(slot);
  CHECK(*slot == 16);
  CHECK_FALSE(e.step(true));
  CHECK_FALSE(e.waiting());
  const auto notes = e.take_notes();
  REQUIRE(notes.size() == 1);
  CHECK(notes[0].find("Item_Ocarina_Fairy") != std::string::npos);
  CHECK(e.take_notes().empty());
  // Once only: the next frame asks nothing.
  settle(e, std::string_view("Item_Ocarina_Fairy"), true, false);
  CHECK_FALSE(e.step(false));
  CHECK_FALSE(e.step(true));  // a call that was not ours
  // The equipment unread: asked again on a later frame, from slot 13.
  e.want("Item_Ocarina_Time");
  settle(e, std::string_view("Item_Ocarina_Time"), true, false);
  REQUIRE(e.step(false) == std::optional<std::uint32_t>(13));
  e.not_now();
  CHECK_FALSE(e.step(true));
  CHECK_FALSE(e.step(false));  // disarmed until the next arm
  e.arm("Item_Ocarina_Time", true, false);
  CHECK(e.step(false) == std::optional<std::uint32_t>(13));
  // Read from the recomp's main (docs/items.md, "The game's own equip").
  CHECK(oc::kEquipFromGameData == 0x2df6c80);
  CHECK(oc::kActorEquipment == 0x270);
  CHECK(oc::kHolderEquipment == 0x200);
  // A name changed while waiting replaces it, from the start of its count.
  OcarinaEquip f;
  f.want("Item_Ocarina_Fairy");
  settle(f, std::string_view("Item_Ocarina_Fairy"), true, false);
  f.want("Item_Ocarina_Time");
  f.arm("Item_Ocarina_Time", true, false);
  CHECK_FALSE(f.step(false));
}

TEST_CASE("OcarinaEquip: a held ocarina is asked once per stay of Link, also with no write of ours (row 103)") {
  using coop::host::OcarinaEquip;
  using coop::host::ocarina_held;
  // Held: the best ocarina the flags have, and only when EquipOcarina names it already.
  CHECK(ocarina_held(1, 0, "Item_Ocarina_Fairy") == "Item_Ocarina_Fairy");
  CHECK(ocarina_held(1, 1, "Item_Ocarina_Time") == "Item_Ocarina_Time");
  CHECK(ocarina_held(1, 1, "Item_Ocarina_Fairy").empty());  // the mend writes the Time name and wants it then
  CHECK(ocarina_held(0, 0, "Item_Ocarina_Fairy").empty());  // a name kept with no ocarina held
  CHECK(ocarina_held(1, 0, "").empty());
  const auto ask = [](OcarinaEquip& e, std::string_view name) {
    for (int i = 0; i < OcarinaEquip::kSettleFrames; ++i) e.arm(name, true, false);
    const auto first = e.step(false);
    const auto second = e.step(true);
    const bool done = !e.step(true);
    return first == std::optional<std::uint32_t>(13) && second == std::optional<std::uint32_t>(16) && done;
  };
  // A save whose flags an earlier co-op mend wrote (0.11.2): nothing to write now, but the stay asks the game.
  OcarinaEquip e;
  e.link(true, "Item_Ocarina_Fairy");
  CHECK(e.waiting());
  CHECK(ask(e, "Item_Ocarina_Fairy"));
  // Once per stay: later frames of the same stay ask nothing.
  for (int i = 0; i < 100; ++i) e.link(true, "Item_Ocarina_Fairy");
  CHECK_FALSE(e.waiting());
  // Link gone (a load or a scene change), then back: asked again.
  e.link(false, {});
  e.link(true, "Item_Ocarina_Fairy");
  CHECK(ask(e, "Item_Ocarina_Fairy"));
  // No ocarina held: nothing asked, and the stay is not spent, so one that arrives held later in it is asked.
  OcarinaEquip g;
  g.link(true, {});
  CHECK_FALSE(g.waiting());
  g.link(true, "Item_Ocarina_Time");
  CHECK(ask(g, "Item_Ocarina_Time"));
  // A want of a write of ours in the same stay is the same ask (one count, one call pair).
  OcarinaEquip h;
  h.want("Item_Ocarina_Fairy");
  h.link(true, "Item_Ocarina_Fairy");
  CHECK(ask(h, "Item_Ocarina_Fairy"));
  CHECK_FALSE(h.waiting());
}

// The randomizer (docs/mods.md, "Items in a randomized seed"): an item from a
// randomized check is shared like any other, and the one who gets it from
// another player must be able to use it. One case per dependency.
namespace {
const coop::host::ItemFlag* item_named(std::string_view name) {
  return find_item(protocol::flag_hash(name));
}
std::uint32_t pouch(std::string_view item) {
  return protocol::flag_hash("PouchData." + std::string(item) + ".StockNum");
}
}  // namespace

TEST_CASE("rando items: progressive tiers and their dependencies are shared by max") {
  for (const char* name :
       {"Item_Hookshot", "Item_Longshot", "Armor_Child_Gauntlet_Goron", "Armor_Adult_Gauntlet_Silver",
        "Armor_Adult_Gauntlet_Gold", "Item_Scale_Silver", "Item_Scale_Gold", "Item_Wallet_Small",
        "Item_Wallet_Medium", "Item_Wallet_Large", "Item_BombBag_Small", "Item_BombBag_Medium",
        "Item_BombBag_Large", "Item_Quiver_Small", "Item_Quiver_Medium", "Item_Quiver_Large",
        "Weapon_Range_Bow", "Weapon_Range_SlingShot", "Item_Arrow_Fire", "Item_Arrow_Ice", "Item_Arrow_Light",
        "Item_DinFire", "Item_FaroreWind", "Item_NayruLove", "Item_Ocarina_Fairy", "Item_Ocarina_Time",
        "Item_FairyBless_MP", "Item_FairyBless_MPDouble"}) {
    CAPTURE(name);
    const auto* item = find_item(pouch(name));
    REQUIRE(item != nullptr);
    CHECK(item->rule == ItemRule::max);  // a higher tier is never taken back by a lower one
  }
  // What a meter fills and what the ammo is stay per player; the maximum is shared.
  CHECK(item_named("PlayerStatus.MP")->rule == ItemRule::per_player);
  CHECK(item_named("PlayerStatus.MPMax")->rule == ItemRule::max);
  CHECK(find_item(pouch("Item_Bomb"))->rule == ItemRule::per_player);
}

TEST_CASE("rando items: a shared magic meter comes full, so spells and magic arrows work") {
  MapStore game = game_with_grant_flags();
  ItemFlagStore store(game);
  const std::uint32_t mp = protocol::flag_hash("PlayerStatus.MP"),
                      mp_max = protocol::flag_hash("PlayerStatus.MPMax");
  CHECK(store.write(owned(pouch("Item_FairyBless_MP"))));
  CHECK(game.value(mp_max) == 240);
  CHECK(game.value(mp) == 240);
  // Magic used, then double magic shared: a new item, filled to the new size.
  game.hold(mp, K::int32, 30);
  CHECK(store.write(owned(pouch("Item_FairyBless_MPDouble"))));
  CHECK(game.value(mp_max) == 480);
  CHECK(game.value(mp) == 480);
  // The meter again (an echo): the magic used stays used.
  game.hold(mp, K::int32, 10);
  CHECK(store.write(owned(pouch("Item_FairyBless_MP"))));
  CHECK(game.value(mp) == 10);
}

TEST_CASE("rando items: a shared bomb bag comes with bombs and room for them") {
  MapStore game = game_with_grant_flags();
  ItemFlagStore store(game);
  CHECK(store.write(owned(pouch("Item_BombBag_Small"))));
  CHECK(game.value(pouch("Item_Bomb")) == 20);
  CHECK(game.value(0xe753508b) == 20);  // the bomb capacity
  CHECK(game.value(protocol::flag_hash("IsGet.Item_Bomb")) == 1);
}

TEST_CASE("rando items: a shared bow comes with arrows, so magic arrows can be shot") {
  MapStore game = game_with_grant_flags();
  ItemFlagStore store(game);
  CHECK(store.write(owned(pouch("Item_Arrow_Fire"))));  // fire arrows first: nothing to shoot yet
  CHECK(game.value(pouch("Item_Arrow_Normal")) == 0);
  CHECK(store.write(owned(pouch("Weapon_Range_Bow"))));
  CHECK(game.value(pouch("Item_Arrow_Normal")) == 30);
  CHECK(game.value(0x418db791) == 30);  // the arrow capacity
}

TEST_CASE("rando items: a shared bracelet lets bomb flowers and bombs be lifted") {
  namespace lift = coop::host::lift;
  // OOTRR's starting Strength and the game's get set the grade; a shared one
  // is raised by the hook (MendLiftGrade) from these.
  CHECK(coop::host::lift_grade_raise(1, 0, 0, lift::kNone) == lift::kLow);
  CHECK(coop::host::lift_grade_raise(1, 1, 0, lift::kLow) == lift::kMiddle);
  CHECK(coop::host::lift_grade_raise(1, 1, 1, lift::kMiddle) == lift::kHigh);
  CHECK(!coop::host::lift_grade_raise(1, 0, 0, lift::kHigh));  // never lowered
  CHECK(lift::kGoron == pouch("Armor_Child_Gauntlet_Goron"));
}

TEST_CASE("rando items: a shared scale dives as deep as OOTRR's (DiveDepth 6 / 9)") {
  namespace dive = coop::host::dive;
  using coop::host::dive_depth_raise;
  CHECK(dive::kDepth == protocol::flag_hash("PlayerStatus.DiveDepth"));
  CHECK(dive::kSilver == pouch("Item_Scale_Silver"));
  CHECK(dive::kGold == pouch("Item_Scale_Gold"));
  CHECK(!dive_depth_raise(0, 0, 3.0f));             // no scale: the game's own 3.0
  CHECK(dive_depth_raise(1, 0, 3.0f) == 6.0f);      // the Silver Scale from another player
  CHECK(dive_depth_raise(1, 1, 6.0f) == 9.0f);      // then the Golden one
  CHECK(dive_depth_raise(0, 1, 3.0f) == 9.0f);      // the Golden alone (progressive: both held, max rule)
  CHECK(!dive_depth_raise(1, 0, 6.0f));             // already there: nothing written
  CHECK(!dive_depth_raise(1, 0, 12.0f));            // never lowered
  CHECK(!dive_depth_raise(1, 0, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("rando items: every wallet tier gives its room for rupees, as the game's own tables say") {
  // GameParameter/ExpansionMaxNumActor/Item_Wallet_*.bgyml: GameData Rupee, MaxNum 500 / 1000; the
  // small wallet's 200 is read from a save. No rupees come with a wallet.
  constexpr std::uint32_t kRupeeCap = 0xe0010a50;
  for (const auto& [item, cap] : {std::pair{"Item_Wallet_Small", 200}, std::pair{"Item_Wallet_Medium", 500},
                                  std::pair{"Item_Wallet_Large", 1000}}) {
    CAPTURE(item);
    MapStore game = game_with_grant_flags();
    ItemFlagStore store(game);
    CHECK(store.write(owned(pouch(item))));
    CHECK(game.value(kRupeeCap) == cap);
    CHECK(game.value(kRupees) == 0);
  }
}

namespace {
protocol::FlagEntry u(std::uint32_t h, std::uint32_t v) { return {h, 0, K::uint32, v}; }
}  // namespace

// Issue 146 (owner's playtest 2026-11-14): the map chest's get sets IsGetMap AND raises
// DungeonInfo.<dungeon>.EnteredFloorBit to every floor (the opener's saves: 12 -> 31). Only
// IsGetMap was shared, so the other games' maps stayed at the floors they had entered.
TEST_CASE("game items: a dungeon's floors are a shared UInt, read and written through the game's UInt table") {
  CHECK(coop::host::is_shared_uint_item(kDekuFloors));
  CHECK_FALSE(coop::host::is_shared_uint_item(kBottleAItem));  // a UInt, but per player
  CHECK_FALSE(coop::host::is_shared_uint_item(kWorldUInt));
  CHECK_FALSE(coop::host::is_shared_uint_item(0x89787395));    // IsGetMap is a Bool
  CHECK(item_shared_flags().contains(FlagKey{kDekuFloors, 0}));

  auto w = game();
  GameFlagStore game_store(w);
  hold_uint(w, kDekuFloors, 12);
  hold_uint(w, kWorldUInt, 5);
  CHECK(game_store.read(FlagKey{kDekuFloors, 0}) == u(kDekuFloors, 12));
  // Any other UInt stays unread and unwritten here, as before.
  CHECK_FALSE(game_store.read(FlagKey{kWorldUInt, 0}).has_value());
  CHECK_FALSE(game_store.write(u(kWorldUInt, 6)));
  CHECK(game_store.write(u(kDekuFloors, 31)));
  CHECK(GameFlags(w).get_uint(kDekuFloors) == 31u);  // queued with the game's UInt slot
  CHECK(coop::host::read_u32(w.mem, kUIntQueue + 4) == kDekuFloors);
  CHECK(coop::host::read_u32(w.mem, kUIntQueue + 8) == 1u);  // the UInt setter's 1 at +8
  CHECK_FALSE(game_store.busy(FlagKey{kDekuFloors, 0}));
}

TEST_CASE("game items: the floors merge as a union in the store, a lower value never clears one") {
  auto w = game();
  GameFlagStore game_store(w);
  ItemFlagStore store(game_store);
  hold_uint(w, kDekuFloors, 0b10100);
  CHECK(store.write(u(kDekuFloors, 0b00011)));
  CHECK(GameFlags(w).get_uint(kDekuFloors) == 0b10111u);
  CHECK(store.write(u(kDekuFloors, 0)));  // a game that entered none: ours stay
  CHECK(GameFlags(w).get_uint(kDekuFloors) == 0b10111u);
  CHECK(store.stats().kept == 1);
  CHECK_FALSE(store.write(i(kDekuFloors, 31)));  // the wrong kind is refused
}

TEST_CASE("game items: the map chest's floors reach the other game with the map (issue 146)") {
  // The host opens the map chest: its game sets IsGetMap and every floor.
  auto hw = game();
  GameFlagStore host_game(hw);
  ItemFlagStore host_store(host_game);
  hold_uint(hw, kDekuFloors, 12);
  FlagWatcher host(item_shared_flags(), FlagRole::host);
  host.frame(host_store);

  auto pw = game();
  GameFlagStore player_game(pw);
  ItemFlagStore player_store(player_game);
  hold_uint(pw, kDekuFloors, 0b00110);  // the player entered other floors meanwhile
  FlagWatcher player(item_shared_flags(), FlagRole::player);
  player.frame(player_store);

  hold_uint(hw, kDekuFloors, 31);  // the map's get
  const auto out = host.frame(host_store);
  REQUIRE_FALSE(out.empty());
  deliver(player, out);
  player.frame(player_store);
  CHECK(GameFlags(pw).get_uint(kDekuFloors) == 31u);

  // A floor the player enters later (already set here) changes nothing; one the host
  // lacked goes to it and is ORed in.
  auto hw2 = game();
  GameFlagStore host2_game(hw2);
  ItemFlagStore host2_store(host2_game);
  hold_uint(hw2, kDekuFloors, 0b00001);
  FlagWatcher host2(item_shared_flags(), FlagRole::host);
  host2.frame(host2_store);
  CHECK(host2.receive(protocol::FlagSet{2, {u(kDekuFloors, 0b01000)}}));
  const auto answer = decoded(host2.frame(host2_store));
  CHECK(GameFlags(hw2).get_uint(kDekuFloors) == 0b01001u);
  REQUIRE(answer.size() == 1);
  const auto* state = std::get_if<protocol::FlagState>(&answer[0]);
  REQUIRE(state);
  CHECK(state->flags == std::vector{u(kDekuFloors, 0b01001)});
}

TEST_CASE("game items: a floor the game drops is put back, a new one still goes out (one-way floors)") {
  auto w = game();
  GameFlagStore game_store(w);
  ItemFlagStore store(game_store);
  CHECK(store.is_one_way(FlagKey{kDekuFloors, 0}));
  CHECK_FALSE(coop::host::is_one_way_item(FlagKey{kDekuFloors, 0}));  // the Bool-only keeper and drop log leave it
  hold_uint(w, kDekuFloors, 31);
  FlagWatcher player(item_shared_flags(), FlagRole::player);
  player.frame(store);
  // The game drops floors 0 and 1 and enters none: written back, nothing sent.
  hold_uint(w, kDekuFloors, 0b11100);
  CHECK(decoded(player.frame(store)).empty());
  CHECK(GameFlags(w).get_uint(kDekuFloors) == 31u);
  CHECK(player.stats().one_way_restores == 1);
}

TEST_CASE("items: Zelda's Letter is spent for everyone once the Kakariko gate's Bool is set (issue 58)") {
  using coop::host::zelda_letter_spent;
  CHECK(zelda_letter_spent(1, true));
  CHECK_FALSE(zelda_letter_spent(1, false));
  CHECK_FALSE(zelda_letter_spent(1, std::nullopt));  // unread: kept
  CHECK_FALSE(zelda_letter_spent(0, true));           // nothing to take
  CHECK(coop::host::flag_hash("PouchData.Item_ZeldaLetter.StockNum") == coop::host::letters::kZelda);
  CHECK(coop::host::flag_hash("Npc_Hylia_Soldier_003_GateOpen") == coop::host::letters::kGateOpen);
}

TEST_CASE("items: Ruto's Letter goes into the first empty bottle while King Zora has not moved here (issue 58)") {
  namespace letters = coop::host::letters;
  using coop::host::ruto_letter_bottle;
  CHECK(coop::host::flag_hash("IsGet.Item_Bottle_Letter") == letters::kRutoGot);
  CHECK(coop::host::flag_hash("Letter") == letters::kLetter);
  CHECK(coop::host::flag_hash("Empty") == letters::kEmpty);
  CHECK(coop::host::flag_hash("Get_StoneOfWater") == letters::kStoneOfWater);
  const char* bottles[] = {"BottleA", "BottleB", "BottleC", "BottleD", "BottleE"};
  for (std::size_t i = 0; i < 5; ++i) {
    const std::string at = std::string("PlayerStatus.AssignableItem.BottleInfo.") + bottles[i];
    CHECK(coop::host::flag_hash(at + ".IsValid") == letters::kBottles[i].valid);
    CHECK(coop::host::flag_hash(at + ".BottleItem") == letters::kBottles[i].item);
  }
  // King Zora: the quest's steps before Step07 wait, Step07 and after have moved.
  CHECK(coop::host::king_zora_moved(coop::host::flag_hash("Step06")) == false);
  CHECK(coop::host::king_zora_moved(coop::host::flag_hash("NotReady")) == false);
  CHECK(coop::host::king_zora_moved(coop::host::flag_hash("Step07")) == true);
  CHECK(coop::host::king_zora_moved(coop::host::flag_hash("Complete")) == true);
  CHECK_FALSE(coop::host::king_zora_moved(coop::host::flag_hash("KakarikoVillageNpc")).has_value());

  // Bottles A (owned, a fish), B (owned, empty), C (owned, empty), D and E not owned.
  std::map<std::uint32_t, bool> valid{{letters::kBottles[0].valid, true}, {letters::kBottles[1].valid, true},
                                      {letters::kBottles[2].valid, true}, {letters::kBottles[3].valid, false},
                                      {letters::kBottles[4].valid, false}};
  std::map<std::uint32_t, std::uint32_t> item{{letters::kBottles[0].item, coop::host::flag_hash("Fish")}};
  for (std::size_t i = 1; i < 5; ++i) item[letters::kBottles[i].item] = letters::kEmpty;
  const auto v = [&](std::uint32_t h) -> std::optional<bool> {
    const auto it = valid.find(h);
    return it == valid.end() ? std::nullopt : std::optional(it->second);
  };
  const auto in = [&](std::uint32_t h) -> std::optional<std::uint32_t> {
    const auto it = item.find(h);
    return it == item.end() ? std::nullopt : std::optional(it->second);
  };
  CHECK(ruto_letter_bottle(true, false, false, v, in) == letters::kBottles[1].item);
  CHECK_FALSE(ruto_letter_bottle(false, false, false, v, in));         // nobody got it
  CHECK_FALSE(ruto_letter_bottle(true, false, true, v, in));           // King Zora moved here: not needed
  CHECK_FALSE(ruto_letter_bottle(true, false, std::nullopt, v, in));   // no manager read: left alone
  CHECK_FALSE(ruto_letter_bottle(std::nullopt, false, false, v, in));
  item[letters::kBottles[2].item] = letters::kLetter;            // a bottle holds it already
  CHECK_FALSE(ruto_letter_bottle(true, false, false, v, in));
  item[letters::kBottles[2].item] = coop::host::flag_hash("Bug");
  item[letters::kBottles[1].item] = coop::host::flag_hash("Fairy");
  CHECK_FALSE(ruto_letter_bottle(true, false, false, v, in));          // no empty bottle: it waits
  valid.erase(letters::kBottles[4].valid);                       // a bottle unread: left alone
  item[letters::kBottles[1].item] = letters::kEmpty;
  CHECK_FALSE(ruto_letter_bottle(true, false, false, v, in));
}

TEST_CASE("a bottled fairy: an owned bottle holding the Enum value Fairy, unread is none (row 162)") {
  namespace letters = coop::host::letters;
  CHECK(coop::host::flag_hash("Fairy") == coop::host::kBottledFairy);
  std::map<std::uint32_t, bool> valid;
  std::map<std::uint32_t, std::uint32_t> item;
  for (const auto& b : letters::kBottles) {
    valid[b.valid] = true;
    item[b.item] = letters::kEmpty;
  }
  const auto v = [&](std::uint32_t h) -> std::optional<bool> {
    if (auto it = valid.find(h); it != valid.end()) return it->second;
    return std::nullopt;
  };
  const auto in = [&](std::uint32_t h) -> std::optional<std::uint32_t> {
    if (auto it = item.find(h); it != item.end()) return it->second;
    return std::nullopt;
  };
  CHECK_FALSE(coop::host::bottled_fairy(v, in));
  item[letters::kBottles[3].item] = coop::host::flag_hash("Fairy");
  CHECK(coop::host::bottled_fairy(v, in));
  valid[letters::kBottles[3].valid] = false;  // a bottle not owned holds nothing
  CHECK_FALSE(coop::host::bottled_fairy(v, in));
  valid.erase(letters::kBottles[3].valid);  // unread
  CHECK_FALSE(coop::host::bottled_fairy(v, in));
  valid[letters::kBottles[3].valid] = true;
  item[letters::kBottles[3].item] = coop::host::flag_hash("BigPoe");
  CHECK_FALSE(coop::host::bottled_fairy(v, in));
}

TEST_CASE("items: Ruto's Letter says why it waits (issue 58, the 0.11.2 big run)") {
  namespace letters = coop::host::letters;
  using coop::host::RutoWait;
  using coop::host::ruto_letter_plan;
  // The run: B owned BottleA (StockNum 1, IsValid 1, BottleItem the default Empty), the others unowned.
  std::map<std::uint32_t, bool> valid;
  std::map<std::uint32_t, std::uint32_t> item;
  for (std::size_t i = 0; i < 5; ++i) {
    valid[letters::kBottles[i].valid] = i == 0;
    item[letters::kBottles[i].item] = letters::kEmpty;
  }
  const auto v = [&](std::uint32_t h) -> std::optional<bool> {
    const auto it = valid.find(h);
    return it == valid.end() ? std::nullopt : std::optional(it->second);
  };
  const auto in = [&](std::uint32_t h) -> std::optional<std::uint32_t> {
    const auto it = item.find(h);
    return it == item.end() ? std::nullopt : std::optional(it->second);
  };
  const auto moved = [](const char* step) { return coop::host::king_zora_moved(coop::host::flag_hash(step)); };
  CHECK(ruto_letter_plan(false, false, moved("Ready"), v, in).why == RutoWait::not_got);
  CHECK(ruto_letter_plan(std::nullopt, false, moved("Ready"), v, in).why == RutoWait::not_got);
  CHECK(ruto_letter_plan(true, false, std::nullopt, v, in).why == RutoWait::king_unread);
  CHECK(ruto_letter_plan(true, false, moved("SomethingElse"), v, in).why == RutoWait::king_unread);
  CHECK(ruto_letter_plan(true, false, moved("Step07"), v, in).why == RutoWait::king_moved);
  for (const char* step : {"NotReady", "Ready", "Step06"}) {
    const auto plan = ruto_letter_plan(true, false, moved(step), v, in);
    CHECK(plan.why == RutoWait::give);
    CHECK(plan.bottle == letters::kBottles[0].item);
  }
  item[letters::kBottles[0].item] = coop::host::flag_hash("Fish");
  CHECK(ruto_letter_plan(true, false, moved("Ready"), v, in).why == RutoWait::no_empty);
  item[letters::kBottles[3].item] = letters::kLetter;
  CHECK(ruto_letter_plan(true, false, moved("Ready"), v, in).why == RutoWait::has_letter);
  item.erase(letters::kBottles[2].item);
  CHECK(ruto_letter_plan(true, false, moved("Ready"), v, in).why == RutoWait::bottle_unread);
  CHECK_FALSE(ruto_letter_plan(true, false, moved("Ready"), v, in).bottle);
  CHECK(coop::host::ruto_wait_text(RutoWait::king_unread).find("ChallengeMgr") != std::string_view::npos);
}

TEST_CASE("items: Ruto's Letter shown to King Zora in one game is spent in every game (row 117)") {
  namespace letters = coop::host::letters;
  using coop::host::RutoWait;
  using coop::host::ruto_letter_plan;
  CHECK(coop::host::flag_hash("HavePlayedEvent.Dm_Zora_0010") == letters::kKingRead);
  // The report: the host's BottleB still holds the letter (BottleA a fish),
  // the non-host showed theirs; the host's own King Zora step comes by the
  // quest catch-up, separately from the scene's Bool.
  std::map<std::uint32_t, bool> valid;
  std::map<std::uint32_t, std::uint32_t> item;
  for (std::size_t i = 0; i < 5; ++i) {
    valid[letters::kBottles[i].valid] = i < 2;
    item[letters::kBottles[i].item] = letters::kEmpty;
  }
  item[letters::kBottles[0].item] = coop::host::flag_hash("Fish");
  item[letters::kBottles[1].item] = letters::kLetter;
  const auto v = [&](std::uint32_t h) -> std::optional<bool> {
    const auto it = valid.find(h);
    return it == valid.end() ? std::nullopt : std::optional(it->second);
  };
  const auto in = [&](std::uint32_t h) -> std::optional<std::uint32_t> {
    const auto it = item.find(h);
    return it == item.end() ? std::nullopt : std::optional(it->second);
  };
  const auto moved = [](const char* step) { return coop::host::king_zora_moved(coop::host::flag_hash(step)); };
  for (const char* step : {"Step07", "Complete"}) {
    const auto plan = ruto_letter_plan(true, true, moved(step), v, in);
    CHECK(plan.why == RutoWait::shown);
    CHECK(plan.spent == letters::kBottles[1].item);  // emptied here, as the scene emptied the shower's
    CHECK_FALSE(plan.bottle);
  }
  // Shown elsewhere, but our King Zora has not moved (the catch-up has not
  // reached Step07, or his step is unread): the letter is the only way past
  // him, so it is kept; the next tick spends it once he has moved.
  for (const auto king : {moved("Ready"), moved("Step06"), std::optional<bool>{}}) {
    const auto plan = ruto_letter_plan(true, true, king, v, in);
    CHECK(plan.why == RutoWait::shown_king_waits);
    CHECK_FALSE(plan.spent);
    CHECK_FALSE(plan.bottle);
  }
  // Not shown yet: the held letter stays (King Zora still needs it here).
  const auto kept = ruto_letter_plan(true, false, moved("Ready"), v, in);
  CHECK(kept.why == RutoWait::has_letter);
  CHECK_FALSE(kept.spent);
  // The scene's flag unread: neither taken nor given.
  const auto unread = ruto_letter_plan(true, std::nullopt, moved("Ready"), v, in);
  CHECK(unread.why == RutoWait::shown_unread);
  CHECK_FALSE(unread.spent);
  CHECK_FALSE(unread.bottle);
  // Shown and no letter held (the shower, or emptied already): nothing to do,
  // and an empty bottle never gets the letter back (the ChallengeMgr lag must
  // not give it again).
  item[letters::kBottles[1].item] = letters::kEmpty;
  const auto done = ruto_letter_plan(true, true, moved("Ready"), v, in);
  CHECK(done.why == RutoWait::shown);
  CHECK_FALSE(done.spent);
  CHECK_FALSE(done.bottle);
  CHECK_FALSE(coop::host::ruto_letter_bottle(true, true, moved("Ready"), v, in));
  // Every other bottle content stays: only the letter is spent.
  item[letters::kBottles[1].item] = coop::host::flag_hash("Fairy");
  CHECK_FALSE(ruto_letter_plan(true, true, moved("Ready"), v, in).spent);
  CHECK(coop::host::ruto_wait_text(RutoWait::shown).find("Dm_Zora_0010") != std::string_view::npos);
}

TEST_CASE("game items: every dungeon's map, compass, boss key and floors ask the HUD refresh (rows 63/76, #146)") {
  // docs/items.md, "Dungeon items": IsGetMap, IsGetCompass, IsGetBossKey, SmallKeyNum, EnteredFloorBit of all 14.
  struct Row {
    std::uint32_t map, compass, boss_key, small_keys, floors;
  };
  const Row rows[] = {
      {0x89787395, 0x06c51ff2, 0xf6995cee, 0x0914ba07, 0x5c6c21c1}, {0xa52c3a19, 0x8b93edf9, 0xfaabc084, 0x3fdbc3d0, 0x46913e20},
      {0xfcadd210, 0x9a2218db, 0x656bd5ee, 0x1470a816, 0x7d59d22a}, {0xe704f9b4, 0xf240eeb2, 0x427333a0, 0x90237092, 0x4321ac04},
      {0x5b3ac0ba, 0x0aa068b6, 0xb72d73df, 0x1df11487, 0x3a014bac}, {0x4a4402ab, 0x2e82c8fe, 0x79daf80a, 0x877bdcee, 0x45408bef},
      {0xf6177b93, 0x1e2feec5, 0xc33c66cf, 0x5edda9e3, 0x445aeb73}, {0xb85002ba, 0x9bd1181c, 0x1984c03f, 0x79eeec8e, 0x36346d00},
      {0x77c47ec9, 0x9d6efaac, 0x9402715c, 0x7dd79d10, 0x7f9b6ea4}, {0x322f2a94, 0xea1846e6, 0x275f4512, 0xfa5d133d, 0x70446601},
      {0x4b36041e, 0x3d9a2750, 0x6d155841, 0xea9786f5, 0xf464c91f}, {0x9bbc0c4a, 0xc4d4734e, 0x2fd59293, 0xfbfd2910, 0xbe80d86d},
      {0xb3b65c1a, 0x874493ce, 0x6d0fea11, 0xa04bfd48, 0xb71065f6}, {0xbcaad6ce, 0x5eb80679, 0x95fbcb7c, 0x3a28b6a4, 0x843ae593},
  };
  for (const Row& r : rows) {
    for (const std::uint32_t h : {r.map, r.compass, r.boss_key, r.floors}) {
      CAPTURE(h);
      REQUIRE(find_item(h) != nullptr);
      CHECK(coop::host::is_hud_dungeon_item(FlagKey{h, 0}));
      CHECK_FALSE(coop::host::is_hud_dungeon_item(FlagKey::key64_flag(h)));  // a placement key, not the item
    }
    CAPTURE(r.small_keys);
    CHECK_FALSE(coop::host::is_hud_dungeon_item(FlagKey{r.small_keys, 0}));  // a count the HUD does not copy
  }
  CHECK_FALSE(coop::host::is_hud_dungeon_item(FlagKey{0x405d8a0f, 0}));  // Weapon_Sword_Kokiri: not a dungeon item
}

TEST_CASE("items: Ruto's Letter on an open-fountain seed becomes an empty bottle in a session (row 117)") {
  // OOTRR's open Zora's Fountain (start_state.rs FOUNTAIN_STEPS/FOUNTAIN_FLAGS)
  // starts with Get_StoneOfWater at Step07 and HavePlayedEvent.Dm_Zora_0010 set.
  // The letter found in a chest then has no use: the hook (only while co-op
  // shares items, never solo) empties its bottle, which stays owned.
  namespace letters = coop::host::letters;
  using coop::host::RutoWait;
  std::map<std::uint32_t, std::uint32_t> item;
  for (const auto& b : letters::kBottles) item[b.item] = letters::kEmpty;
  item[letters::kBottles[0].item] = letters::kLetter;  // the chest's letter, in BottleA
  const auto v = [](std::uint32_t) -> std::optional<bool> { return true; };
  const auto in = [&](std::uint32_t h) -> std::optional<std::uint32_t> { return item.at(h); };
  const auto step07 = coop::host::king_zora_moved(coop::host::flag_hash("Step07"));
  REQUIRE(step07 == true);
  const auto plan = coop::host::ruto_letter_plan(true, true, step07, v, in);
  CHECK(plan.why == RutoWait::shown);
  CHECK(plan.spent == letters::kBottles[0].item);
  CHECK_FALSE(plan.bottle);
  // Emptied: nothing more to do, and it is not given back.
  item[letters::kBottles[0].item] = letters::kEmpty;
  const auto after = coop::host::ruto_letter_plan(true, true, step07, v, in);
  CHECK_FALSE(after.spent);
  CHECK_FALSE(after.bottle);
}

namespace {
// A trade quest view: our step and the trade items held (StockNum), by name.
struct TradeGame {
  std::map<std::uint32_t, std::int32_t> stock;
  TradeGame() {
    for (const auto& h : coop::host::trade::kHolds) stock[h.item] = 0;
  }
  void hold(const char* name, std::int32_t n = 1) {
    stock.at(coop::host::flag_hash(std::string("PouchData.") + name + ".StockNum")) = n;
  }
  std::optional<std::uint32_t> raise(const char* step) const {
    return raise_from(coop::host::flag_hash(step));
  }
  std::optional<std::uint32_t> raise_from(std::optional<std::uint32_t> step) const {
    return coop::host::trade_step_raise(step, [this](std::uint32_t h) -> std::optional<std::int32_t> {
      const auto it = stock.find(h);
      return it == stock.end() ? std::nullopt : std::optional(it->second);
    });
  }
};
std::uint32_t step_hash(const char* name) { return coop::host::flag_hash(name); }
}  // namespace

TEST_CASE("items: the adult trade's tables are the game's names, in its order (row 117)") {
  namespace trade = coop::host::trade;
  CHECK(coop::host::flag_hash("Step_SubEvent_GetSwordBigGoron") == trade::kQuest);
  CHECK(coop::host::flag_hash("SubEvent_GetSwordBigGoron") == trade::kChallenge);
  const char* steps[] = {"NotReady", "Ready", "StartSubEvent", "WaitEggHatch", "EggHatched", "Step01", "Step01_01",
                         "Step02", "Step03", "Step03_01", "Step03_02", "Step03_03", "Step04", "Step05",
                         "Step05_01", "Step06", "Step06_01", "Step06_02", "Step06_03", "Step07", "Step08",
                         "Step08_01", "Step09", "Step09_01", "Step10", "Step11", "Complete"};
  REQUIRE(std::size(steps) == trade::kSteps.size());
  for (std::size_t i = 0; i < trade::kSteps.size(); ++i) {
    CAPTURE(steps[i]);
    CHECK(coop::host::flag_hash(steps[i]) == trade::kSteps[i]);
  }
  const std::pair<const char*, const char*> holds[] = {
      {"Item_PocketEgg", "StartSubEvent"}, {"Item_PocketCucco", "EggHatched"}, {"Item_CojiroCucco", "Step03"},
      {"Item_OddMushroom", "Step04"},      {"Item_OddPowder", "Step05"},       {"Item_PoacherSaw", "Step06"},
      {"Item_BrokenSword", "Step07"},      {"Item_Prescription", "Step08"},    {"Item_EyeballFrog", "Step09"},
      {"Item_EyeDrop", "Step10"},          {"Item_ClaimCheck", "Step11"}};
  REQUIRE(std::size(holds) == trade::kHolds.size());
  for (std::size_t i = 0; i < trade::kHolds.size(); ++i) {
    CAPTURE(holds[i].first);
    CHECK(std::string_view(trade::kHolds[i].name) == holds[i].first);
    const std::uint32_t item = coop::host::flag_hash(std::string("PouchData.") + holds[i].first + ".StockNum");
    CHECK(trade::kHolds[i].item == item);
    CHECK(trade::kHolds[i].step == coop::host::flag_hash(holds[i].second));
    const auto* flag = find_item(item);
    REQUIRE(flag != nullptr);
    CHECK(flag->rule == coop::host::ItemRule::mirror);  // the item crosses; its step must follow
    if (i > 0) {  // the chain goes forward
      CHECK(*coop::host::trade_step_rank(trade::kHolds[i].step) >
            *coop::host::trade_step_rank(trade::kHolds[i - 1].step));
    }
  }
}

TEST_CASE("items: a trade item another player handed on brings our trade step to it (row 117)") {
  namespace trade = coop::host::trade;
  // The chain: a player who never traded (Ready) holds whatever the mirror
  // brought; the step becomes the one the item's own hand-over sets.
  for (const auto& h : trade::kHolds) {
    TradeGame g;
    g.hold(h.name);
    CAPTURE(h.name);
    CHECK(g.raise("Ready") == h.step);
    CHECK_FALSE(g.raise_from(h.step));  // there already
  }
  // A step at a time: B is one trade behind A when A trades the saw for the
  // Broken Sword; B's carpenter (Step06) gave nothing, B's Biggoron needs Step07.
  TradeGame b;
  b.hold("Item_BrokenSword");
  CHECK(b.raise("Step06") == step_hash("Step07"));
  CHECK(b.raise("Step06_03") == step_hash("Step07"));
}

TEST_CASE("items: the trade step only goes forward, from Ready, never to Complete (row 117)") {
  TradeGame g;
  g.hold("Item_OddMushroom");
  CHECK_FALSE(g.raise("Step05"));    // ours is further: kept
  CHECK_FALSE(g.raise("Complete"));  // done here: kept
  CHECK_FALSE(g.raise("NotReady"));  // the manager readies the quest (a child, or before it): left alone
  CHECK_FALSE(g.raise_from(std::nullopt));                       // unread
  CHECK_FALSE(g.raise_from(step_hash("SomethingElse")));        // not in the list
  TradeGame none;
  CHECK_FALSE(none.raise("Ready"));  // nothing held: nothing to follow
  // Biggoron took the Claim Check (mirror: gone everywhere): nothing raises
  // to Complete; Biggoron's Sword is a shared item of its own.
  CHECK_FALSE(none.raise("Step11"));
  // An unread item: left alone.
  TradeGame unread;
  unread.hold("Item_EyeDrop");
  unread.stock.erase(coop::host::trade::kHolds[0].item);
  CHECK_FALSE(unread.raise("Ready"));
  // Two held at once (a mirror between two hand-overs): the furthest wins.
  TradeGame two;
  two.hold("Item_PoacherSaw");
  two.hold("Item_BrokenSword");
  CHECK(two.raise("Ready") == step_hash("Step07"));
}

TEST_CASE("items: the egg and its hatch follow the item too (row 117)") {
  TradeGame egg;
  egg.hold("Item_PocketEgg");
  CHECK(egg.raise("Ready") == step_hash("StartSubEvent"));
  CHECK_FALSE(egg.raise("WaitEggHatch"));  // our own egg is waiting to hatch: kept
  TradeGame chick;
  chick.hold("Item_PocketCucco");  // another player's egg hatched: the mirror brought the cucco
  CHECK(chick.raise("WaitEggHatch") == step_hash("EggHatched"));
  CHECK(chick.raise("StartSubEvent") == step_hash("EggHatched"));
  CHECK_FALSE(chick.raise("Step01_01"));  // waking Talon with it: ours already further
}

TEST_CASE("items: a spoiled trade item's retry and the timers follow the item (row 117)") {
  // Each spoiling item has a spoiled step before its own (Step03_03,
  // Step08_01, Step09_01): the game sets it when the timer runs out and the
  // giver hands a new one over, setting the item's step again. A new item
  // another player got that way brings ours back to the timed step.
  const std::tuple<const char*, const char*, const char*> retries[] = {
      {"Item_OddMushroom", "Step03_03", "Step04"},
      {"Item_EyeballFrog", "Step08_01", "Step09"},
      {"Item_EyeDrop", "Step09_01", "Step10"},
  };
  for (const auto& r : retries) {
    const char* item = std::get<0>(r);
    const char* spoiled = std::get<1>(r);
    const char* step = std::get<2>(r);
    CAPTURE(item);
    TradeGame g;
    g.hold(item);
    CHECK(g.raise(spoiled) == step_hash(step));
    // Spoiled here and gone (the mirror took it everywhere): nothing to follow.
    TradeGame gone;
    CHECK_FALSE(gone.raise(spoiled));
  }
  // The Claim Check: Biggoron's wait (Step11) is reached as the item's own.
  TradeGame claim;
  claim.hold("Item_ClaimCheck");
  CHECK(claim.raise("Step10") == step_hash("Step11"));
  CHECK(claim.raise("Step09_01") == step_hash("Step11"));
}

namespace rewards = coop::host::rewards;
namespace letters = coop::host::letters;

TEST_CASE("rewards: each once-per-save reward's Bool is its name's hash (row 272)") {
  const auto check = [](const auto& sources) {
    for (const rewards::Source& s : sources) {
      INFO(s.name);
      CHECK(coop::protocol::flag_hash(s.name) == s.hash);
    }
  };
  check(rewards::kGuarded);
  check(rewards::kOther);
  for (const rewards::Source& s : rewards::kGuarded) CHECK(s.family != rewards::Family::none);
  for (const rewards::Source& s : rewards::kOther) CHECK(s.family == rewards::Family::none);
}

TEST_CASE("gold race flags: every Skulltula House reward and what only they give, each a known flag (review-01112-h)") {
  const auto race = coop::host::gold_race_flags();
  const std::set<std::uint32_t> flags(race.begin(), race.end());
  CHECK(flags.size() == race.size());
  // Every reward of the Skulltula House (its Bools are named KakarikoVillage_GatheringTokenGold_GetReward<N>).
  std::size_t house = 0;
  const auto each = [&](const auto& sources) {
    for (const rewards::Source& s : sources) {
      if (s.name.find("GatheringTokenGold_GetReward") == std::string_view::npos) continue;
      INFO(s.name);
      CHECK(flags.contains(s.hash));
      ++house;
    }
  };
  each(rewards::kGuarded);
  each(rewards::kOther);
  CHECK(house == 5);
  // The wallet tiers: given only by the House (the wallet family's Bools are the House's).
  for (const FlagKey s : rewards::sources(rewards::Family::wallet)) {
    bool is_house = false;
    for (const rewards::Source& g : rewards::kGuarded) {
      is_house = is_house || (g.hash == s.hash && g.name.find("GatheringTokenGold") != std::string_view::npos);
    }
    CHECK(is_house);
  }
  for (const auto& unit : rewards::units(rewards::Family::wallet)) {
    for (const FlagKey key : unit) CHECK(flags.contains(key.hash));
  }
  // The Stone of Agony (reward 20), by name.
  CHECK(flags.contains(coop::protocol::flag_hash("IsGet.Item_AgonyStone")));
  CHECK(flags.contains(coop::protocol::flag_hash("PouchData.Item_AgonyStone.StockNum")));
  // Each one a flag of the game's item list or a reward Bool; never the count or IsGet the race keeps already.
  for (const std::uint32_t h : race) {
    INFO(h);
    CHECK(h != coop::host::kGoldTokenCount);
    CHECK(h != coop::host::kGoldTokenGet);
  }
  CHECK(race.size() == 5 + 6 + 2);
}

TEST_CASE("rewards: every family has its units and its Bools, each key in one unit (row 272)") {
  std::set<std::uint32_t> seen;
  std::size_t bools = 0;
  for (const rewards::Family family : rewards::kFamilies) {
    const auto units = rewards::units(family);
    CHECK(units.size() == (family == rewards::Family::bottle ? 5U : 3U));
    for (const auto& unit : units) {
      CHECK(unit.size() == 2);
      for (const FlagKey key : unit) {
        INFO(key.hash);
        CHECK(find_item(key.hash) != nullptr);
        CHECK(seen.insert(key.hash).second);
      }
    }
    const auto sources = rewards::sources(family);
    CHECK_FALSE(sources.empty());
    bools += sources.size();
  }
  CHECK(bools == rewards::kGuarded.size());
  CHECK(rewards::units(rewards::Family::none).empty());
  // A bottle slot is its StockNum and its IsValid, A to E; a tier its StockNum and its IsGet.
  const auto bottles = rewards::units(rewards::Family::bottle);
  for (std::size_t i = 0; i < letters::kBottles.size(); ++i) {
    CHECK(bottles[i][1].hash == letters::kBottles[i].valid);
    CHECK(find_item(bottles[i][0].hash)->name.starts_with("PouchData.Bottle"));
  }
  for (const auto family : {rewards::Family::wallet, rewards::Family::quiver, rewards::Family::bomb_bag}) {
    for (const auto& unit : rewards::units(family)) {
      const std::string_view stock = find_item(unit[0].hash)->name;
      const std::string_view got = find_item(unit[1].hash)->name;
      CHECK(stock.starts_with("PouchData."));
      CHECK(got.starts_with("IsGet."));
      CHECK(stock.substr(10, stock.size() - 19) == got.substr(6));  // PouchData.<tier>.StockNum / IsGet.<tier>
    }
  }
}
