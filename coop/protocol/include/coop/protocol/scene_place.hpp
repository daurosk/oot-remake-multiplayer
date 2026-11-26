// The place a scene shows, without the era, for the server and the games alike (one home: coop/host/scene.hpp
// and the server both call these). "Graveyard_Adult" and "Graveyard_Child" are the same graveyard seven years
// apart: a child and an adult player there see each other's puppet, so everything between two PLAYERS (their
// sample and pose streams, voice, lifting, PvP, reviving) goes by place. Everything about the WORLD (shared
// objects, flags, world breaks, dungeon events, bosses) keeps the exact scene: the two eras are two worlds.
#pragma once

#include <string_view>

namespace coop::protocol {

// Only a trailing "_Child" or "_Adult" is dropped, never the whole name.
inline std::string_view place_of(std::string_view scene) {
  for (const std::string_view era : {std::string_view("_Child"), std::string_view("_Adult")}) {
    if (scene.size() > era.size() && scene.substr(scene.size() - era.size()) == era) {
      return scene.substr(0, scene.size() - era.size());
    }
  }
  return scene;
}

// True when both scenes are known (not empty) and show the same place, in either era.
inline bool same_place(std::string_view a, std::string_view b) {
  return !a.empty() && !b.empty() && place_of(a) == place_of(b);
}

}  // namespace coop::protocol
