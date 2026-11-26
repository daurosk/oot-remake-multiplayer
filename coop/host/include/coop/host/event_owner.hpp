// Whose event runs in our game (G1: issues 251, 243, 200, 144, 86; docs/scenes.md "Whose event").
//
// One rule for every site that holds, hides, freezes or hands over something
// because "an event runs". Before, all of them read one bit, "an event runs in
// our game" (PlayerReading::event_running), and so treated three different
// events the same:
//   ours    - an event our own Link is a member of (the game's own member
//             test): a talk, a cutscene our game started, a shared story
//             scene that takes our Link.
//   others  - an event our Link is not a member of (the game's own member
//             test): another player's event that our co-op layer asked our
//             game to play (a live dungeon event, an eye switch's event, a
//             shared scene joined), or any other event that does not take our
//             Link.
//   ocarina - our Link playing the Ocarina freely (the game's event setting
//             Dm_SY_Ocarina / PlayOcarina_Base, read from RSDB EventSetting):
//             an event in the game's terms, but nothing is staged and our Link
//             stands in the open.
// What each kind means for a site:
//   - Hides the other players' puppets, tells the others "in a cutscene"
//     (their games hold our puppet), holds shared changes near our Link,
//     holds Deku Scrub flags and hits, claims townspeople and guessed actors:
//     only `ours` (our_link_in_event).
//   - The actors the running event binds in OUR game act for it here (no
//     owner's motion on them, their changes wait): any kind, since the event
//     in our game waits on them whoever asked for it (event_in_our_game).
//     Only `ours` also claims them from their owner (hands them over): a
//     replay of another player's event leaves them with that player.
//   - "The game is busy" (no second event asked, no quest catch-up, no time
//     nudge): any kind (event_in_our_game).
//   - No revive (event_blocks_revive): ours or ocarina: a revive asks no event; a replay
//     leaves our Link free.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/scene.hpp"

namespace coop::host {

enum class EventWhose : std::uint8_t { ours, others, ocarina };

// The game's own event settings for the Ocarina played freely (RSDB EventSetting, flow Dm_SY_Ocarina,
// base PlayerPos). A song's reaction (BlewOcarinaByPlayer, a warp's Dm_SY_Ocarina_move, a gimmick's
// *_Ocarina) is another setting: a real event, ours.
inline bool free_ocarina_event(std::string_view setting) {
  return setting == "Dm_SY_Ocarina" || setting == "PlayOcarina_Base";
}

inline const char* whose_name(EventWhose w) {
  switch (w) {
    case EventWhose::ours: return "ours";
    case EventWhose::others: return "another player's";
    case EventWhose::ocarina: return "our free Ocarina";
  }
  return "?";
}

// Our Link is in the event that runs: the hold/hide/tell sites.
inline bool our_link_in_event(bool event_running, EventWhose whose) {
  return event_running && whose == EventWhose::ours;
}

// The game runs an event of any kind: the "game busy" sites and the event's own actors here.
inline bool event_in_our_game(bool event_running) { return event_running; }

// The Reviver (coop_dbno_game.cpp) takes no revive while an event holds our Link or our hands:
// our own event, or our free Ocarina (its A button stays the Ocarina's). Another player's event
// replayed here leaves our Link free, so it may revive.
inline bool event_blocks_revive(bool event_running, EventWhose whose) {
  return event_running && whose != EventWhose::others;
}

// local_event_hides with the owner: only our own event hides the others (not another player's
// replayed here, not the Ocarina by its event or by our Link's animation).
inline bool local_event_hides(bool event_running, EventWhose whose, const std::optional<std::string_view>& own_anim) {
  return our_link_in_event(event_running, whose) && local_event_hides(true, own_anim);
}

// Once a frame (the hook's present thread): which kind the running event is. From the game's own
// facts only: no clock, no event names, no distance.
// - Frame: `running` is PlayerReading::event_running (the event word, or our Link an event member),
//   `link_member` our Link is an event member by the game's own test (PlayerReading::link_bound:
//   QueryEventMemberIsEventMemver, a member slot with its flag bit; read in the offline run
//   fable-g1-events, where our Link was a member in every event of its own: the ~1 s event after a
//   scene load, a death with the revival fairy, the Ocarina take-out), `own_anim` our Link's main
//   AS command (the Ocarina's animation counts as the Ocarina where the wrap gives no name: the
//   emulator).
//   - our free Ocarina: the game's own request for it was the last one the manager took in this run
//     (GameAsked), or our Link plays the Ocarina's animation. Checked first: the take-out makes our
//     Link an event member too, but nothing is staged and our Link stands in the open.
//   - ours: our Link is an event member.
//   - another player's: an event runs and our Link is not a member of it. Our game plays it (a
//     replay the co-op layer asked for, a shared scene joined), or it is an event of the world
//     that does not take our Link: either way it does not hold our Link.
//   Each frame is classified on its own, so an event that takes our Link part-way is ours from
//   then on, and one that lets it go is not.
// - GameAsked: the game's own request the manager took (Hookshot's request wrap,
//   CoopLiveEvents::GameRequests). The free Ocarina's setting makes the run `ocarina` until another
//   request is taken in it (a song's reaction, a warp). A request taken before the run starts is
//   the one that starts it (the wrap sees only taken requests). A request is used by the next
//   Frame (the hook feeds the frame's requests first): by the run of that frame, or kept only until
//   the next run starts when no event runs; never by a later run.
// Pure: tested in tests/host/event_owner_test.cpp.
class EventOwnerTracker {
 public:
  enum class By : std::uint8_t { none, member, request, animation };

