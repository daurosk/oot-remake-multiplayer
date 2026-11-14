// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/host/map_drop_log.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

#include "coop/host/game_items.hpp"

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

MapDropLog::MapDropLog(Clock clock) : clock_(std::move(clock)) {}

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
  std::string out = "coop: map-drop: " + std::string(name) + " 1 -> 0 in " + std::string(scene);
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
  const auto w = writes_.find(key);
  if (w != writes_.end() && w->second.value == 0 && now - w->second.at <= kWindowUs) {
    out += "YES (" + w->second.by + ", " + ago(now, w->second.at) + ")";
  } else {
    out += "no";
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
    last_.clear();  // another dungeon's flags
  }
  if (!scene) return lines;
  for (const char* what : {".IsGetMap", ".IsGetCompass"}) {
    const std::string name = "DungeonInfo." + std::string(*scene) + what;
    const FlagKey key{flag_hash(name), 0};
    if (!is_one_way_item(key)) continue;
    const auto value = store.read(key);
    if (!value) continue;
    const auto [it, first] = last_.try_emplace(key, value->value);
    if (first) continue;
    const std::uint32_t was = std::exchange(it->second, value->value);
    if (was != 0 && value->value == 0) {
      lines.push_back(line(now, key, name, *scene, in_play, link));
      note_locked(now, name + " 1 -> 0");
    } else if (was == 0 && value->value != 0) {
      note_locked(now, name + " 0 -> 1");
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
