#include <doctest/doctest.h>

#include <map>
#include <set>
#include <thread>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/game_flag_events.hpp"
#include "coop/protocol/codec.hpp"

// The game's flag writes as events: the ring from the game's threads, the
// setter list, and the watcher reading only what the game wrote. The queue
// decode and the setter routing of GameFlags are in game_flags_test.cpp,
// which has the manager's layout.
using namespace coop::host;
namespace proto = coop::protocol;

namespace {

constexpr FlagKey kChest{0x1000u, 0};
constexpr FlagKey kDoor{0x2000u, 0};
constexpr FlagKey kKey0{0x3000u, 0};
constexpr FlagKey kKey1{0x3000u, 1};

// The game's flags, counting each read.
class CountingStore : public FlagStore {
 public:
  std::map<FlagKey, proto::FlagEntry> values;
  std::map<FlagKey, int> reads;
  int all_reads = 0;

  void set(FlagKey key, std::uint32_t value) { values[key] = {key, proto::FlagKind::boolean, value}; }
  std::optional<proto::FlagEntry> read(FlagKey key) override {
    ++reads[key];
    ++all_reads;
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
  }
  bool write(const proto::FlagEntry& e) override {
    if (!values.contains(key_of(e))) return false;
    values[key_of(e)] = e;
    return true;
  }
};

std::vector<FlagKey> many(std::uint32_t n) {
  std::vector<FlagKey> keys;
  for (std::uint32_t i = 1; i <= n; ++i) keys.push_back({i, 0});
  return keys;
}

std::vector<proto::FlagEntry> sent(const std::vector<Outgoing>& out) {
  std::vector<proto::FlagEntry> flags;
  for (const auto& o : out) {
    proto::Message m;
    REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
    if (const auto* set = std::get_if<proto::FlagSet>(&m)) flags.insert(flags.end(), set->flags.begin(), set->flags.end());
  }
  return flags;
}

}  // namespace

TEST_CASE("flag events: the ring hands writes over in order and remembers an overflow") {
  GameFlagEvents ring;
  GameFlagWrite got;
  CHECK_FALSE(ring.pop(got));
  CHECK(ring.push({GameFlagType::boolean, 0x11, 1}));
  CHECK(ring.push({GameFlagType::key64, 0x8c44828386468bfull, 1}));
  CHECK(ring.push({GameFlagType::int32, 0x22, static_cast<std::uint32_t>(-7)}));
  REQUIRE(ring.pop(got));
  CHECK(got == GameFlagWrite{GameFlagType::boolean, 0x11, 1});
  REQUIRE(ring.pop(got));
  CHECK(got == GameFlagWrite{GameFlagType::key64, 0x8c44828386468bfull, 1});
  CHECK(key_of(got) == FlagKey::key64_flag(0x8c44828386468bfull));
  REQUIRE(ring.pop(got));
  CHECK(key_of(got) == FlagKey{0x22u, 0});
  CHECK_FALSE(ring.pop(got));
  CHECK_FALSE(ring.take_overflow());

  // Full: the push is refused and remembered once; room again after a pop.
  for (std::size_t i = 0; i < GameFlagEvents::kSlots; ++i) CHECK(ring.push({GameFlagType::boolean, i + 1, 1}));
  CHECK_FALSE(ring.push({GameFlagType::boolean, 0x99, 1}));
  CHECK(ring.dropped() == 1);
  CHECK(ring.take_overflow());
  CHECK_FALSE(ring.take_overflow());
  REQUIRE(ring.pop(got));
  CHECK(got.id == 1);
  CHECK(ring.push({GameFlagType::boolean, 0x99, 1}));
  ring.clear();
  CHECK_FALSE(ring.pop(got));
  // Many times around the ring.
  for (std::uint64_t i = 0; i < 3 * GameFlagEvents::kSlots; ++i) {
    REQUIRE(ring.push({GameFlagType::int32, i, 5}));
    REQUIRE(ring.pop(got));
    REQUIRE(got.id == i);
  }
}

TEST_CASE("flag events: pushes from several threads all arrive, each once") {
  GameFlagEvents ring;
  constexpr std::uint64_t kThreads = 4, kEach = 20000;
  std::vector<std::thread> threads;
  for (std::uint64_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ring, t] {
      for (std::uint64_t i = 0; i < kEach; ++i) {
        while (!ring.push({GameFlagType::boolean, t * kEach + i + 1, 1})) std::this_thread::yield();
      }
    });
  }
  std::set<std::uint64_t> seen;
  std::vector<std::uint64_t> last(kThreads, 0);
  bool ordered = true;
  GameFlagWrite got;
  while (seen.size() < kThreads * kEach) {
    if (!ring.pop(got)) {
      std::this_thread::yield();
      continue;
    }
    const std::uint64_t thread = (got.id - 1) / kEach;
    ordered = ordered && got.id > last[thread];  // one thread's writes stay in its order
    last[thread] = got.id;
    seen.insert(got.id);
  }
  for (auto& t : threads) t.join();
  CHECK(ordered);
  CHECK(seen.size() == kThreads * kEach);
  CHECK_FALSE(ring.pop(got));
}

