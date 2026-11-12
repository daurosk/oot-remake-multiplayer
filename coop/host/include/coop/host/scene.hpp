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

// The place a scene shows, without the era: "Graveyard_Adult" and
// "Graveyard_Child" are the same graveyard seven years apart. Only a trailing
// "_Child" or "_Adult" is dropped.
inline std::string_view place_of(std::string_view scene) {
  for (const std::string_view era : {std::string_view("_Child"), std::string_view("_Adult")}) {
    if (scene.size() > era.size() && scene.substr(scene.size() - era.size()) == era) {
      return scene.substr(0, scene.size() - era.size());
    }
  }
  return scene;
}

// True when both scenes are known and show the same place, in either era: a
// child and an adult player in the two Graveyards see each other's puppet.
// Shared objects and flags still need the same scene (same_scene).
inline bool same_place(std::optional<std::string_view> a,
                       std::optional<std::string_view> b) {
  return a && b && !a->empty() && place_of(*a) == place_of(*b);
}

// The "in cutscene" bit of a PLAYER_STATUS flags byte. The sender sets it
// from "event running" (event manager +0x68 not zero, sampler.cpp).
inline bool in_cutscene(std::uint8_t status_flags) {
  return (status_flags & protocol::status::in_cutscene) != 0;
}

// What a player is doing, as far as drawing their puppet goes, from the AS
// command in their main slot (PlayerSample field 7; docs/puppet.md, "Cases").
// The names are the commands in AS/Player_Child and Player_Adult (romfs,
// read 2026-11-09); that slot 0 plays them by these names is seen for
// "Wait", "Walk", "Run" and "EvtWait", not yet for these.
enum class Activity : std::uint8_t {
  none,
  riding,   // on a horse: "HorseMove", "HorseRide", "Evt_Horse_Talk_N", ...
  ocarina,  // playing the Ocarina freely: "DemoOcarina" (seen: the probe, ocarina-b 2026-11-12),
            // "OcarinaReadyStart", "Ocarina_Adult_B_lp", ...
};

inline Activity activity_of(std::optional<std::string_view> anim) {
  if (!anim) return Activity::none;
  const std::string_view a = *anim;
  // Getting off ends on the ground; walking up to the horse is on foot.
  if (a.find("Horse") != std::string_view::npos && a.substr(0, 7) != "Dismoun" &&
      a != "HorseAccess") {
    return Activity::riding;
  }
  if (a.substr(0, 7) == "Ocarina") return Activity::ocarina;
  // Played freely, the game's own command is "DemoOcarina" (slot 0, with DemoOcarinaFace and
  // DemoOcarinaFinger beside it; coop_ocarina_sound.h counts it the same way). Only a song being
  // taught, "DemoOcarinaMelodyGet...", is a cutscene.
  if (a.substr(0, 11) == "DemoOcarina" && a.find("MelodyGet") == std::string_view::npos) {
    return Activity::ocarina;
  }
  return Activity::none;
}

// Whether our own event hides the other players. Every event does, except
// the Ocarina played freely: the game runs it as an event (inferred), and
// nothing is staged there that a puppet could walk into.
inline bool local_event_hides(bool event_running, std::optional<std::string_view> own_anim) {
  return event_running && activity_of(own_anim) != Activity::ocarina;
}

enum class Visibility : std::uint8_t {
  hide,  // other scene, scene unknown, or our cutscene: no puppet, no marker
  ride,  // same place, on a horse: draw and move as usual, with a copy of
         // their horse under the puppet (issue 199; lichii/hook/coop_ride.h)
  hold,  // same place, but their cutscene runs: animation frozen at the last
         // frame, place followed (issue 194, docs/puppet.md)
  show,  // same place, nobody in a cutscene: draw and move as usual
};

// What to do with one remote player this frame.
// - Different or unknown place (same_place: either era counts): hide.
// - Our own cutscene running (`local_hides`, see local_event_hides): hide
//   every puppet, so none walks into the game's staged shot (puppets are not
//   actors the event knows about).
// - They ride a horse: ride. The puppet shows in its riding animation and a
//   copy of their horse is drawn under it (issue 199: parking it made a rider
//   vanish for the others). Also in a talk on horseback (their cutscene).
// - Their cutscene running: hold, since their animation is the event's, which
//   the puppet may not have; their place is still followed (issue 194). Not while they play the Ocarina: then
//   they stand and play, and show copies that.
inline Visibility visibility(std::optional<std::string_view> local_scene,
                             bool local_hides,
                             std::optional<std::string_view> remote_scene,
                             std::uint8_t remote_status_flags,
                             std::optional<std::string_view> remote_anim = std::nullopt) {
  if (!same_place(local_scene, remote_scene)) return Visibility::hide;
  if (local_hides) return Visibility::hide;
  const Activity theirs = activity_of(remote_anim);
  if (theirs == Activity::riding) return Visibility::ride;
  if (in_cutscene(remote_status_flags) && theirs != Activity::ocarina) return Visibility::hold;
  return Visibility::show;
}

}  // namespace coop::host
