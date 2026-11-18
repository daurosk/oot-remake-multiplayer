#include <doctest/doctest.h>
#include <algorithm>
#include <cstddef>

#include <map>
#include <string_view>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/game_items.hpp"
#include "coop/protocol/codec.hpp"

using namespace coop::host;
namespace proto = coop::protocol;

namespace {

constexpr FlagKey kChest{0x1000u, 0};
constexpr FlagKey kDoor{0x2000u, 0};
constexpr FlagKey kKey0{0x3000u, 0};
constexpr FlagKey kKey1{0x3000u, 1};
constexpr FlagKey kHearts{0x4000u, 0};  // in the game, not shared

proto::FlagEntry entry(FlagKey key, std::uint32_t value,
                       proto::FlagKind kind = proto::FlagKind::boolean) {
  return {key, kind, value};
}

// The game's flags, as the emulator would give them.
class FakeStore : public FlagStore {
 public:
  std::map<FlagKey, proto::FlagEntry> values;
  std::set<FlagKey> unreadable;
  std::set<FlagKey> unwritable;
  std::set<FlagKey> no_room;  // refused for room in the game's queue (busy)
  int writes = 0;

  void set(FlagKey key, std::uint32_t value) { values[key] = entry(key, value); }
  std::uint32_t get(FlagKey key) const { return values.at(key).value; }

  std::optional<proto::FlagEntry> read(FlagKey key) override {
    if (unreadable.contains(key)) return std::nullopt;
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
  }
  bool write(const proto::FlagEntry& e) override {
    const FlagKey key = key_of(e);
    if (unwritable.contains(key) || no_room.contains(key) || !values.contains(key)) return false;
    ++writes;
    values[key] = e;
    return true;
  }
  bool busy(FlagKey key) override { return no_room.contains(key); }
};

SharedFlags shared() { return SharedFlags({kChest, kDoor, kKey0, kKey1}); }

FakeStore game() {
  FakeStore store;
  for (FlagKey k : {kChest, kDoor, kKey0, kKey1, kHearts}) store.set(k, 0);
  return store;
}

std::vector<proto::Message> decode(const std::vector<Outgoing>& out) {
  std::vector<proto::Message> messages;
  for (const auto& o : out) {
    proto::Message m;
    REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
    CHECK(proto::check_received(m, proto::Sender::client, o.channel) == proto::Error::none);
    CHECK(o.channel == proto::kChannelProgress);
    messages.push_back(std::move(m));
  }
  return messages;
}

template <typename M>
std::vector<proto::FlagEntry> only(const std::vector<Outgoing>& out) {
  const auto messages = decode(out);
  REQUIRE(messages.size() == 1);
  const auto* m = std::get_if<M>(&messages[0]);
  REQUIRE(m);
  return m->flags;
}

proto::FlagState state(std::vector<proto::FlagEntry> flags) { return {1, std::move(flags)}; }

}  // namespace

TEST_CASE("flag watcher: the first frame is the baseline and sends nothing") {
  FakeStore store = game();
  store.set(kChest, 1);
  FlagWatcher w(shared(), FlagRole::player);
  CHECK(w.frame(store).empty());
  CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher: the game's changes go out as one FLAG_SET") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  store.set(kChest, 1);
  store.set(kKey1, 1);
  store.set(kHearts, 3);  // not shared: never sent
  const auto flags = only<proto::FlagSet>(w.frame(store));
  CHECK(flags == std::vector{entry(kChest, 1), entry(kKey1, 1)});
  CHECK(w.pending() == 2);
  CHECK(w.frame(store).empty());  // no change, nothing more
  CHECK(w.stats().changes_sent == 2);

  // Any change of value or kind counts.
  store.values[kDoor] = entry(kDoor, 0, proto::FlagKind::int32);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kDoor, 0, proto::FlagKind::int32)});
}

TEST_CASE("flag watcher: more than kMaxFlagEntries changes are split into several messages") {
  std::vector<FlagKey> keys;
  FakeStore store;
  for (std::uint32_t i = 0; i < 5000; ++i) {
    keys.push_back({i, 0});
    store.set({i, 0}, 0);
  }
  FlagWatcher w(SharedFlags(keys), FlagRole::player);
  w.frame(store);
  for (FlagKey k : keys) store.set(k, 1);
  const auto messages = decode(w.frame(store));
  REQUIRE(messages.size() == 2);
  CHECK(std::get<proto::FlagSet>(messages[0]).flags.size() == proto::kMaxFlagEntries);
  CHECK(std::get<proto::FlagSet>(messages[1]).flags.size() == 5000 - proto::kMaxFlagEntries);
}

TEST_CASE("flag watcher: a FLAG_STATE is written and not sent back") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1), entry(kHearts, 9)})));
  CHECK(store.get(kDoor) == 0);  // nothing is written outside frame()
  CHECK(w.frame(store).empty());  // written, and no echo
  CHECK(store.get(kDoor) == 1);
  CHECK(store.get(kHearts) == 0);  // not shared: left alone
  CHECK(w.stats().not_shared == 1);
  CHECK(w.frame(store).empty());

  // The same through the wire bytes.
  const auto bytes = proto::encode(state({entry(kDoor, 0)}));
  REQUIRE(bytes);
  CHECK(w.receive(*bytes));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 0);
}

TEST_CASE("flag watcher: a refused write names its key in the stats (events-next-a-11)") {
  FakeStore store = game();
  store.unwritable.insert(kDoor);
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.stats().top_write_failure_count == 0);
  CHECK(w.receive(state({entry(kDoor, 1), entry(kChest, 1)})));
  w.frame(store);
  w.frame(store);  // tried again
  CHECK(store.get(kChest) == 1);
  CHECK(w.stats().write_failures >= 1);
  CHECK(w.stats().top_write_failure == kDoor);
  CHECK(w.stats().top_write_failure_count == w.stats().write_failures);
  CHECK(std::string_view(w.stats().top_write_failure_site) == "write");
}

TEST_CASE("flag watcher: 64-bit keys go out, come in and fill a snapshot") {
  const FlagKey box_a = FlagKey::key64_flag(0x2fa4c1d09b7e6a35ull);
  const FlagKey box_b = FlagKey::key64_flag(0x1000u);  // same number as kChest
  const FlagKey pot = FlagKey::key64_flag(0x77u);       // in the game, not shared
  FakeStore store = game();
  for (FlagKey k : {box_a, box_b, pot}) store.set(k, 0);
  const SharedFlags list({kChest, box_a, box_b});
  FlagWatcher w(list, FlagRole::player);
  w.frame(store);

  store.set(box_a, 1);
  store.set(pot, 1);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(box_a, 1)});
  CHECK(w.pending() == 1);

  // The host's answer over the wire, with box_b opened by someone else.
  const auto bytes = proto::encode(state({entry(box_a, 1), entry(box_b, 1), entry(pot, 0)}));
  REQUIRE(bytes);
  CHECK(w.receive(*bytes));
  CHECK(w.frame(store).empty());
  CHECK(w.pending() == 0);
  CHECK(store.get(box_b) == 1);
  CHECK(store.get(kChest) == 0);  // the named flag with the same number is untouched
  CHECK(store.get(pot) == 1);     // not shared: left alone

  // As host, the snapshot holds both kinds.
  FlagWatcher h(list, FlagRole::host);
  h.request_snapshot();
  const auto parts = decode(h.frame(store));
  REQUIRE(parts.size() == 1);
  CHECK(std::get<proto::FlagSnapshot>(parts[0]).flags ==
        std::vector{entry(kChest, 0), entry(box_b, 1), entry(box_a, 1)});
}

TEST_CASE("flag watcher: a local change is kept until the host answers") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  store.set(kChest, 1);
  w.frame(store);
  REQUIRE(w.pending() == 1);

  // A snapshot made before the host saw the change does not undo it.
  w.receive(proto::FlagSnapshot{0, 1, {entry(kChest, 0), entry(kDoor, 1)}});
  CHECK(w.frame(store).empty());
  CHECK(store.get(kChest) == 1);
  CHECK(store.get(kDoor) == 1);
  CHECK(w.stats().kept_local == 1);

  // The host's answer wins, even when it says no.
  w.receive(state({entry(kChest, 0)}));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kChest) == 0);
  CHECK(w.pending() == 0);
}

TEST_CASE("flag watcher: a snapshot is held until its last part, then written") {
  FakeStore store = game();
  store.set(kKey1, 1);
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);

  w.receive(proto::FlagSnapshot{0, 3, {entry(kChest, 1)}});
  w.receive(proto::FlagSnapshot{1, 3, {entry(kDoor, 1)}});
  CHECK(w.frame(store).empty());
  CHECK(w.holding());
  CHECK(store.get(kChest) == 0);  // nothing written yet

  // The game's own changes while held wait too.
  store.set(kKey0, 1);
  CHECK(w.frame(store).empty());

  // A state after the snapshot is applied after it.
  w.receive(proto::FlagSnapshot{2, 3, {entry(kKey1, 0), entry(kHearts, 5)}});
  w.receive(state({entry(kDoor, 0)}));
  const auto out = w.frame(store);
  CHECK_FALSE(w.holding());
  CHECK(store.get(kChest) == 1);
  CHECK(store.get(kDoor) == 0);
  CHECK(store.get(kKey1) == 0);
  CHECK(store.get(kHearts) == 0);
  // Only the game's own change goes out, not the snapshot's writes.
  CHECK(only<proto::FlagSet>(out) == std::vector{entry(kKey0, 1)});
}

TEST_CASE("flag watcher: states that come while holding wait for the snapshot") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  w.receive(proto::FlagSnapshot{0, 2, {entry(kDoor, 1)}});
  w.frame(store);
  w.receive(state({entry(kDoor, 0)}));
  w.frame(store);
  CHECK(store.get(kDoor) == 0);
  w.receive(proto::FlagSnapshot{1, 2, {}});
  w.frame(store);
  CHECK(store.get(kDoor) == 0);  // the snapshot, then the newer state
}

TEST_CASE("flag watcher: a part out of order drops the snapshot") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  w.receive(proto::FlagSnapshot{0, 3, {entry(kChest, 1)}});
  w.receive(proto::FlagSnapshot{2, 3, {entry(kDoor, 1)}});
  w.frame(store);
  CHECK_FALSE(w.holding());
  CHECK(store.get(kChest) == 0);
  CHECK(w.stats().bad_snapshots == 1);

  // A part with no start is dropped too; a fresh start works.
  w.receive(proto::FlagSnapshot{1, 2, {entry(kChest, 1)}});
  w.receive(proto::FlagSnapshot{0, 1, {entry(kChest, 1)}});
  w.frame(store);
  CHECK(w.stats().bad_snapshots == 2);
  CHECK(store.get(kChest) == 1);
}

TEST_CASE("flag watcher: failed reads are skipped, failed writes tried again") {
  FakeStore store = game();
  store.unreadable.insert(kDoor);
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  store.set(kChest, 1);
  store.set(kDoor, 1);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 1)});
  CHECK(w.stats().read_failures == 2);

  // Readable again: its first good read is a baseline, not a change.
  store.unreadable.clear();
  CHECK(w.frame(store).empty());

  store.unwritable.insert(kKey0);
  w.receive(state({entry(kKey0, 1)}));
  w.frame(store);
  CHECK(store.get(kKey0) == 0);
  CHECK(w.stats().write_failures == 1);
  store.unwritable.clear();
  CHECK(w.frame(store).empty());  // written now, and not sent back
  CHECK(store.get(kKey0) == 1);
}

TEST_CASE("flag watcher: the game's own change wins over a write still to retry") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  store.unwritable.insert(kKey0);
  w.receive(state({entry(kKey0, 1)}));
  w.frame(store);
  store.unwritable.clear();
  store.values[kKey0] = entry(kKey0, 7, proto::FlagKind::int32);
  // The game changed it by itself: the retry is dropped and the change sent.
  CHECK(only<proto::FlagSet>(w.frame(store)) ==
        std::vector{entry(kKey0, 7, proto::FlagKind::int32)});
  CHECK(store.get(kKey0) == 7);
  CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher: every entry that comes is counted, written or not (player reports 106-108)") {
  FakeStore store = game();
  FlagWatcher player(shared(), FlagRole::player);
  player.frame(store);
  player.receive(state({entry(kChest, 1), entry(kDoor, 0)}));  // the door is 0 already: nothing written
  player.receive(proto::FlagSnapshot{0, 1, {entry(kKey0, 1)}});
  player.frame(store);
  CHECK(player.stats().entries_in == 3);
  CHECK(player.stats().applied >= 1);
  FlagWatcher host(shared(), FlagRole::host);
  host.frame(store);
  host.receive(proto::FlagSet{2, {entry(kKey1, 1)}});
  host.frame(store);
  CHECK(host.stats().entries_in == 1);
}

TEST_CASE("flag watcher: a player ignores FLAG_SET, a host snapshots") {
  FlagWatcher player(shared(), FlagRole::player);
  CHECK_FALSE(player.receive(proto::FlagSet{2, {entry(kChest, 1)}}));
  CHECK_FALSE(player.receive(proto::Message{proto::Chat{1, "hi"}}));
  const std::vector<std::uint8_t> junk{1, 2, 3};
  CHECK_FALSE(player.receive(junk));
  FlagWatcher host(shared(), FlagRole::host);
  CHECK_FALSE(host.receive(proto::FlagSnapshot{0, 1, {}}));
  CHECK(host.stats().ignored_messages == 1);
}

TEST_CASE("flag watcher, host: own changes go out as FLAG_STATE") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  CHECK(only<proto::FlagState>(w.frame(store)) == std::vector{entry(kDoor, 1)});
  CHECK(w.pending() == 0);
}

TEST_CASE("flag watcher, host: a FLAG_SET is applied and answered with what came out") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.unwritable.insert(kKey1);
  CHECK(w.receive(proto::FlagSet{2, {entry(kChest, 1), entry(kKey1, 1), entry(kHearts, 1)}}));
  store.set(kDoor, 1);  // the host's own change, same frame
  const auto flags = only<proto::FlagState>(w.frame(store));
  // Chest written; key refused, so the answer is the game's 0; hearts not
  // shared; the door is the host's own change.
  CHECK(flags == std::vector{entry(kChest, 1), entry(kDoor, 1), entry(kKey1, 0)});
  CHECK(store.get(kChest) == 1);
  CHECK(store.get(kHearts) == 0);
  CHECK(w.stats().answers_sent == 2);
  CHECK(w.frame(store).empty());  // the write is not seen as a change
}

