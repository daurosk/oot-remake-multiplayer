// SPDX-License-Identifier: GPL-3.0-or-later
// MapDropLog (minimap-b): one line when the current dungeon's map or compass goes 1 -> 0.
#include <doctest/doctest.h>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop/host/map_drop_log.hpp"

namespace {
using coop::host::FlagKey;
using coop::host::MapDropBecause;
using coop::host::MapDropLog;
namespace proto = coop::protocol;

constexpr FlagKey kDekuMap{0x89787395u, 0};      // DungeonInfo.InsideTheDekuTree.IsGetMap
constexpr FlagKey kDekuCompass{0x06c51ff2u, 0};  // DungeonInfo.InsideTheDekuTree.IsGetCompass
constexpr FlagKey kGotBomb{0x2bb00768u, 0};

class Store : public coop::host::FlagStore {
 public:
  std::map<FlagKey, std::uint32_t> values{{kDekuMap, 1}, {kDekuCompass, 1}, {kGotBomb, 1}};
  std::optional<proto::FlagEntry> read(FlagKey key) override {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return proto::FlagEntry(key, proto::FlagKind::boolean, it->second);
  }
  bool write(const proto::FlagEntry&) override { return false; }
};

bool has(const std::string& line, const std::string& part) { return line.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("map drop log: a map going 1 -> 0 in its dungeon gives one line with the events before it") {
  std::uint64_t now = 1'000'000;
  MapDropLog log([&now] { return now; });
  Store s;
  CHECK(log.frame(s, "InsideTheDekuTree", true).empty());  // the baseline
  log.set_menu(false, false);  // first state: not an event
  now += 1'000'000;
  log.set_menu(true, true);
  now += 500'000;
  log.set_menu(false, false);
  now += 500'000;
  CHECK(log.frame(s, "InsideTheDekuTree", false).empty());  // a room load starts
  s.values[kDekuMap] = 0;
  s.values[kGotBomb] = 0;  // not a map or compass: no line
  now += 300'000;
  const auto lines = log.frame(s, "InsideTheDekuTree", true, std::array<float, 3>{0.2f, 100.9f, 14.0f});
  REQUIRE(lines.size() == 1);
  const std::string& l = lines[0];
  CHECK(has(l, "coop: map-drop: DungeonInfo.InsideTheDekuTree.IsGetMap 1 -> 0 in InsideTheDekuTree at 0.2 100.9 14.0"));
  CHECK(has(l, ", in play"));
  CHECK(has(l, "game menu closed, Map page not shown"));
  CHECK(has(l, "our code wrote it: no"));
  CHECK(has(l, "[-1.3 s game menu opened]"));
  CHECK(has(l, "[-1.3 s Map page shown]"));
  CHECK(has(l, "[-0.8 s Map page gone]"));
  CHECK(has(l, "[-0.3 s load started"));
  CHECK(has(l, "[-0.0 s load ended]"));
  // Once: it stays 0.
  CHECK(log.frame(s, "InsideTheDekuTree", true).empty());
}

TEST_CASE("map drop log: our own write of 0 is named with what made it") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  {
    const MapDropBecause because("the flag watcher: the host's save-load reset");
    log.note_our_write(kDekuCompass, 0, MapDropBecause::now());
  }
  CHECK(std::string(MapDropBecause::now()) == "co-op code");  // back out of it
  log.note_our_write(kGotBomb, 0, "x");  // not a map or compass: not noted
  s.values[kDekuCompass] = 0;
  now += 100'000;
  const auto lines = log.frame(s, "InsideTheDekuTree", true);
  REQUIRE(lines.size() == 1);
  CHECK(has(lines[0], "IsGetCompass 1 -> 0"));
  CHECK(has(lines[0], "our code wrote it: YES (the flag watcher: the host's save-load reset, -0.1 s)"));
  CHECK(has(lines[0], "game menu unknown, Map page unknown"));  // no patch 0018 told it
  CHECK_FALSE(has(lines[0], "Item_Bomb"));
}

TEST_CASE("map drop log: a write of ours older than the window, or of 1, is not the cause") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  log.note_our_write(kDekuMap, 0, "old");
  now += MapDropLog::kWindowUs + 1;
  log.note_our_write(kDekuCompass, 1, "a one");
  s.values[kDekuMap] = 0;
  s.values[kDekuCompass] = 0;
  const auto lines = log.frame(s, "InsideTheDekuTree", true);
  REQUIRE(lines.size() == 2);
  for (const auto& l : lines) CHECK(has(l, "our code wrote it: no"));
  CHECK_FALSE(has(lines[0], "(old)"));  // the old write fell out of the window too
}

