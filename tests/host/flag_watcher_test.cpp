#include <doctest/doctest.h>

#include <map>
#include <set>
#include <variant>
#include <vector>

#include "coop/host/flag_watcher.hpp"
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
  return {key.hash, key.index, kind, value};
}

// The game's flags, as the emulator would give them.
class FakeStore : public FlagStore {
 public:
  std::map<FlagKey, proto::FlagEntry> values;
  std::set<FlagKey> unreadable;
  std::set<FlagKey> unwritable;
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
    if (unwritable.contains(key) || !values.contains(key)) return false;
    ++writes;
    values[key] = e;
    return true;
  }
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

TEST_CASE("flag watcher: more than 4096 changes are split into several messages") {
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
  CHECK(std::get<proto::FlagSet>(messages[0]).flags.size() == 4096);
  CHECK(std::get<proto::FlagSet>(messages[1]).flags.size() == 904);
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

TEST_CASE("flag watcher: a player ignores FLAG_SET, a host FLAG_STATE and snapshots") {
  FlagWatcher player(shared(), FlagRole::player);
  CHECK_FALSE(player.receive(proto::FlagSet{2, {entry(kChest, 1)}}));
  CHECK_FALSE(player.receive(proto::Message{proto::Chat{1, "hi"}}));
  const std::vector<std::uint8_t> junk{1, 2, 3};
  CHECK_FALSE(player.receive(junk));
  FlagWatcher host(shared(), FlagRole::host);
  CHECK_FALSE(host.receive(state({entry(kChest, 1)})));
  CHECK_FALSE(host.receive(proto::FlagSnapshot{0, 1, {}}));
  CHECK(host.stats().ignored_messages == 2);
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
  CHECK(std::get<proto::FlagSnapshot>(parts[2]).flags.size() == 9000 - 2 * 4096);
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
