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
  // A `kept` break (a lock) is kept apart: forget() leaves it.
  bool on_break(const protocol::WorldBreak& b);
  // Keeps the object's latest rest place. Always relayed.
  void on_place(const protocol::WorldPlace& p);
  // Keeps the last hits of each object (a cut sign). Always relayed.
  void on_hit(const protocol::WorldHit& h);
  // Keeps the latest cut words of each sign. Always relayed.
  void on_sign_cut(const protocol::SignCut& c);
  // Everything a player entering `scene` must be told, breaks first.
  std::vector<protocol::Message> replay(const std::string& scene) const;
  // Forgets what the game puts back on a reload; kept breaks stay.
  void forget(const std::string& scene);
  // Forgets the kept breaks too: the host loaded a save (host-save's
  // FLAG_RESET), so the locks are as that save has them again.
  void forget_kept() {
    kept_.clear();
    kept_total_ = 0;
  }
  void clear() {
    scenes_.clear();
    forget_kept();
  }

  std::size_t breaks(const std::string& scene) const;
  std::size_t places(const std::string& scene) const;

 private:
  struct Scene {
    std::map<std::uint64_t, protocol::WorldBreak> breaks;  // by placement hash
    std::deque<std::uint64_t> order;                       // oldest break first
    std::map<std::uint64_t, protocol::WorldPlace> places;
    std::map<std::uint64_t, std::deque<protocol::WorldHit>> hits;
    std::map<std::uint64_t, protocol::SignCut> cuts;
  };
  std::map<std::string, Scene> scenes_;
  // Kept breaks (locks), per scene, oldest first: for the whole session.
  std::map<std::string, std::vector<protocol::WorldBreak>> kept_;
  std::size_t kept_total_ = 0;  // at most kMaxWorldKeptTotal
};

}  // namespace coop::server