TEST_CASE("map drop log: a drop away from its dungeon is logged too, no new baseline on a scene change") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  log.frame(s, "KokiriForest", true);
  s.values[kDekuMap] = 0;
  now += 200'000;
  const auto lines = log.frame(s, "KokiriForest", true);
  REQUIRE(lines.size() == 1);
  CHECK(has(lines[0], "DungeonInfo.InsideTheDekuTree.IsGetMap 1 -> 0 (outside its dungeon InsideTheDekuTree) in KokiriForest"));
  CHECK(has(lines[0], "[-0.2 s scene InsideTheDekuTree -> KokiriForest]"));
  // While no scene is loaded (the title): still read, named so.
  s.values[kDekuMap] = 1;
  log.frame(s, std::nullopt, false);
  s.values[kDekuCompass] = 0;
  CHECK(log.frame(s, std::nullopt, false).empty());  // (row 269: out of play it waits for the game to say why)
  now += MapDropLog::kHoldUs + 1;
  const auto none = log.frame(s, std::nullopt, false);
  REQUIRE(none.size() == 1);
  CHECK(has(none[0], "IsGetCompass 1 -> 0 (outside its dungeon InsideTheDekuTree) in (no scene), loading"));
  CHECK(has(none[0], "no save load and no title followed in 3 s"));
  CHECK(log.take_info().empty());
  // Back in the dungeon: the value it had there is not a new baseline.
  s.values[kDekuMap] = 0;
  CHECK(log.frame(s, "InsideTheDekuTree", true).size() == 1);
}

TEST_CASE("map drop log: three lines a flag, then one summary a minute") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  auto drop_and_back = [&] {
    s.values[kDekuMap] = 0;
    now += 1'000'000;
    auto out = log.frame(s, "InsideTheDekuTree", true);
    s.values[kDekuMap] = 1;
    now += 1'000'000;
    for (auto& l : log.frame(s, "InsideTheDekuTree", true)) out.push_back(l);
    return out;
  };
  for (int i = 1; i <= MapDropLog::kLinesPerFlag; ++i) {
    const auto lines = drop_and_back();
    REQUIRE(lines.size() == 1);
    CHECK(has(lines[0], "IsGetMap 1 -> 0"));
    CHECK(has(lines[0], "one summary a minute") == (i == MapDropLog::kLinesPerFlag));
  }
  int hidden = 0;
  std::vector<std::string> summaries;
  for (int i = 0; i < 40; ++i) {  // 80 s
    for (auto& l : drop_and_back()) summaries.push_back(l);
    ++hidden;
  }
  REQUIRE(summaries.size() == 1);  // at the minute
  CHECK(has(summaries[0], "coop: map-drop: DungeonInfo.InsideTheDekuTree.IsGetMap went 1 -> 0 30 more times"));
  // The compass has its own three.
  s.values[kDekuCompass] = 0;
  CHECK(log.frame(s, "InsideTheDekuTree", true).size() == 1);
  (void)hidden;
}

TEST_CASE("map drop log: many flags at once (a save load) make one line") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  constexpr FlagKey kFireCompass{0x0aa068b6u, 0}, kShadowCompass{0x1e2feec5u, 0}, kIceMap{0x322f2a94u, 0};
  for (FlagKey k : {kFireCompass, kShadowCompass, kIceMap}) s.values[k] = 1;
  log.frame(s, std::nullopt, false);
  for (auto& [k, v] : s.values) v = k == kGotBomb ? v : 0;
  // Row 269 (a tester's log, 13 times): the game clears its table, then says it loads a save. Not a warning.
  CHECK(log.frame(s, std::nullopt, false).empty());
  CHECK(log.take_info().empty());
  now += 200'000;
  log.game_reset("the game starts loading a save");
  CHECK(log.frame(s, std::nullopt, false).empty());
  const auto lines = log.take_info();
  REQUIRE(lines.size() == 1);
  CHECK(has(lines[0], "coop: map-drop: 5 map and compass flags 1 -> 0 at once ("));
  CHECK(has(lines[0], "DungeonInfo.IceCavern.IsGetMap"));
  CHECK(has(lines[0], "our code wrote it: no"));
  CHECK(has(lines[0], "; 0.2 s later: the game starts loading a save: the game's own clear of its flags, no drop in play"));
  CHECK(log.take_info().empty());  // (said once)
  now += MapDropLog::kHoldUs + 1;
  CHECK(log.frame(s, std::nullopt, false).empty());  // (and never as a warning after all)
}

