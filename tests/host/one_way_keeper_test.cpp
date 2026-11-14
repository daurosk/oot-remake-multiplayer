// SPDX-License-Identifier: GPL-3.0-or-later
// OneWayKeeper (minimap-b): a guard for a dungeon's map and compass with no session.
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/game_items.hpp"
#include "coop/host/one_way_keeper.hpp"

namespace {
using coop::host::FlagKey;
using coop::host::OneWayKeeper;
namespace proto = coop::protocol;

constexpr FlagKey kDekuMap{0x89787395u, 0};      // DungeonInfo.InsideTheDekuTree.IsGetMap
constexpr FlagKey kDekuCompass{0x06c51ff2u, 0};  // DungeonInfo.InsideTheDekuTree.IsGetCompass
constexpr FlagKey kGotBomb{0x2bb00768u, 0};      // IsGet.Item_Bomb: not one-way
constexpr std::string_view kDeku = "InsideTheDekuTree";

class Store : public coop::host::FlagStore {
 public:
  std::map<FlagKey, std::uint32_t> values;
  std::set<FlagKey> refused;
  int writes = 0;
  std::optional<proto::FlagEntry> read(FlagKey key) override {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return proto::FlagEntry(key, proto::FlagKind::boolean, it->second);
  }
  bool write(const proto::FlagEntry& e) override {
    if (refused.contains(e.key) || !values.contains(e.key)) return false;
    ++writes;
    values[e.key] = e.value;
    return true;
  }
};

Store dungeon(std::uint32_t map = 0) {
  Store s;
  s.values = {{kDekuMap, map}, {kDekuCompass, 0}, {kGotBomb, 0}};
  return s;
}

// Frames in play (Link read) in `scene`.
void play(OneWayKeeper& k, Store& s, int frames = 1, std::string_view scene = kDeku) {
  for (int i = 0; i < frames; ++i) k.frame(s, true, scene);
}
// Watching from a save without the map; the game gives it in play (the chest).
OneWayKeeper given_map(Store& s) {
  OneWayKeeper k({kDekuMap, kDekuCompass, kGotBomb});
  play(k, s);
  s.values[kDekuMap] = 1;
  play(k, s);
  return k;
}
}  // namespace

TEST_CASE("one-way keeper: the flags kept are the dungeons' IsGetMap and IsGetCompass, each with its dungeon") {
  const auto keys = coop::host::one_way_item_flags();
  CHECK(keys.size() >= 20);
  CHECK(std::find(keys.begin(), keys.end(), kDekuMap) != keys.end());
  CHECK(std::find(keys.begin(), keys.end(), kDekuCompass) != keys.end());
  CHECK(std::find(keys.begin(), keys.end(), kGotBomb) == keys.end());
  for (const FlagKey k : keys) {
    CHECK(coop::host::is_one_way_item(k));
    CHECK(coop::host::one_way_dungeon(k).has_value());
  }
  CHECK(coop::host::one_way_dungeon(kDekuMap) == std::string(kDeku));
  CHECK_FALSE(coop::host::one_way_dungeon(kGotBomb));
  CHECK_FALSE(coop::host::is_one_way_item(FlagKey::key64_flag(0x89787395u)));
}

TEST_CASE("one-way keeper: a map the game gave and then took back in its dungeon is written back, and only that") {
  Store s = dungeon();
  OneWayKeeper k = given_map(s);
  s.values[kGotBomb] = 1;
  play(k, s);
  s.values[kDekuMap] = 0;  // the game takes it back
  s.values[kGotBomb] = 0;  // not a map or compass: never kept
  CHECK(k.frame(s, true, kDeku) == std::vector<FlagKey>{kDekuMap});
  CHECK(s.values[kDekuMap] == 1);
  CHECK(s.values[kGotBomb] == 0);
  CHECK(s.values[kDekuCompass] == 0);  // never got: left at 0
  play(k, s, 5);
  CHECK(s.writes == 1);
  CHECK(k.stats().restores == 1);
}

TEST_CASE("one-way keeper: a map the save already had is not kept (the game did not give it while watched)") {
  Store s = dungeon(1);
  OneWayKeeper k({kDekuMap});
  play(k, s);
  s.values[kDekuMap] = 0;
  CHECK(k.frame(s, true, kDeku).empty());
  CHECK(s.values[kDekuMap] == 0);
  CHECK(s.writes == 0);
}

TEST_CASE("one-way keeper: only in the flag's own dungeon") {
  Store s = dungeon();
  OneWayKeeper k = given_map(s);
  s.values[kDekuMap] = 0;
  CHECK(k.frame(s, true, "KokiriForest").empty());  // taken back elsewhere: left
  play(k, s, 3);
  CHECK(s.values[kDekuMap] == 0);
  CHECK(k.stats().restores == 0);
}

