// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/host/map_drop_log.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

#include "coop/host/game_items.hpp"
#include "coop/host/one_way_keeper.hpp"

namespace coop::host {
namespace {

std::uint64_t steady_us() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

std::string ago(std::uint64_t now, std::uint64_t at) {
  char text[32];
  std::snprintf(text, sizeof text, "-%.1f s", static_cast<double>(now - at) / 1e6);
  return text;
}

std::string_view item_name(FlagKey key) {
  const ItemFlag* item = find_item(key.hash);
  return item != nullptr ? item->name : std::string_view("?");
}

}  // namespace

MapDropLog::MapDropLog() : MapDropLog(steady_us) {}

MapDropLog::MapDropLog(Clock clock) : clock_(std::move(clock)), keys_(one_way_item_flags()) {}

void MapDropLog::prune(std::uint64_t now) {
  while (!events_.empty() && (now - events_.front().at > kWindowUs || events_.size() > kMostEvents)) {
    events_.pop_front();
  }
}

void MapDropLog::note_locked(std::uint64_t now, std::string what) {
  events_.push_back({now, std::move(what)});
  prune(now);
}

void MapDropLog::note(std::string what) {
  const std::uint64_t now = clock_();
  std::lock_guard lock(mutex_);
  note_locked(now, std::move(what));
}

void MapDropLog::note_our_write(FlagKey key, std::uint32_t value, std::string_view by) {
  if (!is_one_way_item(key)) return;
  const std::uint64_t now = clock_();
  std::lock_guard lock(mutex_);
  writes_[key] = {now, value, std::string(by)};
  note_locked(now, std::string("our write ") + std::string(item_name(key)) + " = " + std::to_string(value) +
                       " (" + std::string(by) + ")");
}

void MapDropLog::set_menu(bool menu_open, bool map_page) {
  const std::uint64_t now = clock_();
  std::lock_guard lock(mutex_);
  if (menu_open_ != menu_open) {
    if (menu_open_) note_locked(now, menu_open ? "game menu opened" : "game menu closed");
    menu_open_ = menu_open;
  }
  if (map_page_ != map_page) {
    if (map_page_) note_locked(now, map_page ? "Map page shown" : "Map page gone");
    map_page_ = map_page;
  }
}

std::string MapDropLog::line(std::uint64_t now, FlagKey key, std::string_view name, std::string_view scene,
                             bool in_play, const std::optional<std::array<float, 3>>& link) const {
  std::string out = "coop: map-drop: " + std::string(name) + " 1 -> 0";
  const auto dungeon = one_way_dungeon(key);
  if (dungeon && *dungeon != scene) out += " (outside its dungeon " + *dungeon + ")";
  return out + context(now, key, scene, in_play, link);
}

std::string MapDropLog::context(std::uint64_t now, std::optional<FlagKey> key, std::string_view scene, bool in_play,
                                const std::optional<std::array<float, 3>>& link) const {
  std::string out = " in " + std::string(scene);
  if (link) {
    char at[64];
    std::snprintf(at, sizeof at, " at %.1f %.1f %.1f", (*link)[0], (*link)[1], (*link)[2]);
    out += at;
  }
  out += in_play ? ", in play" : ", loading";
  out += ", game menu ";
  out += menu_open_ ? (*menu_open_ ? "open" : "closed") : "unknown";
  out += ", Map page ";
  out += map_page_ ? (*map_page_ ? "shown" : "not shown") : "unknown";
  out += "; our code wrote it: ";
  if (key) {
    const auto w = writes_.find(*key);
    if (w != writes_.end() && w->second.value == 0 && now - w->second.at <= kWindowUs) {
      out += "YES (" + w->second.by + ", " + ago(now, w->second.at) + ")";
    } else {
      out += "no";
    }
  } else {
    int ours = 0;  // our writes of 0 to any of them in the window
    for (const auto& [k, w] : writes_) ours += w.value == 0 && now - w.at <= kWindowUs ? 1 : 0;
    out += ours != 0 ? "YES for " + std::to_string(ours) + " (see the events)" : "no";
  }
  out += "; last 10 s:";
  if (events_.empty()) out += " nothing";
  for (const Event& e : events_) out += " [" + ago(now, e.at) + " " + e.what + "]";
  return out;
}

std::vector<std::string> MapDropLog::frame(FlagStore& store, std::optional<std::string_view> scene, bool in_play,
                                           std::optional<std::array<float, 3>> link) {
  const std::uint64_t now = clock_();
  std::vector<std::string> lines;
  std::lock_guard lock(mutex_);
  prune(now);
  if (in_play != in_play_) {
    note_locked(now, in_play ? "load ended" : "load started (a room or scene change, or the title)");
    in_play_ = in_play;
  }
  if (scene && *scene != scene_) {
    note_locked(now, "scene " + (scene_.empty() ? std::string("(none)") : scene_) + " -> " + std::string(*scene));
    scene_ = std::string(*scene);
  }
  // Every dungeon's map and compass, wherever Link is: a drop away from its
  // dungeon may be the cause too (the minimap shows it only there).
  std::vector<FlagKey> dropped;
  for (const FlagKey key : keys_) {
    const auto value = store.read(key);
    if (!value) continue;
    const auto [it, first] = last_.try_emplace(key, value->value);
    if (first) continue;
    const std::uint32_t was = std::exchange(it->second, value->value);
    if (was != 0 && value->value == 0) {
      dropped.push_back(key);
      note_locked(now, std::string(item_name(key)) + " 1 -> 0");
    } else if (was == 0 && value->value != 0) {
      note_locked(now, std::string(item_name(key)) + " 0 -> 1");
    }
  }
  const std::string_view where = scene ? *scene : std::string_view("(no scene)");
  if (dropped.size() > kBatch) {
    // Many at once: a save load or a reset of the whole save, one line.
    std::string names;
    for (const FlagKey key : dropped) names += (names.empty() ? "" : ", ") + std::string(item_name(key));
    lines.push_back("coop: map-drop: " + std::to_string(dropped.size()) + " map and compass flags 1 -> 0 at once (" +
                    names + ")" + context(now, std::nullopt, where, in_play, link));
  } else {
    for (const FlagKey key : dropped) {
      Cap& cap = caps_[key];
      if (cap.lines < kLinesPerFlag) {
        ++cap.lines;
        if (cap.lines == kLinesPerFlag) cap.summary_at = now;
        lines.push_back(line(now, key, item_name(key), where, in_play, link) +
                        (cap.lines == kLinesPerFlag ? " (more drops of this flag: one summary a minute)" : ""));
      } else {
        ++cap.hidden;
      }
    }
  }
  for (auto& [key, cap] : caps_) {
    if (cap.hidden > 0 && now - cap.summary_at >= kSummaryUs) {
      lines.push_back("coop: map-drop: " + std::string(item_name(key)) + " went 1 -> 0 " + std::to_string(cap.hidden) +
                      " more times since the last line (lines capped; now in " + std::string(where) + ")");
      cap.hidden = 0;
      cap.summary_at = now;
    }
  }
  return lines;
}

MapDropLog& map_drop_log() {
  static MapDropLog log;
  return log;
}

namespace {
thread_local const char* t_why = nullptr;
}  // namespace

MapDropBecause::MapDropBecause(const char* why) : before_(t_why) { t_why = why; }
MapDropBecause::~MapDropBecause() { t_why = before_; }
const char* MapDropBecause::now() { return t_why != nullptr ? t_why : "co-op code"; }

}  // namespace coop::host
