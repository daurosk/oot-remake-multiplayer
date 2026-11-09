#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "coop/overlay/overlay.hpp"

using namespace coop::overlay;
namespace proto = coop::protocol;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// At the origin looking down -z, 90 degree field of view, 800 x 800 pixels:
// a point at depth d with x = d sits on the right edge.
Camera straight_camera() {
  Camera c;
  c.view = look_at({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
  c.projection = perspective(kPi / 2, 1.0f, 0.1f, 1000.0f);
  c.viewport_width = 800;
  c.viewport_height = 800;
  return c;
}

Shown player(std::uint8_t number, Vec3 position, float fade = 1.0f, std::string scene = "field") {
  Shown s;
  s.number = number;
  s.name = "Player " + std::to_string(number);
  s.position = position;
  s.scene = std::move(scene);
  s.fade = fade;
  return s;
}

Result build(const std::vector<Shown>& players, Settings settings = {}) {
  return Overlay(settings).build(straight_camera(), "field", players);
}

std::optional<Why> why(const Result& r, std::uint8_t number) {
  for (const auto& s : r.skipped) {
    if (s.number == number) return s.why;
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("overlay: known points project to known pixels") {
  Result r = build({player(2, {0, 0, -10}), player(3, {5, 5, -10}), player(4, {-10, -10, -10})});
  REQUIRE(r.error == Error::none);
  REQUIRE(r.items.size() == 3);
  for (const auto& item : r.items) {
    CHECK(item.marker.depth == doctest::Approx(10));
    CHECK_FALSE(item.edge);
    if (item.number == 2) {
      CHECK(item.marker.x == doctest::Approx(400));
      CHECK(item.marker.y == doctest::Approx(400));
    } else if (item.number == 3) {
      CHECK(item.marker.x == doctest::Approx(600));
      CHECK(item.marker.y == doctest::Approx(200));  // up in the world is up on screen
    } else {
      CHECK(item.marker.x == doctest::Approx(0));  // exactly on the edge is on screen
      CHECK(item.marker.y == doctest::Approx(800));
    }
  }
}

TEST_CASE("overlay: a moved and turned camera") {
  Camera c;
  c.view = look_at({100, 0, 0}, {100, 0, 50}, {0, 1, 0});  // looking down +z
  c.projection = perspective(kPi / 2, 2.0f, 0.1f, 1000.0f);
  c.viewport_width = 1600;
  c.viewport_height = 800;
  Result r = Overlay().build(c, "field", {player(2, {100, 0, 20}), player(3, {100, 0, -20})});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].number == 2);
  CHECK(r.items[0].marker.x == doctest::Approx(800));
  CHECK(r.items[0].marker.y == doctest::Approx(400));
  CHECK(r.items[0].marker.depth == doctest::Approx(20));
  CHECK(why(r, 3) == Why::behind);
}

TEST_CASE("overlay: behind, too near and too far are skipped") {
  Settings s;
  s.near_distance = 1;
  s.far_distance = 5000;
  Result r = build({player(2, {0, 0, 10}), player(3, {0, 0, -0.5f}), player(4, {0, 0, -6000}),
                    player(1, {0, 0, 0})},
                   s);
  CHECK(r.items.empty());
  CHECK(why(r, 2) == Why::behind);
  CHECK(why(r, 3) == Why::too_near);
  CHECK(why(r, 4) == Why::too_far);
  CHECK(why(r, 1) == Why::behind);  // at the eye: w is 0
}

TEST_CASE("overlay: just off screen is clamped to the edge as an arrow") {
  Result r = build({player(2, {15, 0, -10}), player(3, {0, 15, -10}), player(4, {-15, -15, -10})});
  REQUIRE(r.items.size() == 3);
  for (const auto& item : r.items) {
    CHECK(item.edge);
    if (item.number == 2) {  // right
      CHECK(item.marker.x == doctest::Approx(784));
      CHECK(item.marker.y == doctest::Approx(400));
      CHECK(item.arrow_angle == doctest::Approx(0));
    } else if (item.number == 3) {  // up
      CHECK(item.marker.x == doctest::Approx(400));
      CHECK(item.marker.y == doctest::Approx(16));
      CHECK(item.arrow_angle == doctest::Approx(-kPi / 2));
      CHECK(item.tag.below);  // no room above at the top edge
    } else {  // down and left, along the diagonal
      CHECK(item.marker.x == doctest::Approx(16));
      CHECK(item.marker.y == doctest::Approx(784));
      CHECK(item.arrow_angle == doctest::Approx(3 * kPi / 4));
    }
  }
}

TEST_CASE("overlay: too far off screen, or arrows off, is skipped") {
  Result r = build({player(2, {30.5f, 0, -10})});
  CHECK(r.items.empty());
  CHECK(why(r, 2) == Why::off_screen);

  Settings s;
  s.edge_arrows = false;
  r = build({player(2, {15, 0, -10}), player(3, {0, 0, -10})}, s);
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].number == 3);
  CHECK(why(r, 2) == Why::off_screen);
}

