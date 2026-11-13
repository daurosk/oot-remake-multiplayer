#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "coop/host/game_flag_events.hpp"
#include "coop/host/game_flags.hpp"
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