TEST_CASE("one-way keeper: a drop across a room or scene load (the Map page, a new room) is put back after it") {
  Store s = dungeon();
  OneWayKeeper k = given_map(s);
  // Loading: Link not read, the scene still known; the game drops the map.
  s.values[kDekuMap] = 0;
  for (int i = 0; i < 200; ++i) CHECK(k.frame(s, false, kDeku).empty());
  CHECK(s.values[kDekuMap] == 0);  // nothing written while loading
  CHECK(k.frame(s, true, kDeku) == std::vector<FlagKey>{kDekuMap});
  CHECK(s.values[kDekuMap] == 1);
  // A short gap with no scene at all (a scene change): still no save load.
  play(k, s, OneWayKeeper::kWriteWait);
  s.values[kDekuMap] = 0;
  for (int i = 0; i < OneWayKeeper::kTitleFrames - 1; ++i) k.frame(s, false, std::nullopt);
  CHECK(k.frame(s, true, kDeku) == std::vector<FlagKey>{kDekuMap});
  CHECK(k.stats().save_loads == 0);
}

TEST_CASE("one-way keeper: a save load drops the baseline, an older save keeps no map") {
  Store s = dungeon();
  OneWayKeeper k = given_map(s);
  for (int i = 0; i < OneWayKeeper::kTitleFrames; ++i) k.frame(s, false, std::nullopt);  // the title, the file list
  s.values[kDekuMap] = 0;  // an older save
  for (int i = 0; i < 30; ++i) k.frame(s, false, kDeku);  // its scene loads
  CHECK(k.frame(s, true, kDeku).empty());
  CHECK(k.take_save_load());
  CHECK_FALSE(k.take_save_load());
  CHECK(k.stats().save_loads == 1);
  play(k, s, 5);
  CHECK(s.values[kDekuMap] == 0);
  // Given again in that save, then taken back: kept again.
  s.values[kDekuMap] = 1;
  play(k, s);
  s.values[kDekuMap] = 0;
  play(k, s);
  CHECK(s.values[kDekuMap] == 1);
}

TEST_CASE("one-way keeper: reset (a session ran) forgets the map, so an old save's map is never written into another") {
  Store s = dungeon();
  OneWayKeeper k = given_map(s);
  // A guest session ends mid-play: its writes and the host's save are not the game's own.
  k.reset();
  s.values[kDekuMap] = 0;  // the host's save had no map
  play(k, s, 5);
  CHECK(s.values[kDekuMap] == 0);
  CHECK(s.writes == 0);
  CHECK(k.stats().resets == 1);
  // Also when the map was 1 at the reset: the baseline, not a gift.
  s.values[kDekuMap] = 1;
  k.reset();
  play(k, s);
  s.values[kDekuMap] = 0;
  play(k, s, 3);
  CHECK(s.values[kDekuMap] == 0);
}

TEST_CASE("one-way keeper: a refused write is tried again, a write on its way is not a second drop") {
  Store s = dungeon();
  OneWayKeeper k = given_map(s);
  s.values[kDekuMap] = 0;
  s.refused.insert(kDekuMap);
  CHECK(k.frame(s, true, kDeku).empty());
  CHECK(k.stats().failures == 1);
  s.refused.clear();
  CHECK(k.frame(s, true, kDeku) == std::vector<FlagKey>{kDekuMap});
  // On Hookshot the setter runs later: the flag still reads 0 for a while.
  s.values[kDekuMap] = 0;
  for (int i = 0; i < OneWayKeeper::kWriteWait - 1; ++i) CHECK(k.frame(s, true, kDeku).empty());
  s.values[kDekuMap] = 1;  // landed
  play(k, s, 3);
  CHECK(k.stats().restores == 1);
  s.values.erase(kDekuCompass);  // not in this game's table: left alone
  play(k, s, 3);
  CHECK(k.stats().restores == 1);
}

TEST_CASE("one-way keeper: with a clock the title screen is real time, not presented frames") {
  Store s = dungeon();
  OneWayKeeper k({kDekuMap});
  std::uint64_t now = 0;
  k.set_clock([&now] { return now; });
  play(k, s);
  s.values[kDekuMap] = 1;
  now += 16'667;
  play(k, s);
  // 90 presents at 165 a second (0.55 s with no scene): no save load.
  for (int i = 0; i < 90; ++i) {
    now += 6'061;
    k.frame(s, false, std::nullopt);
  }
  s.values[kDekuMap] = 0;
  now += 6'061;
  CHECK(k.frame(s, true, kDeku) == std::vector<FlagKey>{kDekuMap});
  CHECK(k.stats().save_loads == 0);
}
