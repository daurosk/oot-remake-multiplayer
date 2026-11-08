#pragma once

// The fake player's path and the receiver's interpolation.
//
// Each player runs round a circle of kRadius game units once every
// kPeriodUs, centred apart from the others, as a function of server time.
// A receiver shows the other players kShowDelayUs behind its estimate of the
// server's clock: between two samples it interpolates in a straight line;
// past the last sample it extrapolates with the sample's velocity for up to
// kExtrapolateUs, then holds (docs/protocol.md, PLAYER_SAMPLE).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace coop::test_tool {

inline constexpr double kRadius = 100.0;  // game units
inline constexpr std::uint64_t kPeriodUs = 4'000'000;
inline constexpr std::uint64_t kShowDelayUs = 100'000;
inline constexpr std::uint64_t kExtrapolateUs = 150'000;
inline constexpr std::size_t kSampleBuffer = 16;

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

enum class Shown : std::uint8_t { interpolated, extrapolated, held };

struct ShownPoint {
  Vec position{};
  Shown how = Shown::interpolated;
};

// What a receiver shows at server time `at_us` from the buffered samples,
// oldest first. Nothing while every sample is still newer than `at_us`.
inline std::optional<ShownPoint> shown_point(const std::deque<PathPoint>& buffer,
                                             std::uint64_t at_us) {
  if (buffer.empty() || buffer.front().time_us > at_us) return std::nullopt;
  std::size_t i = 0;
  while (i + 1 < buffer.size() && buffer[i + 1].time_us <= at_us) ++i;
  const PathPoint& a = buffer[i];
  ShownPoint out;
  if (i + 1 < buffer.size()) {
    const PathPoint& b = buffer[i + 1];
    const double t = static_cast<double>(at_us - a.time_us) /
                     static_cast<double>(b.time_us - a.time_us);
    for (std::size_t k = 0; k < 3; ++k) {
      out.position[k] = a.position[k] + (b.position[k] - a.position[k]) * t;
    }
    out.how = Shown::interpolated;
    return out;
  }
  const std::uint64_t ahead = at_us - a.time_us;
  const std::uint64_t used = ahead < kExtrapolateUs ? ahead : kExtrapolateUs;
  const double dt = static_cast<double>(used) / 1e6;
  for (std::size_t k = 0; k < 3; ++k) out.position[k] = a.position[k] + a.velocity[k] * dt;
  out.how = ahead <= kExtrapolateUs ? Shown::extrapolated : Shown::held;
  return out;
}

}  // namespace coop::test_tool
