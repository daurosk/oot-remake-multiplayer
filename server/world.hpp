// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the server keeps of the shared world objects (docs/world.md): for
// each scene with a player in it, the placed breakables that were broken
// (WORLD_BREAK) and where moved objects came to rest (WORLD_PLACE). A player
// who enters the scene gets both; a scene nobody is in is forgotten, as the
// game itself puts everything back when a scene is loaded again.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::server {

class WorldStore {
 public:
  // Keeps a break; with `restored` forgets it. False when nothing changed
  // (the scene has this break already, or had none to forget): not relayed.
  bool on_break(const protocol::WorldBreak& b);
  // Keeps the object's latest rest place. Always relayed.
  void on_place(const protocol::WorldPlace& p);
  // Keeps the last hits of each object (a cut sign). Always relayed.
  void on_hit(const protocol::WorldHit& h);
  // Everything a player entering `scene` must be told, breaks first.
  std::vector<protocol::Message> replay(const std::string& scene) const;
  void forget(const std::string& scene);
  void clear() { scenes_.clear(); }

  std::size_t breaks(const std::string& scene) const;
  std::size_t places(const std::string& scene) const;

 private:
  struct Scene {
    std::map<std::uint64_t, protocol::WorldBreak> breaks;  // by placement hash
    std::deque<std::uint64_t> order;                       // oldest break first
    std::map<std::uint64_t, protocol::WorldPlace> places;
    std::map<std::uint64_t, std::deque<protocol::WorldHit>> hits;
  };
  std::map<std::string, Scene> scenes_;
};

}  // namespace coop::server
