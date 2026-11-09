// Scene rules for drawing other players: which ones share our scene, and
// what to do with their puppet or marker while a cutscene runs. Plan stage 1
// step 3, "Scenes"; see docs/scenes.md.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "coop/protocol/messages.hpp"

namespace coop::host {

// True when both scenes are known and equal. An unknown scene (the reader
// refused it, or no sample has carried one yet) is never the same scene, so
// a player is hidden rather than drawn in the wrong place.
inline bool same_scene(std::optional<std::string_view> a,
                       std::optional<std::string_view> b) {
  return a && b && !a->empty() && *a == *b;
}

// The "in cutscene" bit of a PLAYER_STATUS flags byte. The sender sets it
// from "event running" (event manager +0x68 not zero, sampler.cpp).
inline bool in_cutscene(std::uint8_t status_flags) {
  return (status_flags & protocol::status::in_cutscene) != 0;
}

enum class Visibility : std::uint8_t {
  hide,  // other scene, or scene unknown: do not draw
  hold,  // same scene, but a cutscene runs: draw frozen at the last pose
  show,  // same scene, nobody in a cutscene: draw and move as usual
};

// What to do with one remote player this frame.
// - Different or unknown scene: hide.
// - Our own cutscene running: hide every puppet, so none walks into the
//   game's staged shot (puppets are not actors the event knows about).
// - Their cutscene running: hold, since their samples would show them
//   being moved by an event we do not see.
inline Visibility visibility(std::optional<std::string_view> local_scene,
                             bool local_in_cutscene,
                             std::optional<std::string_view> remote_scene,
                             std::uint8_t remote_status_flags) {
  if (!same_scene(local_scene, remote_scene)) return Visibility::hide;
  if (local_in_cutscene) return Visibility::hide;
  if (in_cutscene(remote_status_flags)) return Visibility::hold;
  return Visibility::show;
}

}  // namespace coop::host