TEST_CASE("overlay: marker size scales with distance within min and max") {
  Result r = build({player(1, {0, 0, -300}), player(2, {0, 0, -150}), player(3, {0, 0, -50}),
                    player(4, {0, 0, -900})});
  REQUIRE(r.items.size() == 4);
  for (const auto& item : r.items) {
    switch (item.number) {
      case 1: CHECK(item.marker.size == doctest::Approx(24)); break;
      case 2: CHECK(item.marker.size == doctest::Approx(48)); break;
      case 3: CHECK(item.marker.size == doctest::Approx(48)); break;  // 144, kept to max
      default: CHECK(item.marker.size == doctest::Approx(8)); break;
    }
  }
  r = build({player(2, {0, 0, -5000})});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].marker.size == doctest::Approx(8));  // 1.44, kept to min
}

TEST_CASE("overlay: the name tag sits above the marker") {
  Result r = build({player(2, {0, 0, -300})});
  REQUIRE(r.items.size() == 1);
  const auto& item = r.items[0];
  CHECK(item.tag.text == "Player 2");
  CHECK(item.tag.x == doctest::Approx(400));
  CHECK(item.tag.y == doctest::Approx(400 - 12 - 6));  // half the 24 px marker, then the gap
  CHECK_FALSE(item.tag.below);

  Shown nameless = player(3, {0, 0, -300});
  nameless.name.clear();
  r = build({nameless});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].tag.text == "P3");
}

TEST_CASE("overlay: with a head height the tag floats over the head") {
  Shown p = player(2, {1, 0, -10});
  p.head = 2.0f;
  Result r = build({p});
  REQUIRE(r.items.size() == 1);
  const auto& item = r.items[0];
  CHECK(item.tag.head);
  CHECK_FALSE(item.tag.below);
  // 2 above at depth 10 is a fifth of the half screen up: 400 - 80, then the gap.
  CHECK(item.tag.x == doctest::Approx(440));
  CHECK(item.tag.y == doctest::Approx(400 - 80 - 6));
  CHECK(item.marker.y == doctest::Approx(400));  // the marker stays at the feet

  // The head off the top of the screen: under the marker instead.
  p.position = {0, 9, -10};
  r = build({p});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].tag.below);
  CHECK_FALSE(r.items[0].tag.head);

  // Off screen: an edge arrow, the tag by the arrow as before.
  p.position = {15, 0, -10};
  r = build({p});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].edge);
  CHECK_FALSE(r.items[0].tag.head);
}

TEST_CASE("overlay: fading with distance") {
  Settings s;
  s.fade_start = 20;
  s.fade_end = 40;
  Shown off = player(5, {45, 0, -30});  // off screen, depth 30
  Result r = build({player(2, {0, 0, -10}), player(3, {0, 0, -30}, 0.5f), player(4, {0, 0, -50}), off}, s);
  REQUIRE(r.items.size() == 3);
  for (const auto& item : r.items) {
    if (item.number == 2) CHECK(item.alpha == doctest::Approx(1.0f));
    if (item.number == 3) CHECK(item.alpha == doctest::Approx(0.25f));  // half way, times 0.5
    if (item.number == 5) {
      CHECK(item.edge);
      CHECK(item.alpha == doctest::Approx(0.6f));  // edge_min_alpha
    }
  }
  CHECK(why(r, 4) == Why::too_far);
}

TEST_CASE("overlay: fade passes through as alpha; faded out is skipped") {
  Result r = build({player(2, {0, 0, -10}, 0.4f), player(3, {0, 0, -20}, 0.0f),
                    player(4, {0, 0, -30}, 1.0f)});
  REQUIRE(r.items.size() == 2);
  for (const auto& item : r.items) {
    CHECK(item.alpha == doctest::Approx(item.number == 2 ? 0.4f : 1.0f));
  }
  CHECK(why(r, 3) == Why::faded);
}

TEST_CASE("overlay: only players in the local scene") {
  Shown unknown = player(4, {0, 0, -10});
  unknown.scene.reset();
  Result r = build({player(2, {0, 0, -10}), player(3, {0, 0, -10}, 1.0f, "village"), unknown});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].number == 2);
  CHECK(why(r, 3) == Why::other_scene);
  CHECK(why(r, 4) == Why::other_scene);
}

