#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "coop/host/event_owner.hpp"
#include "coop/host/sampler.hpp"
#include "coop/host/scene.hpp"
#include "coop/protocol/messages.hpp"

using coop::host::in_cutscene;
using coop::host::same_scene;
using coop::host::Visibility;
using coop::host::visibility;
namespace status = coop::protocol::status;

TEST_CASE("same_scene needs two known, equal, non-empty scenes") {
  CHECK(same_scene("KokiriForest_Child", "KokiriForest_Child"));
  CHECK_FALSE(same_scene("KokiriForest_Child", "HyruleField_Child"));
  CHECK_FALSE(same_scene("HyruleField_Child", "HyruleField_Adult"));
  CHECK_FALSE(same_scene(std::nullopt, "KokiriForest_Child"));
  CHECK_FALSE(same_scene("KokiriForest_Child", std::nullopt));
  CHECK_FALSE(same_scene(std::nullopt, std::nullopt));
  CHECK_FALSE(same_scene("", ""));
}

TEST_CASE("in_cutscene reads only bit 2") {
  CHECK(in_cutscene(status::in_cutscene));
  CHECK(in_cutscene(status::in_cutscene | status::paused | 0x80));
  CHECK_FALSE(in_cutscene(0));
  CHECK_FALSE(in_cutscene(status::paused | status::loading | status::in_menu | status::dead));
}

TEST_CASE("visibility") {
  const std::string_view a = "KokiriForest_Child", b = "HyruleField_Child";
  CHECK(visibility(a, false, a, 0) == Visibility::show);
  CHECK(visibility(a, false, a, status::in_menu) == Visibility::show);
  CHECK(visibility(a, false, a, status::in_cutscene) == Visibility::hold);
  CHECK(visibility(a, true, a, 0) == Visibility::hide);
  CHECK(visibility(a, true, a, status::in_cutscene) == Visibility::hide);
  CHECK(visibility(a, false, b, 0) == Visibility::hide);
  CHECK(visibility(std::nullopt, false, a, 0) == Visibility::hide);
  CHECK(visibility(a, false, std::nullopt, 0) == Visibility::hide);
}

TEST_CASE("the same place in either era shows the puppet") {
  CHECK(std::string(coop::host::place_of("Graveyard_Adult")) == "Graveyard");
  CHECK(std::string(coop::host::place_of("Graveyard_Child")) == "Graveyard");
  CHECK(std::string(coop::host::place_of("WaterTemple")) == "WaterTemple");
  CHECK(std::string(coop::host::place_of("_Adult")) == "_Adult");
  CHECK(coop::host::same_place("HyruleField_Child", "HyruleField_Adult"));
  CHECK_FALSE(coop::host::same_place("HyruleField_Child", "KokiriForest_Adult"));
  CHECK_FALSE(coop::host::same_place("", ""));
  CHECK_FALSE(coop::host::same_place(std::nullopt, "Graveyard_Adult"));
  CHECK(coop::host::visibility(std::string_view("Graveyard_Child"), false,
                               std::string_view("Graveyard_Adult"), 0) ==
        coop::host::Visibility::show);
}

TEST_CASE("activity_of: riding and the Ocarina by the AS command name") {
  using coop::host::Activity;
  using coop::host::activity_of;
  CHECK(activity_of(std::nullopt) == Activity::none);
  CHECK(activity_of(std::string_view("Wait")) == Activity::none);
  CHECK(activity_of(std::string_view("EvtWait")) == Activity::none);
  CHECK(activity_of(std::string_view("HorseMove")) == Activity::riding);
  CHECK(activity_of(std::string_view("HorseRideOnL_F")) == Activity::riding);
  CHECK(activity_of(std::string_view("Evt_Horse_Talk_N")) == Activity::riding);
  CHECK(activity_of(std::string_view("AimHorseLowerF")) == Activity::riding);
  CHECK(activity_of(std::string_view("Damage_Though_Horse")) == Activity::riding);
  CHECK(activity_of(std::string_view("DismountHorse")) == Activity::none);  // ends on foot
  CHECK(activity_of(std::string_view("DismounHorse")) == Activity::none);
  CHECK(activity_of(std::string_view("HorseAccess")) == Activity::none);  // walking up to it
  CHECK(activity_of(std::string_view("OcarinaReadyStart")) == Activity::ocarina);
  CHECK(activity_of(std::string_view("Ocarina_Adult_B_lp")) == Activity::ocarina);
  // ocarina-b (2026-11-12): free play is "DemoOcarina"; a song taught is "DemoOcarinaMelodyGet".
  CHECK(activity_of(std::string_view("DemoOcarina")) == Activity::ocarina);
  CHECK(activity_of(std::string_view("DemoOcarinaMelodyGet")) == Activity::none);
  CHECK(activity_of(std::string_view("DemoOcarinaFace")) == Activity::ocarina);
  CHECK(activity_of(std::string_view("Ocar")) == Activity::none);
}

