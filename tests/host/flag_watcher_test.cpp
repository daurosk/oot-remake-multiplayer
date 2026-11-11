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
  CHECK(w.take_one_way_restores() == std::vector<FlagKey>{kDekuMap});
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