TEST_CASE("flag watcher, host: a FLAG_SET refused for queue room is answered as asked and written later (issue 175)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.no_room.insert(kChest);
  CHECK(w.receive(proto::FlagSet{2, {entry(kChest, 1)}}));
  // Not the game's old 0: that would turn the player's change back.
  CHECK(only<proto::FlagState>(w.frame(store)) == std::vector{entry(kChest, 1)});
  CHECK(store.get(kChest) == 0);
  CHECK(w.frame(store).empty());  // still no room: kept, nothing sent
  CHECK(store.get(kChest) == 0);
  store.no_room.clear();
  CHECK(w.frame(store).empty());  // written now, and not seen as a change of the host's
  CHECK(store.get(kChest) == 1);
  CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher, host: a FLAG_STATE is written into the host's game and not sent back") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  CHECK(w.receive(state({entry(kChest, 1), entry(kHearts, 1)})));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kChest) == 1);
  CHECK(store.get(kHearts) == 0);  // not shared
  CHECK(w.stats().not_shared == 1);
  CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher, host: the whole table as FLAG_SNAPSHOT when asked") {
  FakeStore store = game();
  store.set(kDoor, 1);
  store.unreadable.insert(kKey0);
  FlagWatcher w(shared(), FlagRole::host);
  w.request_snapshot();
  const auto messages = decode(w.frame(store));
  REQUIRE(messages.size() == 1);
  const auto& part = std::get<proto::FlagSnapshot>(messages[0]);
  CHECK(part.part == 0);
  CHECK(part.part_count == 1);
  CHECK(part.flags == std::vector{entry(kChest, 0), entry(kDoor, 1), entry(kKey1, 0)});
  CHECK(w.frame(store).empty());  // once only

  // A big table comes in several parts.
  std::vector<FlagKey> keys;
  FakeStore big;
  for (std::uint32_t i = 0; i < 9000; ++i) {
    keys.push_back({i, 0});
    big.set({i, 0}, i & 1);
  }
  FlagWatcher h(SharedFlags(keys), FlagRole::host);
  h.request_snapshot();
  const auto parts = decode(h.frame(big));
  REQUIRE(parts.size() == 3);
  CHECK(std::get<proto::FlagSnapshot>(parts[2]).part == 2);
  CHECK(std::get<proto::FlagSnapshot>(parts[2]).part_count == 3);
  CHECK(std::get<proto::FlagSnapshot>(parts[2]).flags.size() == 9000 - 2 * proto::kMaxFlagEntries);
}

TEST_CASE("flag watcher: a snapshot fills a player's game, which a new host then sends") {
  // Host and player wired by hand, with the server's relay in between.
  FakeStore host_game = game();
  FakeStore player_game = game();
  host_game.set(kChest, 1);
  FlagWatcher host(shared(), FlagRole::host);
  FlagWatcher player(shared(), FlagRole::player);
  host.request_snapshot();
  player.frame(player_game);
  for (const auto& o : host.frame(host_game)) player.receive(o.bytes);
  player.frame(player_game);
  CHECK(player_game.get(kChest) == 1);

  // The player opens the door; the host answers; the player keeps it.
  player_game.set(kDoor, 1);
  for (const auto& o : player.frame(player_game)) {
    proto::Message m;
    REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
    REQUIRE(proto::set_sender(m, 2));
    host.receive(m);
  }
  for (const auto& o : host.frame(host_game)) {
    proto::Message m;
    REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
    REQUIRE(proto::set_sender(m, 1));
    player.receive(m);
  }
  CHECK(host_game.get(kDoor) == 1);
  CHECK(player.frame(player_game).empty());
  CHECK(player_game.get(kDoor) == 1);
  CHECK(player.pending() == 0);

  // The host leaves; the player takes over and sends its table.
  player.set_role(FlagRole::host);
  player.request_snapshot();
  const auto messages = decode(player.frame(player_game));
  REQUIRE(messages.size() == 1);
  CHECK(std::get<proto::FlagSnapshot>(messages[0]).flags.size() == 4);
}

TEST_CASE("flag watcher: after a reconnect, unanswered changes are sent again") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  store.set(kChest, 1);
  w.frame(store);
  w.receive(proto::FlagSnapshot{0, 2, {}});
  w.frame(store);
  REQUIRE(w.holding());
  w.reset();
  CHECK_FALSE(w.holding());
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 1)});
  CHECK(w.frame(store).empty());
}

// ------------------------------------------------------- one-time pickups

TEST_CASE("flag watcher: a pickup key shared at run time goes out once taken, never back to 0") {
  const FlagKey skull = FlagKey::key64_flag(0x8796851d6bbe37cull);
  FakeStore store = game();
  store.set(skull, 0);
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.share_pickup(skull));
  CHECK_FALSE(w.share_pickup(skull));
  CHECK_FALSE(w.share_pickup(kChest));  // a named flag is no pickup
  CHECK(w.shared().contains(skull));
  CHECK(w.pickups() == 1);
  CHECK(w.frame(store).empty());  // the first read is the baseline

  store.set(skull, 1);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(skull, 1)});
  w.receive(state({entry(skull, 1)}));  // the host's answer
  CHECK(w.frame(store).empty());

  // The game resets the key when the scene is left: ours only.
  store.set(skull, 0);
  CHECK(w.frame(store).empty());
  CHECK(w.stats().pickup_resets == 1);
  // And a 0 that comes in is not written over a taken one.
  store.set(skull, 1);
  w.frame(store);
  w.receive(state({entry(skull, 0)}));
  w.frame(store);
  CHECK(store.get(skull) == 1);
}

TEST_CASE("flag watcher: another game's taken pickup is learned and written when learning is on") {
  const FlagKey skull = FlagKey::key64_flag(0x55u);
  const FlagKey pot = FlagKey::key64_flag(0x66u);
  FakeStore store = game();
  store.set(skull, 0);
  store.set(pot, 0);

  FlagWatcher off(shared(), FlagRole::player);
  off.frame(store);
  off.receive(state({entry(skull, 1)}));
  off.frame(store);
  CHECK(store.get(skull) == 0);  // off: not shared, left alone
  CHECK(off.stats().not_shared == 1);

  FlagWatcher w(shared(), FlagRole::player);
  w.set_learn_pickups(true);
  w.frame(store);
  w.receive(state({entry(skull, 1), entry(pot, 0)}));
  CHECK(w.frame(store).empty());
  CHECK(store.get(skull) == 1);
  CHECK(store.get(pot) == 0);  // a 0 teaches nothing
  CHECK(w.stats().pickups_learned == 1);
  CHECK(w.pickups() == 1);
  // It is a pickup now: our game resetting it is not sent.
  store.set(skull, 0);
  CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher, host: a player's taken pickup is learned, written and told to everyone") {
  const FlagKey skull = FlagKey::key64_flag(0x55u);
  FakeStore store = game();
  store.set(skull, 0);
  FlagWatcher h(shared(), FlagRole::host);
  h.set_learn_pickups(true);
  h.frame(store);
  proto::FlagSet set;
  set.player_number = 2;
  set.flags = {entry(skull, 1)};
  h.receive(set);
  CHECK(only<proto::FlagState>(h.frame(store)) == std::vector{entry(skull, 1)});
  CHECK(store.get(skull) == 1);
  // In the host's snapshot from then on, for a player who joins later.
  h.request_snapshot();
  const auto parts = decode(h.frame(store));
  REQUIRE(parts.size() == 1);
  const auto& flags = std::get<proto::FlagSnapshot>(parts[0]).flags;
  CHECK(std::find(flags.begin(), flags.end(), entry(skull, 1)) != flags.end());
  // A player's FLAG_SET of a pickup's 0 is not taken.
  set.flags = {entry(skull, 0)};
  h.receive(set);
  CHECK(h.frame(store).empty());
  CHECK(store.get(skull) == 1);
}

TEST_CASE("flag watcher: at most kMaxLearned pickups are learned from messages") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.set_learn_pickups(true);
  w.frame(store);
  std::vector<proto::FlagEntry> flags;
  for (std::uint64_t i = 0; i < FlagWatcher::kMaxLearned + 5; ++i) {
    flags.push_back(entry(FlagKey::key64_flag(0x10000 + i), 1));
  }
  for (std::size_t at = 0; at < flags.size(); at += proto::kMaxFlagEntries) {
    const auto end = std::min(flags.size(), at + proto::kMaxFlagEntries);
    w.receive(state(std::vector<proto::FlagEntry>(flags.begin() + static_cast<std::ptrdiff_t>(at),
                                                  flags.begin() + static_cast<std::ptrdiff_t>(end))));
  }
  w.frame(store);
  CHECK(w.stats().pickups_learned == FlagWatcher::kMaxLearned);
  CHECK(w.pickups() == FlagWatcher::kMaxLearned);
}
TEST_CASE("flag watcher: a guest's save load takes the group's values and sends nothing (issue 129, saves)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.set_loading(true);  // the title screen: no Link yet
  w.frame(store);
  // The group's switch and key come in before the save loads.
  CHECK(w.receive(state({entry(kDoor, 1), entry(kKey0, 1)})));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);

  w.set_scene(false);  // the title screen and file select: no scene loaded
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  // The save loads: an older one, so two of them go back to 0 (with the
  // title screen before, the sign of a save load), and its own progress (the chest) comes in with it. Nothing
  // goes out: a guest's load never changes the group (docs/flags.md, "Saves").
  store.set(kDoor, 0);
  store.set(kKey0, 0);
  store.set(kChest, 1);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);  // put back at once
  CHECK(store.get(kKey0) == 1);
  CHECK(w.stats().load_reverts == 2);

  // Link is there, but the load still runs a moment: the same again.
  w.set_scene(true);
  w.set_loading(false);
  CHECK(w.settling());
  store.set(kDoor, 0);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);

  // The window ends: the save's own chest goes back to the group's 0.
  for (int i = 0; i < FlagWatcher::kSettleFrames; ++i) CHECK(w.frame(store).empty());
  CHECK_FALSE(w.settling());
  CHECK(store.get(kChest) == 0);
  // After it the game's own changes go out as before.
  store.set(kDoor, 0);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kDoor, 0)});
}

namespace {

// A guest at Hookshot's title: joined, the group's table in, the title menu up.
void at_title_with_group(FlagWatcher& w, FlagStore& store, std::vector<proto::FlagEntry> group) {
  w.set_loading(true);
  w.set_scene(false);
  w.set_title(true);
  w.frame(store);
  CHECK(w.receive(proto::FlagSnapshot{0, 1, std::move(group)}));
  CHECK(w.frame(store).empty());
}

// The save is in and Link comes: the load window runs to its end. Everything sent is returned.
std::vector<Outgoing> link_back(FlagWatcher& w, FlagStore& store) {
  w.set_title(false);
  w.set_scene(true);
  w.set_loading(false);
  std::vector<Outgoing> sent;
  for (int i = 0; i <= FlagWatcher::kSettleFrames + 1; ++i) {
    for (auto& o : w.frame(store)) sent.push_back(std::move(o));
  }
  CHECK_FALSE(w.settling());
  return sent;
}

}  // namespace

TEST_CASE("flag watcher: a guest's own richer save loaded through the title sends nothing (#96)") {
  // Player report #96, the second way in: a guest joins on the title, then
  // loads its own file, which is only ahead of the host's new one (no flag
  // goes back, so the flags-back sign never came). Its items went to the host.
  constexpr FlagKey kHookshot{0x8573d2cdu, 0};  // max
  FakeStore game_store;
  for (FlagKey k : {kChest, kDoor, kKey0, kKey1}) game_store.set(k, 0);
  game_store.values[kHookshot] = entry(kHookshot, 0, proto::FlagKind::int32);
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kChest, kDoor, kKey0, kKey1, kHookshot}), FlagRole::player);
  at_title_with_group(w, store, {entry(kDoor, 0), entry(kKey0, 0), entry(kHookshot, 0, proto::FlagKind::int32)});
  for (int i = 0; i < 5; ++i) CHECK(w.frame(store).empty());
  // The richer save loads: only progress forward.
  game_store.set(kDoor, 1);
  game_store.set(kKey0, 1);
  game_store.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  CHECK(w.frame(store).empty());
  CHECK(link_back(w, store).empty());
  CHECK(game_store.get(kDoor) == 0);  // the group's (the host's file)
  CHECK(game_store.get(kKey0) == 0);
  CHECK(game_store.get(kHookshot) == 0);
  CHECK(w.stats().title_loads == 1);
  const auto seen = w.take_save_load();  // reported with 0 flags back, so the hook resets its per-save state
  REQUIRE(seen);
  CHECK(seen->flags == 0);
  // Its own play after that goes out as always.
  game_store.set(kKey1, 1);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kKey1, 1)});
}

TEST_CASE("flag watcher: a guest loading the host's save copy through the title sees no change, sends nothing (#96)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  at_title_with_group(w, store, {entry(kDoor, 1), entry(kKey0, 1), entry(kChest, 0), entry(kKey1, 0)});
  // The co-op folder's copy of the host's save: the group's values. The title's memory may have had others.
  store.set(kDoor, 1);
  store.set(kKey0, 1);
  CHECK(w.frame(store).empty());
  CHECK(link_back(w, store).empty());
  CHECK(store.get(kDoor) == 1);
  CHECK(store.get(kKey0) == 1);
  CHECK(store.get(kChest) == 0);
  CHECK(w.stats().load_reverts == 0);
  store.set(kChest, 1);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 1)});
}

TEST_CASE("flag watcher: the host's load through the title is a save load, also forward only (#96, forest-temple-a)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.set_loading(true);
  w.set_scene(false);
  w.set_title(true);
  for (int i = 0; i < FlagWatcher::kTitleFrames + 2; ++i) w.frame(store);
  store.set(kDoor, 1);  // the host's save: forward only
  w.frame(store);
  const auto sent = link_back(w, store);
  bool door = false;
  for (const auto& m : decode(sent)) {
    if (const auto* r = std::get_if<proto::FlagReset>(&m)) {
      door = door || std::find(r->flags.begin(), r->flags.end(), entry(kDoor, 1)) != r->flags.end();
    }
  }
  CHECK(door);  // the group follows the host's save
  CHECK(w.stats().host_resets == 1);
  CHECK(w.stats().title_loads == 1);
  CHECK(w.take_save_load().has_value());
  CHECK(store.get(kDoor) == 1);
}

TEST_CASE("flag watcher: with no title (the emulator) a guest's forward-only load is still sent (#96)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.set_loading(true);
  w.set_scene(false);
  for (int i = 0; i < FlagWatcher::kTitleFrames + 2; ++i) w.frame(store);
  store.set(kDoor, 1);
  w.frame(store);
  const auto sent = link_back(w, store);
  bool door = false;
  for (const auto& m : decode(sent)) {
    if (const auto* set = std::get_if<proto::FlagSet>(&m)) {
      door = door || std::find(set->flags.begin(), set->flags.end(), entry(kDoor, 1)) != set->flags.end();
    }
  }
  CHECK(door);
  CHECK(w.stats().title_loads == 0);
}

TEST_CASE("flag watcher: the title while not connected: what the game changed meanwhile goes back (#96)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);  // in the game, connected: the baseline (the group's values)
  // The connection goes; the player goes to the title and loads another save.
  w.set_title(true);
  w.set_title(false);
  store.set(kDoor, 1);
  store.set(kKey0, 1);
  w.reset();  // connected again, Link in the game
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 0);
  CHECK(store.get(kKey0) == 0);
  CHECK(w.stats().offline_loads == 1);
  for (int i = 0; i < 5; ++i) CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher: a short drop with no title keeps the game's own change and sends it") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  store.set(kChest, 1);  // taken while the connection was away
  w.reset();
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 1)});
  CHECK(w.stats().offline_loads == 0);
}

TEST_CASE("flag watcher: one flag back with no Link is no save load: the old rules hold for a guest") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  w.frame(store);
  // A cutscene or time travel with no Link: one flag back, one forward.
  w.set_loading(true);
  store.set(kDoor, 0);
  store.set(kChest, 1);
  CHECK(w.frame(store).empty());  // nothing goes out while there is no Link
  CHECK(store.get(kDoor) == 1);   // backwards(): the group's value back at once
  w.set_loading(false);
  // Link is back: the forward change is the game's own, sent as always.
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 1)});
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) w.frame(store);
  CHECK(store.get(kChest) == 1);  // not put back: no save load
}

