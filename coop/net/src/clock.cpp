#include "coop/net/clock.hpp"

#include <limits>

namespace coop::net {

std::optional<ClockSample> clock_sample(std::uint64_t t0_client_send,
                                        std::uint64_t t1_server,
                                        std::uint64_t t2_client_receive) {
  if (t2_client_receive < t0_client_send) return std::nullopt;
  const std::uint64_t round_trip = t2_client_receive - t0_client_send;
  // Midpoint without overflow.
  const std::uint64_t mid = t0_client_send + round_trip / 2;
  // Differences of two u64 times can exceed i64; refuse those, a real clock
  // pair is never that far apart.
  constexpr auto kMax =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  std::int64_t offset = 0;
  if (t1_server >= mid) {
    if (t1_server - mid > kMax) return std::nullopt;
    offset = static_cast<std::int64_t>(t1_server - mid);
  } else {
    if (mid - t1_server > kMax) return std::nullopt;
    offset = -static_cast<std::int64_t>(mid - t1_server);
  }
  return ClockSample{round_trip, offset};
}

void ClockEstimator::add(const ClockSample& sample) {
  samples_[next_] = sample;
  next_ = (next_ + 1) % kWindow;
  if (count_ < kWindow) ++count_;
  latest_ = sample;
}

void ClockEstimator::reset() {
  next_ = 0;
  count_ = 0;
  latest_ = {};
}

std::optional<std::uint64_t> ClockEstimator::round_trip_us() const {
  if (count_ == 0) return std::nullopt;
  return latest_.round_trip_us;
}

std::optional<std::int64_t> ClockEstimator::offset_us() const {
  if (count_ == 0) return std::nullopt;
  const ClockSample* best = &samples_[0];
  for (std::size_t i = 1; i < count_; ++i) {
    if (samples_[i].round_trip_us < best->round_trip_us) best = &samples_[i];
  }
  return best->offset_us;
}

std::uint64_t apply_offset(std::uint64_t time_us, std::int64_t offset_us) {
  if (offset_us >= 0) {
    const auto add = static_cast<std::uint64_t>(offset_us);
    if (time_us > std::numeric_limits<std::uint64_t>::max() - add) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return time_us + add;
  }
  // -(INT64_MIN) does not fit an i64; go through u64.
  const std::uint64_t sub = 0 - static_cast<std::uint64_t>(offset_us);
  return time_us < sub ? 0 : time_us - sub;
}

}  // namespace coop::net
