#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

#include "coop/voice/spatial.hpp"

using namespace coop::voice;

namespace {

float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A camera at `eye` looking along unit `f` with unit `r` to its right and
// unit `u` up, through an OpenGL-style perspective, as the game hands the
// renderer its world-to-clip matrix (row-major, clip = m * p).
std::array<float, 16> clip_matrix(const Vec3& eye, const Vec3& r, const Vec3& u, const Vec3& f) {
  const float sx = 1.2f, sy = 1.6f, a = -1.002f, b = -0.2002f;
  const std::array<float, 4> v0{r[0], r[1], r[2], -dot(r, eye)};
  const std::array<float, 4> v1{u[0], u[1], u[2], -dot(u, eye)};
  const std::array<float, 4> v2{-f[0], -f[1], -f[2], dot(f, eye)};
  std::array<float, 16> m{};
  for (int i = 0; i < 4; ++i) {
    m[static_cast<std::size_t>(i)] = sx * v0[static_cast<std::size_t>(i)];
    m[static_cast<std::size_t>(4 + i)] = sy * v1[static_cast<std::size_t>(i)];
    m[static_cast<std::size_t>(8 + i)] = a * v2[static_cast<std::size_t>(i)] + (i == 3 ? b : 0.0f);
    m[static_cast<std::size_t>(12 + i)] = -v2[static_cast<std::size_t>(i)];
  }
  return m;
}

}  // namespace

TEST_CASE("voice placement: loudness by distance, silent past the range") {
  const Listener l;  // at the origin looking down -z
  SpatialConfig c;   // full 3 m, range 30 m, fade 6 m
  CHECK(place(l, {0, 0, -1}, c).gain == doctest::Approx(1.0f));
  CHECK(place(l, {0, 0, -3}, c).gain == doctest::Approx(1.0f));
  CHECK(place(l, {0, 0, -6}, c).gain == doctest::Approx(0.5f));
  CHECK(place(l, {0, 0, -12}, c).gain == doctest::Approx(0.25f));
  const float near_edge = place(l, {0, 0, -27}, c).gain;
  CHECK(near_edge > 0.0f);
  CHECK(near_edge < 3.0f / 27.0f);  // faded below 1/distance
  CHECK_FALSE(place(l, {0, 0, -30}, c).audible);
  CHECK_FALSE(place(l, {0, 0, -100}, c).audible);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(place(l, {nan, 0, 0}, c).audible);
  // Further is duller.
  CHECK(place(l, {0, 0, -20}, c).lowpass < place(l, {0, 0, -4}, c).lowpass);
}

TEST_CASE("voice placement: left, right, ahead and behind") {
  const Listener l;
  const SpatialConfig c;
  const Placement right = place(l, {5, 0, 0}, c);
  CHECK(right.right > 0.0f);
  CHECK(right.left == doctest::Approx(0.0f).scale(1.0));
  const Placement left = place(l, {-5, 0, 0}, c);
  CHECK(left.left > left.right * 10.0f);
  const Placement ahead = place(l, {0, 0, -5}, c);
  CHECK(ahead.left == doctest::Approx(ahead.right));
  CHECK(ahead.rear_left == doctest::Approx(0.0f).scale(1.0));
  const Placement behind = place(l, {0, 0, 5}, c);
  CHECK(behind.front_left == doctest::Approx(0.0f).scale(1.0));
  CHECK(behind.rear_left > 0.0f);
  CHECK(behind.left < ahead.left);  // behind is a little quieter in stereo
  // Equal power: the pair carries the gain.
  CHECK(right.front_left * right.front_left + right.front_right * right.front_right +
            right.rear_left * right.rear_left + right.rear_right * right.rear_right ==
        doctest::Approx(right.gain * right.gain));
  // Height does not pan.
  const Placement above = place(l, {0, 5, -0.0001f}, c);
  CHECK(above.left == doctest::Approx(above.right).epsilon(0.01));
  // Right on top of us: in the middle.
  const Placement here = place(l, {0.2f, 0, 0}, c);
  CHECK(here.left > here.right * 0.5f);
}

TEST_CASE("voice placement: the listener from the camera's world-to-clip matrix") {
  // Looking along +x from (10, 2, -4): right is +z, up is +y.
  const Vec3 eye{10, 2, -4};
  const auto m = clip_matrix(eye, {0, 0, 1}, {0, 1, 0}, {1, 0, 0});
  const auto l = listener_from_clip(m);
  REQUIRE(l.has_value());
  CHECK(l->position[0] == doctest::Approx(10.0f).epsilon(0.001));
  CHECK(l->position[1] == doctest::Approx(2.0f).epsilon(0.001));
  CHECK(l->position[2] == doctest::Approx(-4.0f).epsilon(0.001));
  CHECK(l->forward[0] == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(l->right[2] == doctest::Approx(1.0f).epsilon(0.001));
  // A voice on the camera's right is on the right.
  const Placement p = place(*l, {10, 2, 1}, SpatialConfig{});
  CHECK(p.right > p.left);
  // An orthographic or broken matrix gives no listener.
  std::array<float, 16> ortho{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CHECK_FALSE(listener_from_clip(ortho).has_value());
  auto bad = m;
  bad[5] = std::numeric_limits<float>::infinity();
  CHECK_FALSE(listener_from_clip(bad).has_value());
}

TEST_CASE("voice placement: a Link's facing as the listener") {
  const Listener l = listener_at({0, 0, 0}, 3.14159265f / 2.0f);  // facing -x
  CHECK(l.forward[0] == doctest::Approx(-1.0f));
  CHECK(l.right[2] == doctest::Approx(-1.0f));
  const Placement p = place(l, {0, 0, -5}, SpatialConfig{});  // -z is its right
  CHECK(p.right > p.left);
}
