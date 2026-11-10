// Tests for the hook's steady-clock periods (lichii/hook/coop_every.h).
#include <doctest/doctest.h>

#include "coop_every.h"

using CoopEvery::Every;

namespace {

// How many times Due is true over `seconds` when called every `frame_us`.
int Fired(Every& every, std::uint64_t frame_us, double seconds, std::uint64_t start_us = 1'000) {
    int fired = 0;
    const auto end = start_us + static_cast<std::uint64_t>(seconds * 1e6);
    for (std::uint64_t t = start_us; t < end; t += frame_us) {
        fired += every.Due(t) ? 1 : 0;
    }
    return fired;
}

} // namespace

TEST_CASE("every: the first call is due, then once a period") {
    Every e{100};
    CHECK(e.Due(5));
    CHECK_FALSE(e.Due(5));
    CHECK_FALSE(e.Due(104));
    CHECK(e.Due(105));
    CHECK_FALSE(e.Due(204));
    CHECK(e.Due(205));
}

TEST_CASE("every: the rate does not follow the presented frame rate") {
    // 30, 40 (the game), 60 and 165 (2.75x high refresh) presented frames a second.
    for (const std::uint64_t frame_us : {33'333u, 25'000u, 16'667u, 6'061u}) {
        Every quarter{CoopEvery::kQuarterUs};
        CHECK(Fired(quarter, frame_us, 10.0) == doctest::Approx(40).epsilon(0.05));
        Every second{CoopEvery::kSecondUs};
        CHECK(Fired(second, frame_us, 10.0) == doctest::Approx(10).epsilon(0.1));
    }
    // The fast scans: 30 a second whenever frames come at least that often.
    for (const std::uint64_t frame_us : {16'667u, 6'061u}) {
        Every scan{CoopEvery::kTwoFramesUs};
        CHECK(Fired(scan, frame_us, 10.0) == doctest::Approx(300).epsilon(0.05));
    }
}

TEST_CASE("every: frames slower than the period keep the average, no burst") {
    // 25 ms frames, 33 ms period: due on most frames, never two in one call.
    Every scan{CoopEvery::kTwoFramesUs};
    CHECK(Fired(scan, 25'000, 10.0) == doctest::Approx(300).epsilon(0.05));
}

TEST_CASE("every: after a stall it starts over instead of catching up") {
    Every e{100};
    CHECK(e.Due(0 + 1));
    CHECK(e.Due(10'000));  // a long pause: one fire, not a hundred
    CHECK_FALSE(e.Due(10'050));
    CHECK(e.Due(10'100));
}

TEST_CASE("every: Reset makes the next call due") {
    Every e{1'000};
    CHECK(e.Due(10));
    CHECK_FALSE(e.Due(20));
    e.Reset();
    CHECK(e.Due(30));
    CHECK(e.Period() == 1'000);
}

TEST_CASE("every: NowUs moves forward") {
    const auto a = CoopEvery::NowUs();
    const auto b = CoopEvery::NowUs();
    CHECK(b >= a);
}
