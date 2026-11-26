#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/event_owner.hpp"
#include "coop/host/sampler.hpp"
#include "coop/host/scene.hpp"
#include "coop/protocol/codec.hpp"

using namespace coop::host;
namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kMs = 1'000;
constexpr std::uint64_t kStart = 10'000'000;

PlayerReading reading(bool event, EventWhose whose) {
  PlayerReading r;
  r.position = {1.0f, 0.0f, 0.0f};
  r.rotation = Quat{0, 0, 0, 1};
  r.age = proto::Age::adult;
  r.scene = "spot00";
  r.event_running = event;
  r.event_whose = whose;
  return r;
}

// The PLAYER_STATUS flags the sampler sends for one reading (after a first, event-free frame).
std::uint8_t sent_flags(const PlayerReading& r) {
  Sampler s;
  s.frame(reading(false, EventWhose::ours), kStart);
  s.frame(r, kStart + 17 * kMs);
  return s.status_flags();
}

}  // namespace

TEST_CASE("event owner: no event, our own event, a world event that does not take our Link") {
  using By = EventOwnerTracker::By;
  EventOwnerTracker t;
  CHECK(t.Frame(false, false) == EventWhose::ours);  // no event: what event_running meant before
  CHECK(t.Decided() == By::none);
  // Our Link a member (a talk of ours, the ~1 s event after a scene load): ours.
  CHECK(t.Frame(true, true) == EventWhose::ours);
  CHECK(t.Decided() == By::member);
  t.Frame(false, false);
  t.GameAsked("Npc_Talk_Something");
  CHECK(t.Frame(true, true) == EventWhose::ours);
  t.Frame(false, false);
  // An event runs and our Link is not a member: it does not hold our Link, whoever asked for it.
  CHECK(t.Frame(true, false) == EventWhose::others);
}

TEST_CASE("event owner: issue 251, another player's event replayed here is not ours") {
  EventOwnerTracker t;
  t.Frame(false, false);
  // A held live event, taken: our Link is not a member of it.
  CHECK(t.Frame(true, false) == EventWhose::others);
  // The whole run stays another player's.
  CHECK(t.Frame(true, false) == EventWhose::others);
  // The event ends; the next one (our own talk) is ours.
  CHECK(t.Frame(false, false) == EventWhose::ours);
  CHECK(t.Frame(true, true) == EventWhose::ours);
  // The sites: the replay hides nobody, tells nobody "in a cutscene", holds nothing near our Link.
  CHECK_FALSE(our_link_in_event(true, EventWhose::others));
  CHECK(event_in_our_game(true));  // the game is busy all the same: no second event asked
  CHECK_FALSE(local_event_hides(true, EventWhose::others, std::string_view("Wait")));
  CHECK(local_event_hides(true, EventWhose::ours, std::string_view("EvtWait")));
  CHECK((sent_flags(reading(true, EventWhose::others)) & proto::status::in_cutscene) == 0);
  CHECK((sent_flags(reading(true, EventWhose::ours)) & proto::status::in_cutscene) != 0);
}

TEST_CASE("event owner: no clock: a replay that takes our Link is ours, a long one of another player's stays theirs") {
  // The old rule called an event that started within 3 s of a co-op ask another player's, and any
  // later one ours. Both cases below failed under it.
  EventOwnerTracker t;
  // A shared story scene joined right after our ask (the old window said "another player's"):
  // the game makes our Link a member of it, so it holds our Link: ours from that frame on.
  CHECK(t.Frame(true, false) == EventWhose::others);
  CHECK(t.Frame(true, true) == EventWhose::ours);
  CHECK(t.Frame(true, true) == EventWhose::ours);
  // ... and when the scene lets our Link go before it ends, not ours any more.
  CHECK(t.Frame(true, false) == EventWhose::others);
  t.Frame(false, false);
  // Another player's event that starts late (a slow load; the old window said "ours") and runs
  // for many seconds: our Link is never a member, so it is another player's all the way.
  for (int frame = 0; frame < 60 * 20; ++frame) {
    CAPTURE(frame);
    REQUIRE(t.Frame(true, false) == EventWhose::others);
  }
  // A request of the game's own in it (not the Ocarina) changes nothing by itself: membership decides.
  t.GameAsked("Npc_Kokiri_Talk");
  CHECK(t.Frame(true, false) == EventWhose::others);
  CHECK(t.Frame(true, true) == EventWhose::ours);
}

