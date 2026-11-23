#include <bit>
#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "coop/host/game_flag_events.hpp"
#include "coop/host/game_flags.hpp"
#include "coop_flag_native.h"
#include "fake_memory.hpp"

// Lays out a GameDataMgr as docs/flags.md describes it, with offsets written
// out again here so a slip in game_flags.cpp is caught.
namespace {

using coop::host::GameFlags;
using coop::host::GameFlagStore;
using coop::host::test::FakeMemory;
namespace protocol = coop::protocol;

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
    if (v && after_cas) after_cas();
    return v;
  }
  FakeMemory mem;
  std::function<void()> after_cas;
};

constexpr std::uint64_t kMgr = 0x8000000000ull;
constexpr std::uint64_t kBoolEntries = 0x8100000000ull, kBoolSlots = 0x8110000000ull,
                        kBoolQueue = 0x8120000000ull;
constexpr std::uint64_t kIntEntries = 0x8200000000ull, kIntSlots = 0x8210000000ull,
                        kIntQueue = 0x8220000000ull;
constexpr std::uint64_t kArrEntries = 0x8300000000ull, kArrSlots = 0x8310000000ull,
                        kArrBits = 0x8320000000ull;
constexpr std::uint64_t kKeys = 0x8400000000ull, kKeyQueue = 0x8410000000ull;
constexpr std::uint64_t kUIntEntries = 0x8500000000ull, kUIntSlots = 0x8510000000ull,
                        kUIntQueue = 0x8520000000ull;
constexpr std::uint64_t kEnumEntries = 0x8600000000ull, kEnumSlots = 0x8610000000ull,
                        kEnumQueue = 0x8620000000ull;
constexpr std::uint32_t kCap = 7, kQueueCap = 8;

void zero(FakeMemory& m, std::uint64_t a, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) m.put_u8(a + i, 0);
}

// One hashed table: `hashes` go in entries 0.., slots probed from hash % kCap.
void table(FakeMemory& m, std::uint64_t base, std::uint64_t entries, std::uint64_t slots,
           std::uint64_t stride, std::initializer_list<std::uint32_t> hashes) {
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
  m.put_u32(kMgr + base + 0x30, 0x30000000);  // round 3, nothing taken
  zero(m, buf, slot * kQueueCap);
}

// Bools 0x11 (entry 0) and 0x18 (entry 1, same start slot as 0x11), Int 0x22,
// BoolArray 0x33 of 40 elements, 64-bit keys 5, 9, 0x8c44828386468bf, UInt 0x44.
Writable game() {
  Writable w;
  auto& m = w.mem;
  m.put_u64(FakeMemory::kBase + 0x4e842a8, kMgr);
  table(m, 0x10, kBoolEntries, kBoolSlots, 0x18, {0x11, 0x18});
  queue(m, 0x10, kBoolQueue, 8);
  table(m, 0xd0, kIntEntries, kIntSlots, 0x28, {0x22});
  queue(m, 0xd0, kIntQueue, 12);
  table(m, 0x790, kUIntEntries, kUIntSlots, 0x28, {0x44});
  queue(m, 0x790, kUIntQueue, 12);
  table(m, 0x250, kEnumEntries, kEnumSlots, 0x38, {0x66});
  queue(m, 0x250, kEnumQueue, 8);
  table(m, 0x70, kArrEntries, kArrSlots, 0x40, {0x33});
  m.put_u32(kArrEntries + 0x20, 40);
  m.put_u64(kArrEntries + 0x28, kArrBits);
  m.put_u32(kArrBits, 0);
  m.put_u32(kArrBits + 4, 1u << 3);  // element 35
  m.put_u32(kMgr + 0xc58, 3);
  m.put_u64(kMgr + 0xc60, kKeys);
  const std::uint64_t keys[] = {5, 9, 0x8c44828386468bfull};
  for (int i = 0; i < 3; ++i) {
    zero(m, kKeys + 16 * i, 16);
    m.put_u64(kKeys + 16 * i, keys[i]);
  }
  m.put_u32(kMgr + 0xc68, kQueueCap);
  m.put_u64(kMgr + 0xc70, kKeyQueue);
  m.put_u32(kMgr + 0xc78, 0);
  for (std::uint32_t i = 0; i < kQueueCap; ++i) {
    m.put_u32(kKeyQueue + 8 * i, 0);
    m.put_u32(kKeyQueue + 8 * i + 4, 0xffffffffu);
  }
  return w;
}

}  // namespace