TEST_CASE("flag watcher: a load puts the group's Int back, a flag the game will not take is let be") {
  FakeStore store = game();
  store.values[kKey1] = entry(kKey1, 2, proto::FlagKind::int32);  // a water level
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kKey1, 3, proto::FlagKind::int32), entry(kChest, 1)})));
  w.frame(store);
  REQUIRE(store.get(kKey1) == 3);

  w.set_loading(true);
  store.values[kKey1] = entry(kKey1, 2, proto::FlagKind::int32);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kKey1) == 3);

  // A per-player flag the game refuses to be written: nothing is sent during
  // the load; once it is over, its value is the game's own, sent as any change.
  store.unwritable.insert(kChest);
  store.set(kChest, 0);
  CHECK(w.frame(store).empty());
  w.set_loading(false);
  std::vector<proto::FlagEntry> sent;
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) {
      if (const auto* set = std::get_if<proto::FlagSet>(&m)) sent.insert(sent.end(), set->flags.begin(), set->flags.end());
    }
  }
  CHECK(sent == std::vector{entry(kChest, 0)});
}

TEST_CASE("flag watcher, host: the host's save load resets the group to its save") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  store.set(kKey0, 1);
  w.frame(store);
  // The host goes to the title screen and loads an older save: the door is
  // shut and the key unused in it.
  w.set_loading(true);
  w.set_scene(false);  // the title screen and file select: no scene loaded
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  store.set(kDoor, 0);
  store.set(kKey0, 0);
  CHECK(w.frame(store).empty());  // nothing during the load
  CHECK(store.get(kDoor) == 0);   // the host's save is the truth: not put back
  w.set_scene(true);
  w.set_loading(false);
  std::vector<proto::FlagEntry> reset;
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) {
      CHECK_FALSE(std::holds_alternative<proto::FlagState>(m));
      if (const auto* r = std::get_if<proto::FlagReset>(&m)) reset.insert(reset.end(), r->flags.begin(), r->flags.end());
    }
  }
  CHECK(reset == std::vector{entry(kDoor, 0), entry(kKey0, 0)});
  CHECK(w.stats().host_resets == 1);
  CHECK(w.take_reset());        // the game's hook clears its session-wide state once
  CHECK_FALSE(w.take_reset());
}

TEST_CASE("flag watcher, host: a cutscene, warp, time travel or death with no Link is no reset") {
  struct Case {
    const char* what;
    std::vector<std::pair<FlagKey, std::uint32_t>> during;  // what the game writes with no Link
  };
  const std::vector<Case> cases{
      {"a cutscene sets story flags", {{kChest, 1}, {kKey0, 1}}},
      {"a warp", {}},
      {"time travel: one flag back, others forward", {{kDoor, 0}, {kChest, 1}}},
      {"a death and respawn", {}},
  };
  for (const auto& c : cases) {
    CAPTURE(c.what);
    FakeStore store = game();
    FlagWatcher w(shared(), FlagRole::host);
    w.frame(store);
    store.set(kDoor, 1);
    w.frame(store);
    w.set_loading(true);
    for (const auto& [k, v] : c.during) store.set(k, v);
    CHECK(w.frame(store).empty());
    w.set_loading(false);
    for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
      for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
    }
    CHECK(w.stats().host_resets == 0);
  }
}

TEST_CASE("flag watcher, host: a door is no save load: no reset") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  w.set_loading(true);
  w.frame(store);
  w.set_loading(false);
  store.set(kDoor, 1);  // the host's own change right after the door
  std::vector<proto::Message> all;
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (auto& m : decode(w.frame(store))) all.push_back(std::move(m));
  }
  REQUIRE(all.size() == 1);
  CHECK(std::get<proto::FlagState>(all[0]).flags == std::vector{entry(kDoor, 1)});
  CHECK(w.stats().host_resets == 0);
}

TEST_CASE("flag watcher: a guest's load with only progress forward is no save load: it is sent when Link is back") {
  FakeStore store = game();
  store.values[kKey1] = entry(kKey1, 2, proto::FlagKind::int32);
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kKey1, 3, proto::FlagKind::int32)})));
  w.frame(store);
  w.set_loading(true);
  store.values[kKey1] = entry(kKey1, 5, proto::FlagKind::int32);
  CHECK(w.frame(store).empty());  // nothing while there is no Link
  w.set_loading(false);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kKey1, 5, proto::FlagKind::int32)});
  CHECK(w.stats().load_reverts == 0);
}

TEST_CASE("flag watcher: trade and max items under a load, through the item store") {
  constexpr FlagKey kEgg{0x07f10aafu, 0};       // PouchData.Item_PocketEgg.StockNum, mirror
  constexpr FlagKey kHookshot{0x8573d2cdu, 0};  // PouchData.Item_Hookshot.StockNum, max
  FakeStore game_store;
  game_store.values[kEgg] = entry(kEgg, 0, proto::FlagKind::int32);
  game_store.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kEgg, kHookshot}), FlagRole::player);
  w.frame(store);
  // The group's egg (the game had none) and Hookshot 2.
  CHECK(w.receive(state({entry(kEgg, 1, proto::FlagKind::int32),
                         entry(kHookshot, 2, proto::FlagKind::int32)})));
  w.frame(store);
  REQUIRE(game_store.get(kEgg) == 1);
  REQUIRE(game_store.get(kHookshot) == 2);

  // A scene change, then Link is back: the egg hatches in the settle time.
  // Back to its value before the group's, but the game's own change: sent.
  w.set_loading(true);
  w.frame(store);
  w.set_loading(false);
  REQUIRE(w.settling());
  game_store.values[kEgg] = entry(kEgg, 0, proto::FlagKind::int32);
  CHECK(only<proto::FlagSet>(w.frame(store)) ==
        std::vector{entry(kEgg, 0, proto::FlagKind::int32)});
  CHECK(game_store.get(kEgg) == 0);

  // A load (no Link) puts the save's Hookshot back: the group's is written
  // once and stays, nothing is sent.
  w.set_loading(true);
  game_store.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  CHECK(w.frame(store).empty());
  CHECK(game_store.get(kHookshot) == 2);
  CHECK(w.frame(store).empty());
  CHECK(w.stats().load_reverts == 1);
}

TEST_CASE("flag watcher: a reconnect forgets the group's values") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  w.frame(store);
  w.reset();  // a new session: the server may not have it any more
  w.set_loading(true);
  store.set(kDoor, 0);
  CHECK(w.frame(store).empty());  // nothing while there is no Link
  w.set_loading(false);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kDoor, 0)});
  CHECK(w.stats().load_reverts == 0);
}

TEST_CASE("flag watcher: a FLAG_RESET is written exactly, 0s and one-way keys included (saves, #148)") {
  constexpr FlagKey kPickup = FlagKey::key64_flag(0x77);
  FakeStore store = game();
  store.set(kPickup, 0);
  FlagWatcher w(SharedFlags({kChest, kDoor, kKey0, kKey1}), FlagRole::player);
  CHECK(w.share_pickup(kPickup));
  w.frame(store);
  // The newer state: the boss door shut, a chest taken (#148).
  CHECK(w.receive(state({entry(kDoor, 1), entry(kPickup, 1)})));
  w.frame(store);
  REQUIRE(store.get(kDoor) == 1);
  REQUIRE(store.get(kPickup) == 1);
  // A 0 in a FLAG_STATE is not taken for the one-way key...
  CHECK(w.receive(state({entry(kPickup, 0)})));
  w.frame(store);
  CHECK(store.get(kPickup) == 1);
  // ...but the host's save load sets both back.
  CHECK(w.receive(proto::Message{proto::FlagReset{1, {entry(kDoor, 0), entry(kPickup, 0)}}}));
  CHECK(w.frame(store).empty());  // nothing is sent back
  CHECK(store.get(kDoor) == 0);
  CHECK(store.get(kPickup) == 0);
  CHECK(w.stats().reset_applied == 2);
  CHECK(w.take_reset());
  CHECK_FALSE(w.take_reset());
  CHECK(w.frame(store).empty());
}

TEST_CASE("flag watcher: a FLAG_RESET lowers an item through the item store (no max merge)") {
  constexpr FlagKey kHookshot{0x8573d2cdu, 0};  // PouchData.Item_Hookshot.StockNum, max
  FakeStore game_store;
  game_store.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kHookshot}), FlagRole::player);
  w.frame(store);
  // A FLAG_STATE 0 does not take it away (max)...
  CHECK(w.receive(state({entry(kHookshot, 0, proto::FlagKind::int32)})));
  w.frame(store);
  CHECK(game_store.get(kHookshot) == 1);
  // ...the host's save without it does.
  CHECK(w.receive(proto::Message{proto::FlagReset{1, {entry(kHookshot, 0, proto::FlagKind::int32)}}}));
  w.frame(store);
  CHECK(game_store.get(kHookshot) == 0);
}

TEST_CASE("flag watcher: a guest's older save joining a host's new file takes the host's items, sends none (#96)") {
  // Player report #96: the host deleted its saves and started a new file; a
  // guest whose game still had the old group's save loaded joined, and its
  // items, songs and quests went into the host's new file at once. The join
  // snapshot was merged by the max rule, so the guest kept its items and its
  // next diff sent them. The host's save decides: the snapshot is exact.
  constexpr FlagKey kHookshot{0x8573d2cdu, 0};  // PouchData.Item_Hookshot.StockNum, max
  constexpr FlagKey kEmerald{0x1234u, 0};
  FakeStore guest_game;
  guest_game.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  guest_game.values[kEmerald] = entry(kEmerald, 1);
  ItemFlagStore store(guest_game);
  FlagWatcher w(SharedFlags({kHookshot, kEmerald}), FlagRole::player);
  CHECK(w.frame(store).empty());  // the baseline: the guest's old save
  w.reset();                      // a new connection
  CHECK(w.receive(proto::FlagSnapshot{
      0, 1, {entry(kHookshot, 0, proto::FlagKind::int32), entry(kEmerald, 0)}}));
  for (int i = 0; i < 10; ++i) CHECK(w.frame(store).empty());  // nothing goes to the group
  CHECK(guest_game.get(kHookshot) == 0);
  CHECK(guest_game.get(kEmerald) == 0);
  CHECK(w.stats().snapshot_exact == 2);
  // From now on the guest plays the host's file: its own new pickup goes out.
  guest_game.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  CHECK(only<proto::FlagSet>(w.frame(store)) ==
        std::vector{entry(kHookshot, 1, proto::FlagKind::int32)});
}

TEST_CASE("flag watcher: a host's new file never gets a joining guest's old items (#96, both games)") {
  constexpr FlagKey kHookshot{0x8573d2cdu, 0};
  FakeStore host_game, guest_game;
  host_game.values[kHookshot] = entry(kHookshot, 0, proto::FlagKind::int32);
  guest_game.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);
  ItemFlagStore host_store(host_game), guest_store(guest_game);
  FlagWatcher h(SharedFlags({kHookshot}), FlagRole::host);
  FlagWatcher g(SharedFlags({kHookshot}), FlagRole::player);
  h.frame(host_store);
  g.frame(guest_store);
  // The join: the host's table reaches the guest (the server passes it on).
  h.request_snapshot();
  for (const auto& o : h.frame(host_store)) CHECK(g.receive(o.bytes));
  for (int i = 0; i < 60; ++i) {
    for (const auto& o : g.frame(guest_store)) CHECK(h.receive(o.bytes));
    for (const auto& o : h.frame(host_store)) CHECK(g.receive(o.bytes));
  }
  CHECK(host_game.get(kHookshot) == 0);
  CHECK(guest_game.get(kHookshot) == 0);
}

TEST_CASE("flag watcher: a guest's own change still on its way is kept over the join snapshot") {
  constexpr FlagKey kHookshot{0x8573d2cdu, 0};
  FakeStore guest_game;
  guest_game.values[kHookshot] = entry(kHookshot, 0, proto::FlagKind::int32);
  ItemFlagStore store(guest_game);
  FlagWatcher w(SharedFlags({kHookshot}), FlagRole::player);
  w.frame(store);
  guest_game.values[kHookshot] = entry(kHookshot, 1, proto::FlagKind::int32);  // taken just now
  CHECK(only<proto::FlagSet>(w.frame(store)) ==
        std::vector{entry(kHookshot, 1, proto::FlagKind::int32)});
  CHECK(w.receive(proto::FlagSnapshot{0, 1, {entry(kHookshot, 0, proto::FlagKind::int32)}}));
  w.frame(store);
  CHECK(guest_game.get(kHookshot) == 1);  // the host's answer decides it
  CHECK(w.stats().kept_local == 1);
}

TEST_CASE("flag watcher: a host ignores a FLAG_RESET, and a new host does not reset") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  w.frame(store);
  w.set_role(FlagRole::host);  // the host left: we take over, no load
  for (int i = 0; i < 5; ++i) {
    for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  }
  CHECK(w.stats().host_resets == 0);
  CHECK_FALSE(w.take_reset());
  CHECK_FALSE(w.receive(proto::Message{proto::FlagReset{2, {entry(kDoor, 0)}}}));
  CHECK(store.get(kDoor) == 1);
}

namespace {
constexpr FlagKey kDekuMap{0x89787395u, 0};      // DungeonInfo.InsideTheDekuTree.IsGetMap
constexpr FlagKey kDekuCompass{0x06c51ff2u, 0};  // DungeonInfo.InsideTheDekuTree.IsGetCompass
constexpr FlagKey kGotBomb{0x2bb00768u, 0};      // IsGet.Item_Bomb: OR too, not one-way
FakeStore dungeon_game() {
  FakeStore store;
  for (FlagKey k : {kDekuMap, kDekuCompass, kGotBomb}) store.set(k, 0);
  return store;
}
}  // namespace

TEST_CASE("flag watcher: floors the game writes with its own map get are kept, not ORed back (#63/#76)") {
  constexpr FlagKey kDekuFloors{0x5c6c21c1u, 0};  // DungeonInfo.InsideTheDekuTree.EnteredFloorBit
  CHECK(coop::host::map_of_floors(kDekuFloors) == std::optional<FlagKey>(kDekuMap));
  CHECK_FALSE(coop::host::map_of_floors(kDekuMap));
  const auto floors = [&](FakeStore& g, std::uint32_t v) { g.values[kDekuFloors] = entry(kDekuFloors, v, proto::FlagKind::uint32); };
  const SharedFlags flags({kDekuMap, kDekuFloors});
  SUBCASE("the map in the same frame") {
    FakeStore game_store = dungeon_game();
    floors(game_store, 0b10000);
    ItemFlagStore store(game_store);
    FlagWatcher w(flags, FlagRole::player);
    w.frame(store);
    floors(game_store, 0b01111);
    game_store.set(kDekuMap, 1);
    w.frame(store);
    CHECK(game_store.get(kDekuFloors) == 0b01111u);
    CHECK(w.stats().one_way_restores == 0);
    const auto told = w.take_one_way_restores();
    REQUIRE(told.size() == 1);
    CHECK(told[0].kept);
    CHECK(told[0].before == 0b10000u);
    CHECK(told[0].now == 0b01111u);
  }
  SUBCASE("the map a frame before, and long after a drop is put back as before") {
    FakeStore game_store = dungeon_game();
    floors(game_store, 0b10000);
    ItemFlagStore store(game_store);
    FlagWatcher w(flags, FlagRole::player);
    w.frame(store);
    game_store.set(kDekuMap, 1);
    w.frame(store);
    floors(game_store, 0b01111);
    w.frame(store);
    CHECK(game_store.get(kDekuFloors) == 0b01111u);
    for (int i = 0; i < 5; ++i) w.frame(store);
    floors(game_store, 0b00001);
    w.frame(store);
    CHECK(game_store.get(kDekuFloors) == 0b01111u);
    CHECK(w.stats().one_way_restores == 1);
  }
}