  // Kept for the next Frame only, which applies it to the frame's run (review of c431aafc: a request
  // applied in a run used to stay pending and decide the run after it, even minutes later).
  void GameAsked(std::string_view name) {
    asked_ocarina_ = free_ocarina_event(name);
    asked_ = true;
  }

  // Our scene this frame (before Frame). A request still waiting for its run when the scene changes is
  // dropped: a load ends every event the manager held, so no run after it was started by that request
  // (review of e3542a8a: a taken request whose run never read as running stayed pending until any later
  // run, even minutes later, and could mark it our free Ocarina). A fact, not a clock.
  void Scene(std::string_view scene) {
    if (!scene_.empty() && scene != scene_) asked_ = false;
    scene_ = std::string(scene);
  }

  EventWhose Frame(bool running, bool link_member, const std::optional<std::string_view>& own_anim = std::nullopt) {
    if (!running) {
      running_ = false;
      ocarina_ = false;
      by_ = By::none;
      return EventWhose::ours;
    }
    if (!running_) {  // the run starts: the request taken for it, if the wrap saw one
      running_ = true;
      ocarina_ = asked_ && asked_ocarina_;
    } else if (asked_) {  // taken in the run: a song's reaction ends the free play; a new free play starts one
      ocarina_ = asked_ocarina_;
    }
    asked_ = false;  // a request is used by the run of the frame it was taken in, or the run it starts
    if (ocarina_) {
      by_ = By::request;
      return EventWhose::ocarina;
    }
    if (activity_of(own_anim) == Activity::ocarina) {
      by_ = By::animation;
      return EventWhose::ocarina;
    }
    by_ = By::member;
    return link_member ? EventWhose::ours : EventWhose::others;
  }

  // What decided the running event's kind: our Link's membership, the game's request or our Link's
  // animation (both the free Ocarina).
  By Decided() const { return by_; }

 private:
  std::string scene_;
  bool asked_ = false;
  bool asked_ocarina_ = false;
  bool running_ = false;
  bool ocarina_ = false;
  By by_ = By::none;
};

inline const char* by_name(EventOwnerTracker::By b) {
  switch (b) {
    case EventOwnerTracker::By::none: return "no event";
    case EventOwnerTracker::By::member: return "by our Link's event membership";
    case EventOwnerTracker::By::request: return "by the game's request";
    case EventOwnerTracker::By::animation: return "by our Link's animation";
  }
  return "?";
}

}  // namespace coop::host
