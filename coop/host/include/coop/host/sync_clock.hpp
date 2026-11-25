// The one time base of everything that must happen at the same moment in every game (sync plan S5/S6 events,
// S4 platform clocks): server time in microseconds, as coop::net::Client::server_now_us() and every `time` /
// `server_time_us` field on the wire already are.
//
// What was missing in coop/net/clock.hpp (the PING/PONG maths, unchanged and still the source of the samples):
// - its offset is the best of the last 8 samples, so it STEPS whenever the best one leaves the window: a platform
//   word or an event's time jumps forward or back by the difference (milliseconds at 100 ms round trip);
// - nothing says how wrong the time may be, so nothing could be tested against "less than a frame plus jitter".
// SyncClock is the smooth, never-backwards server time over the same samples; Playout is the jitter buffer delay
// of a stream of timed samples; elapsed_us / due are how a game uses a time that came over the wire.
//
// Use (one rule for events and platforms): the sender writes WHEN (server time) into the message, never "now".
// A receiver does not start at arrival: it plays from `elapsed_us(when, clock.At(local))`, so two games show the
// same moment of the same event whatever their delay; a state that must flip at once in all games carries a `when`
// a little ahead (lead_us of the worst Playout delay) and each game flips it in the first frame where due() holds.
// Error budget, checked by tests/host/sync_clock_test.cpp at 20 to 200 ms round trip: both games' clocks agree
// within the jitter of the link (half the asymmetry of each one's best round trip), so two flips differ by less
// than one frame plus that jitter.
// Pure: numbers in, numbers out; no sockets, no system clock.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace coop::host {

class SyncClock {
 public:
  static constexpr std::size_t kWindow = 16;               // samples kept
  static constexpr std::int64_t kAgePpm = 100;             // what an old sample may be off by, per second of age
  static constexpr std::int64_t kSlewPpm = 20'000;         // the shown time nears the estimate by at most 2 %
  static constexpr std::int64_t kStepUs = 250'000;         // farther off than this: set at once (a new server)
  static constexpr std::uint64_t kDriftSpanUs = 60'000'000;  // drift is said only over a minute or more

  // One PING/PONG: our send time, the server's time in the PONG, our receive time (our steady clock, microseconds).
  void Add(std::uint64_t t0_send, std::uint64_t t1_server, std::uint64_t t2_receive) {
    if (t2_receive < t0_send) return;
    Sample s;
    s.round_trip_us = t2_receive - t0_send;
    s.local_us = t0_send + s.round_trip_us / 2;
    s.offset_us = static_cast<std::int64_t>(t1_server) - static_cast<std::int64_t>(s.local_us);
    samples_[next_] = s;
    next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    if (!has_first_ || s.round_trip_us <= first_.round_trip_us) {
      if (!has_first_) first_ = s;
      has_first_ = true;
    }
    latest_good_ = Best(s.local_us);
  }
  void Reset() { *this = SyncClock{}; }
  bool Ready() const { return count_ > 0; }

  // Server time at our local time. Never goes back; nears a changed estimate smoothly (kSlewPpm), so nothing
  // that is driven by it jumps.
  std::uint64_t At(std::uint64_t local_us) {
    if (count_ == 0) return local_us;
    const std::int64_t target = static_cast<std::int64_t>(local_us) + Best(local_us).offset_us;
    std::int64_t out = target;
    if (started_) {
      const std::int64_t elapsed = local_us > last_local_ ? static_cast<std::int64_t>(local_us - last_local_) : 0;
      const std::int64_t predicted = last_out_ + elapsed;
      const std::int64_t error = target - predicted;
      if (error <= kStepUs && error >= -kStepUs) {
        const std::int64_t most = elapsed * kSlewPpm / 1'000'000 + 1;
        out = predicted + std::clamp(error, -most, most);
      }
      out = std::max(out, last_out_);
    }
    started_ = true;
    last_local_ = local_us;
    last_out_ = std::max<std::int64_t>(out, 0);
    return static_cast<std::uint64_t>(last_out_);
  }

