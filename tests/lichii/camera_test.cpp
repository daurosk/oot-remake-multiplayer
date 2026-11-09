// Tests for the camera handoff between the renderer and the hook
// (lichii/hook/coop_camera.cpp). No emulator code.
#include <doctest/doctest.h>

#include <array>

#include "coop_camera.h"

namespace {

std::array<float, 16> filled(float v) {
  std::array<float, 16> m{};
  m.fill(v);
  return m;
}

}  // namespace

TEST_CASE("camera: nothing until a frame is finished") {
  CoopCamera::SetWanted(true);
  CHECK(CoopCamera::Wanted());
  CHECK_FALSE(CoopCamera::Latest());

  const auto a = filled(1.0f);
  CoopCamera::Publish(10, a.data(), 1280, 720, false);
  CHECK_FALSE(CoopCamera::Latest());  // frame 10 may still have a bigger target

  const auto b = filled(2.0f);
  CoopCamera::Publish(11, b.data(), 1280, 720, true);
  const auto latest = CoopCamera::Latest();
  REQUIRE(latest);
  CHECK(latest->frame == 10);
  CHECK(latest->clip_from_world == a);
  CHECK(latest->width == 1280);
  CHECK(latest->height == 720);
  CHECK_FALSE(latest->y_down);
  CoopCamera::SetWanted(false);
}

TEST_CASE("camera: the largest target of a frame wins") {
  CoopCamera::SetWanted(true);
  const auto half = filled(1.0f);
  const auto full = filled(2.0f);
  CHECK(CoopCamera::Better(20, 960, 540));
  CoopCamera::Publish(20, half.data(), 960, 540, false);
  CHECK(CoopCamera::Better(20, 1920, 1080));
  CoopCamera::Publish(20, full.data(), 1920, 1080, false);
  CHECK_FALSE(CoopCamera::Better(20, 960, 540));    // a smaller reflection pass
  CHECK_FALSE(CoopCamera::Better(20, 1920, 1080));  // the same size again
  CHECK(CoopCamera::Better(21, 960, 540));          // a new frame takes anything

  CoopCamera::Publish(21, half.data(), 960, 540, false);
  const auto latest = CoopCamera::Latest();
  REQUIRE(latest);
  CHECK(latest->frame == 20);
  CHECK(latest->clip_from_world == full);
  CoopCamera::SetWanted(false);
}

TEST_CASE("camera: turning it off forgets the camera") {
  CoopCamera::SetWanted(true);
  const auto m = filled(3.0f);
  CoopCamera::Publish(30, m.data(), 1280, 720, false);
  CoopCamera::Publish(31, m.data(), 1280, 720, false);
  REQUIRE(CoopCamera::Latest());
  CoopCamera::SetWanted(false);
  CHECK_FALSE(CoopCamera::Wanted());
  CHECK_FALSE(CoopCamera::Latest());
}
