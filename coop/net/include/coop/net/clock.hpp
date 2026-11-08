#pragma once

// Round trip and clock offset from PING and PONG (docs/protocol.md,
// "Keep-alive and clock"). Pure maths on numbers, no sockets, so it can be
// tested on fixed values.
//
// The client sends PING at its time t0. The server answers PONG with t0
// echoed and its own time t1. The client receives the PONG at its time t2.
//   round trip = t2 - t0
//   offset     = t1 - (t0 + t2) / 2   (server clock minus client clock)
// The offset assumes the two directions take the same time; the error is at
// most half the round trip, so the estimator trusts the sample with the
// shortest round trip among the recent ones.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace coop::net {

struct ClockSample {
  std::uint64_t round_trip_us = 0;
  std::int64_t offset_us = 0;  // server time minus client time
  bool operator==(const ClockSample&) const = default;
};

// One sample from the three times. Nothing when t2 is before t0 (a PONG
// that does not answer one of our PINGs, or a clock that went backwards).
std::optional<ClockSample> clock_sample(std::uint64_t t0_client_send,
                                        std::uint64_t t1_server,
                                        std::uint64_t t2_client_receive);

// Keeps the last few samples. The round trip it reports is the latest one;
// the offset is the one from the sample with the shortest round trip in the
// window, which is the least wrong.
class ClockEstimator {
 public:
  static constexpr std::size_t kWindow = 8;

  void add(const ClockSample& sample);
  void reset();

  bool has_estimate() const { return count_ > 0; }
  std::optional<std::uint64_t> round_trip_us() const;
  std::optional<std::int64_t> offset_us() const;

 private:
  std::array<ClockSample, kWindow> samples_{};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
  ClockSample latest_{};
};

// Client time to server time and back, with the offset from the estimator.
// Saturates at 0 and at the u64 maximum instead of wrapping.
std::uint64_t apply_offset(std::uint64_t time_us, std::int64_t offset_us);

}  // namespace coop::net
