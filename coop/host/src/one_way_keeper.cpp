// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/host/one_way_keeper.hpp"

#include <utility>

#include "coop/host/game_items.hpp"

namespace coop::host {

OneWayKeeper::OneWayKeeper() : OneWayKeeper(one_way_item_flags()) {}

OneWayKeeper::OneWayKeeper(std::vector<FlagKey> keys) : keys_(std::move(keys)) {}

std::vector<FlagKey> OneWayKeeper::frame(FlagStore& store, bool in_play, bool scene) {
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
  const bool take_all = std::exchange(save_load_, false);
  if (take_all) ++stats_.save_loads;
  for (const FlagKey key : keys_) {
    const auto now = store.read(key);
    if (!now || now->kind != protocol::FlagKind::boolean) continue;
    const bool one = now->value != 0;
    const auto [it, first] = got_.try_emplace(key, one);
    if (first || take_all || one || !it->second) {
      it->second = one;
      continue;
    }
    // Got before, 0 now, no save load between: the game took it back.
    if (store.write(protocol::FlagEntry(key, protocol::FlagKind::boolean, 1))) {
      ++stats_.restores;
      restored.push_back(key);
    } else {
      ++stats_.failures;  // still got: tried again next frame
    }
  }
  return restored;
}

}  // namespace coop::host
