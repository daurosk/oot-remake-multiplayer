#pragma once

// The fake player's path.
//
// Each player runs round a circle of kRadius game units once every
// kPeriodUs, centred apart from the others, as a function of server time.
// Receivers draw the others with coop/presence (docs/presence.md) and
// compare what it shows against this path.

#include <array>
#include <cmath>
#include <cstdint>

namespace coop::test_tool {

inline constexpr double kRadius = 100.0;  // game units
inline constexpr std::uint64_t kPeriodUs = 4'000'000;

using Vec = std::array<double, 3>;

struct PathPoint {
  std::uint64_t time_us = 0;  // server time
  Vec position{};
  Vec velocity{};  // units a second
};

// Where player `player` (1 to 4) really is at server time `time_us`.
inline PathPoint true_point(std::uint8_t player, std::uint64_t time_us) {
  constexpr double kTau = 6.283185307179586;
  const double phase =
      static_cast<double>(time_us % kPeriodUs) / static_cast<double>(kPeriodUs);
  const double angle = kTau * phase + player;  // players start apart
  const double speed = kTau * kRadius * 1e6 / static_cast<double>(kPeriodUs);
  const double cx = 1000.0 * player;
  PathPoint p;
  p.time_us = time_us;
  p.position = {cx + kRadius * std::cos(angle), 0.0, kRadius * std::sin(angle)};
  p.velocity = {-speed * std::sin(angle), 0.0, speed * std::cos(angle)};
  return p;
}

inline double distance(const Vec& a, const Vec& b) {
  const double x = a[0] - b[0];
  const double y = a[1] - b[1];
  const double z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

}  // namespace coop::test_tool
