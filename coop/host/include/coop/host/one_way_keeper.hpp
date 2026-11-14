// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A guard, not a fix: a dungeon's map and compass, kept when no co-op
// session runs (minimap-b). The root cause is not found yet.
//
// Players reported the Dungeon Map gone from the minimap after opening the
// Map page or entering a room, also playing alone with the mod. The game's
// minimap and Map page read DungeonInfo.<dungeon>.IsGetMap again only when
// the page opens or a room loads (a research run: the flag set while Link
// stands still leaves the minimap dark until the Map page is opened), so a
// map lost earlier shows up there. What takes the flag back was not seen in
// three research runs; MapDropLog (map_drop_log.hpp) logs each drop with
// what happened before it, so players' logs can find it. In a session the
// flag watcher puts a dropped map or compass back (issue 146); with no
// session this does, narrowly:
// - only a map or compass the game itself gave while this keeper watched
//   (seen going 0 -> 1 in play, with no session), in this save: the baseline
//   is dropped at every save load (no scene for kTitleFrames: the title
//   screen or the file list) and by reset(), which the hook calls whenever a
//   session has run (its writes are not the game's own);
// - only while Link is in that flag's own dungeon (the scene of the same
//   name: InsideTheDekuTree for DungeonInfo.InsideTheDekuTree.IsGetMap);
// - written back to 1 through the game's own Bool setter (GameFlagStore: the
//   game's setter on its calc thread on Hookshot).
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/ticks.hpp"

namespace coop::host {

// The dungeon a map or compass flag belongs to ("InsideTheDekuTree" for
// DungeonInfo.InsideTheDekuTree.IsGetMap), or none for another flag.
std::optional<std::string> one_way_dungeon(FlagKey key);

class OneWayKeeper {
 public:
  // The 1/60 s ticks with no scene that make a save load (the title screen,
  // the file list). FlagWatcher takes 30 together with flags going back;
  // alone it is longer, so a scene change's own gap never counts.
  static constexpr int kTitleFrames = 90;
  // Ticks a written-back flag may still read 0 before it counts as dropped again.
  static constexpr int kWriteWait = 30;

  // `keys`: the flags kept (default: one_way_item_flags()); a key that is
  // not a dungeon's map or compass is never kept.
  OneWayKeeper();
  explicit OneWayKeeper(std::vector<FlagKey> keys);

  // Counts in real time (GameTicks); without a clock every frame is a tick.
  void set_clock(GameTicks::Clock clock) { ticks_.set_clock(std::move(clock)); }

  // Forgets every value seen and every map the game gave: from the next
  // frame on, only what the game gives from then is kept. For a session
  // that started or ended.
  void reset();

  // One game frame. `in_play`: Link is read (not loading). `scene`: the
  // scene loaded, none while there is none. Returns the flags written back.
  std::vector<FlagKey> frame(FlagStore& store, bool in_play, std::optional<std::string_view> scene);

  // Whether a save load was taken since the last call (for the log).
  bool take_save_load() { return std::exchange(save_load_seen_, false); }

  struct Stats {
    std::uint64_t restores = 0;    // a map or compass written back to 1
    std::uint64_t failures = 0;    // the write was refused (tried again next frame)
    std::uint64_t save_loads = 0;  // the baseline dropped for a save load
    std::uint64_t resets = 0;      // reset()
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Kept {
    FlagKey key;
    std::string dungeon;
  };
  std::vector<Kept> keys_;
  std::map<FlagKey, bool> seen_;  // the value last seen in play
  std::set<FlagKey> given_;       // seen going 0 -> 1 in play: the game gave it
  std::map<FlagKey, int> wait_;   // written back: ticks left for the write to land
  GameTicks ticks_;
  int no_scene_ = 0;
  bool save_load_ = false;
  bool save_load_seen_ = false;
  Stats stats_;
};

}  // namespace coop::host