TEST_CASE("game flags: reads Bool, Int, BoolArray and 64-bit keys from the tables") {
  auto w = game();
  w.mem.put_u8(kBoolEntries + 0x18 + 0x16, 1);  // 0x18 true
  w.mem.put_u32(kIntEntries + 0x1c, static_cast<std::uint32_t>(-7));
  w.mem.put_u8(kKeys + 16 * 2 + 0xc, 1);
  GameFlags f(w);
  CHECK(f.get_bool(0x11) == false);
  CHECK(f.get_bool(0x18) == true);  // found by probing past 0x11
  CHECK_FALSE(f.get_bool(0x25));    // empty slot: not a flag
  CHECK_FALSE(f.get_bool(0));
  CHECK_FALSE(f.get_bool(0x22));    // an Int is not a Bool
  CHECK(f.get_int(0x22) == -7);
  CHECK(f.get_bool_array(0x33, 35) == true);
  CHECK(f.get_bool_array(0x33, 34) == false);
  CHECK_FALSE(f.get_bool_array(0x33, 40));
  CHECK(f.get_key64(0x8c44828386468bfull) == true);
  CHECK(f.get_key64(5) == false);
  CHECK_FALSE(f.get_key64(6));
}

TEST_CASE("game flags: nothing before the manager is up or on bad memory") {
  auto w = game();
  w.mem.put_u64(FakeMemory::kBase + 0x4e842a8, 0);
  CHECK_FALSE(GameFlags(w).get_bool(0x11));
  CHECK_FALSE(GameFlags::set_bool(w, 0x11, true));
  auto v = game();
  v.mem.erase(kBoolEntries + 0x16, 1);
  CHECK_FALSE(GameFlags(v).get_bool(0x11));
}

TEST_CASE("game flags: a set is queued as the game's setter does, and read back at once") {
  auto w = game();
  GameFlags f(w);
  REQUIRE(GameFlags::set_bool(w, 0x11, true));
  CHECK(coop::host::read_u8(w.mem, kBoolQueue) == 1);
  CHECK(coop::host::read_u32(w.mem, kBoolQueue + 4) == 0x11u);
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30100001u);  // 1 taken, 1 finished
  CHECK(coop::host::read_u8(w.mem, kBoolEntries + 0x16) == 0);       // not applied yet
  CHECK(f.get_bool(0x11) == true);                                   // but seen
  REQUIRE(GameFlags::set_bool(w, 0x11, false));
  CHECK(f.get_bool(0x11) == false);  // newest wins

  REQUIRE(GameFlags::set_int(w, 0x22, 1234));
  CHECK(coop::host::read_u32(w.mem, kIntQueue) == 1234u);
  CHECK(coop::host::read_u32(w.mem, kIntQueue + 4) == 0x22u);
  CHECK(f.get_int(0x22) == 1234);

  REQUIRE(GameFlags::set_key64(w, 0x8c44828386468bfull, true));
  CHECK(coop::host::read_u32(w.mem, kKeyQueue + 4) == 2u);  // the record index, not the key
  CHECK(f.get_key64(0x8c44828386468bfull) == true);
  CHECK(f.get_key64(9) == false);
}

TEST_CASE("game flags: each Int write that was queued is told to the hook (row 238, the HUD gauges' refresh)") {
  static std::vector<std::pair<std::uint32_t, std::int32_t>> told;
  told.clear();
  GameFlags::on_int_written([](std::uint32_t hash, std::int32_t value) { told.emplace_back(hash, value); });
  auto w = game();
  REQUIRE(GameFlags::set_int(w, 0x22, 16));
  CHECK_FALSE(GameFlags::set_int(w, 0x99, 1));  // an unknown flag: nothing queued, nothing told
  REQUIRE(GameFlags::set_bool(w, 0x11, true));  // not an Int
  GameFlags::on_int_written(nullptr);
  REQUIRE(GameFlags::set_int(w, 0x22, 20));     // no hook: still written
  REQUIRE(told.size() == 1);
  CHECK(told[0].first == 0x22u);
  CHECK(told[0].second == 16);
}

TEST_CASE("game flags: UInt flags are read and queued like Int, in their own table") {
  auto w = game();
  w.mem.put_u32(kUIntEntries + 0x1c, 0xfffffff0u);
  GameFlags f(w);
  CHECK(f.get_uint(0x44) == 0xfffffff0u);
  CHECK_FALSE(f.get_uint(0x22));   // an Int is not a UInt
  CHECK_FALSE(f.get_int(0x44));    // nor the other way
  CHECK_FALSE(GameFlags::set_uint(w, 0x22, 1));
  REQUIRE(GameFlags::set_uint(w, 0x44, 35));
  CHECK(coop::host::read_u32(w.mem, kUIntQueue) == 35u);
  CHECK(coop::host::read_u32(w.mem, kUIntQueue + 4) == 0x44u);
  CHECK(coop::host::read_u32(w.mem, kUIntQueue + 8) == 1u);  // as the game's UInt setter
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x7c0) == 0x30100001u);  // 1 taken, 1 finished
  CHECK(coop::host::read_u32(w.mem, kUIntEntries + 0x1c) == 0xfffffff0u);  // not applied yet
  CHECK(f.get_uint(0x44) == 35u);                                         // but seen
  CHECK(coop::host::read_u32(w.mem, kIntQueue + 4) == 0u);  // the Int queue is untouched
  REQUIRE(GameFlags::set_int(w, 0x22, 5));
  CHECK(coop::host::read_u32(w.mem, kIntQueue + 8) == 0u);  // an Int slot has no third word
}