TEST_CASE("flag watcher: a dungeon map or compass the game drops to 0 is put back (issue 146)") {
  FakeStore game_store = dungeon_game();
  ItemFlagStore store(game_store);
  CHECK(store.is_one_way(kDekuMap));
  CHECK(store.is_one_way(kDekuCompass));
  CHECK_FALSE(store.is_one_way(kGotBomb));
  CHECK_FALSE(store.is_one_way(kChest));  // not an item
  FlagWatcher w(SharedFlags({kDekuMap, kDekuCompass, kGotBomb}), FlagRole::player);
  w.frame(store);
  game_store.set(kDekuMap, 1);
  game_store.set(kGotBomb, 1);
  w.frame(store);  // both go out
  // The game takes them back, with no load: the map is written back and
  // nothing is sent for it; any other OR item still goes out as it is.
  game_store.set(kDekuMap, 0);
  game_store.set(kGotBomb, 0);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kGotBomb, 0)});
  CHECK(game_store.get(kDekuMap) == 1);
  CHECK(w.stats().one_way_restores == 1);
  {
    const auto told = w.take_one_way_restores();
    REQUIRE(told.size() == 1);
    CHECK(told[0].key == kDekuMap);
    CHECK(told[0].before == 1);
    CHECK(told[0].now == 0);
    CHECK(told[0].put == 1);
    CHECK_FALSE(told[0].kept);
  }
  CHECK(w.take_one_way_restores().empty());
  CHECK(w.frame(store).empty());  // and it stays
  CHECK(game_store.get(kDekuMap) == 1);
  // Again: put back again, counted, but given for the log only the first time.
  game_store.set(kDekuMap, 0);
  CHECK(w.frame(store).empty());
  CHECK(game_store.get(kDekuMap) == 1);
  CHECK(w.stats().one_way_restores == 2);
  CHECK(w.take_one_way_restores().empty());
  // While loading the one-way rule stays out (the load revert has that window).
  w.set_loading(true);
  game_store.set(kDekuMap, 0);
  w.frame(store);
  CHECK(w.stats().one_way_restores == 2);
  CHECK(w.take_one_way_restores().empty());
}

TEST_CASE("flag watcher: a one-way value the game lowers again at once is left to the game, not fought (row 140)") {
  constexpr FlagKey kDekuFloors{0x5c6c21c1u, 0};  // DungeonInfo.InsideTheDekuTree.EnteredFloorBit
  FakeStore game_store = dungeon_game();
  const auto floors = [&](std::uint32_t v) { game_store.values[kDekuFloors] = entry(kDekuFloors, v, proto::FlagKind::uint32); };
  floors(0b01100);
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap, kDekuFloors}), FlagRole::player);
  w.frame(store);
  // The game writes its own copy (a new floor, the group's floors gone) every frame, as in Revirie's host log.
  floors(0b10000);
  w.frame(store);
  CHECK(game_store.get(kDekuFloors) == 0b11100u);  // a first drop is put back (issue 146)
  CHECK(w.take_hud_refresh());                       // the HUD screen's copy asked to take it
  CHECK_FALSE(w.take_hud_refresh());
  floors(0b10000);
  w.frame(store);
  CHECK(game_store.get(kDekuFloors) == 0b11100u);  // undone once: put back once more
  CHECK(w.take_hud_refresh());                       // every put-back, not only the key's first (review)
  CHECK(w.stats().one_way_restores == 2);
  CHECK(w.take_one_way_left().empty());
  floors(0b10000);
  const auto out = w.frame(store);  // undone twice in a row: the game's own now
  CHECK(game_store.get(kDekuFloors) == 0b10000u);
  CHECK(w.stats().one_way_left == 1);
  CHECK(w.take_one_way_left() == std::vector{kDekuFloors});
  // Its new floor still goes out, ORed with ours: the group never loses a floor.
  CHECK(only<proto::FlagSet>(out) == std::vector{entry(kDekuFloors, 0b11100, proto::FlagKind::uint32)});
  const int writes = game_store.writes;
  for (int i = 0; i < 120; ++i) {
    floors(i % 2 == 0 ? 0b11100 : 0b10000);  // a write of ours from elsewhere, and the game's at once
    w.frame(store);
  }
  CHECK(game_store.writes == writes);  // no fight: two seconds, no put-back
  CHECK(w.stats().one_way_restores == 2);
  CHECK(w.take_one_way_left().empty());  // said once
  CHECK_FALSE(w.take_hud_refresh());
  // A room load window (it reads the game's copy in again): when it ends, the group's floors are put back once,
  // ORed with the game's, and the HUD refresh is asked, so the other player's floors reach the HUD screen's copy.
  w.set_loading(true);
  w.frame(store);
  floors(0b10010);  // the game's own write at the load: a new floor of ours
  w.frame(store);
  w.set_loading(false);
  for (int i = 0; i < FlagWatcher::kSettleFrames + 10; ++i) w.frame(store);  // past the settle
  CHECK(game_store.get(kDekuFloors) == 0b11110u);
  CHECK(w.stats().one_way_owed == 1);
  CHECK(w.stats().one_way_restores == 3);
  CHECK(w.take_hud_refresh());
  for (int i = 0; i < 10; ++i) w.frame(store);
  CHECK(w.stats().one_way_owed == 1);  // once per load
  // The refresh took it: the game writes the merged copy back, no fight. If it still lowers it at once, it is
  // left again after kMaxQuickUndos, and owed again for the next load.
  floors(0b10010);
  w.frame(store);
  CHECK(game_store.get(kDekuFloors) == 0b11110u);  // a later one-off drop: put back
  floors(0b10010);
  w.frame(store);
  floors(0b10010);
  w.frame(store);
  CHECK(game_store.get(kDekuFloors) == 0b10010u);
  CHECK(w.stats().one_way_left == 2);
  CHECK(w.stats().one_way_restores == 5);
}

TEST_CASE("flag watcher: a floor another player enters after a key was left is owed too (row 140 review)") {
  constexpr FlagKey kDekuFloors{0x5c6c21c1u, 0};
  FakeStore game_store = dungeon_game();
  const auto floors = [&](std::uint32_t v) { game_store.values[kDekuFloors] = entry(kDekuFloors, v, proto::FlagKind::uint32); };
  floors(0b01100);
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap, kDekuFloors}), FlagRole::player);
  w.frame(store);
  for (int n = 0; n < 3; ++n) {  // put back, undone, put back, undone: left
    floors(0b10000);
    w.frame(store);
  }
  CHECK(w.stats().one_way_left == 1);
  // The other player enters floor 0b00010: the group's floors come and are written here...
  CHECK(w.receive(state({entry(kDekuFloors, 0b11110, proto::FlagKind::uint32)})));
  w.frame(store);
  CHECK(game_store.get(kDekuFloors) == 0b11110u);
  // ...and the game lowers them again from its HUD copy: left, not fought.
  floors(0b10000);
  w.frame(store);
  CHECK(game_store.get(kDekuFloors) == 0b10000u);
  // A room load: the put-back after it has that floor too.
  w.set_loading(true);
  w.frame(store);
  w.set_loading(false);
  for (int i = 0; i < FlagWatcher::kSettleFrames + 10; ++i) w.frame(store);
  CHECK(w.stats().one_way_owed == 1);
  CHECK(game_store.get(kDekuFloors) == 0b11110u);
  CHECK(w.take_hud_refresh());
}

TEST_CASE("flag watcher: floors left to the game are not owed into another save after a save load (row 140)") {
  constexpr FlagKey kDekuFloors{0x5c6c21c1u, 0};
  FakeStore game_store = dungeon_game();
  const auto floors = [&](std::uint32_t v) { game_store.values[kDekuFloors] = entry(kDekuFloors, v, proto::FlagKind::uint32); };
  floors(0b01100);
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap, kDekuFloors}), FlagRole::player);
  w.frame(store);
  for (int n = 0; n < 3; ++n) {  // put back, undone, put back, undone: left
    floors(0b10000);
    w.frame(store);
  }
  CHECK(w.stats().one_way_left == 1);
  // The title, another save (a save load window): nothing owed from the old one.
  w.set_loading(true);
  w.set_scene(false);
  w.set_title(true);
  w.frame(store);
  floors(0b00001);
  w.frame(store);
  w.set_title(false);
  w.set_scene(true);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames + 10; ++i) w.frame(store);
  CHECK(w.stats().one_way_owed == 0);
}

TEST_CASE("flag watcher: one-off drops far apart are each put back (row 140 keeps issue 146)") {
  FakeStore game_store = dungeon_game();
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap}), FlagRole::player);
  w.frame(store);
  game_store.set(kDekuMap, 1);
  w.frame(store);
  for (int n = 0; n < 5; ++n) {
    game_store.set(kDekuMap, 0);
    w.frame(store);
    CHECK(game_store.get(kDekuMap) == 1);
    for (int i = 0; i < 3; ++i) w.frame(store);
  }
  CHECK(w.stats().one_way_restores == 5);
  CHECK(w.stats().one_way_left == 0);
}

TEST_CASE("flag watcher: the key of a write that keeps failing is counted and said once (row 140)") {
  FakeStore game_store = dungeon_game();
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap}), FlagRole::player);
  game_store.unwritable.insert(kDekuMap);
  w.frame(store);
  for (std::uint64_t n = 0; n < FlagWatcher::kFailureSayAt; ++n) {
    game_store.set(kDekuMap, 1);
    w.frame(store);
    game_store.set(kDekuMap, 0);  // a one-way drop whose put-back the game refuses
    w.frame(store);
  }
  CHECK(w.stats().write_failures == FlagWatcher::kFailureSayAt);
  CHECK(w.stats().top_write_failure == kDekuMap);
  CHECK(w.stats().top_write_failure_count == FlagWatcher::kFailureSayAt);
  CHECK(std::string_view(w.stats().top_write_failure_site) == "diff put back");  // g1-events' site name
  const auto said = w.take_keeps_failing();
  REQUIRE(said.size() == 1);
  CHECK(said[0].key == kDekuMap);
  CHECK(said[0].count == FlagWatcher::kFailureSayAt);
  game_store.set(kDekuMap, 1);
  w.frame(store);
  game_store.set(kDekuMap, 0);
  w.frame(store);
  CHECK(w.take_keeps_failing().empty());  // once per key
}

TEST_CASE("flag watcher: the host's save load through the title with a scene read is a save load (forest-temple-a)") {
  // A player's 0.11.2 host log: the host went to Hookshot's title mid-session and loaded a save without the Forest
  // Temple map; no save load was seen, and the old save's floors were put back into the new one, once a frame.
  FakeStore game_store = dungeon_game();
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap, kDekuCompass, kGotBomb}), FlagRole::host);
  w.set_scene(true);
  w.frame(store);
  game_store.set(kDekuMap, 1);
  game_store.set(kDekuCompass, 1);
  w.frame(store);  // in play: the old save's map and compass
  // The title menu: a scene is still read there, so only the title sign says it is a save load.
  w.set_loading(true);
  w.set_title(true);
  w.frame(store);
  game_store.set(kDekuMap, 0);  // the new save has no map: one flag back, fewer than kSaveLoadMin
  for (int i = 0; i < 5; ++i) w.frame(store);
  bool reset = false;
  for (const auto& m : decode(link_back(w, store))) {
    if (const auto* r = std::get_if<proto::FlagReset>(&m)) {
      reset = std::find(r->flags.begin(), r->flags.end(), entry(kDekuMap, 0)) != r->flags.end();
    }
  }
  CHECK(reset);  // the group follows the host's new save (#96)
  CHECK(w.stats().host_resets == 1);
  CHECK(w.stats().title_loads == 1);
  CHECK(w.stats().one_way_restores == 0);  // the old save's map is not put into the new save
  CHECK(game_store.get(kDekuMap) == 0);
  for (int i = 0; i < 10; ++i) w.frame(store);
  CHECK(game_store.get(kDekuMap) == 0);
  CHECK(w.stats().one_way_restores == 0);
}

TEST_CASE("flag watcher: the game's own load nodes make the save load, Link read or not (patch 0024)") {
  // The System menu's Load or Game Over's Try Again: the save is swapped while Link may still be read.
  FakeStore game_store = dungeon_game();
  ItemFlagStore store(game_store);
  FlagWatcher w(SharedFlags({kDekuMap, kDekuCompass, kGotBomb}), FlagRole::host);
  w.set_native_loads(true);
  w.set_scene(true);
  w.frame(store);
  game_store.set(kDekuMap, 1);
  w.frame(store);  // in play: the old save's map
  w.note_save_load_start();
  w.set_loading(false);  // Link still read
  game_store.set(kDekuMap, 0);  // the new save has no map
  for (int i = 0; i < 3; ++i) {
    for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagSet>(m));
    CHECK(w.settling());
  }
  CHECK(game_store.get(kDekuMap) == 0);  // never put back: no one-way restore while it loads
  w.note_save_loaded();
  bool reset = false;
  for (const auto& m : decode(link_back(w, store))) {
    if (const auto* r = std::get_if<proto::FlagReset>(&m)) {
      reset = std::find(r->flags.begin(), r->flags.end(), entry(kDekuMap, 0)) != r->flags.end();
    }
  }
  CHECK(reset);  // the group follows the host's save
  CHECK(w.stats().native_loads == 1);
  CHECK(w.stats().host_resets == 1);
  CHECK(w.stats().one_way_restores == 0);
  const auto seen = w.take_save_load();
  REQUIRE(seen.has_value());
  CHECK(seen->native);
  CHECK(game_store.get(kDekuMap) == 0);
}

