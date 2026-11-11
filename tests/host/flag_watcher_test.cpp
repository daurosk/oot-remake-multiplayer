#include <doctest/doctest.h>
#include <algorithm>
#include <cstddef>

#include <map>
#include <set>
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
TEST_CASE("flag watcher: a save load does not undo or send back what the group set (issue 129)") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::player);
  w.set_loading(true);  // the title screen: no Link yet
  w.frame(store);
  // The group's switch comes in before the save loads.
  CHECK(w.receive(state({entry(kDoor, 1)})));
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);

  // The save loads: its own 0 is written over it, and its own progress (the
  // chest) comes in with it.
  store.set(kDoor, 0);
  store.set(kChest, 1);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 1)});
  CHECK(store.get(kDoor) == 1);  // put back, not sent
  CHECK(w.stats().load_reverts == 1);

  // Link is there, but the load still runs a moment: the same again.
  w.set_loading(false);
  CHECK(w.settling());
  store.set(kDoor, 0);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);

  // A group value written during the load and lost to it comes back too.
  CHECK(w.receive(state({entry(kKey0, 1)})));
  w.frame(store);
  store.set(kKey0, 0);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kKey0) == 1);
  CHECK(w.stats().load_reverts == 3);

  // After the settle time the game's own changes go out as before, a switch
  // turned back off included.
  for (int i = 0; i < FlagWatcher::kSettleFrames; ++i) w.frame(store);
  CHECK_FALSE(w.settling());
  store.set(kDoor, 0);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kDoor, 0)});
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

  // A per-player flag the game refuses to be written: its load value is the
  // game's own, sent as any change.
  store.unwritable.insert(kChest);
  store.set(kChest, 0);
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kChest, 0)});
}

TEST_CASE("flag watcher, host: a host's own load does not send the group's switch back to 0") {
  FakeStore store = game();
  FlagWatcher w(shared(), FlagRole::host);
  w.frame(store);
  store.set(kDoor, 1);
  CHECK(only<proto::FlagState>(w.frame(store)) == std::vector{entry(kDoor, 1)});
  w.set_loading(true);
  store.set(kDoor, 0);
  CHECK(w.frame(store).empty());
  CHECK(store.get(kDoor) == 1);
}

TEST_CASE("flag watcher: during a load an Int goes out unless it is the old value coming back") {
  FakeStore store = game();
  store.values[kKey1] = entry(kKey1, 2, proto::FlagKind::int32);
  FlagWatcher w(shared(), FlagRole::player);
  w.frame(store);
  CHECK(w.receive(state({entry(kKey1, 3, proto::FlagKind::int32)})));
  w.frame(store);
  w.set_loading(true);
  // More progress than the group's (a save, or the game's own change): sent.
  store.values[kKey1] = entry(kKey1, 5, proto::FlagKind::int32);
  CHECK(only<proto::FlagSet>(w.frame(store)) ==
        std::vector{entry(kKey1, 5, proto::FlagKind::int32)});
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
  CHECK(only<proto::FlagSet>(w.frame(store)) == std::vector{entry(kDoor, 0)});
  CHECK(w.stats().load_reverts == 0);
}

TEST_CASE("flag watcher: a named Bool shared one way (a chest's opened flag) is noted and never closed again") {
  FakeStore store = game();
  FlagWatcher w(SharedFlags({kKey0}), FlagRole::player);
  CHECK(w.share_one_way(kChest));
  CHECK_FALSE(w.share_one_way(kChest));                          // once
  CHECK_FALSE(w.share_one_way(FlagKey::key64_flag(0x1234)));      // 64-bit keys: share_pickup
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
