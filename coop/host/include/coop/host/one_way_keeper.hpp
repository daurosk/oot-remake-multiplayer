// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A dungeon's map and compass, kept when no co-op session runs (minimap-b).
//
// Players reported the Dungeon Map gone from the minimap after opening the
// Map page or entering a room, also playing alone with the mod. The game's
// minimap and Map page read DungeonInfo.<dungeon>.IsGetMap again only when
// the page opens or a room loads (a research run: the flag set while Link
// stands still leaves the minimap dark until the Map page is opened), so a
// map lost earlier shows up there. In a session the flag watcher puts a
// dropped map or compass back (FlagWatcher, ItemFlagStore::is_one_way, issue
// 146); with no session nothing did. This does the same alone: a map or
// compass that goes from 1 to 0 is written back to 1 through the game's own
// Bool setter (GameFlagStore: the game's setter on its calc thread on
// Hookshot), in play or across a room or scene load. A save load (no scene
// for kTitleFrames: the title screen or the file list) takes the save's
// values as they are: an older save without the map keeps it lost.
#include <cstdint>
#include <map>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/ticks.hpp"

namespace coop::host {

class OneWayKeeper {
 public:
  // The 1/60 s ticks with no scene that make a save load (the title screen,
  // the file list). FlagWatcher takes 30 together with flags going back;
  // alone it is longer, so a scene change's own gap never counts.
  static constexpr int kTitleFrames = 90;

  // `keys`: the flags kept (default: one_way_item_flags()).
  OneWayKeeper();
  explicit OneWayKeeper(std::vector<FlagKey> keys);

  // Counts in real time (GameTicks); without a clock every frame is a tick.
  void set_clock(GameTicks::Clock clock) { ticks_.set_clock(std::move(clock)); }

  // One game frame. `in_play`: Link is read (not loading). `scene`: a scene
  // is loaded. Returns the flags written back this frame.
  std::vector<FlagKey> frame(FlagStore& store, bool in_play, bool scene);

  struct Stats {
    std::uint64_t restores = 0;    // a map or compass written back to 1
    std::uint64_t failures = 0;    // the write was refused (tried again next frame)
    std::uint64_t save_loads = 0;  // the save's values taken as they are
  };
  const Stats& stats() const { return stats_; }

 private:
  std::vector<FlagKey> keys_;
  std::map<FlagKey, bool> got_;  // the value last seen in play
  GameTicks ticks_;
  int no_scene_ = 0;
  bool save_load_ = false;
  Stats stats_;
};

}  // namespace coop::host
