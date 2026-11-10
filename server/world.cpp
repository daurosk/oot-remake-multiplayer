// SPDX-License-Identifier: GPL-3.0-or-later
#include "world.hpp"

#include <algorithm>

namespace coop::server {

using namespace coop::protocol;

bool WorldStore::on_break(const WorldBreak& b) {
  if ((b.flags & world_break::restored) != 0) {
    auto it = scenes_.find(b.scene);
    if (it == scenes_.end() || it->second.breaks.erase(b.hash) == 0) return false;
    std::erase(it->second.order, b.hash);
    return true;
  }
  Scene& s = scenes_[b.scene];
  if (!s.breaks.emplace(b.hash, b).second) return false;
  s.order.push_back(b.hash);
  s.places.erase(b.hash);  // a broken object rests nowhere
  if (s.order.size() > kMaxWorldBreaks) {
    s.breaks.erase(s.order.front());
    s.order.pop_front();
  }
  return true;
}

void WorldStore::on_place(const WorldPlace& p) {
  Scene& s = scenes_[p.scene];
  if (s.breaks.contains(p.hash)) return;
  auto it = s.places.find(p.hash);
  if (it != s.places.end()) {
    it->second = p;
  } else if (s.places.size() < kMaxWorldPlaces) {
    s.places.emplace(p.hash, p);
  }
}

std::vector<Message> WorldStore::replay(const std::string& scene) const {
  std::vector<Message> out;
  auto it = scenes_.find(scene);
  if (it == scenes_.end()) return out;
  for (std::uint64_t hash : it->second.order) out.emplace_back(it->second.breaks.at(hash));
  for (const auto& [hash, place] : it->second.places) out.emplace_back(place);
  return out;
}

void WorldStore::forget(const std::string& scene) { scenes_.erase(scene); }

std::size_t WorldStore::breaks(const std::string& scene) const {
  auto it = scenes_.find(scene);
  return it == scenes_.end() ? 0 : it->second.breaks.size();
}

std::size_t WorldStore::places(const std::string& scene) const {
  auto it = scenes_.find(scene);
  return it == scenes_.end() ? 0 : it->second.places.size();
}

}  // namespace coop::server
