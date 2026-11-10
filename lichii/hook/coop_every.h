// SPDX-License-Identifier: GPL-3.0-or-later
//
// A period on the steady clock, for the hook's scans, logs and checks that
// run on the present thread. Counting presented frames ties a rate to the
// display: Fritchii's high refresh (Ctrl+U) presents up to 2.75 times as many
// frames as the game draws. Tested in tests/lichii/every_test.cpp.
//
//     CoopEvery::Every scan_{CoopEvery::kScanUs};
//     if (!scan_.Due(CoopEvery::NowUs())) return;
//
// Due is true on the first call, then at most once a period. It keeps the
// average rate when calls do not land on the period (25 ms frames, 33 ms
// period), and after a stall (a load, a pause) it starts over instead of
// firing once for every period missed.
#pragma once

#include <chrono>
#include <cstdint>

namespace CoopEvery {

// Today's frame-counted rates at 60 presented frames a second.
inline constexpr std::uint64_t kTwoFramesUs = 33'333;    // was every 2 frames
inline constexpr std::uint64_t kQuarterUs = 250'000;     // was every 15 frames
inline constexpr std::uint64_t kSecondUs = 1'000'000;    // was every 60 frames

// Microseconds on the steady clock (never 0 once the process has run a moment).
inline std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

class Every {
public:
    explicit constexpr Every(std::uint64_t period_us) : period_us_(period_us) {}

    bool Due(std::uint64_t now_us) {
        if (started_ && now_us < next_us_) {
            return false;
        }
        // On time or a little late: keep the beat. Far behind: start over.
        next_us_ = started_ && now_us - next_us_ < period_us_ ? next_us_ + period_us_
                                                              : now_us + period_us_;
        started_ = true;
        return true;
    }

    // The next Due is true at once.
    void Reset() { started_ = false; }

    std::uint64_t Period() const { return period_us_; }

private:
    std::uint64_t period_us_;
    std::uint64_t next_us_ = 0;
    bool started_ = false;
};

} // namespace CoopEvery