  // The estimate itself (server minus local), without the smoothing: for the log and the digest.
  std::int64_t OffsetUs(std::uint64_t local_us) const { return count_ ? Best(local_us).offset_us : 0; }
  // How wrong the estimate may be: half the best sample's round trip plus its age allowance.
  std::uint64_t ErrorUs(std::uint64_t local_us) const { return count_ ? Cost(Best(local_us), local_us) : 0; }
  // Our clock against the server's, parts per million, once two good samples are a minute apart; 0 before. For
  // the log only: within the window the age allowance covers it (16 s at 100 ppm is 1.6 ms).
  double DriftPpm() const {
    if (!has_first_ || latest_good_.local_us < first_.local_us + kDriftSpanUs) return 0.0;
    return static_cast<double>(latest_good_.offset_us - first_.offset_us) * 1.0e6 /
           static_cast<double>(latest_good_.local_us - first_.local_us);
  }

 private:
  struct Sample {
    std::uint64_t local_us = 0;  // the middle of the round trip
    std::int64_t offset_us = 0;
    std::uint64_t round_trip_us = 0;
  };
  static std::uint64_t Cost(const Sample& s, std::uint64_t local_us) {
    const std::uint64_t age = local_us > s.local_us ? local_us - s.local_us : 0;
    return s.round_trip_us / 2 + age * static_cast<std::uint64_t>(kAgePpm) / 1'000'000;
  }
  Sample Best(std::uint64_t local_us) const {
    Sample best = samples_[0];
    for (std::size_t i = 1; i < count_; ++i) {
      if (Cost(samples_[i], local_us) < Cost(best, local_us)) best = samples_[i];
    }
    return best;
  }

  std::array<Sample, kWindow> samples_{};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
  Sample first_{};
  Sample latest_good_{};
  bool has_first_ = false;
  bool started_ = false;
  std::uint64_t last_local_ = 0;
  std::int64_t last_out_ = 0;
};

// The jitter buffer of one stream of timed samples (an object's samples, a platform's clock words): how far behind
// the sender a receiver plays so that a late sample is still in time. Mean transit and its deviation as RTP does
// (RFC 3550: gain 1/16); the delay is the mean plus four deviations, held between kMinUs and kMaxUs.
class Playout {
 public:
  static constexpr std::uint64_t kMinUs = 0;
  static constexpr std::uint64_t kMaxUs = 500'000;

  // A sample stamped `sent_us` (server time) came at `arrived_us` (our SyncClock's server time).
  void Seen(std::uint64_t sent_us, std::uint64_t arrived_us) {
    const double transit = static_cast<double>(static_cast<std::int64_t>(arrived_us) - static_cast<std::int64_t>(sent_us));
    if (!seen_) {
      mean_ = transit;
      jitter_ = 0.0;
      seen_ = true;
      return;
    }
    const double off = transit > mean_ ? transit - mean_ : mean_ - transit;
    jitter_ += (off - jitter_) / 16.0;
    mean_ += (transit - mean_) / 16.0;
  }
  bool Ready() const { return seen_; }
  std::uint64_t JitterUs() const { return static_cast<std::uint64_t>(jitter_); }
  std::uint64_t DelayUs() const {
    const double d = mean_ + 4.0 * jitter_;
    if (d <= static_cast<double>(kMinUs)) return kMinUs;
    return std::min(static_cast<std::uint64_t>(d), kMaxUs);
  }
  // When to show what was stamped `sent_us`.
  std::uint64_t PlayAt(std::uint64_t sent_us) const { return sent_us + DelayUs(); }

 private:
  bool seen_ = false;
  double mean_ = 0.0;
  double jitter_ = 0.0;
};

// How long ago `when_us` was (0 before it): a receiver plays an event's phase from here, not from its arrival.
inline std::uint64_t elapsed_us(std::uint64_t when_us, std::uint64_t now_us) {
  return now_us > when_us ? now_us - when_us : 0;
}
// The frame in which a thing stamped `when_us` happens: the first one at or after it.
inline bool due(std::uint64_t when_us, std::uint64_t now_us) { return now_us >= when_us; }
// How far ahead a sender stamps a flip all games must do in the same frame: the worst receiver's playout delay
// plus one frame, never more than kMaxLeadUs (a longer wait is felt; the late game then plays from elapsed_us).
inline constexpr std::uint64_t kMaxLeadUs = 250'000;
inline std::uint64_t lead_us(std::uint64_t worst_delay_us, std::uint64_t frame_us) {
  return std::min(worst_delay_us + frame_us, kMaxLeadUs);
}

}  // namespace coop::host
