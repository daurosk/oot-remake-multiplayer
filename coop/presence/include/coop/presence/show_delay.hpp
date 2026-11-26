#pragma once

// How late another player's game is shown, from how old its samples are when
// they come (player report #37: puppets and the enemies they own "super
// laggy" at 150-350 ms ping).
//
// A sample carries the server time its game made it at; we draw a player at
// our server time minus a delay and interpolate between the two samples
// around that time. With a fixed 100 ms delay that only works while samples
// come younger than 100 ms less one send interval. Over the internet a
// sample from another game is its trip to the server plus the server's trip
// to us old (150-300 ms at the players' pings): the shown time was always
// past the newest sample, so the puppet was carried on by its velocity for
// up to 150 ms, then held, and jumped at each new sample.
//
// ShowDelay keeps the ages of the last kAges samples from one player and
// aims at their 90th percentile plus kMarginUs (one send interval, so the
// sample after the shown time is in too), never under the floor (the old
// fixed delay: nothing changes on a LAN) nor over kMaxUs. It moves toward
// that aim at most kRise of the time passed when it grows (the player plays
// at half speed for a moment, never backwards) and kFall when it shrinks.
// Pure; Presence keeps one per player.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace coop::presence {

class ShowDelay {
 public:
  static constexpr std::size_t kAges = 32;           // about 1.5 s of samples at 20 a second
  static constexpr std::size_t kFewest = 8;          // fewer: the floor
  static constexpr std::uint64_t kMarginUs = 60'000;  // a 20 Hz send interval and a frame
  static constexpr std::uint64_t kMaxUs = 600'000;
  static constexpr double kRise = 0.5;
  static constexpr double kFall = 0.05;

  // A sample made at `sample_us` came at `now_us` (both server time). A time
  // of 0 (the sender's clock not known yet) is left out.
  void seen(std::uint64_t sample_us, std::uint64_t now_us) {
    if (sample_us == 0 || now_us == 0) return;
    const std::uint64_t age = now_us > sample_us ? now_us - sample_us : 0;
    ages_[next_] = (std::min)(age, kMaxUs);
    next_ = (next_ + 1) % kAges;
    if (count_ < kAges) ++count_;
  }

  // The delay to draw with at `now_us` (server time), at least `floor_us`.
  std::uint64_t step(std::uint64_t now_us, std::uint64_t floor_us) {
    const std::uint64_t aim = target(floor_us);
    if (current_us_ == 0 || last_us_ == 0 || now_us < last_us_) {
      current_us_ = (std::max)(current_us_, floor_us);
    } else {
      const double dt = static_cast<double>(now_us - last_us_);
      if (aim > current_us_) {
        current_us_ += (std::min)(aim - current_us_, static_cast<std::uint64_t>(dt * kRise));
      } else {
        current_us_ -= (std::min)(current_us_ - aim, static_cast<std::uint64_t>(dt * kFall));
      }
    }
    current_us_ = (std::max)(current_us_, floor_us);
    last_us_ = now_us;
    return current_us_;
  }

  // Where step() is heading: the 90th percentile age plus kMarginUs.
  std::uint64_t target(std::uint64_t floor_us) const {
    if (count_ < kFewest) return floor_us;
    std::array<std::uint64_t, kAges> sorted{};
    std::copy_n(ages_.begin(), count_, sorted.begin());
    const std::size_t at = (count_ * 9) / 10;
    std::nth_element(sorted.begin(), sorted.begin() + at, sorted.begin() + count_);
    return std::clamp(sorted[at] + kMarginUs, floor_us, (std::max)(floor_us, kMaxUs));
  }

  std::uint64_t current_us() const { return current_us_; }

 private:
  std::array<std::uint64_t, kAges> ages_{};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
  std::uint64_t current_us_ = 0;
  std::uint64_t last_us_ = 0;
};

}  // namespace coop::presence
