#include "test_delay.hpp"

#include <algorithm>

namespace coop::server {

std::uint64_t DelayClock::due(std::uint64_t lane, std::uint64_t now_us) {
  // Half the round trip each way, in microseconds, then +- the jitter.
  std::int64_t wait = std::int64_t{config_.round_trip_ms} * 500;
  if (config_.jitter_ms != 0) {
    const std::int64_t j = std::int64_t{config_.jitter_ms} * 1000;
    wait += std::uniform_int_distribution<std::int64_t>(-j, j)(random_);
  }
  std::uint64_t at = now_us + static_cast<std::uint64_t>((std::max<std::int64_t>)(wait, 0));
  std::uint64_t& last = last_[lane];
  at = (std::max)(at, last);  // jitter alone never reorders a lane
  last = at;
  return at;
}

std::uint64_t DelayClock::last(std::uint64_t lane) const {
  auto it = last_.find(lane);
  return it == last_.end() ? 0 : it->second;
}

}  // namespace coop::server
