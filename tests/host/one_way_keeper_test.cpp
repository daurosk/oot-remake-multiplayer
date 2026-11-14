// SPDX-License-Identifier: GPL-3.0-or-later
// OneWayKeeper (minimap-b): a dungeon's map and compass kept with no session.
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
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

// Frames in play (Link read, a scene loaded).
void play(OneWayKeeper& k, Store& s, int frames = 1) {
  for (int i = 0; i < frames; ++i) k.frame(s, true, true);
}
}  // namespace

TEST_CASE("one-way keeper: the map and compass flags are the dungeons' IsGetMap and IsGetCompass") {
  const auto keys = coop::host::one_way_item_flags();
  CHECK(keys.size() >= 20);
  CHECK(std::find(keys.begin(), keys.end(), kDekuMap) != keys.end());
  CHECK(std::find(keys.begin(), keys.end(), kDekuCompass) != keys.end());
  CHECK(std::find(keys.begin(), keys.end(), kGotBomb) == keys.end());
  for (const FlagKey k : keys) CHECK(coop::host::is_one_way_item(k));
  CHECK_FALSE(coop::host::is_one_way_item(kGotBomb));
  CHECK_FALSE(coop::host::is_one_way_item(FlagKey::key64_flag(0x89787395u)));
}

TEST_CASE("one-way keeper: a map got and then dropped in play is written back, and only that") {
  Store s = dungeon();
  OneWayKeeper k({kDekuMap, kDekuCompass, kGotBomb});
  play(k, s);
  s.values[kDekuMap] = 1;  // the chest
  play(k, s);
  s.values[kDekuMap] = 0;  // the game takes it back
  CHECK(k.frame(s, true, true) == std::vector<FlagKey>{kDekuMap});
  CHECK(s.values[kDekuMap] == 1);
  CHECK(k.stats().restores == 1);
  play(k, s, 5);
  CHECK(s.writes == 1);  // nothing else written while it stays
  CHECK(s.values[kDekuCompass] == 0);  // never got: left at 0
}

TEST_CASE("one-way keeper: a drop across a room or scene load (the Map page, a new room) is put back after it") {
  Store s = dungeon(1);  // the save has the map
  OneWayKeeper k({kDekuMap});
  play(k, s);
  // Loading: Link not read, the scene still known; the game drops the map.
  s.values[kDekuMap] = 0;
  for (int i = 0; i < 200; ++i) CHECK(k.frame(s, false, true).empty());
  CHECK(s.values[kDekuMap] == 0);  // nothing written while loading
  CHECK(k.frame(s, true, true) == std::vector<FlagKey>{kDekuMap});
  CHECK(s.values[kDekuMap] == 1);
  // A scene change with a short gap with no scene at all: still no save load.
  s.values[kDekuMap] = 0;
  for (int i = 0; i < OneWayKeeper::kTitleFrames - 1; ++i) k.frame(s, false, false);
  CHECK(k.frame(s, true, true) == std::vector<FlagKey>{kDekuMap});
  CHECK(k.stats().save_loads == 0);
}

TEST_CASE("one-way keeper: a save load (the title screen) takes the save's values: an older save keeps no map") {
  Store s = dungeon(1);
  OneWayKeeper k({kDekuMap});
  play(k, s);
  for (int i = 0; i < OneWayKeeper::kTitleFrames; ++i) k.frame(s, false, false);  // the title, the file list
  s.values[kDekuMap] = 0;  // an older save
  for (int i = 0; i < 30; ++i) k.frame(s, false, true);  // its scene loads
  CHECK(k.frame(s, true, true).empty());
  CHECK(s.values[kDekuMap] == 0);
  CHECK(k.stats().save_loads == 1);
  play(k, s, 5);
  CHECK(s.values[kDekuMap] == 0);
  // Got again in that save, then dropped: kept again.
  s.values[kDekuMap] = 1;
  play(k, s);
  s.values[kDekuMap] = 0;
  play(k, s);
  CHECK(s.values[kDekuMap] == 1);
}

TEST_CASE("one-way keeper: a refused write is tried again, an unreadable flag is left alone") {
  Store s = dungeon(1);
  OneWayKeeper k({kDekuMap, kDekuCompass});
  play(k, s);
  s.values[kDekuMap] = 0;
  s.refused.insert(kDekuMap);
  CHECK(k.frame(s, true, true).empty());
  CHECK(k.stats().failures == 1);
  s.refused.clear();
  CHECK(k.frame(s, true, true) == std::vector<FlagKey>{kDekuMap});
  CHECK(s.values[kDekuMap] == 1);
  s.values.erase(kDekuCompass);  // not in this game's table
  play(k, s, 3);
  CHECK(k.stats().restores == 1);
}

TEST_CASE("one-way keeper: with a clock the title screen is real time, not presented frames") {
  Store s = dungeon(1);
  OneWayKeeper k({kDekuMap});
  std::uint64_t now = 0;
  k.set_clock([&now] { return now; });
  play(k, s);
  // 90 presents at 165 a second (0.55 s with no scene): no save load.
  for (int i = 0; i < 90; ++i) {
    now += 6'061;
    k.frame(s, false, false);
  }
  s.values[kDekuMap] = 0;
  now += 6'061;
  CHECK(k.frame(s, true, true) == std::vector<FlagKey>{kDekuMap});
  CHECK(k.stats().save_loads == 0);
}
