#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "coop/overlay/map_icons.hpp"

using namespace coop::overlay::map;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// A quaternion turning about +Y by `a` radians.
Quat yaw(float a) { return {0.0f, std::sin(a / 2), 0.0f, std::cos(a / 2)}; }

Player at(std::uint8_t n, float x, float y, float z, std::string scene = "HyruleField") {
  Player p;
  p.number = n;
  p.position = {x, y, z};
  p.scene = std::move(scene);
  p.rgb = 0x102030u * n;
  return p;
}

// 200 x 200 pixels at (1000, 600), 100 m across: 2 pixels a metre.
Minimap square() {
  Minimap m;
  m.box = {1000, 600, 200, 200};
  m.metres_across = 100;
  return m;
}

const char* kText =
    "# made by map_captures.py\n"
    "map KokiriForest -5 -12 120 300 0 KokiriForest_Child,KokiriForest_Adult -95:- -20:-\n"
    "map HyruleField -23 197 200 736 0 HyruleField_Child,HyruleField_Adult\n"
    "map InsideJabuJabuBelly -1 -92 200 328 1 InsideJabuJabuBelly -:- 93:105 128:133\n"
    "map HyruleCastle 0 -80 200 242 0 HyruleCastle,GanonCastle\n"
    "map GanonCastle 61 297 200 268 1 GanonCastle 55:216\n";

}  // namespace

TEST_CASE("coop_maps.txt lines parse, bad ones are counted and skipped") {
  int bad = -1;
  const auto caps = parse_captures(std::string(kText) +
                                       "map Broken 1 2 3\n"                 // too short
                                       "map Zero 0 0 200 0 0 Zero\n"         // no height
                                       "map BadFloor 0 0 200 10 1 X 3\n"     // floor without ':'
                                       "map NaN nan 0 200 10 0 X\n"          // not finite
                                       "nope 1 2 3 4 5 6 7\n",
                                   &bad);
  CHECK(bad == 5);
  REQUIRE(caps.size() == 5);
  CHECK(caps[0].location == "KokiriForest");
  CHECK(caps[0].scenes == std::vector<std::string>{"KokiriForest_Child", "KokiriForest_Adult"});
  CHECK(caps[0].height == 300.0f);
  CHECK_FALSE(caps[0].dungeon);
  REQUIRE(caps[0].floors.size() == 2);
  CHECK(*caps[0].floors[0].near_m == -95.0f);
  CHECK_FALSE(caps[0].floors[0].far_m);
  CHECK(caps[1].floors.size() == 1);  // none listed: one uncut floor
  CHECK(caps[2].dungeon);
  REQUIRE(caps[2].floors.size() == 3);
  CHECK_FALSE(caps[2].floors[0].near_m);
  CHECK(*caps[2].floors[2].far_m == 133.0f);
}

TEST_CASE("a scene finds its map by location, scene name, or without its era") {
  const Table t(parse_captures(kText));
  REQUIRE(t.size() == 5);
  CHECK(t.find("GanonCastle")->dungeon);  // its own map, not HyruleCastle's that lists it
  CHECK(t.find("HyruleField")->location == "HyruleField");
  CHECK(t.find("KokiriForest_Adult")->location == "KokiriForest");
  CHECK(t.find("HyruleField_Child")->location == "HyruleField");
  CHECK(t.find("InsideJabuJabuBelly")->dungeon);
  CHECK(t.find("LakeHylia") == nullptr);
  CHECK(t.find("") == nullptr);
  CHECK(Table().find("HyruleField") == nullptr);
}

TEST_CASE("facing: +Z is down the map, +X right") {
  CHECK(map_angle(yaw(0)).value() == doctest::Approx(kPi / 2));        // looks +Z: down
  CHECK(map_angle(yaw(kPi / 2)).value() == doctest::Approx(0.0f));     // +X: right
  CHECK(std::fabs(map_angle(yaw(kPi)).value()) == doctest::Approx(kPi / 2));  // -Z: up
  CHECK(map_angle(yaw(-kPi / 2)).value() == doctest::Approx(kPi).epsilon(1e-4));  // -X: left
  // Looking straight up (turned 90 degrees about X): no facing on a map.
  CHECK_FALSE(map_angle({std::sin(-kPi / 4), 0, 0, std::cos(-kPi / 4)}));
  CHECK_FALSE(map_angle({kNaN, 0, 0, 1}));
}

