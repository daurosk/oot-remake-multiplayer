#pragma once

// Frame counts in real time (issue 106). FlagWatcher and LoadMend count
// "frames" (settle 120, title screen 30, mend 90) but are called once per
// presented frame, and Fritchii's high refresh (Ctrl+U) presents up to 165
// a second instead of 60: a 2 s settle became 0.73 s. With a clock, step()
// gives the 60-a-second ticks that passed since the last call instead, so
// the counts keep their time at any refresh. Without one (unit tests that
// count calls) every call is one tick, as before.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>

namespace coop::host {

class GameTicks {
 public:
  using Clock = std::function<std::uint64_t()>;  // microseconds, steady

  static constexpr std::uint64_t kTickUs = 16'667;  // 60 a second
  // A long stall (a load, a hitch) counts as this many ticks at most, as it
  // did when one call was one frame.
  static constexpr unsigned kMaxStep = 4;

  void set_clock(Clock clock) {
    clock_ = std::move(clock);
    started_ = false;
  }

  // The ticks since the last call: 0 between two presents in the same 1/60 s,
  // 1 at 60, 2 at 30. The first call is one tick.
  unsigned step() {
    if (!clock_) return 1;
    const std::uint64_t tick = clock_() / kTickUs;
    if (!started_) {
      started_ = true;
      last_ = tick;
      return 1;
    }
    const std::uint64_t passed = tick > last_ ? tick - last_ : 0;
    last_ = std::max(last_, tick);
    return static_cast<unsigned>(std::min<std::uint64_t>(passed, kMaxStep));
  }

 private:
  Clock clock_;
  bool started_ = false;
  std::uint64_t last_ = 0;
};

}  // namespace coop::host