TEST_CASE("game flags: Enum flags hold a name's hash, queued in 8-byte slots (held-gear-2)") {
  auto w = game();
  w.mem.put_u32(kEnumEntries + 0x1c, coop::host::flag_hash("Empty"));
  GameFlags f(w);
  CHECK(f.get_enum(0x66) == coop::host::flag_hash("Empty"));
  CHECK(coop::host::flag_hash("Empty") == 0xbe69de49u);  // GameDataList's default for the item buttons
  CHECK_FALSE(f.get_enum(0x44));  // a UInt is not an Enum
  CHECK_FALSE(GameFlags::set_enum(w, 0x22, 1));
  REQUIRE(GameFlags::set_enum(w, 0x66, coop::host::flag_hash("Bow")));
  CHECK(coop::host::read_u32(w.mem, kEnumQueue) == coop::host::flag_hash("Bow"));
  CHECK(coop::host::read_u32(w.mem, kEnumQueue + 4) == 0x66u);
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x280) == 0x30100001u);  // 1 taken, 1 finished
  CHECK(f.get_enum(0x66) == coop::host::flag_hash("Bow"));
  REQUIRE(GameFlags::set_enum(w, 0x66, coop::host::flag_hash("Hookshot")));
  CHECK(coop::host::read_u32(w.mem, kEnumQueue + 8) == coop::host::flag_hash("Hookshot"));  // next slot
  CHECK(f.get_enum(0x66) == coop::host::flag_hash("Hookshot"));  // newest wins
}

TEST_CASE("game flags: refused when unknown, full, or the queue was applied meanwhile") {
  auto w = game();
  CHECK_FALSE(GameFlags::set_bool(w, 0x25, true));
  CHECK_FALSE(GameFlags::set_bool(w, 0x22, true));  // an Int hash
  CHECK_FALSE(GameFlags::set_key64(w, 6, true));
  // Half the queue stays the game's (issue 175).
  for (std::uint32_t i = 0; i < kQueueCap / 2; ++i) REQUIRE(GameFlags::set_bool(w, 0x18, true));
  CHECK_FALSE(GameFlags::set_bool(w, 0x18, true));
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30400004u);  // 4 of 8 taken, 4 finished

  auto v = game();
  int cas = 0;
  v.after_cas = [&] {
    if (++cas == 1) v.mem.put_u32(kMgr + 0x40, 0x40000000);  // the game emptied the queue
  };
  CHECK_FALSE(GameFlags::set_bool(v, 0x11, true));
}

TEST_CASE("game flags: slots the game took count against our half (issue 175)") {
  auto w = game();
  w.mem.put_u32(kMgr + 0x40, 0x30300003u);  // the game's own sets: 3 taken, 3 finished
  REQUIRE(GameFlags::set_bool(w, 0x11, true));  // the 4th slot
  CHECK_FALSE(GameFlags::set_bool(w, 0x11, false));  // half of 8 taken: the rest is the game's
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30400004u);
  w.mem.put_u32(kMgr + 0x40, 0x40000000u);  // the game applied the queue: room again
  CHECK(GameFlags::set_bool(w, 0x11, false));
}

TEST_CASE("game flags: the queue sizes, for the log (issue 175)") {
  auto w = game();
  const auto q = GameFlags(w).queue_sizes();
  REQUIRE(q);
  CHECK(q->boolean == kQueueCap);
  CHECK(q->int32 == kQueueCap);
  CHECK(q->uint32 == kQueueCap);
  CHECK(q->enumeration == kQueueCap);
  CHECK(q->key64 == kQueueCap);
  w.mem.put_u64(FakeMemory::kBase + 0x4e842a8, 0);
  CHECK_FALSE(GameFlags(w).queue_sizes());  // no manager yet
}

TEST_CASE("game flag store: busy while its queue has no room for us (issue 175)") {
  auto w = game();
  GameFlagStore store(w);
  const auto chest = protocol::FlagKey::key64_flag(0x8c44828386468bfull);
  CHECK_FALSE(store.busy({0x11, 0}));
  w.mem.put_u32(kMgr + 0x40, 0x30400004u);  // the Bool queue: 4 of 8 taken
  CHECK(store.busy({0x11, 0}));
  CHECK_FALSE(store.write({0x11, 0, protocol::FlagKind::boolean, 1}));
  CHECK_FALSE(store.busy({0x22, 0}));  // an Int: its own queue
  CHECK_FALSE(store.busy(chest));
  w.mem.put_u32(kMgr + 0xc78, 4);
  CHECK(store.busy(chest));
  CHECK_FALSE(store.busy({0x25, 0}));  // unknown: refused for that, not for room
  CHECK_FALSE(store.busy({0x33, 1}));  // a BoolArray element is never written
  coop::host::HoldFlagStore held(store, [](coop::host::FlagKey) { return false; });
  CHECK(held.busy({0x11, 0}));  // the wrappers ask the game's store
}

