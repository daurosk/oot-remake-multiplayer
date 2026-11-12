// Tests for the puppet's eased jumps and snap meter (lichii/hook/coop_snap.h, issue 194).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "coop_snap.h"

using CoopSnap::Meter;
using CoopSnap::Smoother;
using CoopSnap::Vec3;

namespace {

// Draws `frames` frames 33 ms apart toward `target`, from `t0`; the longest drawn step.
float Run(Smoother& s, const Vec3& target, std::uint64_t& now, int frames, Vec3* last = nullptr) {
    Meter m;
    if (s.Drawn()) {
        m.Step(*s.Drawn());
    }
    float worst = 0.0f;
    for (int i = 0; i < frames; ++i) {
        now += 33;
        const Vec3 d = s.Step(target, now);
        if (s.Drawn()) {
            if (const auto snap = m.Step(d)) {
                worst = std::max(worst, *snap);
            }
        }
        if (last != nullptr) {
            *last = d;
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("snap: the first place is drawn as given, walking is drawn as given") {
    Smoother s;
    CHECK(s.Step({1, 2, 3}, 0) == Vec3{1, 2, 3});
    CHECK(s.Step({1.2f, 2, 3}, 33) == Vec3{1.2f, 2, 3});
    CHECK_FALSE(s.Easing());
}

TEST_CASE("snap: a held puppet's cutscene end (a door, 4 m) is eased, no drawn step past kSnapM") {
    Smoother s;
    std::uint64_t now = 0;
    s.Step({0, 0, 0}, now);
    float jump = 0.0f;
    now += 33;
    // The place jumps 4 m at once (the end of their door cutscene before the fix).
    CHECK(s.Step({4, 0, 0}, now, &jump) == Vec3{0, 0, 0});
    CHECK(jump == doctest::Approx(4.0f));
    CHECK(s.Easing());
    Vec3 last{};
    CHECK(Run(s, {4, 0, 0}, now, 30, &last) == 0.0f);  // no snap
    CHECK(last[0] == doctest::Approx(4.0f));            // and it got there
    CHECK_FALSE(s.Easing());
}

TEST_CASE("snap: a long jump closes at kEaseSpeed, still no snap at 30 fps") {
    Smoother s;
    std::uint64_t now = 0;
    s.Step({0, 0, 0}, now);
    CHECK(CoopSnap::EaseMs(15.0f) == 1000);
    CHECK(CoopSnap::EaseMs(1.0f) == CoopSnap::kEaseMs);
    Vec3 last{};
    CHECK(Run(s, {15, 0, 0}, now, 60, &last) == 0.0f);
    CHECK(last[0] == doctest::Approx(15.0f));
}

TEST_CASE("snap: while easing, the target's own movement is followed") {
    Smoother s;
    std::uint64_t now = 0;
    s.Step({0, 0, 0}, now);
    now += 33;
    s.Step({3, 0, 0}, now);  // jump: ease starts
    // They walk on 0.2 m a frame; the drawn place keeps closing in and ends on them.
    Meter m;
    m.Step(*s.Drawn());
    float x = 3.0f;
    Vec3 d{};
    for (int i = 0; i < 20; ++i) {
        now += 33;
        x += 0.2f;
        d = s.Step({x, 0, 0}, now);
        CHECK_FALSE(m.Step(d).has_value());
    }
    CHECK(d[0] == doctest::Approx(x));
}

TEST_CASE("snap: a warp (past kWarpM) is drawn at once and counted as a warp, not a near snap") {
    Smoother s;
    Meter m;
    s.Step({0, 0, 0}, 0);
    m.Step({0, 0, 0});
    const Vec3 d = s.Step({50, 0, 0}, 33);
    CHECK(d == Vec3{50, 0, 0});
    CHECK_FALSE(s.Easing());
    const auto snap = m.Step(d);
    REQUIRE(snap.has_value());
    CHECK(*snap == doctest::Approx(50.0f));
    CHECK(m.Snaps() == 1);
    CHECK(m.NearSnaps() == 0);
}

TEST_CASE("snap: the meter counts a near snap, and Set (a hide) starts again with no ease") {
    Meter m;
    m.Step({0, 0, 0});
    CHECK_FALSE(m.Step({0.5f, 0, 0}).has_value());
    CHECK(m.Step({3, 0, 0}).has_value());
    CHECK(m.NearSnaps() == 1);
    m.Reset();
    CHECK_FALSE(m.Step({100, 0, 0}).has_value());

    Smoother s;
    s.Step({0, 0, 0}, 0);
    s.Set({5, 0, 0});  // hidden at their place: shown again there, no ease from the old spot
    CHECK(s.Step({5, 0, 0}, 33) == Vec3{5, 0, 0});
    CHECK_FALSE(s.Easing());
}

TEST_CASE("snap: a broken place is passed through, not eased") {
    Smoother s;
    s.Step({0, 0, 0}, 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Vec3 d = s.Step({nan, 0, 0}, 33);
    CHECK(std::isnan(d[0]));
    CHECK(s.Step({0.1f, 0, 0}, 66) == Vec3{0.1f, 0, 0});
}
