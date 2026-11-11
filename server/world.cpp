// SPDX-License-Identifier: GPL-3.0-or-later
#include "world.hpp"

#include <algorithm>

namespace coop::server {

using namespace coop::protocol;

bool WorldStore::on_break(const WorldBreak& b) {
  if ((b.flags & world_break::kept) != 0) {
    auto& kept = kept_[b.scene];
    for (const WorldBreak& k : kept) {
      if (k.hash == b.hash) return false;
    }
    if (kept.size() >= kMaxWorldKept || kept_total_ >= kMaxWorldKeptTotal) return false;
    kept.push_back(b);
    ++kept_total_;
    return true;
  }
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

void WorldStore::on_hit(const WorldHit& h) {
  Scene& s = scenes_[h.scene];
  auto it = s.hits.find(h.hash);
  if (it == s.hits.end()) {
    if (s.hits.size() >= kMaxWorldHitObjects) return;
    it = s.hits.emplace(h.hash, std::deque<WorldHit>{}).first;
  }
  it->second.push_back(h);
  if (it->second.size() > kMaxWorldHitsPerObject) it->second.pop_front();
}

void WorldStore::on_sign_cut(const SignCut& c) {
  Scene& s = scenes_[c.scene];
  auto it = s.cuts.find(c.hash);
  if (it != s.cuts.end()) {
    it->second = c;
  } else if (s.cuts.size() < kMaxWorldHitObjects) {
    s.cuts.emplace(c.hash, c);
  }
}

std::vector<Message> WorldStore::replay(const std::string& scene) const {
  std::vector<Message> out;
  if (auto k = kept_.find(scene); k != kept_.end()) {
    for (const WorldBreak& b : k->second) out.emplace_back(b);
  }
  auto it = scenes_.find(scene);
  if (it == scenes_.end()) return out;
  for (std::uint64_t hash : it->second.order) out.emplace_back(it->second.breaks.at(hash));
  for (const auto& [hash, place] : it->second.places) out.emplace_back(place);
  for (const auto& [hash, hits] : it->second.hits) {
    for (const WorldHit& h : hits) out.emplace_back(h);
  }
  for (const auto& [hash, cut] : it->second.cuts) out.emplace_back(cut);
  return out;
}

void WorldStore::forget(const std::string& scene) { scenes_.erase(scene); }

std::size_t WorldStore::breaks(const std::string& scene) const {
  auto it = scenes_.find(scene);
  auto k = kept_.find(scene);
  return (it == scenes_.end() ? 0 : it->second.breaks.size()) +
         (k == kept_.end() ? 0 : k->second.size());
}

std::size_t WorldStore::places(const std::string& scene) const {
  auto it = scenes_.find(scene);
  return it == scenes_.end() ? 0 : it->second.places.size();
}

}  // namespace coop::server