TEST_CASE("flag watcher: with the game's own load nodes the old signs are only counted (patch 0024)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.set_native_loads(true);
  w.frame(store);
  // The title menu in a load window, no load node: the old sign would call it a save load.
  w.set_loading(true);
  w.set_scene(false);
  w.set_title(true);
  for (int i = 0; i < FlagWatcher::kTitleFrames + 2; ++i) w.frame(store);
  store.set(kDoor, 1);
  w.frame(store);
  for (const auto& m : decode(link_back(w, store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  CHECK(w.stats().host_resets == 0);
  CHECK(w.stats().old_sign_loads == 1);
  CHECK(w.stats().native_loads == 0);
  CHECK_FALSE(w.take_save_load().has_value());
}

TEST_CASE("flag watcher: a guest's own load node puts the group's values back (patch 0024)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.set_native_loads(true);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  w.frame(store);
  REQUIRE(store.get(kDoor) == 1);
  w.note_save_load_start();
  store.set(kDoor, 0);  // the guest's own older file
  w.frame(store);
  w.note_save_loaded();
  bool door0 = false;
  for (const auto& m : decode(link_back(w, store))) {
    if (const auto* set = std::get_if<proto::FlagSet>(&m)) {
      door0 = door0 || std::find(set->flags.begin(), set->flags.end(), entry(kDoor, 0)) != set->flags.end();
    }
  }
  CHECK_FALSE(door0);  // never sent as the guest's progress
  CHECK(store.get(kDoor) == 1);  // the group's value back
  CHECK(w.stats().native_loads == 1);
}

TEST_CASE("flag watcher: the host's Game Over Try Again keeps the group's progress, no FLAG_RESET (patch 0024)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.set_native_loads(true);
  w.frame(store);
  store.set(kDoor, 1);  // the group's progress since the last save
  w.frame(store);
  w.note_save_load_start();
  store.set(kDoor, 0);  // the last save, from before it
  w.frame(store);
  w.note_save_loaded(true);  // after Game Over's own save, no title between
  for (const auto& m : decode(link_back(w, store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  CHECK(store.get(kDoor) == 1);  // the group's value back in the host's game
  CHECK(w.stats().host_resets == 0);
  CHECK(w.stats().retry_loads == 1);
  CHECK(w.stats().native_loads == 1);
  CHECK_FALSE(w.take_reset());
}

TEST_CASE("flag watcher: the host's Try Again puts the group's values back at once, not after the settle") {
  // The room the host respawns in loads with the group's state, as a guest's (review of a8c86567).
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.set_native_loads(true);
  w.frame(store);
  store.set(kDoor, 1);
  w.frame(store);
  w.note_save_load_start();
  store.set(kDoor, 0);  // the last save
  w.frame(store);
  w.note_save_loaded(true);
  w.frame(store);  // the frame the save is in: still loading
  CHECK(w.settling());
  CHECK(store.get(kDoor) == 1);  // back already
  store.set(kDoor, 0);           // the game's load writes the save's value again
  w.frame(store);
  CHECK(store.get(kDoor) == 1);
  for (const auto& m : decode(link_back(w, store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  CHECK(store.get(kDoor) == 1);
  CHECK(w.stats().host_resets == 0);
}

TEST_CASE("flag watcher: a held flag's retries are counted as held, not as write failures (events-next-a-12)") {
  // The events-next-a-11 probe logged "top write failure: named 0xb3a4fe84[0]" ~60 times a second: a played-event
  // flag HoldFlagStore held (by design) while our Link stayed in the dungeon, retried every frame. A hold is waiting,
  // not a failure: write_failures stays for writes the game itself refused.
  FakeStore game_store = game();
  game_store.unwritable.insert(kChest);
  bool hold = true;
  HoldFlagStore store(game_store, [&hold](FlagKey key) { return hold && key == kDoor; });
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1), entry(kChest, 1)})));
  for (int i = 0; i < 5; ++i) w.frame(store);
  CHECK(game_store.get(kDoor) == 0);           // still held
  CHECK(w.stats().held_retries >= 5);          // tried every frame, counted as held
  CHECK(w.stats().write_failures >= 5);        // the chest the game refuses: a real failure
  CHECK(w.stats().top_write_failure == kChest);  // the held door never names itself
  CHECK(w.stats().top_write_failure_count == w.stats().write_failures);
  hold = false;
  w.frame(store);
  CHECK(game_store.get(kDoor) == 1);  // written once the hold ends
}

TEST_CASE("flag watcher: the host's Try Again into a held flag writes the group's value once the hold ends") {
  // A held flag (issue 129, HoldFlagStore): kept in unwritten_ and written later, not dropped by the retry as if our
  // game had changed it (review of 159a4fd7).
  FakeStore game_store = game();
  bool hold = false;
  HoldFlagStore store(game_store, [&hold](FlagKey key) { return hold && key == kDoor; });
  FlagWatcher w(shared(), FlagRole::host);
  w.set_native_loads(true);
  w.frame(store);
  game_store.set(kDoor, 1);
  w.frame(store);
  hold = true;
  w.note_save_load_start();
  game_store.set(kDoor, 0);  // the last save
  w.frame(store);
  w.note_save_loaded(true);
  for (int i = 0; i < 5; ++i) w.frame(store);
  CHECK(game_store.get(kDoor) == 0);  // held: not yet
  hold = false;
  for (const auto& m : decode(link_back(w, store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  CHECK(game_store.get(kDoor) == 1);  // the group's value, once the hold ended
  CHECK(w.stats().host_resets == 0);
}

TEST_CASE("flag watcher: a guest's held load revert is written once the hold ends, not dropped") {
  // The same missed assignment in the loading branch (no save load seen): the group's value kept for a held flag.
  FakeStore game_store = game();
  bool hold = false;
  HoldFlagStore store(game_store, [&hold](FlagKey key) { return hold && key == kDoor; });
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  w.frame(store);
  REQUIRE(game_store.get(kDoor) == 1);
  hold = true;
  w.set_loading(true);
  game_store.set(kDoor, 0);  // a load puts an older value back
  for (int i = 0; i < 5; ++i) w.frame(store);
  CHECK(game_store.get(kDoor) == 0);
  hold = false;
  w.frame(store);
  CHECK(game_store.get(kDoor) == 1);
}

TEST_CASE("flag watcher: a save in with no load start (the title's New Game) is still the save load (patch 0024)") {
  // NormalTitle.module runs AfterLoadSaveData itself: the on-load node with no SeqExecuteLoadSaveData before it.
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.set_native_loads(true);
  w.frame(store);
  store.set(kDoor, 1);
  w.frame(store);
  store.set(kDoor, 0);  // a new file
  w.note_save_loaded();
  bool reset = false;
  for (const auto& m : decode(link_back(w, store))) {
    if (const auto* r = std::get_if<proto::FlagReset>(&m)) {
      reset = reset || std::find(r->flags.begin(), r->flags.end(), entry(kDoor, 0)) != r->flags.end();
    }
  }
  CHECK(reset);  // the group follows the host's new file (host-save)
  CHECK(w.stats().native_loads == 1);
  CHECK(w.stats().retry_loads == 0);
  CHECK(w.stats().host_resets == 1);
  CHECK(store.get(kDoor) == 0);
}

TEST_CASE("flag watcher: a load node's start with no save in ends after kNativeLoadMaxFrames (patch 0024)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.set_native_loads(true);
  w.frame(store);
  w.note_save_load_start();
  w.set_loading(false);
  for (std::uint64_t i = 0; i <= FlagWatcher::kNativeLoadMaxFrames + FlagWatcher::kSettleFrames + 2; ++i) {
    w.set_loading(false);
    w.frame(store);
  }
  CHECK(w.stats().native_timeouts == 1);
  CHECK_FALSE(w.settling());
  store.set(kDoor, 1);  // play again: our change goes out
  bool sent = false;
  for (const auto& m : decode(w.frame(store))) {
    if (const auto* set = std::get_if<proto::FlagSet>(&m)) {
      sent = sent || std::find(set->flags.begin(), set->flags.end(), entry(kDoor, 1)) != set->flags.end();
    }
  }
  CHECK(sent);
}

TEST_CASE("flag watcher: both players open the map chest at the same time and both keep the map (issue 146)") {
  FakeStore host_game = dungeon_game();
  FakeStore player_game = dungeon_game();
  ItemFlagStore host_store(host_game);
  ItemFlagStore player_store(player_game);
  const SharedFlags flags({kDekuMap, kDekuCompass, kGotBomb});
  FlagWatcher host(flags, FlagRole::host);
  FlagWatcher player(flags, FlagRole::player);
  host.frame(host_store);
  player.frame(player_store);

  // The same frame: each game's own chest sets the map.
  host_game.set(kDekuMap, 1);
  player_game.set(kDekuMap, 1);
  const auto from_host = host.frame(host_store);
  const auto from_player = player.frame(player_store);
  for (const auto& m : decode(from_player)) CHECK(host.receive(m));
  const auto answer = host.frame(host_store);
  for (const auto& m : decode(from_host)) CHECK(player.receive(m));
  for (const auto& m : decode(answer)) CHECK(player.receive(m));
  for (int i = 0; i < 5; ++i) {
    for (const auto& m : decode(player.frame(player_store))) CHECK(host.receive(m));
    for (const auto& m : decode(host.frame(host_store))) CHECK(player.receive(m));
    CHECK(host_game.get(kDekuMap) == 1);
    CHECK(player_game.get(kDekuMap) == 1);
  }

  // One game's own chest flow then drops it (the second chest's get): it
  // comes back there, and the other game is not touched.
  player_game.set(kDekuMap, 0);
  CHECK(player.frame(player_store).empty());
  CHECK(player_game.get(kDekuMap) == 1);
  CHECK(host_game.get(kDekuMap) == 1);
  host_game.set(kDekuMap, 0);
  CHECK(host.frame(host_store).empty());
  CHECK(host_game.get(kDekuMap) == 1);
}

TEST_CASE("flag watcher: a named Bool shared one way (a chest's opened flag) is noted and never closed again") {
  FakeStore store = game();
  FlagWatcher w(SharedFlags({kKey0}), FlagRole::player);
  CHECK(w.share_pickup_named(kChest));
  CHECK_FALSE(w.share_pickup_named(kChest));                          // once
  CHECK_FALSE(w.share_pickup_named(FlagKey::key64_flag(0x1234)));      // 64-bit keys: share_pickup
  CHECK(w.shared().contains(kChest));
  w.frame(store);

  // Opened in another game: written here and noted for the hook.
  CHECK(w.receive(state({entry(kChest, 1)})));
  w.frame(store);
  CHECK(store.get(kChest) == 1);
  CHECK(w.take_written_for_others() == std::vector<FlagKey>{kChest});
  // A 0 coming in is not taken, and the game's own 0 is not sent.
  CHECK(w.receive(state({entry(kChest, 0)})));
  w.frame(store);
  CHECK(store.get(kChest) == 1);
  store.set(kChest, 0);
  CHECK(w.frame(store).empty());
  // A plain shared named flag is not noted.
  FakeStore other = game();
  FlagWatcher plain(SharedFlags({kDoor}), FlagRole::player);
  plain.frame(other);
  CHECK(plain.receive(state({entry(kDoor, 1)})));
  plain.frame(other);
  CHECK(plain.take_written_for_others().empty());
}

TEST_CASE("flag watcher: a one-way room flag the game resets is no save load and no part of a reset") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  CHECK(w.share_one_way(kChest));  // a room's own switch state
  w.frame(store);
  store.set(kChest, 1);
  w.frame(store);
  // A door out of that room: the game sets it back to 0 itself, and one
  // more flag goes back (time travel, say): no save load.
  w.set_loading(true);
  store.set(kChest, 0);
  store.set(kDoor, 0);
  w.frame(store);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  }
  CHECK(w.stats().host_resets == 0);

  // A real save load (two saved flags back) leaves the one-way flag out.
  store.set(kDoor, 1);
  store.set(kKey0, 1);
  store.set(kChest, 1);
  w.frame(store);
  w.set_loading(true);
  w.set_scene(false);  // the title screen and file select: no scene loaded
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  store.set(kDoor, 0);
  store.set(kKey0, 0);
  store.set(kChest, 0);
  w.frame(store);
  w.set_scene(true);
  w.set_loading(false);
  std::vector<proto::FlagEntry> reset;
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) {
      if (const auto* r = std::get_if<proto::FlagReset>(&m)) reset.insert(reset.end(), r->flags.begin(), r->flags.end());
    }
  }
  CHECK(reset == std::vector{entry(kDoor, 0), entry(kKey0, 0)});
}

TEST_CASE("flag watcher: a max-rule value our game lowers is ours: not sent, no echo (MaxLife loop)") {
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};  // PlayerStatus.MaxLife, max
  FakeStore game_store;
  game_store.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  ItemFlagStore store(game_store);
  CHECK(store.is_max(kMaxLife));
  CHECK_FALSE(store.is_count(kMaxLife));
  FlagWatcher w(SharedFlags({kMaxLife}), FlagRole::player);
  w.frame(store);
  // The group's 16 comes in; our game writes its own 12 back (no object
  // walk raised Link's hearts). Nothing goes out, every time.
  for (int round = 0; round < 5; ++round) {
    CHECK(w.receive(state({entry(kMaxLife, 16, proto::FlagKind::int32)})));
    CHECK(w.frame(store).empty());
    CHECK(game_store.get(kMaxLife) == 16u);
    game_store.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
    CHECK(w.frame(store).empty());
    CHECK(w.frame(store).empty());
  }
  CHECK(w.stats().max_drops_kept == 5);
  // A rise of our own still goes out.
  game_store.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kMaxLife, 20, proto::FlagKind::int32)});
  // A host's game lowering it sends no FLAG_STATE either.
  FakeStore host_game;
  host_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);
  ItemFlagStore host_store(host_game);
  FlagWatcher h(SharedFlags({kMaxLife}), FlagRole::host);
  h.frame(host_store);
  host_game.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  CHECK(h.frame(host_store).empty());
}

TEST_CASE("flag watcher: host and player settle a MaxLife the player's game keeps lowering") {
  // The run that found it: after the host's save load, B's game wrote its own
  // max life back every frame; B sent it, the host answered with its higher
  // one, and the two went round for ever with a notice each time.
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};
  FakeStore host_game, player_game;
  host_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);
  player_game.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  ItemFlagStore host_store(host_game), player_store(player_game);
  FlagWatcher h(SharedFlags({kMaxLife}), FlagRole::host);
  FlagWatcher p(SharedFlags({kMaxLife}), FlagRole::player);
  h.frame(host_store);
  p.frame(player_store);
  CHECK(p.receive(state({entry(kMaxLife, 16, proto::FlagKind::int32)})));  // the host's value arrives
  int sent = 0;
  int raised = 0;
  for (int frame = 0; frame < 300; ++frame) {
    for (const auto& o : p.frame(player_store)) {
      ++sent;
      CHECK(h.receive(o.bytes));
    }
    for (const auto& o : h.frame(host_store)) CHECK(p.receive(o.bytes));
    for (const auto& r : p.take_received()) {
      if (r.key == kMaxLife && r.after.value > r.before.value) ++raised;
    }
    // The player's game puts its own hearts back each frame.
    player_game.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  }
  CHECK(sent == 0);
  CHECK(raised == 1);  // the host's value, once
  CHECK(host_game.get(kMaxLife) == 16);
}

TEST_CASE("item store: a reset that lowers the hearts holds life to the new maximum") {
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};
  constexpr FlagKey kLife{0x31ab5580u, 0};
  FakeStore game_store;
  game_store.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);
  game_store.values[kLife] = entry(kLife, 18, proto::FlagKind::int32);
  ItemFlagStore store(game_store);
  CHECK(store.write_exact(entry(kMaxLife, 12, proto::FlagKind::int32)));
  CHECK(game_store.get(kMaxLife) == 12);
  CHECK(game_store.get(kLife) == 12);
  // Life under the new maximum is left alone.
  game_store.values[kLife] = entry(kLife, 8, proto::FlagKind::int32);
  CHECK(store.write_exact(entry(kMaxLife, 10, proto::FlagKind::int32)));
  CHECK(game_store.get(kLife) == 8);
}

TEST_CASE("item store: a guest's load write-back never takes a floor off, the host's reset decides (#63/#76)") {
  constexpr FlagKey kFloors{0x46913e20u, 0};  // DungeonInfo.DodongoCavern.EnteredFloorBit
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};
  FakeStore game_store;
  game_store.values[kFloors] = entry(kFloors, 0b0110, proto::FlagKind::uint32);
  game_store.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);
  ItemFlagStore store(game_store);
  // The guest's own load, the group's floors written back: the floors it entered stay, the group's are added.
  CHECK(store.write_load_back(entry(kFloors, 0b0001, proto::FlagKind::uint32)));
  CHECK(game_store.get(kFloors) == 0b0111);
  CHECK(store.write_load_back(entry(kFloors, 0, proto::FlagKind::uint32)));
  CHECK(game_store.get(kFloors) == 0b0111);
  // Any other item still takes the exact value on a load write-back.
  CHECK(store.write_load_back(entry(kMaxLife, 12, proto::FlagKind::int32)));
  CHECK(game_store.get(kMaxLife) == 12);
  // The host's FLAG_RESET or the join snapshot: the host's save decides, floors too (docs/flags.md, "Saves").
  CHECK(store.write_exact(entry(kFloors, 0b0001, proto::FlagKind::uint32)));
  CHECK(game_store.get(kFloors) == 0b0001);
  // And the next plain write merges again (the bits rule's union).
  CHECK(store.write(entry(kFloors, 0b0100, proto::FlagKind::uint32)));
  CHECK(game_store.get(kFloors) == 0b0101);
}

