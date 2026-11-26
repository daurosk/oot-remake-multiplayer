// SPDX-License-Identifier: GPL-3.0-or-later
// LentKeys (dungeon-sync-b, player report #75): a key lent at a lock another
// player opened stays in our game; the group never sees it or its spend.
#include <doctest/doctest.h>

#include <map>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/lent_keys.hpp"
#include "coop/protocol/codec.hpp"

namespace {
using namespace coop::host;
namespace proto = coop::protocol;

constexpr FlagKey kKeys{0x877bdceeu, 0};  // DungeonInfo.WaterTemple.SmallKeyNum

// The game's small key count, a count as ItemFlagStore makes it.
class KeyStore : public FlagStore {
 public:
  std::int32_t keys = 1;
  int writes = 0;
  std::optional<proto::FlagEntry> read(FlagKey key) override {
    if (key != kKeys) return std::nullopt;
    return proto::FlagEntry{kKeys, proto::FlagKind::int32, static_cast<std::uint32_t>(keys)};
  }
  bool write(const proto::FlagEntry& e) override {
    if (e.key != kKeys) return false;
    ++writes;
    if (e.kind == proto::FlagKind::count_delta) {
      keys = std::max(keys + static_cast<std::int32_t>(e.value), 0);
    } else {
      keys = static_cast<std::int32_t>(e.value);
    }
    return true;
  }
  bool is_count(FlagKey key) override { return key == kKeys; }
};

// Every entry the watcher sent in these frames.
std::vector<proto::FlagEntry> sent(const std::vector<Outgoing>& out) {
  std::vector<proto::FlagEntry> all;
  for (const auto& o : out) {
    proto::Message m;
    REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
    if (const auto* s = std::get_if<proto::FlagSet>(&m)) all.insert(all.end(), s->flags.begin(), s->flags.end());
    if (const auto* s = std::get_if<proto::FlagState>(&m)) all.insert(all.end(), s->flags.begin(), s->flags.end());
  }
  return all;
}

// One frame as the hook runs it: the lent keys' writes, then the watcher on the view.
std::vector<proto::FlagEntry> frame(LentKeys& lent, KeyStore& game, FlagWatcher& w) {
  lent.frame(game);
  LentKeyStore view(game, lent);
  return sent(w.frame(view));
}

proto::FlagEntry delta(std::int32_t n) {
  return {kKeys, proto::FlagKind::count_delta, static_cast<std::uint32_t>(n)};
}
proto::FlagEntry total(std::int32_t n) {
  return {kKeys, proto::FlagKind::int32, static_cast<std::uint32_t>(n)};
}

}  // namespace

TEST_CASE("lent keys: a lent key and our spend of it never reach the group") {
  for (const FlagRole role : {FlagRole::player, FlagRole::host}) {
    KeyStore game;
    game.keys = 0;  // the opener spent the group's only key
    LentKeys lent;
    FlagWatcher w(SharedFlags({kKeys}), role);
    CHECK(frame(lent, game, w).empty());  // the baseline
    lent.lend(kKeys);
    CHECK(frame(lent, game, w).empty());
    CHECK(game.keys == 1);  // our player can open the lock
    CHECK(lent.lent(kKeys) == 1);
    CHECK(frame(lent, game, w).empty());
    // Our player opens the lock: the game spends the key.
    game.keys = 0;
    CHECK(frame(lent, game, w).empty());
    CHECK(lent.lent(kKeys) == 0);
    CHECK(lent.take_spent() == std::vector<FlagKey>{kKeys});
    CHECK(lent.take_spent().empty());
    CHECK(frame(lent, game, w).empty());
  }
}

TEST_CASE("lent keys: a lent key not used is taken back, unseen, and is no spend") {
  KeyStore game;
  LentKeys lent;
  FlagWatcher w(SharedFlags({kKeys}), FlagRole::player);
  frame(lent, game, w);
  lent.lend(kKeys);
  CHECK(frame(lent, game, w).empty());
  CHECK(game.keys == 2);
  lent.give_back(kKeys);
  CHECK(frame(lent, game, w).empty());
  CHECK(game.keys == 1);
  CHECK(lent.lent(kKeys) == 0);
  CHECK(lent.take_spent().empty());
  // Asked back after it was spent: nothing more is taken.
  lent.lend(kKeys);
  frame(lent, game, w);
  game.keys = 1;  // spent
  CHECK(frame(lent, game, w).empty());
  lent.give_back(kKeys);
  CHECK(frame(lent, game, w).empty());
  CHECK(game.keys == 1);
}

TEST_CASE("lent keys: a key found while one is lent is the group's, and the lent one is spent first") {
  KeyStore game;
  game.keys = 0;
  LentKeys lent;
  FlagWatcher w(SharedFlags({kKeys}), FlagRole::player);
  frame(lent, game, w);
  lent.lend(kKeys);
  frame(lent, game, w);
  game.keys = 2;  // a key from a chest
  CHECK(frame(lent, game, w) == std::vector{delta(1)});
  game.keys = 1;  // the lock: the lent key
  CHECK(frame(lent, game, w).empty());
  CHECK(lent.take_spent().size() == 1);
  game.keys = 0;  // another door: the group's key
  CHECK(frame(lent, game, w) == std::vector{delta(-1)});
}

TEST_CASE("lent keys: the group's total or another player's change keeps our lent key") {
  KeyStore game;
  game.keys = 2;
  LentKeys lent;
  FlagWatcher w(SharedFlags({kKeys}), FlagRole::player);
  frame(lent, game, w);
  lent.lend(kKeys);
  frame(lent, game, w);
  CHECK(game.keys == 3);
  // Another player spent one: the host's total is 1.
  CHECK(w.receive(proto::FlagState{1, {total(1)}}));
  CHECK(frame(lent, game, w).empty());
  CHECK(game.keys == 2);  // the group's 1 and our lent key
  CHECK(lent.lent(kKeys) == 1);
  CHECK(lent.take_spent().empty());
  // A change added as a delta (a host adds a player's spend) is no spend of ours either.
  LentKeyStore view(game, lent);
  CHECK(view.write(delta(-1)));
  CHECK(game.keys == 1);
  CHECK(view.read(kKeys)->value == 0);
  CHECK(lent.lent(kKeys) == 1);
  CHECK(lent.take_spent().empty());
}

TEST_CASE("lent keys: a save load forgets them and writes nothing") {
  KeyStore game;
  LentKeys lent;
  lent.lend(kKeys);
  lent.frame(game);
  CHECK(game.keys == 2);
  const int writes = game.writes;
  lent.forget();
  CHECK(lent.frame(game).empty());
  CHECK(game.writes == writes);
  CHECK(lent.lent(kKeys) == 0);
  LentKeyStore view(game, lent);
  CHECK(view.read(kKeys)->value == 2);  // the save's value, as it is
}

TEST_CASE("lent keys: a count that cannot be read or written lends nothing") {
  KeyStore game;
  LentKeys lent;
  lent.lend(FlagKey{0x1234u, 0});
  const auto lines = lent.frame(game);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].find("not lent") != std::string::npos);
  CHECK(lent.lent(FlagKey{0x1234u, 0}) == 0);
}
