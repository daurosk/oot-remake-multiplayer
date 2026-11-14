// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One log line whenever a dungeon's map or compass goes from 1 to 0,
// wherever Link is (minimap-b): to find what takes it back, which three research runs did
// not show (players see it as the map gone from the minimap after the Map
// page or a new room; docs/dungeon.md, "The map and compass stay got").
//
// The line names the flag (and that Link is outside its dungeon), the scene, Link's place, whether the game was
// loading, whether a game menu or the Map page was in front, whether our
// code wrote that flag to 0 in the last kWindowUs (and through what), and
// the events of the last kWindowUs: the Map page and menus opening and
// closing (Hookshot with patch 0018), loads (a room or scene change), scene
// changes, save loads, sessions starting and ending, the flag watcher's
// snapshots and resets, and every write of ours to a map or compass flag.
// Any thread: one lock.
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flags.hpp"

namespace coop::host {

class MapDropLog {
 public:
  static constexpr std::uint64_t kWindowUs = 10'000'000;  // the last 10 s
  static constexpr std::size_t kMostEvents = 40;
  // Full lines per flag; then one summary a minute with the count.
  static constexpr int kLinesPerFlag = 3;
  static constexpr std::uint64_t kSummaryUs = 60'000'000;
  // More flags than this dropping in one frame: one line for them all.
  static constexpr std::size_t kBatch = 3;

  using Clock = std::function<std::uint64_t()>;  // microseconds, steady
  MapDropLog();
  explicit MapDropLog(Clock clock);

  // Something that happened (kept kWindowUs).
  void note(std::string what);
  // A write of ours to a map or compass flag (GameFlagStore); `by` says
  // through what. Other flags are ignored.
  void note_our_write(FlagKey key, std::uint32_t value, std::string_view by);
  // The game's menus as Hookshot's patch 0018 tells them; noted when they
  // change. Never called: "unknown" in the line.
  void set_menu(bool menu_open, bool map_page);

  // One game frame: `scene` loaded (none while none), `in_play` (Link read),
  // Link's place when read. Reads every dungeon's map and compass, wherever
  // Link is; returns a line for each that went from 1 to 0 since the last
  // frame (capped: kLinesPerFlag, then a summary a minute; more than kBatch
  // at once: one line). Also notes loads and scene changes.
  std::vector<std::string> frame(FlagStore& store, std::optional<std::string_view> scene, bool in_play,
                                 std::optional<std::array<float, 3>> link = std::nullopt);

 private:
  struct Event {
    std::uint64_t at;
    std::string what;
  };
  struct Write {
    std::uint64_t at;
    std::uint32_t value;
    std::string by;
  };
  void note_locked(std::uint64_t now, std::string what);
  void prune(std::uint64_t now);
  std::string line(std::uint64_t now, FlagKey key, std::string_view name, std::string_view scene, bool in_play,
                   const std::optional<std::array<float, 3>>& link) const;
  std::string context(std::uint64_t now, std::optional<FlagKey> key, std::string_view scene, bool in_play,
                      const std::optional<std::array<float, 3>>& link) const;
  struct Cap {
    int lines = 0;
    std::uint64_t hidden = 0;
    std::uint64_t summary_at = 0;
  };

  Clock clock_;
  std::vector<FlagKey> keys_;  // every dungeon's map and compass
  std::map<FlagKey, Cap> caps_;
  mutable std::mutex mutex_;
  std::deque<Event> events_;
  std::map<FlagKey, Write> writes_;  // our last write to each flag
  std::map<FlagKey, std::uint32_t> last_;
  std::string scene_;
  bool in_play_ = false;
  std::optional<bool> menu_open_, map_page_;
};

// The process's one (the hook, the module and GameFlags' setters note into it).
MapDropLog& map_drop_log();

// While one lives, our flag writes on this thread are noted as made by `why`
// (the flag watcher's join snapshot, a host's save-load reset, the solo
// keeper...). Nested: the innermost wins.
class MapDropBecause {
 public:
  explicit MapDropBecause(const char* why);
  ~MapDropBecause();
  MapDropBecause(const MapDropBecause&) = delete;
  MapDropBecause& operator=(const MapDropBecause&) = delete;
  // The innermost reason on this thread, or "co-op code".
  static const char* now();

 private:
  const char* before_;
};

}  // namespace coop::host
