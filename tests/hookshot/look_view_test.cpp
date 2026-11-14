// SPDX-License-Identifier: GPL-3.0-or-later
// The Customize page's live picture: the game's camera around your own Link (hookshot/src/coop_look_view.h).
#include <doctest/doctest.h>

#include <cmath>

#include "coop_look_view.h"

using CoopLookView::Orbit;
using CoopLookView::Project;
using CoopLookView::Shot;
using CoopLookView::Subject;
using CoopLookView::Vec3;
using CoopLookView::Window;

namespace {

constexpr double kPi = 3.141592653589793;

// The picture's square as Hookshot's menu puts it on a 16:9 window: over the help column's top.
Window Square() {
    return {0.62f, 0.12f, 0.90f, 0.62f, 16.0f / 9.0f};
}

double Distance(const Vec3& a, const Vec3& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

}  // namespace

TEST_CASE("look view: Link's middle at the window's middle, his height filling it, from any side") {
    const Window w = Square();
    const std::array<float, 3> feet{12.5f, 3.0f, -40.0f};
    for (const double yaw : {0.0, 1.0, kPi, -2.5}) {
        for (const float height : {1.35f, 1.9f}) {
            const Shot s = CoopLookView::Aim(feet, height, yaw, w);
            const auto middle = Project(s, {feet[0], feet[1] + height * 0.5, feet[2]}, w.aspect);
            CHECK(middle[0] == doctest::Approx((w.x0 + w.x1) * 0.5).epsilon(1e-6));
            CHECK(middle[1] == doctest::Approx((w.y0 + w.y1) * 0.5).epsilon(1e-6));
            // Head to feet: kFill of the window's height (the elevation tilts it a little).
            const auto top = Project(s, {feet[0], feet[1] + height, feet[2]}, w.aspect);
            const auto bottom = Project(s, {feet[0], feet[1], feet[2]}, w.aspect);
            const double filled = (bottom[1] - top[1]) / (w.y1 - w.y0);
            CHECK(filled == doctest::Approx(CoopLookView::kFill).epsilon(0.03));
            CHECK(top[1] > w.y0);
            CHECK(bottom[1] < w.y1);
        }
    }
}

TEST_CASE("look view: the eye is on the side the yaw names, a little above him, further for a taller Link") {
    const Window w = Square();
    const std::array<float, 3> feet{0, 0, 0};
    const Shot front = CoopLookView::Aim(feet, 1.9f, 0.0, w);   // yaw 0: from +z
    CHECK(front.eye[2] > 2.0);
    CHECK(front.eye[1] > 0.95);   // (above his middle)
    const Shot side = CoopLookView::Aim(feet, 1.9f, kPi / 2, w);   // from +x
    CHECK(side.eye[0] > 2.0);
    const Shot child = CoopLookView::Aim(feet, 1.35f, 0.0, w);
    CHECK(Distance(child.eye, {0, 0.675, 0}) < Distance(front.eye, {0, 0.95, 0}));
}

TEST_CASE("look view: the turn starts at his front, from the game's camera behind him") {
    // The game's camera behind Link, who faces -z: its eye at +z.
    const Vec3 game_eye{0, 2, 5}, game_target{0, 1, 0};
    CHECK(CoopLookView::FrontYaw(game_eye, game_target) == doctest::Approx(kPi));
    Orbit orbit;
    const Subject link{{0, 0, 0}, 1.9f};
    orbit.Ask(Square(), 1000);
    const auto first = orbit.Camera(game_eye, game_target, link, 1000);
    REQUIRE(first);
    CHECK(first->eye[2] < -2.0);   // in front of him: -z
}

TEST_CASE("look view: one turn every kTurnSeconds, smoothly") {
    Orbit orbit;
    const Subject link{{0, 0, 0}, 1.9f};
    const Vec3 eye{0, 2, 5}, target{0, 1, 0};
    std::uint64_t now = 5000;
    orbit.Ask(Square(), now);
    const auto start = orbit.Camera(eye, target, link, now);
    REQUIRE(start);
    // A quarter turn on: the eye moved round a quarter.
    std::optional<Shot> quarter;
    const auto quarter_ms = static_cast<std::uint64_t>(CoopLookView::kTurnSeconds * 250);
    for (std::uint64_t t = 0; t <= quarter_ms; t += 100) {
        orbit.Ask(Square(), now + t);
        quarter = orbit.Camera(eye, target, link, now + t);
        REQUIRE(quarter);
    }
    const double a0 = std::atan2(start->eye[0] - start->target[0], start->eye[2] - start->target[2]);
    const double a1 = std::atan2(quarter->eye[0] - quarter->target[0], quarter->eye[2] - quarter->target[2]);
    CHECK(std::fabs(std::remainder(a1 - a0, 2 * kPi)) == doctest::Approx(kPi / 2).epsilon(0.02));
    // A whole turn: back where it started.
    const auto whole_ms = static_cast<std::uint64_t>(CoopLookView::kTurnSeconds * 1000);
    orbit.Ask(Square(), now + whole_ms);
    const auto whole = orbit.Camera(eye, target, link, now + whole_ms);
    REQUIRE(whole);
    CHECK(Distance(whole->eye, start->eye) < 1e-6);
}

TEST_CASE("look view: not asked for any more, or no Link: the game's camera, and the next turn starts anew") {
    Orbit orbit;
    const Subject link{{0, 0, 0}, 1.9f};
    const Vec3 eye{0, 2, 5}, target{0, 1, 0};
    CHECK_FALSE(orbit.Camera(eye, target, link, 100));   // never asked
    orbit.Ask(Square(), 1000);
    CHECK(orbit.Camera(eye, target, link, 1000 + CoopLookView::kHoldMs));
    CHECK_FALSE(orbit.Camera(eye, target, link, 1001 + CoopLookView::kHoldMs));   // the page left, the menu closed
    // Asked again later, with the game's camera elsewhere (behind a Link facing +x): the turn starts at his front again.
    orbit.Ask(Square(), 9000);
    const auto again = orbit.Camera({-5, 2, 0}, {0, 1, 0}, link, 9000);
    REQUIRE(again);
    CHECK(again->eye[0] > 2.0);
    // No Link (a cutscene, the title): none, though asked.
    orbit.Ask(Square(), 9100);
    CHECK_FALSE(orbit.Camera(eye, target, std::nullopt, 9100));
}

TEST_CASE("look view: a square that is not one is not asked for") {
    CHECK(Square().Valid());
    CHECK_FALSE(Window{}.Valid());
    CHECK_FALSE((Window{0.5f, 0.1f, 0.4f, 0.6f, 1.7f}.Valid()));   // x1 before x0
    CHECK_FALSE((Window{0.5f, 0.1f, 1.2f, 0.6f, 1.7f}.Valid()));   // off the window
    CHECK_FALSE((Window{0.5f, 0.1f, 0.9f, 0.6f, 0.0f}.Valid()));   // no aspect
    CHECK_FALSE((Window{0.5f, NAN, 0.9f, 0.6f, 1.7f}.Valid()));
    Orbit orbit;
    orbit.Ask(Window{}, 1000);
    CHECK_FALSE(orbit.Asked(1000));
}