TEST_CASE("minimap: others around our Link, at the map's scale, same scene only") {
  std::vector<Icon> out;
  std::vector<Player> others{at(2, 10, 0, 0), at(3, 0, 5, -20), at(4, 0, 0, 0, "LakeHylia"), at(5, 1, 1, 1)};
  others[0].rotation = yaw(kPi / 2);
  others[3].scene.reset();
  minimap_icons(square(), "HyruleField", {0, 0, 0}, others, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0].number == 2);
  CHECK(out[0].x == doctest::Approx(1120));  // centre 1100 + 10 m * 2
  CHECK(out[0].y == doctest::Approx(700));
  CHECK(out[0].angle.value() == doctest::Approx(0.0f));
  CHECK_FALSE(out[0].clamped);
  CHECK(out[0].rgb == 0x204060u);
  CHECK(out[1].number == 3);
  CHECK(out[1].x == doctest::Approx(1100));
  CHECK(out[1].y == doctest::Approx(660));  // 20 m north: up
  CHECK_FALSE(out[1].angle);                // no rotation sent: a dot
}

TEST_CASE("minimap: off the map, held at the edge on the line to them") {
  std::vector<Icon> out;
  Minimap m = square();
  m.edge_inset = 10;  // 90 pixels from the centre at most
  minimap_icons(m, "HyruleField", {0, 0, 0}, {at(2, 200, 0, 50), at(3, 0, 0, 30)}, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0].clamped);
  CHECK(out[0].x == doctest::Approx(1190));       // right edge
  CHECK(out[0].y == doctest::Approx(700 + 22.5f));  // 400:100 scaled to 90:22.5
  CHECK_FALSE(out[1].clamped);                    // 60 pixels: inside
  CHECK(out[1].y == doctest::Approx(760));

  m.round = true;
  minimap_icons(m, "HyruleField", {0, 0, 0}, {at(2, 100, 0, 100)}, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].clamped);
  const float dx = out[0].x - 1100, dy = out[0].y - 700;
  CHECK(std::sqrt(dx * dx + dy * dy) == doctest::Approx(90));
  CHECK(dx == doctest::Approx(dy));
}

TEST_CASE("minimap: relative to our Link, wherever we are") {
  std::vector<Icon> out;
  minimap_icons(square(), "HyruleField", {500, 3, -200}, {at(2, 505, 9, -210)}, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].x == doctest::Approx(1110));
  CHECK(out[0].y == doctest::Approx(680));
}

TEST_CASE("minimap: bad numbers draw nothing, and the list is reused") {
  std::vector<Icon> out;
  out.reserve(16);
  const auto* before = out.data();
  Minimap m = square();
  minimap_icons(m, "HyruleField", {kNaN, 0, 0}, {at(2, 1, 0, 1)}, out);
  CHECK(out.empty());
  minimap_icons(m, "", {0, 0, 0}, {at(2, 1, 0, 1)}, out);
  CHECK(out.empty());
  m.metres_across = 0;
  minimap_icons(m, "HyruleField", {0, 0, 0}, {at(2, 1, 0, 1)}, out);
  CHECK(out.empty());
  m = square();
  m.box.w = kNaN;
  minimap_icons(m, "HyruleField", {0, 0, 0}, {at(2, 1, 0, 1)}, out);
  CHECK(out.empty());
  std::vector<Player> sixteen;
  for (int i = 1; i <= 16; ++i) sixteen.push_back(at(static_cast<std::uint8_t>(i), 1.0f * i, 0, 0));
  sixteen[3].position[0] = kNaN;
  minimap_icons(square(), "HyruleField", {0, 0, 0}, sixteen, out);
  CHECK(out.size() == 15);
  CHECK(out.data() == before);  // no new allocation
}