TEST_CASE("flag watcher, host: two flags back with a scene loaded is no save load (doors, enter flags, death)") {
  // A door out of a room whose "enter" flags reset, a Game Over continue in
  // the same scene, a warp: Link is gone a moment, the scene stays loaded.
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  store.set(kKey0, 1);
  w.frame(store);
  w.set_loading(true);
  store.set(kDoor, 0);
  store.set(kKey0, 0);
  for (int i = 0; i < FlagWatcher::kTitleFrames + 5; ++i) w.frame(store);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  }
  CHECK(w.stats().host_resets == 0);
}

TEST_CASE("flag watcher, host: the title screen with only one flag back is no save load") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  w.frame(store);
  w.set_loading(true);
  w.set_scene(false);
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  store.set(kDoor, 0);
  w.frame(store);
  w.set_scene(true);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  }
  CHECK(w.stats().host_resets == 0);
}

TEST_CASE("flag watcher, host: one flag held at 0 for many frames counts once") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  w.frame(store);
  w.set_loading(true);
  w.set_scene(false);
  store.set(kDoor, 0);  // one flag back, seen again every frame of the title
  for (int i = 0; i < FlagWatcher::kTitleFrames + 10; ++i) w.frame(store);
  CHECK_FALSE(w.take_save_load().has_value());
  w.set_scene(true);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) CHECK_FALSE(std::holds_alternative<proto::FlagReset>(m));
  }
  CHECK(w.stats().host_resets == 0);
}

TEST_CASE("flag watcher, host: flags back early in the title, the title long enough later: a save load") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  store.set(kKey0, 1);
  w.frame(store);
  w.set_loading(true);
  w.set_scene(false);
  w.frame(store);
  store.set(kDoor, 0);  // the save is read early...
  store.set(kKey0, 0);
  w.frame(store);
  CHECK_FALSE(w.take_save_load().has_value());  // ...the title has not lasted yet
  // Nothing changes any more; the title goes on until it has lasted.
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  const auto seen = w.take_save_load();
  REQUIRE(seen.has_value());
  CHECK(seen->flags == 2);
  CHECK(seen->frames >= FlagWatcher::kTitleFrames);
  CHECK_FALSE(w.take_save_load().has_value());  // once
  w.set_scene(true);
  w.set_loading(false);
  std::vector<proto::FlagEntry> reset;
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) {
      if (const auto* r = std::get_if<proto::FlagReset>(&m)) reset.insert(reset.end(), r->flags.begin(), r->flags.end());
    }
  }
  CHECK(reset == std::vector{entry(kDoor, 0), entry(kKey0, 0)});
}

// Issue 89: a Gold Skulltula's token stayed in the other game, which counted
// it again. The token count is guarded by the Skulltulas' keys (guard_count).
namespace {

constexpr FlagKey kTokens{0x04735978u, 0};
const FlagKey kSkull = FlagKey::key64_flag(0x032f42f523be6425ull);
const FlagKey kSkull2 = FlagKey::key64_flag(0x6a8d9b2373d08fcbull);

// The game's flags with the token count, a count as ItemFlagStore makes it.
class TokenStore : public FakeStore {
 public:
  bool is_count(FlagKey key) override { return key == kTokens; }
  bool write(const proto::FlagEntry& e) override {
    if (e.kind != proto::FlagKind::count_delta) return FakeStore::write(e);
    const FlagKey key = key_of(e);
    if (!values.contains(key)) return false;
    proto::FlagEntry now = values[key];
    now.value = static_cast<std::uint32_t>(static_cast<std::int32_t>(now.value) +
                                           static_cast<std::int32_t>(e.value));
    return FakeStore::write(now);
  }
  void tokens(std::int32_t n) {
    values[kTokens] = proto::FlagEntry{kTokens, proto::FlagKind::int32, static_cast<std::uint32_t>(n)};
  }
  std::int32_t tokens() const { return static_cast<std::int32_t>(values.at(kTokens).value); }
};

TokenStore token_game() {
  TokenStore store;
  for (FlagKey k : {kChest, kDoor, kKey0, kKey1, kHearts}) store.set(k, 0);
  store.set(kSkull, 0);
  store.set(kSkull2, 0);
  store.tokens(5);
  return store;
}

SharedFlags token_shared() { return SharedFlags({kChest, kTokens}); }

proto::FlagEntry token_delta(std::int32_t n) {
  return {kTokens, proto::FlagKind::count_delta, static_cast<std::uint32_t>(n)};
}
proto::FlagEntry token_total(std::int32_t n) {
  return {kTokens, proto::FlagKind::int32, static_cast<std::uint32_t>(n)};
}

void guard(FlagWatcher& w) {
  const FlagKey sources[] = {kSkull, kSkull2};
  w.guard_count(kTokens, sources);
}

// Every entry of a frame's messages, in order.
std::vector<proto::FlagEntry> entries(const std::vector<Outgoing>& out) {
  std::vector<proto::FlagEntry> all;
  for (const auto& m : decode(out)) {
    if (const auto* s = std::get_if<proto::FlagSet>(&m)) all.insert(all.end(), s->flags.begin(), s->flags.end());
    if (const auto* s = std::get_if<proto::FlagState>(&m)) all.insert(all.end(), s->flags.begin(), s->flags.end());
  }
  return all;
}

bool has(const std::vector<proto::FlagEntry>& all, const proto::FlagEntry& e) {
  return std::find(all.begin(), all.end(), e) != all.end();
}

// Frames until the hold is over; what they sent.
std::vector<proto::FlagEntry> run_hold(FlagWatcher& w, FlagStore& store) {
  std::vector<proto::FlagEntry> all;
  for (std::uint64_t f = 0; f <= FlagWatcher::kGuardHoldFrames; ++f) {
    const auto e = entries(w.frame(store));
    all.insert(all.end(), e.begin(), e.end());
  }
  return all;
}

}  // namespace

TEST_CASE("token guard: the Skulltulas' keys are shared from the start") {
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  CHECK(w.shared().contains(kSkull));
  CHECK(w.shared().contains(kSkull2));
  CHECK(w.pickups() == 2);
}

TEST_CASE("token guard: a token taken here counts at once") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  // The pickup: the Skulltula's key and the count in the same scan.
  store.set(kSkull, 1);
  store.tokens(6);
  const auto sent = entries(w.frame(store));
  CHECK(has(sent, entry(kSkull, 1)));
  CHECK(has(sent, token_delta(1)));
  CHECK(run_hold(w, store).empty());
  CHECK(store.tokens() == 6);
  CHECK(w.stats().counts_guarded == 0);
}

TEST_CASE("token guard: the key a little after the count still counts") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  store.tokens(6);
  CHECK(w.frame(store).empty());  // held
  CHECK(w.frame(store).empty());
  store.set(kSkull, 1);
  CHECK(has(entries(w.frame(store)), token_delta(1)));
  CHECK(run_hold(w, store).empty());
  CHECK(store.tokens() == 6);
}

TEST_CASE("token guard: a token another player took, counted again here, is put back") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  // Another player took the Skulltula: its key, and the group's count 6.
  w.receive(state({entry(kSkull, 1), token_total(6)}));
  w.frame(store);
  CHECK(store.get(kSkull) == 1);
  CHECK(store.tokens() == 6);
  CHECK(w.take_written_for_others() == std::vector{kSkull});
  // The token stayed here and our Link takes it: the game counts it again
  // and sets the key it has already.
  store.tokens(7);
  CHECK(run_hold(w, store).empty());  // nothing goes out
  CHECK(store.tokens() == 6);         // and the count is the group's again
  CHECK(w.stats().counts_guarded == 1);
  const auto told = w.take_counts_guarded();
  REQUIRE(told.size() == 1);
  CHECK(told[0].count == kTokens);
  CHECK(told[0].amount == 1);
  // Then the world sync gives the same token back (both took it): our game
  // goes to 5; that is put back up once, and nothing is sent.
  store.tokens(5);
  CHECK(w.frame(store).empty());
  CHECK(store.tokens() == 6);
  // A real token later still counts.
  store.set(kSkull2, 1);
  store.tokens(7);
  CHECK(has(entries(w.frame(store)), token_delta(1)));
}

TEST_CASE("token guard: a give-back while the gain is held cancels it") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  w.receive(state({entry(kSkull, 1), token_total(6)}));
  w.frame(store);
  store.tokens(7);
  CHECK(w.frame(store).empty());
  store.tokens(6);  // the world sync's give-back
  CHECK(run_hold(w, store).empty());
  CHECK(store.tokens() == 6);
  CHECK(w.stats().counts_guarded == 0);
}

TEST_CASE("token guard: a gain with no take anywhere is sent after the hold") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  store.tokens(6);  // the cheat window's give
  CHECK(w.frame(store).empty());
  CHECK(run_hold(w, store) == std::vector{token_delta(1)});
  CHECK(store.tokens() == 6);
}

TEST_CASE("token guard: a join snapshot's keys are no take now") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  proto::FlagSnapshot part;
  part.part = 0;
  part.part_count = 1;
  part.flags = {entry(kSkull, 1), token_total(6)};
  w.receive(part);
  w.frame(store);
  CHECK(store.get(kSkull) == 1);
  store.tokens(7);
  CHECK(run_hold(w, store) == std::vector{token_delta(1)});
}

TEST_CASE("token guard: the host's total keeps a held gain in our game") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  store.tokens(6);  // ours, held
  CHECK(w.frame(store).empty());
  w.receive(state({token_total(8)}));  // two tokens from others
  w.frame(store);
  CHECK(store.tokens() == 9);
  store.set(kSkull, 1);  // our key comes: ours counts
  CHECK(has(entries(w.frame(store)), token_delta(1)));
  CHECK(store.tokens() == 9);
}

TEST_CASE("token guard, host: a player's take, counted again by the host, is put back") {
  TokenStore store = token_game();
  FlagWatcher h(token_shared(), FlagRole::host);
  guard(h);
  h.frame(store);
  proto::FlagSet set;
  set.player_number = 2;
  set.flags = {entry(kSkull, 1), token_delta(1)};
  h.receive(set);
  CHECK(has(entries(h.frame(store)), token_total(6)));
  CHECK(store.tokens() == 6);
  // The token stayed in the host's game and its Link takes it.
  store.tokens(7);
  CHECK(h.frame(store).empty());
  // A player's own token meanwhile: the host's answer leaves the held gain out.
  set.flags = {entry(kSkull2, 1), token_delta(1)};
  h.receive(set);
  CHECK(has(entries(h.frame(store)), token_total(7)));
  CHECK(store.tokens() == 8);
  CHECK_FALSE(has(run_hold(h, store), token_total(8)));
  CHECK(store.tokens() == 7);
  CHECK(h.stats().counts_guarded == 1);
}

// Player report 33: two players took the same token at the same time, each
// game set its Skulltula's key 0 to 1 and counted it, and the group's count
// went up by 2. The host counts a token once: a player's gain for a key the
// group had already is not added, and the total is stated again.
TEST_CASE("token race, host: two players take the same token at once: counted once") {
  TokenStore store = token_game();  // 5 tokens
  FlagWatcher h(token_shared(), FlagRole::host);
  guard(h);
  h.frame(store);
  h.receive(proto::FlagSet{2, {entry(kSkull, 1), token_delta(1)}});
  CHECK(has(entries(h.frame(store)), token_total(6)));
  // Player 3 took it too, before it heard of player 2's take.
  h.receive(proto::FlagSet{3, {entry(kSkull, 1), token_delta(1)}});
  CHECK(has(entries(h.frame(store)), token_total(6)));  // stated again: 3 puts its count right
  CHECK(store.tokens() == 6);
  CHECK(h.stats().raced_gains == 1);
  const auto told = h.take_counts_guarded();
  REQUIRE(told.size() == 1);
  CHECK(told[0].amount == 1);
  // A real token of player 3's later still counts.
  h.receive(proto::FlagSet{3, {entry(kSkull2, 1), token_delta(1)}});
  CHECK(has(entries(h.frame(store)), token_total(7)));
  CHECK(store.tokens() == 7);
}

TEST_CASE("token race, host: the host and a player take the same token at once: counted once") {
  TokenStore store = token_game();
  FlagWatcher h(token_shared(), FlagRole::host);
  guard(h);
  h.frame(store);
  // The host's Link takes it: key and count in the same scan.
  store.set(kSkull, 1);
  store.tokens(6);
  CHECK(has(entries(h.frame(store)), token_total(6)));
  // The player's take of the same token comes after; the delta before the key
  // in the set, as a set may hold them.
  h.receive(proto::FlagSet{2, {token_delta(1), entry(kSkull, 1)}});
  CHECK(has(entries(h.frame(store)), token_total(6)));
  CHECK(store.tokens() == 6);
  CHECK(h.stats().raced_gains == 1);
}

TEST_CASE("token race, host: the player's key a frame before its gain still counts once") {
  TokenStore store = token_game();
  FlagWatcher h(token_shared(), FlagRole::host);
  guard(h);
  h.frame(store);
  h.receive(proto::FlagSet{2, {entry(kSkull, 1), token_delta(1)}});
  h.frame(store);
  h.receive(proto::FlagSet{3, {entry(kSkull, 1)}});
  h.frame(store);
  h.receive(proto::FlagSet{3, {token_delta(1)}});
  h.frame(store);
  CHECK(store.tokens() == 6);
  // A raced key drops one gain only, and only for a while: a later gain of
  // player 3's with no key (a randomizer's token from a chest) counts.
  h.receive(proto::FlagSet{3, {entry(kSkull, 1)}});
  for (std::uint64_t f = 0; f <= FlagWatcher::kRaceFrames; ++f) h.frame(store);
  h.receive(proto::FlagSet{3, {token_delta(1)}});
  h.frame(store);
  CHECK(store.tokens() == 7);
  CHECK(h.stats().raced_gains == 1);
}