TEST_CASE("game flag store: a value the game already holds takes no slot (issue 175)") {
  auto w = game();
  GameFlagStore store(w);
  CHECK(store.write({0x11, 0, protocol::FlagKind::boolean, 0}));  // already 0
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30000000u);  // nothing taken
  CHECK(store.write({0x11, 0, protocol::FlagKind::boolean, 1}));
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30100001u);
  CHECK(store.write({0x11, 0, protocol::FlagKind::boolean, 1}));  // queued already
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30100001u);
  const auto chest = protocol::FlagKey::key64_flag(0x8c44828386468bfull);
  CHECK(store.write({chest, protocol::FlagKind::boolean, 0}));
  CHECK(coop::host::read_u32(w.mem, kMgr + 0xc78) == 0u);  // nothing taken
  CHECK_FALSE(store.write({0x11, 0, protocol::FlagKind::int32, 1}));  // still refused
}

TEST_CASE("game flags: the finished count wraps as the game's does") {
  auto w = game();
  w.mem.put_u32(kMgr + 0x40, 0x3ff00000);  // finished count full
  REQUIRE(GameFlags::set_bool(w, 0x11, true));
  CHECK(coop::host::read_u32(w.mem, kMgr + 0x40) == 0x30000001u);
}

TEST_CASE("game flag store: kinds for FlagWatcher") {
  auto w = game();
  w.mem.put_u32(kIntEntries + 0x1c, 3);
  GameFlagStore store(w);
  CHECK(store.read({0x11, 0}) == protocol::FlagEntry{0x11, 0, protocol::FlagKind::boolean, 0});
  CHECK(store.read({0x22, 0}) == protocol::FlagEntry{0x22, 0, protocol::FlagKind::int32, 3});
  CHECK(store.read({0x33, 35}) == protocol::FlagEntry{0x33, 35, protocol::FlagKind::boolean, 1});
  CHECK_FALSE(store.read({0x25, 0}));
  CHECK(store.write({0x11, 0, protocol::FlagKind::boolean, 1}));
  CHECK(store.read({0x11, 0})->value == 1);
  CHECK_FALSE(store.write({0x11, 0, protocol::FlagKind::boolean, 2}));
  CHECK_FALSE(store.write({0x11, 0, protocol::FlagKind::int32, 1}));  // Bool hash, Int kind
  CHECK_FALSE(store.write({0x33, 1, protocol::FlagKind::boolean, 1}));
  CHECK(store.write({0x22, 0, protocol::FlagKind::int32, static_cast<std::uint32_t>(-1)}));
  CHECK(store.read({0x22, 0})->value == 0xffffffffu);
}

TEST_CASE("game flag store: 64-bit keys for FlagWatcher") {
  auto w = game();
  GameFlagStore store(w);
  const auto chest = protocol::FlagKey::key64_flag(0x8c44828386468bfull);
  CHECK(store.read(chest) == protocol::FlagEntry{chest, protocol::FlagKind::boolean, 0});
  CHECK_FALSE(store.read(protocol::FlagKey::key64_flag(6)));  // not in the table
  CHECK_FALSE(store.read(protocol::FlagKey::key64_flag(0x8c44828386468bfull, 1)));  // no table 1

  CHECK(store.write({chest, protocol::FlagKind::boolean, 1}));
  CHECK(coop::host::read_u32(w.mem, kKeyQueue + 4) == 2u);  // queued by record index
  CHECK(store.read(chest)->value == 1);
  CHECK_FALSE(store.write({chest, protocol::FlagKind::boolean, 2}));
  CHECK_FALSE(store.write({chest, protocol::FlagKind::int32, 1}));
  CHECK_FALSE(store.write({protocol::FlagKey::key64_flag(6), protocol::FlagKind::boolean, 1}));
  CHECK_FALSE(store.write({protocol::FlagKey::key64_flag(0x8c44828386468bfull, 1),
                           protocol::FlagKind::boolean, 1}));
}

TEST_CASE("game flags: a String64 written at once into its buffer, and its type found") {
  auto w = game();
  auto& m = w.mem;
  constexpr std::uint64_t kStrEntries = 0x8500000000ull, kStrSlots = 0x8510000000ull,
                          kStrText = 0x8520000000ull;
  table(m, 0x610, kStrEntries, kStrSlots, 0x108, {0x44});
  m.put_u64(kStrEntries + 0x68, kStrText);
  m.put_u32(kStrEntries + 0x70, 64);
  zero(m, kStrText, 64);
  CHECK(GameFlags::set_string64_now(w, 0x44, "KakarikoVillage_Child"));
  GameFlags flags(w);
  CHECK(flags.get_string64(0x44) == "KakarikoVillage_Child");
  CHECK(GameFlags::set_string64_now(w, 0x44, ""));
  CHECK(flags.get_string64(0x44) == "");
  CHECK_FALSE(GameFlags::set_string64_now(w, 0x45, "x"));               // unknown
  CHECK_FALSE(GameFlags::set_string64_now(w, 0x44, std::string(64, 'a')));  // too long
  CHECK_FALSE(GameFlags::set_string64_now(w, 0x44, "a\nb"));
  CHECK(flags.find_type(0x44) == 16);
  CHECK(flags.find_type(0x22) == 2);
  CHECK(flags.find_type(0x11) == 0);
  CHECK_FALSE(flags.find_type(0x99));
}