TEST_CASE("local_event_hides: every event of ours but the Ocarina") {
  using coop::host::local_event_hides;
  CHECK_FALSE(local_event_hides(false, std::nullopt));
  CHECK_FALSE(local_event_hides(false, std::string_view("HorseMove")));
  CHECK(local_event_hides(true, std::nullopt));
  CHECK(local_event_hides(true, std::string_view("EvtWait")));
  CHECK_FALSE(local_event_hides(true, std::string_view("DemoOcarina")));  // playing freely (#86)
  CHECK(local_event_hides(true, std::string_view("DemoOcarinaMelodyGet")));  // a song taught
  CHECK_FALSE(local_event_hides(true, std::string_view("OcarinaReadyStart")));
}

TEST_CASE("visibility with their animation: riding shows with a horse (issue 199), the Ocarina shows") {
  const std::string_view a = "HyruleField_Child", b = "LonLonRanch";
  const auto ride = std::optional<std::string_view>("HorseMove");
  const auto play = std::optional<std::string_view>("Ocarina_Adult_B_lp");
  const auto demo = std::optional<std::string_view>("DemoOcarina");
  const auto taught = std::optional<std::string_view>("DemoOcarinaMelodyGet");
  CHECK(visibility(a, false, a, 0, ride) == Visibility::ride);
  CHECK(visibility(a, false, a, status::in_cutscene, ride) == Visibility::ride);
  CHECK(visibility(a, true, a, 0, ride) == Visibility::hide);   // our cutscene first
  CHECK(visibility(a, false, b, 0, ride) == Visibility::hide);  // elsewhere first
  CHECK(visibility(a, false, a, status::in_cutscene, play) == Visibility::show);
  CHECK(visibility(a, false, a, 0, play) == Visibility::show);
  CHECK(visibility(a, false, a, status::in_cutscene, demo) == Visibility::show);  // free play (#86)
  CHECK(visibility(a, false, a, status::in_cutscene, taught) == Visibility::hold);
  CHECK(visibility(a, false, a, status::in_cutscene, std::nullopt) == Visibility::hold);
  // Paused, in a menu or loading: the bits are not cutscenes; they stand.
  CHECK(visibility(a, false, a, status::paused | status::in_menu | status::loading, std::nullopt) ==
        Visibility::show);
}

namespace {

// One game's state this frame, as the hook reads it (coop_hook.cpp's view loop).
struct Side {
  std::optional<std::string> scene = std::string("spot04");  // Kokiri Forest
  bool event = false;                                          // our Link in an event (a talk, a sign, ...)
  std::optional<std::string> anim;                             // the main AS command
  bool menu = false;
  bool dead = false;
  bool loading = false;  // no Link read: a scene or save loading
};

std::optional<coop::host::PlayerReading> reading_of(const Side& s) {
  if (s.loading) return std::nullopt;
  coop::host::PlayerReading r;
  r.scene = s.scene;
  r.event_running = s.event;
  r.event_whose = coop::host::EventWhose::ours;
  if (s.anim) {
    r.animation.emplace();
    r.animation->name = *s.anim;
  }
  r.in_menu = s.menu;
  r.dead = s.dead;
  return r;
}

struct Decision {
  bool puppet_hidden;
  bool voice_heard;
};

// The hook's rule for one remote player: the listener `ours`, the talker `theirs` (their flags as they send them).
Decision decide(const Side& ours, const Side& theirs) {
  const auto me = reading_of(ours);
  const bool local_hides =
      me && coop::host::local_event_hides(me->event_running.value_or(false), me->event_whose,
                                          coop::host::name_of(me->animation));
  const std::uint8_t their_flags = coop::host::status_bits(0, reading_of(theirs), false);
  // (made in place, scene.hpp's view_of note: no conditional optionals)
  std::optional<std::string_view> our_scene;
  if (me && me->scene) our_scene.emplace(*me->scene);
  std::optional<std::string_view> their_scene;
  if (!theirs.loading && theirs.scene) their_scene.emplace(*theirs.scene);
  return {coop::host::visibility(our_scene, local_hides, their_scene, their_flags) == Visibility::hide,
          coop::host::voice_heard(our_scene, their_scene)};
}

}  // namespace