TEST_CASE("overlay: sorted far to near, then by number") {
  Result r = build({player(1, {0, 0, -10}), player(4, {-1, 0, -50}), player(3, {1, 0, -20}),
                    player(2, {0, 0, -50})});
  REQUIRE(r.items.size() == 4);
  CHECK(r.items[0].number == 2);
  CHECK(r.items[1].number == 4);
  CHECK(r.items[2].number == 3);
  CHECK(r.items[3].number == 1);
}

TEST_CASE("overlay: NaN and infinite input is refused") {
  Result r = build({player(2, {kNaN, 0, -10}), player(3, {0, 0, -10}, kNaN), player(4, {0, kInf, -10}),
                    player(1, {0, 0, -10})});
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].number == 1);
  CHECK(why(r, 2) == Why::invalid);
  CHECK(why(r, 3) == Why::invalid);
  CHECK(why(r, 4) == Why::invalid);

  Camera c = straight_camera();
  c.view[5] = kNaN;
  r = Overlay().build(c, "field", {player(1, {0, 0, -10})});
  CHECK(r.error == Error::camera);
  CHECK(r.items.empty());

  c = straight_camera();
  c.projection[0] = kInf;
  CHECK(Overlay().build(c, "field", {player(1, {0, 0, -10})}).error == Error::camera);

  c = straight_camera();
  c.viewport_width = 0;
  CHECK(Overlay().build(c, "field", {player(1, {0, 0, -10})}).error == Error::camera);
  c.viewport_width = kNaN;
  CHECK(Overlay().build(c, "field", {player(1, {0, 0, -10})}).error == Error::camera);

  // Huge but finite: no NaN comes out (here it is simply too far).
  r = build({player(2, {3e38f, 3e38f, -3e38f})});
  CHECK(r.items.empty());
  CHECK(why(r, 2) == Why::too_far);
}

TEST_CASE("overlay: bad settings draw nothing") {
  Settings s;
  CHECK(valid(s));
  s.near_distance = 0;
  CHECK_FALSE(valid(s));
  s = {};
  s.far_distance = s.near_distance;
  CHECK_FALSE(valid(s));
  s = {};
  s.min_size = 50;  // above max_size
  CHECK_FALSE(valid(s));
  s = {};
  s.marker_size = kNaN;
  CHECK_FALSE(valid(s));
  s = {};
  s.fade_start = 10;
  s.fade_end = 5;  // not above fade_start
  CHECK_FALSE(valid(s));
  s = {};
  s.edge_min_alpha = 2;
  CHECK_FALSE(valid(s));
  s = {};
  s.marker_size = kNaN;
  Result r = build({player(2, {0, 0, -10})}, s);
  CHECK(r.error == Error::settings);
  CHECK(r.items.empty());
}

TEST_CASE("overlay: collect takes what presence shows") {
  coop::presence::Presence presence;
  proto::Welcome w;
  w.player_number = 1;
  w.players = {{2, "Ana"}, {3, "Ben"}};
  presence.on_welcome(w);
  proto::PlayerSample s;
  s.player_number = 2;
  s.time = {1'000'000, 1};
  s.position = Vec3{1, 2, 3};
  s.scene = "field";
  presence.on_sample(s);

  std::vector<Shown> shown = collect(presence, 1'000'000 + presence.settings().show_delay_us);
  REQUIRE(shown.size() == 1);  // player 3 has sent nothing yet
  CHECK(shown[0].number == 2);
  CHECK(shown[0].name == "Ana");
  CHECK(shown[0].scene == std::optional<std::string>("field"));
  CHECK(shown[0].position == Vec3{1, 2, 3});
  CHECK(shown[0].fade == doctest::Approx(1));

  // And it builds: player 2 at depth 3 in front of a camera behind it.
  Camera c = straight_camera();
  c.view = look_at({1, 2, 13}, {1, 2, 3}, {0, 1, 0});
  Result r = Overlay().build(c, "field", shown);
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].marker.x == doctest::Approx(400));
  CHECK(r.items[0].marker.depth == doctest::Approx(10));
}

TEST_CASE("overlay: an adult's marker is lifted to the taller Link") {
  Shown child = player(2, {0, 0, -10});
  Shown adult = player(3, {0, 0, -10});
  adult.adult = true;
  Result r = build({child, adult});
  REQUIRE(r.error == Error::none);
  REQUIRE(r.items.size() == 2);
  for (const auto& item : r.items) {
    // 40 pixels a world unit at depth 10; the default lift is 0.5.
    CHECK(item.marker.y == doctest::Approx(item.number == 3 ? 380 : 400));
  }
}
