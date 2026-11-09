#include <doctest/doctest.h>

#include <set>
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
constexpr std::uint32_t kMaxLife = 0xfbe01da1;         // PlayerStatus.MaxLife, max, off
constexpr std::uint32_t kWorldBool = 0x11, kWorldInt = 0x22;  // not items

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
const std::vector<std::uint32_t> kInts{kHookshot,   kPocketEgg,  kRupees, kBottleA,
                                       kBottleAItem, kForestKeys, kWorldInt};

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
  return w;
}

// Sets a value as the game holds it after its own update (not queued).
void hold_bool(Writable& w, std::uint32_t h, bool v) {
  w.mem.put_u8(kBoolEntries + kBoolStride * at(kBools, h) + kBoolValue, v ? 1 : 0);
}
void hold_int(Writable& w, std::uint32_t h, std::int32_t v) {
  w.mem.put_u32(kIntEntries + kIntStride * at(kInts, h) + kIntValue, static_cast<std::uint32_t>(v));
}

protocol::FlagEntry b(std::uint32_t h, bool v) { return {h, 0, K::boolean, v ? 1u : 0u}; }
protocol::FlagEntry i(std::uint32_t h, std::int32_t v) {
  return {h, 0, K::int32, static_cast<std::uint32_t>(v)};
}
std::uint32_t bits(std::int32_t v) { return static_cast<std::uint32_t>(v); }

}  // namespace

TEST_CASE("game items: every name hashes to its hash, sorted, each once") {
  const auto items = item_flags();
  CHECK(items.size() == 300);
  std::uint32_t last = 0;
  for (const auto& item : items) {
    CAPTURE(item.name);
    CHECK(protocol::flag_hash(item.name) == item.hash);
    CHECK(item.hash > last);
    last = item.hash;
    CHECK(find_item(item.hash) == &item);
    // Owned counts are Int, IsGet and the dungeon items Bool.
    if (item.name.starts_with("PouchData.")) CHECK(item.kind == K::int32);
    const bool dungeon_bool =
        item.name.starts_with("DungeonInfo.") && !item.name.ends_with(".SmallKeyNum");
    if (item.name.starts_with("IsGet.") || dungeon_bool)
      CHECK((item.kind == K::boolean && item.rule == ItemRule::any));
    if (item.rule == ItemRule::count) CHECK_FALSE(item.on);
  }
  CHECK(find_item(0) == nullptr);
  CHECK(find_item(kWorldInt) == nullptr);
}

TEST_CASE("game items: Caleb's rule, item by item") {
  struct Want {
    std::uint32_t hash;
    ItemRule rule;
    bool on;
  };
  for (const auto& w : {
           Want{kHookshot, ItemRule::max, true},        // key item: shared
           Want{kHookshotGet, ItemRule::any, true},
           Want{kPocketEgg, ItemRule::mirror, true},    // trade item
           Want{kRupees, ItemRule::per_player, true},   // rupees stay with the player
           Want{0xdb1bcd4f, ItemRule::per_player, true},  // bombs (ammo)
           Want{0xbd3ca2c6, ItemRule::max, true},       // the bomb bag tier is shared
           Want{kBottleA, ItemRule::any, true},         // the bottle is shared
           Want{kBottleAValid, ItemRule::any, true},
           Want{kBottleAItem, ItemRule::per_player, true},  // what is in it is not
           Want{kForestKeys, ItemRule::count, false},
           Want{kGoldTokens, ItemRule::count, false},
           Want{kHeartPieces, ItemRule::count, false},
           Want{kMaxLife, ItemRule::max, false},
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
  CHECK_FALSE(shared.contains(FlagKey{kForestKeys, 0}));  // count: not on yet
  CHECK_FALSE(shared.contains(FlagKey{kMaxLife, 0}));
  CHECK(shared.size() == 300 - 18 - 17);
  const auto all = item_shared_flags(true);
  CHECK(all.contains(FlagKey{kForestKeys, 0}));
  CHECK(all.contains(FlagKey{kMaxLife, 0}));
  CHECK_FALSE(all.contains(FlagKey{kRupees, 0}));
  CHECK(all.size() == 300 - 18);
}

TEST_CASE("game items: merge rules") {
  CHECK(merge_item(ItemRule::max, K::int32, 0, 1) == 1);
  CHECK(merge_item(ItemRule::max, K::int32, 2, 1) == 2);
  CHECK(merge_item(ItemRule::max, K::int32, bits(-1), 0) == 0);  // signed
  CHECK(merge_item(ItemRule::any, K::boolean, 0, 1) == 1);
  CHECK(merge_item(ItemRule::any, K::boolean, 1, 0) == 1);
  CHECK(merge_item(ItemRule::any, K::int32, 1, 0) == 1);  // a bottle stays owned
  CHECK(merge_item(ItemRule::mirror, K::int32, 1, 0) == 0);  // the egg was handed over
  CHECK(merge_item(ItemRule::mirror, K::int32, 0, 1) == 1);
  CHECK(merge_item(ItemRule::per_player, K::int32, 50, 99) == 50);
  // Count: the sender went from 2 to 3 keys (+1) while this game spent one (2 -> 1).
  CHECK(merge_item(ItemRule::count, K::int32, 1, 3, 2u) == 2);
  CHECK(merge_item(ItemRule::count, K::int32, 3, 2, 3u) == 2);  // -1 used
  CHECK(merge_item(ItemRule::count, K::int32, 5, 7) == 7);      // no base: as it came
  CHECK(merge_item(ItemRule::count, K::int32, 0x7fffffff, 1, 0u) == 0x7fffffffu);  // clamped
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
