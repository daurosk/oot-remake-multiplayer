#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>

#include "coop/host/scene.hpp"

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
  CHECK(activity_of(std::string_view("DemoOcarina")) == Activity::none);  // a song taught
  CHECK(activity_of(std::string_view("Ocar")) == Activity::none);
}

TEST_CASE("local_event_hides: every event of ours but the Ocarina") {
  using coop::host::local_event_hides;
  CHECK_FALSE(local_event_hides(false, std::nullopt));
  CHECK_FALSE(local_event_hides(false, std::string_view("HorseMove")));
  CHECK(local_event_hides(true, std::nullopt));
  CHECK(local_event_hides(true, std::string_view("EvtWait")));
  CHECK(local_event_hides(true, std::string_view("DemoOcarina")));
  CHECK_FALSE(local_event_hides(true, std::string_view("OcarinaReadyStart")));
}

TEST_CASE("visibility with their animation: riding shows with a horse (issue 199), the Ocarina shows") {
  const std::string_view a = "HyruleField_Child", b = "LonLonRanch";
  const auto ride = std::optional<std::string_view>("HorseMove");
  const auto play = std::optional<std::string_view>("Ocarina_Adult_B_lp");
  const auto demo = std::optional<std::string_view>("DemoOcarina");
  CHECK(visibility(a, false, a, 0, ride) == Visibility::ride);
  CHECK(visibility(a, false, a, status::in_cutscene, ride) == Visibility::ride);
  CHECK(visibility(a, true, a, 0, ride) == Visibility::hide);   // our cutscene first
  CHECK(visibility(a, false, b, 0, ride) == Visibility::hide);  // elsewhere first
  CHECK(visibility(a, false, a, status::in_cutscene, play) == Visibility::show);
  CHECK(visibility(a, false, a, 0, play) == Visibility::show);
  CHECK(visibility(a, false, a, status::in_cutscene, demo) == Visibility::hold);
  CHECK(visibility(a, false, a, status::in_cutscene, std::nullopt) == Visibility::hold);
  // Paused, in a menu or loading: the bits are not cutscenes; they stand.
  CHECK(visibility(a, false, a, status::paused | status::in_menu | status::loading, std::nullopt) ==
        Visibility::show);
}