TEST_CASE("map drop log: a save's one map cleared at a save load or the title is no warning either (row 269)") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  // Back to the title: the load starts, the flags are cleared, the title comes up.
  CHECK(log.frame(s, std::nullopt, false).empty());
  s.values[kDekuMap] = 0;
  now += 100'000;
  CHECK(log.frame(s, std::nullopt, false).empty());
  now += 2'000'000;
  log.game_reset("the title");
  const auto told = log.take_info();
  REQUIRE(told.size() == 1);
  CHECK(has(told[0], "DungeonInfo.InsideTheDekuTree.IsGetMap 1 -> 0"));
  CHECK(has(told[0], "; 2.0 s later: the title: the game's own clear"));
  // The game said it first (a load node before the clear): the same.
  s.values[kDekuMap] = 1;
  log.frame(s, std::nullopt, false);
  log.game_reset("the game starts loading a save");
  now += 300'000;
  s.values[kDekuMap] = 0;
  CHECK(log.frame(s, std::nullopt, false).empty());
  const auto after = log.take_info();
  REQUIRE(after.size() == 1);
  CHECK(has(after[0], "; the game starts loading a save (-0.3 s): the game's own clear"));
}

TEST_CASE("map drop log: a drop in play is a warning at once, many at once too, whatever was loaded before") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  constexpr FlagKey kFireCompass{0x0aa068b6u, 0}, kShadowCompass{0x1e2feec5u, 0}, kIceMap{0x322f2a94u, 0};
  for (FlagKey k : {kFireCompass, kShadowCompass, kIceMap}) s.values[k] = 1;
  log.frame(s, "InsideTheDekuTree", true);
  log.game_reset("the game loaded a save");
  now += 1'000'000;
  for (auto& [k, v] : s.values) v = k == kGotBomb ? v : 0;
  const auto lines = log.frame(s, "InsideTheDekuTree", true);
  REQUIRE(lines.size() == 1);
  CHECK(has(lines[0], "5 map and compass flags 1 -> 0 at once ("));
  CHECK(has(lines[0], ", in play"));
  CHECK(has(lines[0], "[-1.0 s the game loaded a save]"));
  CHECK(log.take_info().empty());
}

TEST_CASE("map drop log: a drop in a room load with no save load after it is still a warning, 3 s later") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  CHECK(log.frame(s, "InsideTheDekuTree", false).empty());  // a room load
  s.values[kDekuMap] = 0;
  CHECK(log.frame(s, "InsideTheDekuTree", false).empty());
  now += 500'000;
  s.values[kDekuCompass] = 0;
  CHECK(log.frame(s, "InsideTheDekuTree", true).size() == 1);  // in play again: this one at once
  now += MapDropLog::kHoldUs;
  const auto lines = log.frame(s, "InsideTheDekuTree", true);
  REQUIRE(lines.size() == 1);
  CHECK(has(lines[0], "DungeonInfo.InsideTheDekuTree.IsGetMap 1 -> 0 in InsideTheDekuTree, loading"));
  CHECK(has(lines[0], "no save load and no title followed in 3 s"));
  // A save load long after it explains nothing.
  log.game_reset("the game starts loading a save");
  CHECK(log.take_info().empty());
}

TEST_CASE("map drop log: events older than the window fall out") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  log.note("co-op session started");
  now += MapDropLog::kWindowUs + 1;
  log.note("join snapshot applied, 3 flags");
  s.values[kDekuMap] = 0;
  const auto lines = log.frame(s, "InsideTheDekuTree", true);
  REQUIRE(lines.size() == 1);
  CHECK_FALSE(has(lines[0], "session started"));
  CHECK(has(lines[0], "join snapshot applied, 3 flags"));
}
