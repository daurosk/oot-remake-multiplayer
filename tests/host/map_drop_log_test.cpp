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

TEST_CASE("map drop log: another scene starts a new baseline, outside a dungeon nothing is read") {
  std::uint64_t now = 0;
  MapDropLog log([&now] { return now; });
  Store s;
  log.frame(s, "InsideTheDekuTree", true);
  s.values[kDekuMap] = 0;
  CHECK(log.frame(s, "KokiriForest", true).empty());  // left the dungeon: not read there
  CHECK(log.frame(s, std::nullopt, false).empty());
  CHECK(log.frame(s, "InsideTheDekuTree", true).empty());  // back: a new baseline, 0
  s.values[kDekuMap] = 1;
  log.frame(s, "InsideTheDekuTree", true);
  s.values[kDekuMap] = 0;
  const auto lines = log.frame(s, "InsideTheDekuTree", true);
  REQUIRE(lines.size() == 1);
  CHECK(has(lines[0], "[-0.0 s scene KokiriForest -> InsideTheDekuTree]"));
  CHECK(has(lines[0], "DungeonInfo.InsideTheDekuTree.IsGetMap 0 -> 1"));
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