TEST_CASE("game flags: the writes waiting in the game's queues, as its applier is about to take them") {
  using coop::host::GameFlagType;
  using coop::host::GameFlagWrite;
  auto w = game();
  std::vector<GameFlagWrite> got;
  const auto sink = [&](const GameFlagWrite& write) { got.push_back(write); };
  CHECK(coop::host::queued_flag_writes(w, 0, sink) == 0);  // nothing queued

  // Queued as the game's setters do (here: our own queue writes, the same slots).
  REQUIRE(GameFlags::set_bool(w, 0x18, true));
  REQUIRE(GameFlags::set_bool(w, 0x11, false));
  REQUIRE(GameFlags::set_int(w, 0x22, -3));
  REQUIRE(GameFlags::set_enum(w, 0x66, 0xabcdef01u));
  REQUIRE(GameFlags::set_uint(w, 0x44, 7));
  REQUIRE(GameFlags::set_key64(w, 0x8c44828386468bfull, true));
  REQUIRE(GameFlags::set_key64(w, 5, false));
  CHECK(coop::host::queued_flag_writes(w, kMgr, sink) == 7);
  CHECK(got == std::vector<GameFlagWrite>{
                   {GameFlagType::boolean, 0x18, 1},
                   {GameFlagType::boolean, 0x11, 0},
                   {GameFlagType::int32, 0x22, static_cast<std::uint32_t>(-3)},
                   {GameFlagType::enum32, 0x66, 0xabcdef01u},
                   {GameFlagType::uint32, 0x44, 7},
                   {GameFlagType::key64, 0x8c44828386468bfull, 1},
                   {GameFlagType::key64, 5, 0},
               });
  CHECK(coop::host::key_of(got[0]) == coop::host::FlagKey{0x18u, 0});
  CHECK(coop::host::key_of(got[5]) == coop::host::FlagKey::key64_flag(0x8c44828386468bfull));

  // The manager main points to when none is given; a Bool slot's other bytes are not its value.
  got.clear();
  w.mem.put_u32(kBoolQueue, 0xffffff00u);  // slot 0: value byte 0, junk above it
  CHECK(coop::host::queued_flag_writes(w, 0, sink) == 7);
  CHECK(got[0] == GameFlagWrite{GameFlagType::boolean, 0x18, 0});

  // A count past the queue's size is held to it; an empty slot and a record index past the table are skipped.
  got.clear();
  w.mem.put_u32(kMgr + 0x10 + 0x30, 0x300fffffu);
  w.mem.put_u32(kKeyQueue + 8 * 1 + 4, 77);
  CHECK(coop::host::queued_flag_writes(w, 0, sink) == 6);
  CHECK(got[0].id == 0x18);
  CHECK(got[1].id == 0x11);
  CHECK(got[2].type == GameFlagType::int32);
  CHECK(got.back() == GameFlagWrite{GameFlagType::key64, 0x8c44828386468bfull, 1});

  // After the applier emptied the queues: nothing. No manager: nothing.
  for (const std::uint64_t word : {0x10 + 0x30, 0xd0 + 0x30, 0x250 + 0x30, 0x790 + 0x30, 0xc78}) {
    w.mem.put_u32(kMgr + word, 0x40000000u);
  }
  CHECK(coop::host::queued_flag_writes(w, 0, sink) == 0);
  w.mem.put_u64(FakeMemory::kBase + 0x4e842a8, 0);
  CHECK(coop::host::queued_flag_writes(w, 0, sink) == 0);
}

