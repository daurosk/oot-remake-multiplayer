// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chat notices (docs/setup.md, "Chat notices"; Caleb, 2026-11-11: "Notify
// players in the chat when a player does something that affects them"). The
// shared values another player's change wrote into our game (FlagWatcher::
// take_received) become short local chat lines: "Tobi got the Fairy
// Slingshot - you got one too!", "Tobi found a Small Key (Deku Tree) - the
// group has 2". Only what changes the player's game is told: items and
// upgrades, songs, small keys, maps, compasses and boss keys, heart
// containers, Gold Skulltula tokens, and what the game adds (Describe: an
// opened chest, a boss beaten). A join or a host's save load is one summary
// line, never one line a flag; live lines are merged per player and kept to
// kMaxLines in kWindowMs, the rest said as one "and N more" line.
// No emulator code. Tested in tests/host/notify_test.cpp.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flag_watcher.hpp"

namespace coop::host {

// A player's item, song or dungeon in English: "Weapon_Range_SlingShot" ->
// "Fairy Slingshot", "InsideTheDekuTree" -> "Deku Tree"; a name not in the
// tables is spelt out from its parts ("Item_PocketEgg" -> "Pocket Egg").
std::string ItemName(std::string_view id);
std::string SongName(std::string_view id);
std::string DungeonName(std::string_view id);

class Notifier {
 public:
  static constexpr std::size_t kMaxLines = 4;      // live lines in a window
  static constexpr std::uint64_t kWindowMs = 3000;

  // The name of player n (1 to 4) as the chat shows it.
  using PlayerName = std::function<std::string(std::uint8_t)>;
  // What a flag the item table does not know means, as words after the
  // player's name ("opened a chest"), or nothing: from the game's own tables.
  using Extra = std::function<std::optional<std::string>(FlagKey, const protocol::FlagEntry& after)>;

  Notifier(PlayerName name, Extra extra = {})
      : name_(std::move(name)), extra_(std::move(extra)) {}

  // Once a frame: what was received, our own player number and the time.
  // Returns the lines to show.
  std::vector<std::string> Take(const std::vector<FlagWatcher::Received>& received, std::uint8_t self,
                                std::uint64_t now_ms);

  // One received value as a notice: its kind (for the summary and merging)
  // and its words after the player's name. Nothing for what is not told.
  struct Notice {
    std::string kind;   // "item", "song", "key", "map", "heart", "token", "other"
    std::string what;   // "got the Fairy Slingshot"
    std::string you;    // " - you got one too!" (may be empty)
  };
  std::optional<Notice> Explain(const FlagWatcher::Received& r) const;

 private:
  std::string Who(std::uint8_t player) const;
  void Say(std::string line, std::uint64_t now_ms, std::vector<std::string>& out);

  PlayerName name_;
  Extra extra_;
  std::deque<std::uint64_t> said_ms_;  // live lines in the window
  std::size_t held_back_ = 0;          // live lines not said (the window was full)
  std::map<FlagKey, std::int32_t> hearts_told_;  // highest MaxLife a notice named, by key
};

}  // namespace coop::host
