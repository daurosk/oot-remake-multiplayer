// SPDX-License-Identifier: GPL-3.0-or-later
// The Customize page's live picture: the game's camera around your own Link (hookshot/src/coop_look_view.h).
#include <doctest/doctest.h>

#include <cmath>
#include <string>

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
            // Head to feet: kFill of the window's height (the distance allows for the elevation's tilt).
            const auto top = Project(s, {feet[0], feet[1] + height, feet[2]}, w.aspect);
            const auto bottom = Project(s, {feet[0], feet[1], feet[2]}, w.aspect);
            const double filled = (bottom[1] - top[1]) / (w.y1 - w.y0);
            CHECK(filled == doctest::Approx(CoopLookView::kFill).epsilon(0.03));
            CHECK(top[1] > w.y0);
            CHECK(bottom[1] < w.y1);
        }
    }
}

TEST_CASE("look view: the eye is on the side the yaw names, above him, further for a taller Link") {
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

TEST_CASE("look view: his feet show over a 0.5 m step in front of him (the Kakariko ledge)") {
    // beta-test-1: at 8 degrees up the line to his feet fell about 16 degrees, so a stone step's edge 0.3 m high
    // 1 m in front of him hid them. Now it falls kFeetDrop, for either age, any side, any window and field of view.
    const std::array<float, 3> feet{-119.1f, 96.7f, 58.7f};
    const Window small{0.70f, 0.10f, 0.92f, 0.49f, 16.0f / 9.0f};
    const Window wide{0.60f, 0.15f, 0.95f, 0.75f, 21.0f / 9.0f};
    for (const Window& w : {Square(), small, wide}) {
        for (const double fov : {0.785398, 0.872665, 1.047198}) {   // 45, 50, 60 degrees
            for (const double yaw : {0.0, 1.5707, 3.0, -2.2}) {
                for (const float height : {1.35f, 1.9f}) {
                    CAPTURE(fov);
                    CAPTURE(yaw);
                    CAPTURE(height);
                    const Shot s = CoopLookView::Aim(feet, height, yaw, w, fov);
                    CHECK(CoopLookView::FeetDrop(s, feet) == doctest::Approx(CoopLookView::kFeetDrop).epsilon(0.005));
                    // The step's edge, 0.9 m from him toward the eye: the line to his feet passes over it.
                    const double out = std::hypot(s.eye[0] - feet[0], s.eye[2] - feet[2]);
                    const double line_at_edge = (s.eye[1] - feet[1]) * 0.9 / out;
                    CHECK(line_at_edge > 0.5);
                    // Still all of him in the window, his middle at its middle, from the side the yaw names.
                    const auto middle = Project(s, {feet[0], feet[1] + height * 0.5, feet[2]}, w.aspect, fov);
                    CHECK(middle[0] == doctest::Approx((w.x0 + w.x1) * 0.5).epsilon(1e-6));
                    CHECK(middle[1] == doctest::Approx((w.y0 + w.y1) * 0.5).epsilon(1e-6));
                    const auto top = Project(s, {feet[0], feet[1] + height, feet[2]}, w.aspect, fov);
                    const auto bottom = Project(s, {feet[0], feet[1], feet[2]}, w.aspect, fov);
                    CHECK(top[1] > w.y0);
                    CHECK(bottom[1] < w.y1);
                    CHECK((bottom[1] - top[1]) / (w.y1 - w.y0) == doctest::Approx(CoopLookView::kFill).epsilon(0.01));
                    // The view looks along the yaw. Aimed beside him (the window is off the screen's middle), the
                    // eye stands to one side of that line, more the steeper the view, but well in front of him.
                    const double look = std::atan2(s.eye[0] - s.target[0], s.eye[2] - s.target[2]);
                    CHECK(std::fabs(std::remainder(look - yaw, 2 * kPi)) < 1e-6);
                    const double side = std::atan2(s.eye[0] - feet[0], s.eye[2] - feet[2]);
                    CHECK(std::fabs(std::remainder(side - yaw, 2 * kPi)) < 0.8);
                }
            }
        }
    }
}

TEST_CASE("look view: the camera never lower than kElevationMin, where his feet fall more than kFeetDrop") {
    // The whole screen as the window at a wide field of view: he is framed from so close that the line to his feet
    // falls more than kFeetDrop even at the lowest elevation. The camera stays at kElevationMin, never lower.
    const Window full{0.0f, 0.0f, 1.0f, 1.0f, 16.0f / 9.0f};
    const std::array<float, 3> feet{0, 0, 0};
    for (const float height : {1.35f, 1.9f}) {
        const Shot s = CoopLookView::Aim(feet, height, 0.0, full, 2.0);
        const double pitch =
            std::atan2(s.eye[1] - s.target[1], std::hypot(s.eye[0] - s.target[0], s.eye[2] - s.target[2]));
        CHECK(pitch == doctest::Approx(CoopLookView::kElevationMin).epsilon(1e-6));
        CHECK(CoopLookView::FeetDrop(s, feet) > CoopLookView::kFeetDrop);
    }
}

TEST_CASE("look view: the camera never steeper than kElevationMax nor lower than kElevationMin") {
    // A window so short that Link would be framed from close up: the drop wants more than 45 degrees.
    const Window tiny{0.80f, 0.10f, 0.84f, 0.13f, 16.0f / 9.0f};
    const std::array<float, 3> feet{0, 0, 0};
    const Shot s = CoopLookView::Aim(feet, 1.35f, 0.0, tiny);
    const double pitch = std::atan2(s.eye[1] - s.target[1], std::hypot(s.eye[0] - s.target[0], s.eye[2] - s.target[2]));
    CHECK(pitch >= CoopLookView::kElevationMin - 1e-9);
    CHECK(pitch <= CoopLookView::kElevationMax + 1e-9);
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

TEST_CASE("look view: the log says the page's picture once per change and per opening") {
    using CoopLookView::Showing;
    CoopLookView::Watch watch;
    const auto first = watch.Drawn(Showing::live_child, 1000);
    REQUIRE(first);
    CHECK(first->find("child") != std::string::npos);
    watch.Framed(1010);
    CHECK_FALSE(watch.Drawn(Showing::live_child, 1016));   // the same picture: nothing
    CHECK_FALSE(watch.Drawn(Showing::live_child, 1032));
    // Our event starts: said once, with why.
    const auto event = watch.Drawn(Showing::live_event, 1048);
    REQUIRE(event);
    CHECK(event->find("event") != std::string::npos);
    CHECK_FALSE(watch.Drawn(Showing::live_event, 1064));
    // The page left (not drawn for more than kHoldMs) and opened again: said again.
    const auto again = watch.Drawn(Showing::live_event, 1064 + CoopLookView::kHoldMs + 1);
    REQUIRE(again);
    const auto adult = watch.Drawn(Showing::live_adult, 1400);
    REQUIRE(adult);
    CHECK(adult->find("adult") != std::string::npos);
    const auto no_link = watch.Drawn(Showing::flat_no_link, 1416);
    REQUIRE(no_link);
    CHECK(no_link->find("no Link") != std::string::npos);
}

TEST_CASE("look view: a live picture whose camera is never framed is said once") {
    using CoopLookView::Showing;
    CoopLookView::Watch watch;
    const std::uint64_t step = 16, unframed = CoopLookView::Watch::kUnframedMs;
    REQUIRE(watch.Drawn(Showing::live_adult, 1000));
    std::uint64_t now = 1000 + step;
    for (; now < 1000 + unframed; now += step) {
        CHECK_FALSE(watch.Drawn(Showing::live_adult, now));
    }
    const auto said = watch.Drawn(Showing::live_adult, 1000 + unframed);
    REQUIRE(said);
    CHECK(said->find("not framed") != std::string::npos);
    CHECK_FALSE(watch.Drawn(Showing::live_adult, 1000 + unframed + step));   // once
    // Framed by the game: never said.
    CoopLookView::Watch framed;
    REQUIRE(framed.Drawn(Showing::live_child, 1000));
    for (now = 1000 + step; now < 1000 + 2 * unframed; now += step) {
        framed.Framed(now - 8);
        CHECK_FALSE(framed.Drawn(Showing::live_child, now));
    }
    // A camera framed before this opening does not count for it.
    CoopLookView::Watch old;
    old.Framed(500);
    REQUIRE(old.Drawn(Showing::live_child, 1000));
    for (now = 1000 + step; now < 1000 + unframed; now += step) {
        CHECK_FALSE(old.Drawn(Showing::live_child, now));
    }
    CHECK(old.Drawn(Showing::live_child, 1000 + unframed));
}

namespace {

// A world-to-clip matrix as a game's camera block has it (row-major, clip = m * p): a perspective of `fov_y` (its y
// row flipped when `y_down`) times the view from `eye` along yaw and pitch (the views' right, up, back rows).
std::array<float, 16> Clip(double fov_y, double aspect, const Vec3& eye, double yaw, double pitch, bool y_down) {
    const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    const double rows[3][3] = {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}};
    const double f = 1.0 / std::tan(fov_y * 0.5), near_z = 0.1;
    const double scale[4] = {f / aspect, y_down ? -f : f, near_z, -1.0};   // (z: an infinite reversed one)
    const int from[4] = {0, 1, 2, 2};
    std::array<float, 16> m{};
    for (int r = 0; r < 4; ++r) {
        const double* axis = rows[from[r]];
        const double s = r == 2 ? 0.0 : scale[r];
        for (int c = 0; c < 3; ++c) {
            m[4 * r + c] = static_cast<float>(s * axis[c]);
        }
        m[4 * r + 3] = static_cast<float>(-s * (axis[0] * eye[0] + axis[1] * eye[1] + axis[2] * eye[2]) +
                                          (r == 2 ? near_z : 0.0));
    }
    return m;
}

}  // namespace

TEST_CASE("look view: the game's field of view read from its camera matrix, any view, y up or down") {
    for (const double degrees : {30.0, 45.0, 60.0, 75.0}) {
        for (const bool y_down : {false, true}) {
            const double fov = degrees * kPi / 180.0;
            const auto m = Clip(fov, 16.0 / 9.0, {12.0, 5.0, -40.0}, 2.2, -0.3, y_down);
            const auto got = CoopLookView::FovFromClip(m);
            REQUIRE(got.has_value());
            CHECK(*got == doctest::Approx(fov).epsilon(1e-4));
        }
    }
    CHECK_FALSE(CoopLookView::FovFromClip({}).has_value());   // (no camera: zeros)
    std::array<float, 16> flat{};
    flat[0] = flat[5] = flat[10] = flat[15] = 1.0f;   // an identity: not a perspective (90 degrees, w row empty)
    CHECK_FALSE(CoopLookView::FovFromClip(flat).has_value());
    // Outside 10 to 120 degrees: not believed.
    CHECK_FALSE(CoopLookView::FovFromClip(Clip(150 * kPi / 180, 1.0, {0, 0, 0}, 0, 0, false)).has_value());
}

TEST_CASE("look view: framed with the game's field of view, Link stays at the window's middle (owner: centred)") {
    const Window w = Square();
    const std::array<float, 3> feet{3.0f, 1.0f, 7.0f};
    const double game_fov = 60.0 * kPi / 180.0;
    for (const float height : {1.35f, 1.9f}) {
        const Shot right = CoopLookView::Aim(feet, height, 0.7, w, game_fov);
        const auto middle = Project(right, {feet[0], feet[1] + height * 0.5, feet[2]}, w.aspect, game_fov);
        CHECK(middle[0] == doctest::Approx((w.x0 + w.x1) * 0.5).epsilon(1e-6));
        CHECK(middle[1] == doctest::Approx((w.y0 + w.y1) * 0.5).epsilon(1e-6));
        // The measured 45 degrees in a 60-degree game: he slides towards the picture's middle, out of the square's.
        const Shot guessed = CoopLookView::Aim(feet, height, 0.7, w);
        const auto off = Project(guessed, {feet[0], feet[1] + height * 0.5, feet[2]}, w.aspect, game_fov);
        CHECK(std::abs(off[0] - (w.x0 + w.x1) * 0.5) > 0.03);
    }
}

TEST_CASE("look view: the orbit frames with the fov it is given") {
    Orbit orbit;
    orbit.Ask(Square(), 1000);
    const Subject link{{0, 0, 0}, 1.9f};
    const double game_fov = 60.0 * kPi / 180.0;
    const auto shot = orbit.Camera({0, 2, 5}, {0, 1, 0}, link, 1000, game_fov);
    REQUIRE(shot.has_value());
    const auto middle = Project(*shot, {0, 0.95, 0}, Square().aspect, game_fov);
    CHECK(middle[0] == doctest::Approx((Square().x0 + Square().x1) * 0.5).epsilon(1e-6));
    CHECK(middle[1] == doctest::Approx((Square().y0 + Square().y1) * 0.5).epsilon(1e-6));
}

TEST_CASE("look view: Link missing for a moment (a scene entry) keeps the last one, longer is none (#74)") {
    using CoopLookView::Keep;
    using CoopLookView::Seen;
    using CoopLookView::Showing;
    Keep keep;
    CHECK_FALSE(keep.Take(std::nullopt, 1000).has_value());   // never seen: none (the title)
    const Seen adult{Subject{{1.0f, 2.0f, 3.0f}, 1.9f}, Showing::live_adult};
    const auto now = keep.Take(adult, 2000);
    REQUIRE(now);
    CHECK(now->showing == Showing::live_adult);
    const auto gap = keep.Take(std::nullopt, 2000 + Keep::kGapMs);
    REQUIRE(gap);   // a frame or two without him: still him
    CHECK(gap->link.feet[0] == doctest::Approx(1.0f));
    CHECK(gap->showing == Showing::live_adult);
    CHECK_FALSE(keep.Take(std::nullopt, 2001 + Keep::kGapMs).has_value());   // gone longer: none
    CHECK_FALSE(keep.Take(std::nullopt, 2002 + Keep::kGapMs).has_value());
    // Our own event: the picture stays live (the camera still turns around him).
    const Seen event{Subject{{0, 0, 0}, 1.9f}, Showing::live_event};
    REQUIRE(keep.Take(event, 9000));
    CHECK(keep.Take(std::nullopt, 9100)->showing == Showing::live_event);
}