TEST_CASE("flag calls: writes wait in order, the newest value wins, and the game's refusal keeps the rest") {
  GameFlagCalls calls;
  CHECK(calls.put({GameFlagType::boolean, 0x11, 1}));
  CHECK(calls.put({GameFlagType::int32, 0x22, 5}));
  CHECK(calls.put({GameFlagType::key64, 9, 1}));
  CHECK(calls.put({GameFlagType::int32, 0x22, 6}));  // the same flag: its value, its old place
  CHECK(calls.size() == 3);
  CHECK(calls.pending(GameFlagType::int32, 0x22) == 6u);
  CHECK(calls.pending(GameFlagType::boolean, 0x11) == 1u);
  CHECK_FALSE(calls.pending(GameFlagType::boolean, 0x22));  // another type's flag of that hash
  CHECK_FALSE(calls.pending(GameFlagType::key64, 5));

  std::vector<GameFlagWrite> made;
  // One a run.
  CHECK(calls.run(1, [&](const GameFlagWrite& w) { made.push_back(w); return true; }) == 1);
  CHECK(made == std::vector{GameFlagWrite{GameFlagType::boolean, 0x11, 1}});
  CHECK_FALSE(calls.pending(GameFlagType::boolean, 0x11));  // in the game's queue now
  // The game has no room for the Int: it and the key after it stay.
  CHECK(calls.run(8, [&](const GameFlagWrite&) { return false; }) == 0);
  CHECK(calls.size() == 2);
  CHECK(calls.pending(GameFlagType::int32, 0x22) == 6u);
  CHECK(calls.run(8, [&](const GameFlagWrite& w) { made.push_back(w); return true; }) == 2);
  CHECK(made[1] == GameFlagWrite{GameFlagType::int32, 0x22, 6});
  CHECK(made[2] == GameFlagWrite{GameFlagType::key64, 9, 1});
  CHECK(calls.size() == 0);
  CHECK(calls.handed() == 3);
  CHECK(calls.run(8, [&](const GameFlagWrite&) { return true; }) == 0);
}

TEST_CASE("flag calls: a value put while the setter runs is handed over next") {
  GameFlagCalls calls;
  CHECK(calls.put({GameFlagType::boolean, 0x11, 1}));
  std::vector<std::uint32_t> made;
  CHECK(calls.run(1, [&](const GameFlagWrite& w) {
    made.push_back(w.value);
    calls.put({GameFlagType::boolean, 0x11, 0});  // (from the watcher's thread, in the game)
    return true;
  }) == 1);
  CHECK(calls.pending(GameFlagType::boolean, 0x11) == 0u);
  CHECK(calls.run(4, [&](const GameFlagWrite& w) { made.push_back(w.value); return true; }) == 1);
  CHECK(made == std::vector<std::uint32_t>{1, 0});
  CHECK(calls.size() == 0);
}

TEST_CASE("flag calls: the setters' addresses and argument order are the game's") {
  // (manager, value, hash or key) for each: docs/flags.md, "Functions".
  CHECK(setter_offset(GameFlagType::boolean) == 0x682b20);
  CHECK(setter_offset(GameFlagType::int32) == 0x163b70);
  CHECK(setter_offset(GameFlagType::uint32) == 0x57af30);
  CHECK(setter_offset(GameFlagType::enum32) == 0x3e0980);
  CHECK(setter_offset(GameFlagType::key64) == 0x3761720);
}

TEST_CASE("flag watcher, events: only the flags the game wrote are read while playing") {
  CountingStore store;
  const auto keys = many(1000);
  for (FlagKey k : keys) store.set(k, 0);
  FlagWatcher w(SharedFlags(keys), FlagRole::player);
  w.use_events(true);
  CHECK(w.frame(store).empty());  // the baseline: the whole list
  CHECK(store.all_reads == 1000);
  CHECK(w.stats().full_reads == 1);

  const std::size_t slice = (1000 + FlagWatcher::kAuditFrames - 1) / FlagWatcher::kAuditFrames;
  store.all_reads = 0;
  CHECK(w.frame(store).empty());
  CHECK(store.all_reads == static_cast<int>(slice));  // nothing written: the audit's slice only

  // The game writes two flags; a third event names a flag that did not change, a fourth one not in the list.
  store.set({700u, 0}, 1);
  store.set({900u, 0}, 1);
  w.game_wrote({700u, 0});
  w.game_wrote({900u, 0});
  w.game_wrote({800u, 0});
  w.game_wrote({5000u, 0});
  w.game_wrote(FlagKey::key64_flag(77));
  store.all_reads = 0;
  const auto flags = sent(w.frame(store));
  CHECK(flags == std::vector<proto::FlagEntry>{{{700u, 0}, proto::FlagKind::boolean, 1},
                                               {{900u, 0}, proto::FlagKind::boolean, 1}});
  CHECK(store.all_reads == static_cast<int>(3 + slice));
  CHECK(w.stats().event_reads == 3);
  CHECK(w.stats().event_misses == 0);
  CHECK(w.stats().full_reads == 1);
  CHECK(w.frame(store).empty());  // sent once
}