TEST_CASE("event owner: the revive and PvP rules (G1 review)") {
  // DBNO revive waits in our own event and the Ocarina (our Link is held by the game's event),
  // not in another player's event replayed here (our Link walks freely). PvP: our Link's event only.
  // The Reviver's gate is event_blocks_revive (coop_dbno_game.cpp), tested here as it runs.
  CHECK(event_blocks_revive(true, EventWhose::ours));
  CHECK(event_blocks_revive(true, EventWhose::ocarina));  // the free Ocarina keeps its A button
  CHECK_FALSE(event_blocks_revive(true, EventWhose::others));
  CHECK_FALSE(event_blocks_revive(false, EventWhose::ours));
  CHECK_FALSE(our_link_in_event(true, EventWhose::ocarina));  // PvP: hittable while playing
}

TEST_CASE("event owner: a downed Link's body follows only our Link's own event (down-crawl + G1)") {
  // body.event (coop_dbno_game.cpp) = our_link_in_event: our own event drives the downed body;
  // another player's replay and our free Ocarina leave it crawling.
  CHECK(our_link_in_event(true, EventWhose::ours));
  CHECK_FALSE(our_link_in_event(true, EventWhose::others));
  CHECK_FALSE(our_link_in_event(true, EventWhose::ocarina));
  CHECK_FALSE(our_link_in_event(false, EventWhose::ours));
}

TEST_CASE("event owner: issue 86, the free Ocarina's take-out holds and hides nobody") {
  using By = EventOwnerTracker::By;
  EventOwnerTracker t;
  t.Frame(false, false);
  t.GameAsked("Dm_SY_Ocarina");  // the game's own setting for the free Ocarina
  // The offline run fable-g1-events: the take-out makes our Link an event member from its first
  // frame, and its first ~4 s our Link's animation is a plain EvtWait. The request says it all the
  // same, from the first frame.
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ocarina);
  CHECK(t.Decided() == By::request);
  CHECK(t.Frame(true, true, std::string_view("DemoOcarina")) == EventWhose::ocarina);
  CHECK((sent_flags(reading(true, EventWhose::ocarina)) & proto::status::in_cutscene) == 0);
  CHECK_FALSE(local_event_hides(true, EventWhose::ocarina, std::string_view("EvtWait")));
  // A song's reaction is a real event: ours from its request on.
  t.GameAsked("Npc_Bonooru_BlewOcarinaByPlayer");
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ours);
  CHECK(t.Decided() == By::member);
  t.Frame(false, false);
  // The request is for the run it starts only: the next run is decided again.
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ours);
  t.Frame(false, false);
  // Without the wrap (the emulator): our Link's Ocarina animation alone, as before.
  EventOwnerTracker e;
  CHECK(e.Frame(true, true, std::string_view("DemoOcarina")) == EventWhose::ocarina);
  CHECK(e.Decided() == By::animation);
  CHECK(e.Frame(true, true, std::string_view("DemoOcarinaMelodyGet_Saria")) == EventWhose::ours);
  CHECK(free_ocarina_event("PlayOcarina_Base"));
  CHECK_FALSE(free_ocarina_event("Dm_SY_Ocarina_move"));  // a warp song's travel: a cutscene
}