TEST_CASE("game ticks: calls become 1/60 s of the clock (issue 106)") {
  coop::host::GameTicks none;
  CHECK(none.step() == 1);  // no clock: one call is one frame
  CHECK(none.step() == 1);

  std::uint64_t now = 1'000'000;
  // `calls` intervals: calls + 1 calls span one second; the first is 1 tick.
  auto run = [&](std::uint64_t every_us, int calls) {
    coop::host::GameTicks t;
    t.set_clock([&] { return now; });
    unsigned sum = 0;
    for (int i = 0; i <= calls; ++i) {
      sum += t.step();
      now += every_us;
    }
    return sum;
  };
  const unsigned at165 = run(6'061, 165);  // one second of high refresh
  CHECK(at165 >= 59);
  CHECK(at165 <= 62);
  const unsigned at60 = run(16'667, 60);
  CHECK(at60 >= 59);
  CHECK(at60 <= 62);
  const unsigned at30 = run(33'333, 30);
  CHECK(at30 >= 59);
  CHECK(at30 <= 62);

  coop::host::GameTicks t;
  t.set_clock([&] { return now; });
  t.step();
  now += 3'000'000;  // a 3 s stall counts as a few frames, as one call did
  CHECK(t.step() == coop::host::GameTicks::kMaxStep);
}

TEST_CASE("flag watcher: the settle lasts 2 s at 165 presents a second, not 0.73 s (issue 106)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  std::uint64_t now = 5'000'000;
  w.set_clock([&] { return now; });
  w.set_loading(true);
  w.frame(store);
  now += 6'061;
  w.set_loading(false);
  const std::uint64_t loaded = now;
  while (w.settling() && now - loaded < 5'000'000) {
    w.frame(store);
    now += 6'061;
  }
  const std::uint64_t settle = now - loaded;
  CHECK(settle >= 1'950'000);
  CHECK(settle <= 2'100'000);
}

// Issue 195: our Link opened again a chest another player had opened (it is
// shown open here, but its AI lets it open). Its content counted once for the
// group already, so this game's next count gain is put back, not sent.
TEST_CASE("doubled chest: the next count gain is put back, not sent") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  w.frame(store);
  w.expect_doubled_gain();
  store.tokens(6);
  const auto sent = entries(w.frame(store));
  CHECK_FALSE(has(sent, token_delta(1)));
  CHECK(store.tokens() == 5);
  CHECK(w.stats().doubled_gains == 1);
  const auto guarded = w.take_counts_guarded();
  REQUIRE(guarded.size() == 1);
  CHECK(guarded[0].count == kTokens);
  CHECK(guarded[0].amount == 1);
  // Only one: the next gain is ours and counts.
  CHECK(entries(w.frame(store)).empty());
  store.tokens(6);
  CHECK(has(entries(w.frame(store)), token_delta(1)));
  CHECK(store.tokens() == 6);
}

TEST_CASE("doubled chest: unused, it lapses, and a drop is never taken back") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  w.frame(store);
  w.expect_doubled_gain();
  store.tokens(4);  // spent one: sent as ever
  CHECK(has(entries(w.frame(store)), token_delta(-1)));
  for (std::uint64_t f = 0; f <= FlagWatcher::kDoubledGainFrames; ++f) w.frame(store);
  store.tokens(5);  // a gain after the window: counts
  CHECK(has(entries(w.frame(store)), token_delta(1)));
  CHECK(store.tokens() == 5);
  CHECK(w.stats().doubled_gains == 0);
}

TEST_CASE("doubled chest: the host takes it back too and sends no total") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::host);
  w.frame(store);
  w.expect_doubled_gain();
  store.tokens(6);
  CHECK_FALSE(has(entries(w.frame(store)), token_total(6)));
  CHECK(store.tokens() == 5);
  CHECK(w.stats().doubled_gains == 1);
}

TEST_CASE("doubled chest: a guarded count (tokens, never in a chest) is left to its guard") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  guard(w);
  w.frame(store);
  w.expect_doubled_gain();
  store.set(kSkull, 1);  // a token taken here: its own key and the count
  store.tokens(6);
  CHECK(has(entries(w.frame(store)), token_delta(1)));
  CHECK(store.tokens() == 6);
  CHECK(w.stats().doubled_gains == 0);
}

TEST_CASE("doubled chest: once its get scene ends, a key counts again") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  w.frame(store);
  w.expect_doubled_gain();
  w.frame(store);
  w.end_doubled_gain();  // a map chest: no count in its scene
  store.tokens(6);       // a real key just after
  CHECK(has(entries(w.frame(store)), token_delta(1)));
  CHECK(store.tokens() == 6);
  CHECK(w.stats().doubled_gains == 0);
}

TEST_CASE("flag watcher, host: a busy count delta is added later, summed, and the other answers still go") {
  TokenStore store = token_game();  // 5 tokens
  FlagWatcher w(token_shared(), FlagRole::host);
  w.frame(store);
  store.no_room.insert(kTokens);
  CHECK(w.receive(proto::FlagSet{2, {entry(kChest, 1), token_delta(2)}}));
  // The Bool's answer goes out; no delta in it (a FLAG_STATE cannot carry one).
  CHECK(entries(w.frame(store)) == std::vector{entry(kChest, 1)});
  CHECK(w.receive(proto::FlagSet{3, {token_delta(3)}}));
  CHECK(w.frame(store).empty());
  CHECK(store.tokens() == 5);
  store.no_room.clear();
  // Both gains, added once, and the new total stated.
  CHECK(entries(w.frame(store)) == std::vector{token_total(10)});
  CHECK(store.tokens() == 10);
  CHECK(w.frame(store).empty());
  CHECK(w.stats().entries_unsent == 0);
}

// Boss cutscenes (one heart container for all): A (the host here) takes the
// container, 12 -> 16; B's game then picks up the same container, still on
// its screen, before it went away there. B's rise past the cap is put back
// and never sent: the host and A stay at 16, B too.
TEST_CASE("flag watcher: a second pickup of a boss's heart container never raises the group (cap_max)") {
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};
  FakeStore host_game, player_game;
  host_game.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  player_game.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  ItemFlagStore host_store(host_game), player_store(player_game);
  FlagWatcher h(SharedFlags({kMaxLife}), FlagRole::host);
  FlagWatcher p(SharedFlags({kMaxLife}), FlagRole::player);
  h.frame(host_store);
  p.frame(player_store);
  auto exchange = [&](int frames) {
    for (int i = 0; i < frames; ++i) {
      for (const auto& o : p.frame(player_store)) CHECK(h.receive(o.bytes));
      for (const auto& o : h.frame(host_store)) CHECK(p.receive(o.bytes));
    }
  };
  // Both games cap at "the max when the boss died + one container".
  h.cap_max(kMaxLife, 16);
  p.cap_max(kMaxLife, 16);
  SUBCASE("B's pickup after A's value came") {
    host_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);  // A's pickup
    exchange(3);
    CHECK(player_game.get(kMaxLife) == 16u);
    player_game.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);  // B's late pickup
    exchange(5);
  }
  SUBCASE("B's pickup before A's value came") {
    host_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);
    player_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);  // the race: both +1
    exchange(1);
    player_game.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);  // and once more
    exchange(5);
  }
  CHECK(host_game.get(kMaxLife) == 16u);
  CHECK(player_game.get(kMaxLife) == 16u);
  CHECK(p.stats().max_caps_kept == 1);
  // Without the cap (we left the boss's area) a rise of our own goes out again.
  p.clear_max_cap(kMaxLife);
  player_game.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);
  exchange(3);
  CHECK(host_game.get(kMaxLife) == 20u);
}

TEST_CASE("flag watcher: a capped rise whose put-back fails is still never sent, and is tried again") {
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};
  FakeStore host_game, player_game;
  host_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);
  player_game.values[kMaxLife] = entry(kMaxLife, 16, proto::FlagKind::int32);
  ItemFlagStore host_store(host_game), player_store(player_game);
  FlagWatcher h(SharedFlags({kMaxLife}), FlagRole::host);
  FlagWatcher p(SharedFlags({kMaxLife}), FlagRole::player);
  h.frame(host_store);
  p.frame(player_store);
  p.cap_max(kMaxLife, 16);
  player_game.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);  // a second pickup
  player_game.unwritable.insert(kMaxLife);                                     // and the game takes no write
  for (int i = 0; i < 3; ++i) {
    for (const auto& o : p.frame(player_store)) CHECK(h.receive(o.bytes));
    for (const auto& o : h.frame(host_store)) CHECK(p.receive(o.bytes));
  }
  CHECK(host_game.get(kMaxLife) == 16u);
  CHECK(p.stats().write_failures >= 3);
  CHECK(p.stats().max_caps_kept == 0);
  // The game takes writes again: put back on the next frame, still nothing sent.
  player_game.unwritable.clear();
  CHECK(p.frame(player_store).empty());
  CHECK(player_game.get(kMaxLife) == 16u);
  CHECK(p.stats().max_caps_kept == 1);
  CHECK(host_game.get(kMaxLife) == 16u);
}

// map-get review (822e9828): a rise past the cap from below it is put back to the CAP, and that put-back
// value is what the watcher has seen: the next frame sends nothing (seeing the old value made it send
// the rise to the cap).
TEST_CASE("flag watcher: a capped rise from below the cap puts the cap back and sends nothing after") {
  constexpr FlagKey kMaxLife{0xfbe01da1u, 0};
  FakeStore game;
  game.values[kMaxLife] = entry(kMaxLife, 12, proto::FlagKind::int32);
  ItemFlagStore store(game);
  FlagWatcher p(SharedFlags({kMaxLife}), FlagRole::player);
  p.frame(store);
  p.cap_max(kMaxLife, 16);
  game.values[kMaxLife] = entry(kMaxLife, 20, proto::FlagKind::int32);  // two containers at once
  CHECK(entries(p.frame(store)).empty());
  CHECK(game.get(kMaxLife) == 16u);
  CHECK(p.stats().max_caps_kept == 1);
  for (int i = 0; i < 3; ++i) CHECK(entries(p.frame(store)).empty());  // the cap is not a rise of ours
  CHECK(p.stats().max_caps_kept == 1);
}

// map-get review (822e9828): after a doubled chest's gain is put back, the watcher has seen the put-back
// value, so the frames after send nothing and take nothing back again.
TEST_CASE("doubled chest: after the put-back, the next frames send nothing") {
  TokenStore store = token_game();
  FlagWatcher w(token_shared(), FlagRole::player);
  w.frame(store);
  w.expect_doubled_gain();
  store.tokens(7);  // +2 in one frame
  CHECK(entries(w.frame(store)).empty());
  CHECK(store.tokens() == 5);
  for (int i = 0; i < 3; ++i) CHECK(entries(w.frame(store)).empty());
  CHECK(w.stats().doubled_gains == 1);
  CHECK(store.tokens() == 5);
}

namespace {

// Every FLAG_SET and FLAG_STATE entry the next `frames` frames send.
std::vector<proto::FlagEntry> sent(FlagWatcher& w, FakeStore& store, int frames) {
  std::vector<proto::FlagEntry> out;
  for (int i = 0; i < frames; ++i) {
    for (const auto& m : decode(w.frame(store))) {
      if (const auto* s = std::get_if<proto::FlagSet>(&m)) out.insert(out.end(), s->flags.begin(), s->flags.end());
      if (const auto* s = std::get_if<proto::FlagState>(&m)) out.insert(out.end(), s->flags.begin(), s->flags.end());
    }
  }
  return out;
}

}  // namespace

TEST_CASE("flag watcher: a down-only Bool at 0 here takes no 1 from the group, and our 0 goes out (issue 84)") {
  // Mido's blocker: the game only ever lowers it. A group's 1 (host's state,
  // its save load, a join snapshot) must not shut the path again here.
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  CHECK(w.share_down_only(kDoor));
  CHECK_FALSE(w.share_down_only(kDoor));
  w.frame(store);

  CHECK(w.receive(state({entry(kDoor, 1), entry(kChest, 1)})));
  auto out = sent(w, store, 3);
  CHECK(store.get(kDoor) == 0);
  CHECK(store.get(kChest) == 1);  // other flags as before
  CHECK(has(out, entry(kDoor, 0)));
  CHECK(w.stats().down_only_kept >= 1);

  CHECK(w.receive(proto::Message{proto::FlagReset{1, {entry(kDoor, 1)}}}));
  sent(w, store, 3);
  CHECK(store.get(kDoor) == 0);

  w.receive(proto::FlagSnapshot{0, 1, {entry(kDoor, 1)}});
  sent(w, store, 3);
  CHECK(store.get(kDoor) == 0);
}

TEST_CASE("flag watcher: a down-only Bool still goes down from the group (issue 84)") {
  FakeStore store = game();
  store.set(kDoor, 1);
  FlagWatcher w(shared(), FlagRole::player);
  w.share_down_only(kDoor);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 0)})));
  sent(w, store, 2);
  CHECK(store.get(kDoor) == 0);
  CHECK(w.stats().down_only_kept == 0);
}

TEST_CASE("flag watcher: a host at 0 answers a player's 1 of a down-only Bool with 0 (issue 84)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.share_down_only(kDoor);
  w.frame(store);
  CHECK(w.receive(proto::FlagSet{2, {entry(kDoor, 1)}}));
  const auto out = sent(w, store, 2);
  CHECK(store.get(kDoor) == 0);
  CHECK(has(out, entry(kDoor, 0)));
  CHECK_FALSE(has(out, entry(kDoor, 1)));
}

TEST_CASE("flag watcher: an up-only Bool at 1 here takes no 0 from the group, and our 1 goes out (issue 84)") {
  // The Deku Tree's mouth: the game only ever opens it. A group's 0 (host's
  // state, its save load, a join snapshot) must not shut it again here.
  FakeStore store = game();
  store.set(kDoor, 1);
  FlagWatcher w(shared(), FlagRole::player);
  CHECK(w.share_up_only(kDoor));
  CHECK_FALSE(w.share_up_only(kDoor));
  CHECK_FALSE(w.share_down_only(kDoor));  // one way only
  w.frame(store);

  CHECK(w.receive(state({entry(kDoor, 0), entry(kChest, 1)})));
  auto out = sent(w, store, 3);
  CHECK(store.get(kDoor) == 1);
  CHECK(store.get(kChest) == 1);  // other flags as before
  CHECK(has(out, entry(kDoor, 1)));
  CHECK(w.stats().up_only_kept >= 1);
  CHECK(w.stats().down_only_kept == 0);

  CHECK(w.receive(proto::Message{proto::FlagReset{1, {entry(kDoor, 0)}}}));
  sent(w, store, 3);
  CHECK(store.get(kDoor) == 1);

  w.receive(proto::FlagSnapshot{0, 1, {entry(kDoor, 0)}});
  sent(w, store, 3);
  CHECK(store.get(kDoor) == 1);
}

TEST_CASE("flag watcher: an up-only Bool still goes up from the group (issue 84)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.share_up_only(kDoor);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  sent(w, store, 2);
  CHECK(store.get(kDoor) == 1);
  CHECK(w.stats().up_only_kept == 0);
}

TEST_CASE("flag watcher: an up-only gate from the host's save load reaches a guest at the title and stays through its own load (issue 61)") {
  // The final 0.11.2 smoke's order (quest-flags, 20261115-143129): the guest joins at the
  // title (the join snapshot has the host's title 0), the host loads a save with Zora's
  // waterfall open (FLAG_RESET 1), then the guest loads its own save, which has it shut.
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  REQUIRE(w.share_up_only(kDoor));
  w.set_loading(true);
  w.set_scene(false);
  w.frame(store);
  CHECK(w.receive(proto::FlagSnapshot{0, 1, {entry(kDoor, 0), entry(kChest, 0), entry(kKey0, 0)}}));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 0);

  CHECK(w.receive(proto::Message{proto::FlagReset{1, {entry(kDoor, 1), entry(kChest, 1), entry(kKey0, 1)}}}));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) CHECK(w.frame(store).empty());

  // The guest's own save (a test's own-save guest): none of the three.
  store.set(kDoor, 0);
  store.set(kChest, 0);
  store.set(kKey0, 0);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);  // the group's 1 back at once
  w.set_scene(true);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);
  CHECK(store.get(kChest) == 1);
  CHECK(w.stats().up_only_kept == 0);
  // Later frames leave it open and send nothing.
  for (int i = 0; i < 10; ++i) CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);
}

TEST_CASE("flag watcher: an up-only gate the guest's load alone shuts again is put back (issue 61)") {
  // Only the gate goes back to 0 (one flag back: no sign of a save load): the
  // load's older value is still not the group's.
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.share_up_only(kDoor);
  w.frame(store);
  CHECK(w.receive(state({entry(kDoor, 1)})));
  w.frame(store);
  REQUIRE(store.get(kDoor) == 1);
  w.set_loading(true);
  w.set_scene(false);
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  store.set(kDoor, 0);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);
  w.set_scene(true);
  w.set_loading(false);
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);
}