TEST_CASE("game flags: with the game's setters a write waits in the call list, not in the game's queue") {
  using coop::host::GameFlagType;
  using coop::host::GameFlagWrite;
  auto w = game();
  coop::host::GameFlagCalls calls;
  GameFlags::use_calls(&calls);
  struct Off {
    ~Off() { GameFlags::use_calls(nullptr); }
  } off;
  GameFlags flags(w);
  GameFlagStore store(w);

  CHECK(GameFlags::set_bool(w, 0x18, true));
  CHECK(GameFlags::set_int(w, 0x22, 41));
  CHECK(GameFlags::set_uint(w, 0x44, 3));
  CHECK(GameFlags::set_enum(w, 0x66, 0x1234));
  CHECK(GameFlags::set_key64(w, 9, true));
  CHECK_FALSE(GameFlags::set_bool(w, 0x25, true));  // not a flag: refused as before
  CHECK_FALSE(GameFlags::set_key64(w, 6, true));
  CHECK(calls.size() == 5);
  // Nothing went into the game's queues or tables.
  for (const std::uint64_t word : {0x10 + 0x30, 0xd0 + 0x30, 0x250 + 0x30, 0x790 + 0x30}) {
    CHECK(*coop::host::read_u32(w.mem, kMgr + word) == 0x30000000u);
  }
  CHECK(*coop::host::read_u32(w.mem, kMgr + 0xc78) == 0);
  // A read sees the waiting write, as it sees a queued one.
  CHECK(flags.get_bool(0x18) == true);
  CHECK(flags.get_bool(0x11) == false);
  CHECK(flags.get_int(0x22) == 41);
  CHECK(flags.get_uint(0x44) == 3u);
  CHECK(flags.get_enum(0x66) == 0x1234u);
  CHECK(flags.get_key64(9) == true);
  CHECK(flags.get_key64(5) == false);
  // The store: a value that already waits takes no second place; never busy while the list has room.
  const protocol::FlagEntry door{{0x18u, 0}, protocol::FlagKind::boolean, 1};
  CHECK(store.write(door));
  CHECK(calls.size() == 5);
  CHECK_FALSE(store.busy({0x18u, 0}));
  CHECK(store.write({{0x11u, 0}, protocol::FlagKind::boolean, 1}));
  CHECK(calls.size() == 6);

  // The calc thread hands them to the game's setter while its queue has room: (manager, value, hash or key).
  CHECK(GameFlags::setter_has_room(w, GameFlagType::boolean));
  CHECK(GameFlags::manager_address(w) == kMgr);
  std::vector<GameFlagWrite> made;
  CHECK(calls.run(64, [&](const GameFlagWrite& write) {
    if (!GameFlags::setter_has_room(w, write.type)) return false;
    made.push_back(write);
    return true;
  }) == 6);
  CHECK(made.front() == GameFlagWrite{GameFlagType::boolean, 0x18, 1});
  CHECK(made.back() == GameFlagWrite{GameFlagType::boolean, 0x11, 1});
  CHECK(flags.get_bool(0x18) == false);  // (this test's setter wrote nothing: the table's value again)

  // No room: half of the game's queue is taken (kQueueCap 8), so nothing of ours is handed over.
  w.mem.put_u32(kMgr + 0x10 + 0x30, 0x30000004u);
  CHECK_FALSE(GameFlags::setter_has_room(w, GameFlagType::boolean));
  CHECK(GameFlags::setter_has_room(w, GameFlagType::int32));
  CHECK(GameFlags::set_bool(w, 0x18, true));
  CHECK(calls.run(64, [&](const GameFlagWrite& write) { return GameFlags::setter_has_room(w, write.type); }) == 0);
  CHECK(calls.size() == 1);
  CHECK(flags.get_bool(0x18) == true);
}

TEST_CASE("game flags: without the call list the setters write the game's queues as before") {
  auto w = game();
  GameFlags::use_calls(nullptr);
  CHECK(GameFlags::set_bool(w, 0x18, true));
  CHECK((*coop::host::read_u32(w.mem, kMgr + 0x10 + 0x30) & 0xfffff) == 1);
}

TEST_CASE("game flags: Float flags (PlayerStatus.DiveDepth) read and set as the game's Float setter does") {
  using coop::host::GameFlagType;
  constexpr std::uint64_t kFloatEntries = 0x8700000000ull, kFloatSlots = 0x8710000000ull,
                          kFloatQueue = 0x8720000000ull;
  auto w = game();
  table(w.mem, 0x190, kFloatEntries, kFloatSlots, 0x28, {0x77});
  queue(w.mem, 0x190, kFloatQueue, 12);
  w.mem.put_u32(kFloatEntries + 0x1c, std::bit_cast<std::uint32_t>(3.0f));
  GameFlags f(w);
  CHECK(f.get_float(0x77) == 3.0f);
  CHECK_FALSE(f.get_float(0x22));  // an Int is not a Float
  CHECK_FALSE(GameFlags::set_float(w, 0x22, 6.0f));
  // Without the call list: into the Float queue, {f32 value, u32 hash}, as f_7100ba6f80 writes it.
  REQUIRE(GameFlags::set_float(w, 0x77, 6.0f));
  CHECK(coop::host::read_u32(w.mem, kFloatQueue) == std::bit_cast<std::uint32_t>(6.0f));
  CHECK(coop::host::read_u32(w.mem, kFloatQueue + 4) == 0x77u);
  CHECK(f.get_float(0x77) == 6.0f);
  CHECK(coop::host::read_u32(w.mem, kIntQueue + 4) == 0u);  // the Int queue is untouched
  CHECK(GameFlags::setter_has_room(w, GameFlagType::float32));
  // With the game's setters: the write waits in the call list as a float32 with the value's bits.
  coop::host::GameFlagCalls calls;
  GameFlags::use_calls(&calls);
  struct Off {
    ~Off() { GameFlags::use_calls(nullptr); }
  } off;
  REQUIRE(GameFlags::set_float(w, 0x77, 9.0f));
  CHECK(calls.pending(GameFlagType::float32, 0x77) == std::bit_cast<std::uint32_t>(9.0f));
  CHECK(f.get_float(0x77) == 9.0f);
}