TEST_CASE("minimap: the fade carries over and is kept in 0..1") {
  std::vector<Icon> out;
  auto p = at(2, 1, 0, 1);
  p.fade = 0.25f;
  auto q = at(3, 1, 0, 1);
  q.fade = 7.0f;
  auto r = at(4, 1, 0, 1);
  r.fade = kNaN;
  minimap_icons(square(), "HyruleField", {0, 0, 0}, {p, q, r}, out);
  REQUIRE(out.size() == 3);
  CHECK(out[0].alpha == doctest::Approx(0.25f));
  CHECK(out[1].alpha == 1.0f);
  CHECK(out[2].alpha == 0.0f);
}

TEST_CASE("picture: the capture's centre is the picture's centre, its height the picture's height") {
  const Table t(parse_captures(kText));
  const Capture& field = *t.find("HyruleField");
  const Box pic{100, 50, 1000, 500};  // 2:1, so 1472 m across
  auto c = to_picture(pic, field, {-23, 0, 197});
  CHECK(c[0] == doctest::Approx(600));
  CHECK(c[1] == doctest::Approx(300));
  c = to_picture(pic, field, {-23 + 368, 0, 197 - 184});  // a quarter across right, a quarter up
  CHECK(c[0] == doctest::Approx(850));
  CHECK(c[1] == doctest::Approx(175));

  std::vector<Icon> out;
  picture_icons(pic, field, 0,
                {at(1, -23, 0, 197, "HyruleField_Adult"), at(2, 5000, 0, 197), at(3, 0, 0, 0, "LakeHylia")}, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0].x == doctest::Approx(600));
  CHECK_FALSE(out[0].clamped);
  CHECK(out[1].clamped);
  CHECK(out[1].x == doctest::Approx(1100));  // the right edge
  CHECK(out[1].floor_delta == 0);
}

TEST_CASE("dungeon floors: the floor whose cut is nearest above the player") {
  const Table t(parse_captures(kText));
  const Capture& jabu = *t.find("InsideJabuJabuBelly");
  // Camera at 200: floor 1 holds 95..107, floor 2 holds 67..72, floor 0 everything.
  CHECK(floor_of(jabu, 100) == 1);
  CHECK(floor_of(jabu, 68) == 2);
  CHECK(floor_of(jabu, 150) == 0);
  CHECK(floor_of(jabu, kNaN) == 0);
  std::vector<Icon> out;
  picture_icons({0, 0, 400, 400}, jabu, 1, {at(1, -1, 100, -92, "InsideJabuJabuBelly"),
                                            at(2, -1, 68, -92, "InsideJabuJabuBelly")}, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0].floor == 1);
  CHECK(out[0].floor_delta == 0);
  CHECK(out[1].floor == 2);
  CHECK(out[1].floor_delta == 1);
}

TEST_CASE("zoom setting") {
  CHECK(parse_zoom("0.5").value() == doctest::Approx(0.5f));
  CHECK(parse_zoom("100").value() == 100.0f);
  CHECK_FALSE(parse_zoom("0"));
  CHECK_FALSE(parse_zoom("-1"));
  CHECK_FALSE(parse_zoom("101"));
  CHECK_FALSE(parse_zoom("abc"));
  CHECK_FALSE(parse_zoom("1.5x"));
  CHECK_FALSE(parse_zoom(""));
  CHECK_FALSE(parse_zoom("inf"));
}

TEST_CASE("minimap in a dungeon: a player on another floor is marked by how many floors away") {
  const Table t(parse_captures(kText));
  const Capture& jabu = *t.find("InsideJabuJabuBelly");
  std::vector<Icon> out;
  std::vector<Player> others{at(2, 1, 100, 0, "InsideJabuJabuBelly"), at(3, 1, 68, 0, "InsideJabuJabuBelly")};
  minimap_icons(square(), "InsideJabuJabuBelly", {0, 100, 0}, others, out, &jabu);
  REQUIRE(out.size() == 2);
  CHECK(out[0].floor_delta == 0);
  CHECK(out[1].floor == 2);
  CHECK(out[1].floor_delta == 1);
  // An area: no floors counted, nor without the capture.
  const Capture& field = *t.find("HyruleField");
  minimap_icons(square(), "InsideJabuJabuBelly", {0, 100, 0}, others, out, &field);
  CHECK(out[1].floor_delta == 0);
  minimap_icons(square(), "InsideJabuJabuBelly", {0, 100, 0}, others, out);
  CHECK(out[1].floor_delta == 0);
}