TEST_CASE("flag watcher, host: an up-only gate goes out live and in the host's save-load reset (issue 61)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.share_up_only(kDoor);
  w.frame(store);
  // Zelda's Lullaby at the falls: the game sets the gate.
  store.set(kDoor, 1);
  CHECK(only<proto::FlagState>(w.frame(store)) == std::vector{entry(kDoor, 1)});

  // Back to the title, then a save with the gate open loads (the title's
  // defaults had it 0; two other flags going back are the sign of a load).
  store.set(kDoor, 0);
  store.set(kChest, 1);
  store.set(kKey0, 1);
  w.frame(store);
  w.set_loading(true);
  w.set_scene(false);
  for (int i = 0; i < FlagWatcher::kTitleFrames; ++i) w.frame(store);
  store.set(kChest, 0);
  store.set(kKey0, 0);
  store.set(kDoor, 1);
  CHECK(w.frame(store).empty());
  w.set_scene(true);
  w.set_loading(false);
  std::vector<proto::FlagEntry> reset;
  for (int i = 0; i <= FlagWatcher::kSettleFrames; ++i) {
    for (const auto& m : decode(w.frame(store))) {
      if (const auto* r = std::get_if<proto::FlagReset>(&m)) reset.insert(reset.end(), r->flags.begin(), r->flags.end());
    }
  }
  CHECK(std::find(reset.begin(), reset.end(), entry(kDoor, 1)) != reset.end());
  CHECK(w.stats().host_resets == 1);
}

TEST_CASE("flag watcher: a host at 1 answers a player's 0 of an up-only Bool with 1 (issue 84)") {
  FakeStore store = game();
  store.set(kDoor, 1);
  FlagWatcher w(shared(), FlagRole::host);
  w.share_up_only(kDoor);
  w.frame(store);
  CHECK(w.receive(proto::FlagSet{2, {entry(kDoor, 0)}}));
  const auto out = sent(w, store, 2);
  CHECK(store.get(kDoor) == 1);
  CHECK(has(out, entry(kDoor, 1)));
  CHECK_FALSE(has(out, entry(kDoor, 0)));
}

TEST_CASE("flag watcher: a down-only Bool cannot also be up-only (issue 84)") {
  FlagWatcher w(shared(), FlagRole::player);
  CHECK(w.share_down_only(kDoor));
  CHECK_FALSE(w.share_up_only(kDoor));
}

namespace {

// One dungeon's rows of the item list (docs/items.md, "Dungeon items").
struct DungeonRow {
  std::string name;  // "DungeonInfo.ForestTemple"
  FlagKey keys, boss_key, map, compass;
};

std::vector<DungeonRow> dungeon_rows() {
  constexpr std::string_view kStart = "DungeonInfo.", kKeys = ".SmallKeyNum";
  const auto row = [](const std::string& name) {
    for (const ItemFlag& item : item_flags()) {
      if (item.name == name) return FlagKey{item.hash, 0};
    }
    FAIL("no item " << name);
    return FlagKey{0u, 0};
  };
  std::vector<DungeonRow> rows;
  for (const ItemFlag& item : item_flags()) {
    if (!item.name.starts_with(kStart) || !item.name.ends_with(kKeys)) continue;
    const std::string d(item.name.substr(0, item.name.size() - kKeys.size()));
    rows.push_back({d, FlagKey{item.hash, 0}, row(d + ".IsGetBossKey"), row(d + ".IsGetMap"), row(d + ".IsGetCompass")});
  }
  return rows;
}

// A host's game and a guest's, each with the item rules, in one session.
struct TwoGames {
  FakeStore host_game, guest_game;
  ItemFlagStore host_store{host_game}, guest_store{guest_game};
  FlagWatcher host, guest;
  DungeonRow r;

  explicit TwoGames(const DungeonRow& row)
      : host(SharedFlags({row.keys, row.boss_key, row.map, row.compass}), FlagRole::host),
        guest(SharedFlags({row.keys, row.boss_key, row.map, row.compass}), FlagRole::player),
        r(row) {
    for (FakeStore* s : {&host_game, &guest_game}) {
      s->values[r.keys] = entry(r.keys, 0, proto::FlagKind::int32);
      for (FlagKey k : {r.boss_key, r.map, r.compass}) s->set(k, 0);
    }
    host.set_native_loads(true);
    guest.set_native_loads(true);
    host.frame(host_store);
    guest.frame(guest_store);
  }
  void keys(FakeStore& game, int n) {
    game.values[r.keys] = entry(r.keys, static_cast<std::uint32_t>(n), proto::FlagKind::int32);
  }
  int keys(const FakeStore& game) const { return static_cast<int>(game.get(r.keys)); }
  // Frames of both games, every message handed to the other one.
  void run(int frames = 4) {
    for (int i = 0; i < frames; ++i) {
      for (const auto& o : guest.frame(guest_store)) CHECK(host.receive(o.bytes));
      for (const auto& o : host.frame(host_store)) CHECK(guest.receive(o.bytes));
    }
  }
};

}  // namespace

TEST_CASE("dungeon items: every dungeon's rows are shared: keys as a count, boss key, map and compass owned once") {
  const auto rows = dungeon_rows();
  CHECK(rows.size() == 14);
  for (const DungeonRow& r : rows) {
    CAPTURE(r.name);
    const ItemFlag* keys = find_item(r.keys.hash);
    REQUIRE(keys != nullptr);
    CHECK(keys->on);
    CHECK(keys->rule == ItemRule::count);
    CHECK(keys->kind == proto::FlagKind::int32);
    for (FlagKey k : {r.boss_key, r.map, r.compass}) {
      const ItemFlag* item = find_item(k.hash);
      REQUIRE(item != nullptr);
      CHECK(item->on);
      CHECK(item->rule == ItemRule::any);
      CHECK(item->kind == proto::FlagKind::boolean);
    }
  }
}

TEST_CASE("dungeon items: a key found or used in one game is found or used in every game, in every dungeon (row 106)") {
  for (const DungeonRow& r : dungeon_rows()) {
    CAPTURE(r.name);
    TwoGames t(r);
    t.keys(t.guest_game, 1);  // the guest opens a key chest
    t.run();
    CHECK(t.keys(t.host_game) == 1);
    CHECK(t.keys(t.guest_game) == 1);
    t.keys(t.host_game, 3);  // the host finds two
    t.run();
    CHECK(t.keys(t.guest_game) == 3);
    t.keys(t.guest_game, 2);  // the guest uses one on a door
    t.run();
    CHECK(t.keys(t.host_game) == 2);
    t.keys(t.host_game, 1);  // the host uses one
    t.run();
    CHECK(t.keys(t.guest_game) == 1);
    // In the same frame: the guest finds one, the host uses one. One group count, no key lost or made.
    t.keys(t.guest_game, 2);
    t.keys(t.host_game, 0);
    t.run();
    CHECK(t.keys(t.host_game) == 1);
    CHECK(t.keys(t.guest_game) == 1);
    // The boss key, map and compass: owned by all once one game has them, either way.
    t.guest_game.set(r.boss_key, 1);
    t.host_game.set(r.map, 1);
    t.guest_game.set(r.compass, 1);
    t.run();
    for (FlagKey k : {r.boss_key, r.map, r.compass}) {
      CHECK(t.host_game.get(k) == 1);
      CHECK(t.guest_game.get(k) == 1);
    }
    CHECK(t.keys(t.host_game) == 1);  // nothing of it touched the count
    CHECK(t.keys(t.guest_game) == 1);
  }
}

TEST_CASE("dungeon items: a guest's own save load takes the group's keys and dungeon items, then its play counts again (rows 106/107)") {
  for (const DungeonRow& r : dungeon_rows()) {
    CAPTURE(r.name);
    TwoGames t(r);
    t.keys(t.host_game, 2);
    t.host_game.set(r.boss_key, 1);
    t.host_game.set(r.map, 1);
    t.host_game.set(r.compass, 1);
    t.run();
    REQUIRE(t.keys(t.guest_game) == 2);
    REQUIRE(t.guest_game.get(r.boss_key) == 1);
    // The guest loads a file of its own: other key count, no dungeon items (the game's load nodes say so).
    t.guest.note_save_load_start();
    t.keys(t.guest_game, 5);
    for (FlagKey k : {r.boss_key, r.map, r.compass}) t.guest_game.set(k, 0);
    t.run(1);
    t.guest.note_save_loaded();
    t.run(FlagWatcher::kSettleFrames + 4);
    CHECK_FALSE(t.guest.settling());
    CHECK(t.keys(t.guest_game) == 2);  // the group's count, not the file's
    CHECK(t.keys(t.host_game) == 2);   // and the file's never reached the group
    for (FlagKey k : {r.boss_key, r.map, r.compass}) {
      CHECK(t.guest_game.get(k) == 1);
      CHECK(t.host_game.get(k) == 1);
    }
    // After the load the guest's play counts as before: a key found, then used.
    t.keys(t.guest_game, 3);
    t.run();
    CHECK(t.keys(t.host_game) == 3);
    t.keys(t.guest_game, 2);
    t.run();
    CHECK(t.keys(t.host_game) == 2);
    CHECK(t.keys(t.guest_game) == 2);
  }
}

TEST_CASE("dungeon items: the host's save load gives every game that save's keys and dungeon items, lower too (row 107)") {
  for (const DungeonRow& r : dungeon_rows()) {
    CAPTURE(r.name);
    TwoGames t(r);
    t.keys(t.host_game, 3);
    for (FlagKey k : {r.boss_key, r.map, r.compass}) t.host_game.set(k, 1);
    t.run();
    REQUIRE(t.keys(t.guest_game) == 3);
    REQUIRE(t.guest_game.get(r.compass) == 1);
    // The host loads another file: one key, the map only.
    t.host.note_save_load_start();
    t.keys(t.host_game, 1);
    t.host_game.set(r.boss_key, 0);
    t.host_game.set(r.compass, 0);
    t.run(1);
    t.host.note_save_loaded();
    t.run(FlagWatcher::kSettleFrames + 4);
    CHECK_FALSE(t.host.settling());
    CHECK(t.host.stats().host_resets == 1);
    CHECK(t.keys(t.host_game) == 1);  // the host's file is the truth: nothing of the old one put back
    CHECK(t.host_game.get(r.boss_key) == 0);
    CHECK(t.host_game.get(r.compass) == 0);
    CHECK(t.keys(t.guest_game) == 1);
    CHECK(t.guest_game.get(r.boss_key) == 0);
    CHECK(t.guest_game.get(r.compass) == 0);
    CHECK(t.guest_game.get(r.map) == 1);
    // Play goes on from that file in both games.
    t.keys(t.guest_game, 0);  // the guest uses the key
    t.guest_game.set(r.boss_key, 1);
    t.run();
    CHECK(t.keys(t.host_game) == 0);
    CHECK(t.host_game.get(r.boss_key) == 1);
  }
}

// Rows 106/107 (dungeon-items-next): a join snapshot whose last part never comes held a guest's game for as long
// as the connection lasted: none of its own changes sent, no FLAG_STATE written, nothing logged.
TEST_CASE("flag watcher: a join snapshot whose last part never comes is dropped after kHoldMaxFrames") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  w.receive(proto::FlagSnapshot{0, 2, {entry(kChest, 1)}});
  CHECK(w.frame(store).empty());
  CHECK(w.holding());
  store.set(kKey0, 1);              // our own change while held
  w.receive(state({entry(kDoor, 1)}));  // another game's change while held
  for (std::uint64_t i = 1; i < FlagWatcher::kHoldMaxFrames; ++i) {
    CHECK(w.frame(store).empty());
  }
  CHECK(w.holding());
  CHECK(w.stats().holding_frames >= FlagWatcher::kHoldMaxFrames - 1);
  CHECK(w.holding_for() >= FlagWatcher::kHoldMaxFrames - 1);
  CHECK(store.get(kDoor) == 0);     // held, not written
  CHECK(w.stats().hold_timeouts == 0);

  // Past the limit: dropped, the state written, our change sent; the unfinished snapshot is not written.
  std::vector<Outgoing> out;
  for (int i = 0; i < 3 && out.empty(); ++i) out = w.frame(store);
  CHECK_FALSE(w.holding());
  CHECK(w.holding_for() == 0);
  CHECK(w.stats().hold_timeouts == 1);
  CHECK(w.stats().bad_snapshots == 1);
  CHECK(w.take_hold_timeout());
  CHECK_FALSE(w.take_hold_timeout());
  CHECK(store.get(kDoor) == 1);
  CHECK(store.get(kChest) == 0);
  CHECK(only<proto::FlagSet>(out) == std::vector{entry(kKey0, 1)});

  // A whole snapshot after it is taken as usual.
  w.receive(proto::FlagSnapshot{0, 1, {entry(kChest, 1)}});
  w.frame(store);
  CHECK_FALSE(w.holding());
  CHECK(store.get(kChest) == 1);
  CHECK(w.stats().hold_timeouts == 1);
}

TEST_CASE("flag watcher: a snapshot that finishes in time is not dropped") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  w.receive(proto::FlagSnapshot{0, 2, {entry(kChest, 1)}});
  for (std::uint64_t i = 0; i + 2 < FlagWatcher::kHoldMaxFrames; ++i) w.frame(store);
  CHECK(w.holding());
  w.receive(proto::FlagSnapshot{1, 2, {entry(kDoor, 1)}});
  w.frame(store);
  CHECK_FALSE(w.holding());
  CHECK(store.get(kChest) == 1);
  CHECK(store.get(kDoor) == 1);
  CHECK(w.stats().hold_timeouts == 0);
  CHECK(w.stats().bad_snapshots == 0);
  CHECK_FALSE(w.take_hold_timeout());
}

TEST_CASE("flag watcher: more FLAG_STATEs than kHeldStatesMax while holding drop the snapshot at once") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  w.receive(proto::FlagSnapshot{0, 2, {}});
  w.frame(store);
  for (std::size_t i = 0; i < FlagWatcher::kHeldStatesMax; ++i) {
    w.receive(state({entry(kDoor, static_cast<std::uint32_t>(i % 2))}));
  }
  w.frame(store);
  CHECK(w.holding());  // full, not past it
  CHECK(store.get(kDoor) == 0);
  w.receive(state({entry(kDoor, 1)}));
  w.frame(store);
  CHECK_FALSE(w.holding());
  CHECK(w.stats().hold_timeouts == 1);
  CHECK(store.get(kDoor) == 1);  // every held state, in order, then the newest
}

TEST_CASE("flag watcher: time in a load window is counted, and how long the open one has lasted") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.window_for() == 0);
  w.set_loading(true);
  for (int i = 0; i < 10; ++i) w.frame(store);
  CHECK(w.window_for() >= 9);
  CHECK(w.stats().window_frames >= 10);
  w.set_loading(false);
  for (int i = 0; i < FlagWatcher::kSettleFrames + 4; ++i) w.frame(store);
  CHECK(w.window_for() == 0);
}

TEST_CASE("dungeon items: a guest held by an unfinished join snapshot sends its key after the limit, every dungeon (rows 106/107)") {
  for (const DungeonRow& r : dungeon_rows()) {
    CAPTURE(r.name);
    TwoGames t(r);
    t.keys(t.host_game, 2);
    t.run();
    REQUIRE(t.keys(t.guest_game) == 2);
    t.guest.receive(proto::FlagSnapshot{0, 2, {}});
    t.keys(t.guest_game, 3);   // the guest opens a key chest while held
    t.keys(t.host_game, 1);    // the host uses a key meanwhile
    t.run(2);
    CHECK(t.keys(t.host_game) == 1);   // nothing of the guest's arrived
    CHECK(t.guest.holding());
    t.run(static_cast<int>(FlagWatcher::kHoldMaxFrames) + 4);
    CHECK_FALSE(t.guest.holding());
    // One group count: 2, the guest's +1 and the host's -1, in both games.
    CHECK(t.keys(t.host_game) == 2);
    CHECK(t.keys(t.guest_game) == 2);
    CHECK(t.guest.stats().hold_timeouts == 1);
  }
}