TEST_CASE("voice_heard: the place alone, no event (row 155: the voice stopped in an NPC talk)") {
  const Side idle;
  // The report: the listener talks with an NPC. The puppet hides (it must not walk into the shot), the voice goes on.
  Side talk;
  talk.event = true;
  talk.anim = "EvtWait";
  CHECK(decide(talk, idle).puppet_hidden);  // (was: the voice hid with it)
  CHECK(decide(talk, idle).voice_heard);
  // The talker in the talk: held for the puppet, heard.
  CHECK_FALSE(decide(idle, talk).puppet_hidden);
  CHECK(decide(idle, talk).voice_heard);
  // Both in their own talks.
  CHECK(decide(talk, talk).voice_heard);
}

TEST_CASE("voice_heard: signs, shops, item gets and cutscenes are events like a talk, on either side") {
  const Side idle;
  for (const char* anim : {"EvtWait", "ItemGet", "DemoOcarinaMelodyGet", "Wait"}) {
    Side ev;
    ev.event = true;
    ev.anim = std::string(anim);
    CAPTURE(anim);
    CHECK(decide(ev, idle).voice_heard);
    CHECK(decide(idle, ev).voice_heard);
  }
  Side no_anim;  // a cutscene with no command read
  no_anim.event = true;
  CHECK(decide(no_anim, idle).voice_heard);
  CHECK(decide(idle, no_anim).voice_heard);
}

TEST_CASE("voice_heard: the pause menu, Epona, being downed") {
  const Side idle;
  Side paused;
  paused.menu = true;
  CHECK(decide(paused, idle).voice_heard);
  CHECK(decide(idle, paused).voice_heard);
  Side epona;
  epona.anim = "HorseRide";
  CHECK(decide(epona, idle).voice_heard);
  CHECK(decide(idle, epona).voice_heard);
  Side epona_talk = epona;  // a talk on horseback
  epona_talk.event = true;
  epona_talk.anim = "Evt_Horse_Talk_N";
  CHECK(decide(epona_talk, idle).voice_heard);
  CHECK(decide(idle, epona_talk).voice_heard);
  Side downed;
  downed.dead = true;
  CHECK(decide(downed, idle).voice_heard);
  CHECK(decide(idle, downed).voice_heard);
}

TEST_CASE("voice_heard: another scene or a scene loading is not heard, in either era the same place is") {
  const Side idle;
  Side field;
  field.scene = std::string("spot00");
  CHECK_FALSE(decide(field, idle).voice_heard);
  CHECK_FALSE(decide(idle, field).voice_heard);
  Side loading;
  loading.loading = true;
  CHECK_FALSE(decide(loading, idle).voice_heard);
  CHECK_FALSE(decide(idle, loading).voice_heard);
  CHECK(coop::host::voice_heard("Graveyard_Adult", "Graveyard_Child"));
  CHECK_FALSE(coop::host::voice_heard("", ""));
}

TEST_CASE("voice in a talk: the talker's place keeps going out (fake clock), so its voice stays placed") {
  // The listener places a voice at the talker's presence pose; a pose with no sample for 400 ms fades out.
  // A talker standing still in a talk still sends one at least every keep-alive.
  coop::host::Sampler sampler;
  Side talk;
  talk.event = true;
  talk.anim = "EvtWait";
  const auto r = reading_of(talk);
  constexpr std::uint64_t kStart = 1'000'000, kFrameUs = 16'667;
  std::uint64_t last = 0;
  std::uint64_t longest = 0;
  for (std::uint64_t i = 0; i < 300; ++i) {  // 5 s of frames
    const std::uint64_t now = kStart + i * kFrameUs;
    for (const auto& o : sampler.frame(r, now)) {
      if (o.channel != coop::protocol::kChannelPlayerSamples) continue;
      if (last != 0) longest = std::max(longest, now - last);
      last = now;
    }
  }
  CHECK(last != 0);
  CHECK(longest <= 250'000);
}
