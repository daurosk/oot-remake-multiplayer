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