namespace {
// String64 0x44 (entry 0) with its live text, and the game's String64 queue
// (M + 0x630 count, slots of 0x58 at M + 0x638, word at M + 0x640): each slot
// has its own 64-byte text buffer at +0, its size at +8, the hash at +0x50.
constexpr std::uint64_t kS64Entries = 0x8700000000ull, kS64Slots = 0x8710000000ull, kS64Text = 0x8720000000ull,
                        kS64Queue = 0x8730000000ull, kS64SlotTexts = 0x8740000000ull;
constexpr std::uint64_t kS64Word = kMgr + 0x640;
Writable game_with_strings() {
  auto w = game();
  auto& m = w.mem;
  table(m, 0x610, kS64Entries, kS64Slots, 0x108, {0x44});
  m.put_u64(kS64Entries + 0x68, kS64Text);
  m.put_u32(kS64Entries + 0x70, 64);
  zero(m, kS64Text, 64);
  queue(m, 0x610, kS64Queue, 0x58);
  for (std::uint32_t i = 0; i < kQueueCap; ++i) {
    m.put_u64(kS64Queue + 0x58 * i, kS64SlotTexts + 64 * i);
    m.put_u32(kS64Queue + 0x58 * i + 8, 64);
    zero(m, kS64SlotTexts + 64 * i, 64);
  }
  return w;
}
std::string slot_text(FakeMemory& m, std::uint32_t i) {
  std::string out;
  for (std::uint64_t a = kS64SlotTexts + 64 * i; *coop::host::read_u8(m, a) != 0; ++a) {
    out.push_back(static_cast<char>(*coop::host::read_u8(m, a)));
  }
  return out;
}
}  // namespace

TEST_CASE("game flags: a String64 is queued as the game's push does, so the saved copy changes too (issue 57)") {
  auto w = game_with_strings();
  GameFlags::use_calls(nullptr);
  GameFlags flags(w);
  CHECK(flags.get_string64(0x44) == "");
  REQUIRE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Fairy"));
  // One slot taken and finished (round 3), the hash at +0x50, the text in the slot's own buffer.
  CHECK(*coop::host::read_u32(w.mem, kS64Word) == 0x30000000u + 1 + 0x100000);
  CHECK(*coop::host::read_u32(w.mem, kS64Queue + 0x50) == 0x44u);
  CHECK(slot_text(w.mem, 0) == "Item_Ocarina_Fairy");
  CHECK(*coop::host::read_u8(w.mem, kS64Text) == 0);  // the table is the applier's to write
  // Read back at once from the queue, the newest write first.
  CHECK(flags.get_string64(0x44) == "Item_Ocarina_Fairy");
  REQUIRE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Time"));
  CHECK(slot_text(w.mem, 1) == "Item_Ocarina_Time");
  CHECK(flags.get_string64(0x44) == "Item_Ocarina_Time");
  // Refused: unknown, too long, not printable.
  CHECK_FALSE(GameFlags::set_string64(w, 0x45, "x"));
  CHECK_FALSE(GameFlags::set_string64(w, 0x44, std::string(64, 'a')));
  CHECK_FALSE(GameFlags::set_string64(w, 0x44, "a\nb"));
  // Our half of the queue (4 of 8) is full after two more.
  CHECK(GameFlags::setter_has_room(w, coop::host::GameFlagType::string64));
  CHECK(GameFlags::set_string64(w, 0x44, "a"));
  CHECK(GameFlags::set_string64(w, 0x44, "b"));
  CHECK_FALSE(GameFlags::setter_has_room(w, coop::host::GameFlagType::string64));
  CHECK_FALSE(GameFlags::set_string64(w, 0x44, "c"));
}

TEST_CASE("game flags: a String64 slot whose buffer is too small is finished empty, and the write refused") {
  auto w = game_with_strings();
  GameFlags::use_calls(nullptr);
  w.mem.put_u32(kS64Queue + 8, 4);  // slot 0's buffer holds 3 characters
  CHECK_FALSE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Fairy"));
  // Finished (the applier walks only to the first unfinished slot), with no flag in it.
  CHECK(*coop::host::read_u32(w.mem, kS64Word) == 0x30000000u + 1 + 0x100000);
  CHECK(*coop::host::read_u32(w.mem, kS64Queue + 0x50) == 0u);
  CHECK(GameFlags(w).get_string64(0x44) == "");
}

