// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/host/one_way_keeper.hpp"

#include <utility>

#include "coop/host/game_items.hpp"

namespace coop::host {

std::optional<std::string> one_way_dungeon(FlagKey key) {
  if (!is_one_way_item(key)) return std::nullopt;
  const ItemFlag* item = find_item(key.hash);
  constexpr std::string_view kPrefix = "DungeonInfo.";
  const std::string_view rest = item->name.substr(kPrefix.size());
  const auto dot = rest.find('.');
  if (dot == std::string_view::npos || dot == 0) return std::nullopt;
  return std::string(rest.substr(0, dot));
}

OneWayKeeper::OneWayKeeper() : OneWayKeeper(one_way_item_flags()) {}

OneWayKeeper::OneWayKeeper(std::vector<FlagKey> keys) {
  for (const FlagKey key : keys) {
    if (auto dungeon = one_way_dungeon(key)) keys_.push_back({key, std::move(*dungeon)});
  }
}

void OneWayKeeper::reset() {
  seen_.clear();
  given_.clear();
  wait_.clear();
  ++stats_.resets;
}

std::vector<FlagKey> OneWayKeeper::frame(FlagStore& store, bool in_play, std::optional<std::string_view> scene) {
  const unsigned step = ticks_.step();
  std::vector<FlagKey> restored;
  if (!in_play) {
    // Loading. A room or scene change keeps what we saw; long enough with
    // no scene at all is the title screen: a save is being loaded.
    no_scene_ = scene ? 0 : no_scene_ + static_cast<int>(step);
    if (no_scene_ >= kTitleFrames) save_load_ = true;
    return restored;
  }
  no_scene_ = 0;
  if (std::exchange(save_load_, false)) {
    // Another save (or the same one again): nothing seen before counts.
    seen_.clear();
    given_.clear();
    wait_.clear();
    ++stats_.save_loads;
    save_load_seen_ = true;
  }
  for (const Kept& k : keys_) {
    const auto now = store.read(k.key);
    if (!now || now->kind != protocol::FlagKind::boolean) continue;
    const bool one = now->value != 0;
    if (auto w = wait_.find(k.key); w != wait_.end()) {
      // Our write is on its way (on Hookshot the game's setter runs on its
      // calc thread): not read as a new drop until it lands or times out.
      w->second -= static_cast<int>(step);
      if (!one && w->second > 0) continue;
      wait_.erase(w);
    }
    const auto [it, first] = seen_.try_emplace(k.key, one);
    if (first) continue;  // the baseline: what the save (or the session) left
    const bool was = std::exchange(it->second, one);
    if (one) {
      if (!was) given_.insert(k.key);  // the game gave it while we watched
      continue;
    }
    if (!was || !given_.contains(k.key) || scene != std::string_view(k.dungeon)) continue;
    // Given here, 0 now, in its dungeon, no save load between: the game
    // took it back.
    if (store.write(protocol::FlagEntry(k.key, protocol::FlagKind::boolean, 1))) {
      ++stats_.restores;
      restored.push_back(k.key);
      wait_[k.key] = kWriteWait;
    } else {
      ++stats_.failures;
    }
    it->second = true;  // still given: a refused write is tried again next frame
  }
  return restored;
}

}  // namespace coop::host