TEST_CASE("flag watcher, events: a change no event named is found by the audit, sent and counted") {
  CountingStore store;
  const auto keys = many(1000);
  for (FlagKey k : keys) store.set(k, 0);
  FlagWatcher w(SharedFlags(keys), FlagRole::player);
  w.use_events(true);
  w.frame(store);
  store.set({999u, 0}, 1);  // the last but one of the list, and no event
  std::vector<proto::FlagEntry> flags;
  std::size_t frames = 0;
  while (flags.empty() && frames < 2 * FlagWatcher::kAuditFrames) {
    flags = sent(w.frame(store));
    ++frames;
  }
  CHECK(flags == std::vector<proto::FlagEntry>{{{999u, 0}, proto::FlagKind::boolean, 1}});
  CHECK(frames <= FlagWatcher::kAuditFrames);  // one pass of the audit at most
  CHECK(w.stats().event_misses == 1);
  // A change an event named is no miss, even when the audit reads it the same frame.
  store.set({1u, 0}, 1);
  w.game_wrote({1u, 0});
  CHECK(sent(w.frame(store)).size() == 1);
  CHECK(w.stats().event_misses == 1);
}

TEST_CASE("flag watcher, events: a load window, lost events and a new flag read the whole list") {
  CountingStore store;
  const auto keys = many(500);
  for (FlagKey k : keys) store.set(k, 0);
  FlagWatcher w(SharedFlags(keys), FlagRole::host);
  w.use_events(true);
  w.frame(store);
  w.frame(store);
  CHECK(w.stats().full_reads == 1);

  // Lost events (the ring overflowed): everything is read once, and a change is found at once.
  store.set({321u, 0}, 1);
  w.game_wrote_all();
  store.all_reads = 0;
  CHECK(w.frame(store).size() == 1);
  CHECK(store.all_reads >= 500);
  CHECK(w.stats().full_reads == 2);
  CHECK(w.stats().event_misses == 0);
  w.frame(store);
  CHECK(w.stats().full_reads == 2);

  // No Link (a door, a save load): every frame of the window reads the list, as without events.
  w.set_loading(true);
  w.frame(store);
  w.frame(store);
  CHECK(w.stats().full_reads == 4);
  w.set_loading(false);
  w.frame(store);  // still settling
  CHECK(w.stats().full_reads == 5);

  // A flag that joins the list gets its baseline with a whole read.
  FlagWatcher v(SharedFlags(keys), FlagRole::host);
  v.use_events(true);
  v.frame(store);
  v.frame(store);
  const FlagKey pickup = FlagKey::key64_flag(0x77);
  store.set(pickup, 0);
  REQUIRE(v.share_pickup(pickup));
  const auto before = v.stats().full_reads;
  v.frame(store);
  CHECK(v.stats().full_reads == before + 1);
}

TEST_CASE("flag watcher, events: a value another player sent is written and its own event sends nothing back") {
  CountingStore store;
  for (FlagKey k : {kChest, kDoor, kKey0, kKey1}) store.set(k, 0);
  FlagWatcher w(SharedFlags({kChest, kDoor, kKey0, kKey1}), FlagRole::player);
  w.use_events(true);
  w.frame(store);
  CHECK(w.receive(proto::Message{proto::FlagState{1, {{kDoor, proto::FlagKind::boolean, 1}}}}));
  CHECK(w.frame(store).empty());
  CHECK(store.values[kDoor].value == 1);
  // The game's applier then takes our write: its event names the door.
  w.game_wrote(kDoor);
  CHECK(w.frame(store).empty());
  CHECK(w.stats().event_misses == 0);
}

TEST_CASE("flag watcher: without events every frame reads the whole list, as before") {
  CountingStore store;
  const auto keys = many(100);
  for (FlagKey k : keys) store.set(k, 0);
  FlagWatcher w(SharedFlags(keys), FlagRole::player);
  w.game_wrote({1u, 0});  // ignored
  w.frame(store);
  w.frame(store);
  w.frame(store);
  CHECK(store.all_reads == 300);
  CHECK(w.stats().full_reads == 0);
  CHECK(w.stats().event_reads == 0);
}