TEST_CASE("game flags: with the game's setters a String64 waits in the call list with its text") {
  using coop::host::GameFlagType;
  using coop::host::GameFlagWrite;
  auto w = game_with_strings();
  coop::host::GameFlagCalls calls;
  GameFlags::use_calls(&calls);
  struct Off {
    ~Off() { GameFlags::use_calls(nullptr); }
  } off;
  GameFlags flags(w);
  REQUIRE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Fairy"));
  CHECK(*coop::host::read_u32(w.mem, kS64Word) == 0x30000000u);  // nothing in the game's queue
  CHECK(calls.size() == 1);
  CHECK(flags.get_string64(0x44) == "Item_Ocarina_Fairy");
  // The same flag again keeps its place, with the newer text.
  REQUIRE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Time"));
  CHECK(calls.size() == 1);
  std::vector<std::string> handed;
  CHECK(calls.run(8, [&](const GameFlagWrite& write) {
    CHECK(write.type == GameFlagType::string64);
    CHECK(write.id == 0x44u);
    handed.push_back(calls.text(write).value_or("?"));
    return true;
  }) == 1);
  CHECK(handed == std::vector<std::string>{"Item_Ocarina_Time"});
  CHECK(calls.size() == 0);
  CHECK_FALSE(calls.pending_string(0x44));
  CHECK(flags.get_string64(0x44) == "");  // (this test's setter wrote nothing)

  // A newer text put while the setter has the older one is not lost: it is handed over after it.
  REQUIRE(GameFlags::set_string64(w, 0x44, "A"));
  std::vector<std::string> seen;
  CHECK(calls.run(8, [&](const GameFlagWrite& write) {
    seen.push_back(calls.text(write).value_or("?"));
    if (seen.size() == 1) calls.put_string(0x44, "B");
    return true;
  }) == 2);
  CHECK(seen == std::vector<std::string>{"A", "B"});
  CHECK_FALSE(calls.pending_string(0x44));
  // A setter that cannot take it now keeps it waiting; clear() drops it.
  REQUIRE(GameFlags::set_string64(w, 0x44, "C"));
  CHECK(calls.run(8, [](const GameFlagWrite&) { return false; }) == 0);
  CHECK(calls.pending_string(0x44) == "C");
  calls.clear();
  CHECK_FALSE(calls.pending_string(0x44));
}

TEST_CASE("flag setters: a String64 goes to the game's push with its text on the game's stack") {
  auto w = game_with_strings();
  coop::host::GameFlagCalls& calls = CoopFlagNative::Calls();
  calls.clear();
  GameFlags::use_calls(&calls);
  struct Off {
    ~Off() {
      GameFlags::use_calls(nullptr);
      CoopFlagNative::Calls().clear();
    }
  } off;
  REQUIRE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Fairy"));
  REQUIRE(GameFlags::set_bool(w, 0x18, true));
  constexpr std::uint64_t kS64Stack = 0x8750000000ull;
  std::vector<std::uint8_t> stack;
  struct Made {
    std::uint64_t function, x0, x1, x2;
  };
  std::vector<Made> made;
  const bool ok = CoopFlagNative::RunSetters(
      w,
      [&](std::uint64_t function, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2,
          std::optional<std::uint32_t>) {
        made.push_back({function, x0, x1, x2});
        return true;
      },
      [&](const void* data, std::size_t n) -> std::uint64_t {
        stack.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + n);
        return kS64Stack;
      });
  CHECK(ok);
  REQUIRE(made.size() == 2);
  // (manager + 0x608, &{char* text, u32 hash}) to f_710376b9b0.
  CHECK(made[0].function == FakeMemory::kBase + 0x376b9b0);
  CHECK(made[0].x0 == kMgr + 0x608);
  CHECK(made[0].x1 == kS64Stack);
  REQUIRE(stack.size() >= 0x10 + 19);
  std::uint64_t text_at = 0;
  std::uint32_t hash = 0;
  std::memcpy(&text_at, stack.data(), 8);
  std::memcpy(&hash, stack.data() + 8, 4);
  CHECK(text_at == kS64Stack + 0x10);
  CHECK(hash == 0x44u);
  CHECK(std::string(reinterpret_cast<const char*>(stack.data() + 0x10)) == "Item_Ocarina_Fairy");
  CHECK(made[1].function == FakeMemory::kBase + 0x682b20);  // the Bool after it, as before
  CHECK(calls.size() == 0);

  // No scratch (an older Hookshot): the setters stop, and the caller goes back to the queues.
  REQUIRE(GameFlags::set_string64(w, 0x44, "Item_Ocarina_Time"));
  CHECK_FALSE(CoopFlagNative::RunSetters(
      w, [](std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::optional<std::uint32_t>) { return true; },
      [](const void*, std::size_t) -> std::uint64_t { return 0; }));
  CHECK(calls.size() == 1);
}