TEST_CASE("event owner: a request taken in a run decides that run only, never the next one") {
  // Review of c431aafc: the free Ocarina taken out while another event runs (the ~1 s event after a
  // scene load, a talk) set the pending request, nothing cleared it, and the NEXT run, minutes later,
  // was classified `ocarina`: no hide, no hold, no in_cutscene for our own cutscene.
  EventOwnerTracker t;
  t.Frame(false, false);
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ours);  // the load's event
  t.GameAsked("Dm_SY_Ocarina");  // the take-out, taken in that run
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ocarina);
  CHECK(t.Frame(false, false) == EventWhose::ours);  // the run ends
  for (int i = 0; i < 600; ++i) t.Frame(false, false);
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ours);  // our own cutscene
  CHECK(t.Decided() == EventOwnerTracker::By::member);
  t.Frame(false, false);
  // The same with a request that is not the Ocarina's (a song's reaction): the next run is decided
  // by our Link's membership, not by that request.
  t.Frame(true, false);
  t.GameAsked("Npc_Bonooru_BlewOcarinaByPlayer");
  CHECK(t.Frame(true, false) == EventWhose::others);
  t.Frame(false, false);
  CHECK(t.Frame(true, false) == EventWhose::others);
  CHECK(t.Decided() == EventOwnerTracker::By::member);
  t.Frame(false, false);
  // A request taken in the frame the run ends starts the next run: kept over the gap.
  t.Frame(true, true);
  t.GameAsked("Dm_SY_Ocarina");
  CHECK(t.Frame(false, false) == EventWhose::ours);
  CHECK(t.Frame(false, false) == EventWhose::ours);
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ocarina);
  CHECK(t.Decided() == EventOwnerTracker::By::request);
  t.Frame(false, false);
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ours);  // used once
}

TEST_CASE("event owner: a request with no run is dropped when the scene changes (review of e3542a8a)") {
  EventOwnerTracker t;
  t.Scene("InsideTheDekuTree");
  t.Frame(false, false);
  t.GameAsked("Dm_SY_Ocarina");  // taken, but its run never reads as running here
  CHECK(t.Frame(false, false) == EventWhose::ours);
  t.Scene("KokiriForest");  // a load: every event the manager held ends
  CHECK(t.Frame(false, false) == EventWhose::ours);
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ours);  // the load's event, not the Ocarina
  CHECK(t.Decided() == EventOwnerTracker::By::member);
  t.Frame(false, false);
  // The same scene keeps it over the gap, and a request taken in the load's own frame starts that scene's run.
  t.GameAsked("Dm_SY_Ocarina");
  t.Scene("KokiriForest");
  t.Frame(false, false);
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ocarina);
  t.Frame(false, false);
  t.Scene("InsideTheDekuTree");
  t.GameAsked("Dm_SY_Ocarina");  // fed after the scene, as the hook does
  CHECK(t.Frame(true, true, std::string_view("EvtWait")) == EventWhose::ocarina);
}

TEST_CASE("event owner: issues 144 and 243, the scrub holds follow only our Link's own event") {
  // The hook's scrub talk hold, sale hold and Scrub Brothers' hit hold take our_link_in_event:
  // a scrub's talk of ours holds; another player's dungeon event replayed here does not delay
  // another game's scrub flags or hits (they come at once, as without any event).
  CHECK(our_link_in_event(true, EventWhose::ours));
  CHECK_FALSE(our_link_in_event(true, EventWhose::others));
  CHECK_FALSE(our_link_in_event(true, EventWhose::ocarina));
  CHECK_FALSE(our_link_in_event(false, EventWhose::ours));
}

TEST_CASE("event owner: issue 200, only the game whose event it is hides others for it") {
  // Another player's event here: the other players stay drawn as usual (show), our own hides them.
  CHECK(visibility(std::string_view("spot00"), local_event_hides(true, EventWhose::others, std::nullopt),
                   std::string_view("spot00"), 0) == Visibility::show);
  CHECK(visibility(std::string_view("spot00"), local_event_hides(true, EventWhose::ours, std::nullopt),
                   std::string_view("spot00"), 0) == Visibility::hide);
}
