// SPDX-License-Identifier: GPL-3.0-or-later
// The Customize page's camera turned with the orbit (patch 0040, hos_coop_turn.h): the game's camera given the
// yaw and pitch that put its eye in the orbit's direction, and the picture framed from the eye the game placed
// (hookshot/src/coop_look_view.h Steer, Framed, ReAim).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <optional>
#include <string>

#include "coop_look_view.h"

using CoopLookView::Angles;
using CoopLookView::Direction;
using CoopLookView::Project;
using CoopLookView::Shot;
using CoopLookView::Steer;
using CoopLookView::Subject;
using CoopLookView::Vec3;
using CoopLookView::Window;
using CoopLookView::Wrap;

namespace {

constexpr double kRad = 0.017453292519943295;

Window Square() {
    return {0.62f, 0.12f, 0.90f, 0.62f, 16.0f / 9.0f};
}

double Distance(const Vec3& a, const Vec3& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

Vec3 EyeAt(const Vec3& target, double yaw_deg, double elevation_deg, double dist) {
    const double y = yaw_deg * kRad, e = elevation_deg * kRad;
    return {target[0] + dist * std::cos(e) * std::sin(y), target[1] + dist * std::sin(e),
            target[2] + dist * std::cos(e) * std::cos(y)};
}

// A game camera: its eye's direction is sign * angle + offset, seen `lag` frames late; its pitch within the
// field's limits {-85, 89}; `frozen`: it does not turn at all.
struct Camera {
    Angles angles{12.0f, 5.0f};
    double sign_yaw = 1, offset_yaw = 0, sign_pitch = 1, offset_pitch = 0;
    int lag = 0;
    bool frozen = false;
    double dist = 4.0;
    Vec3 target{3.0, 1.0, -7.0};
    std::deque<Angles> history;

    void Take(const std::optional<Angles>& a) {
        if (a && !frozen) {
            angles.yaw = static_cast<float>(Wrap(a->yaw));
            angles.pitch = std::clamp(a->pitch, -85.0f, 89.0f);
        }
        history.push_back(angles);
        while (static_cast<int>(history.size()) > lag + 1) {
            history.pop_front();
        }
    }
    Vec3 Eye() const {
        const Angles& a = history.front();
        return EyeAt(target, sign_yaw * a.yaw + offset_yaw, sign_pitch * a.pitch + offset_pitch, dist);
    }
};

// One game frame: the camera hook, the camera's update, the view hook.
void Frame(Steer& steer, Camera& cam, const std::optional<Direction>& want, bool asked = true, int state = 3) {
    cam.Take(steer.Command(cam.angles, state, asked));
    steer.Observe(cam.Eye(), cam.target);
    steer.Want(want);
}

std::optional<Direction> Seen(const Camera& cam) {
    return CoopLookView::DirectionOf(cam.target, cam.Eye());
}

}  // namespace

TEST_CASE("look steer: angles wrap into -180..180, and a direction is the eye's from the target") {
    CHECK(Wrap(190.0) == doctest::Approx(-170.0));
    CHECK(Wrap(-190.0) == doctest::Approx(170.0));
    CHECK(Wrap(720.0 + 45.0) == doctest::Approx(45.0));
    const auto d = CoopLookView::DirectionOf({0, 0, 0}, EyeAt({0, 0, 0}, 120.0, 30.0, 5.0));
    REQUIRE(d);
    CHECK(d->yaw == doctest::Approx(120.0));
    CHECK(d->elevation == doctest::Approx(30.0));
    CHECK_FALSE(CoopLookView::DirectionOf({1, 2, 3}, {1, 2, 3}));
}

TEST_CASE("look steer: the eye goes where the orbit wants it, whatever the camera's signs, offsets and lag") {
    const Direction want{40.0, 30.0};
    for (const double sy : {1.0, -1.0}) {
        for (const double sp : {1.0, -1.0}) {
            for (const double oy : {0.0, 90.0, -137.0, 180.0}) {
                for (const double op : {0.0, -10.0}) {
                    for (const int lag : {0, 1}) {
                        CAPTURE(sy);
                        CAPTURE(sp);
                        CAPTURE(oy);
                        CAPTURE(op);
                        CAPTURE(lag);
                        Steer steer;
                        Camera cam;
                        cam.sign_yaw = sy, cam.offset_yaw = oy, cam.sign_pitch = sp, cam.offset_pitch = op;
                        cam.lag = lag;
                        for (int f = 0; f < 60; ++f) {
                            Frame(steer, cam, want);
                        }
                        CHECK(steer.phase() == Steer::Phase::drive);
                        const auto seen = Seen(cam);
                        REQUIRE(seen);
                        CHECK(std::fabs(Wrap(seen->yaw - want.yaw)) < 0.5);
                        CHECK(std::fabs(seen->elevation - want.elevation) < 0.5);
                    }
                }
            }
        }
    }
}

TEST_CASE("look steer: the camera keeps up with the orbit's turn (22.5 degrees a second at 30 frames)") {
    for (const int lag : {0, 1}) {
        CAPTURE(lag);
        Steer steer;
        Camera cam;
        cam.sign_yaw = -1, cam.offset_yaw = 33.0, cam.lag = lag;
        double worst = 0;
        for (int f = 0; f < 600; ++f) {
            const Direction want{Wrap(170.0 + 0.75 * f), 28.0};
            Frame(steer, cam, want);
            if (f >= 30) {
                const auto seen = Seen(cam);
                REQUIRE(seen);
                worst = std::max(worst, std::fabs(Wrap(seen->yaw - want.yaw)));
            }
        }
        CHECK(steer.phase() == Steer::Phase::drive);
        CHECK(worst < 3.0);
    }
}

TEST_CASE("look steer: the camera's angles before the first turn are put back when the picture is left") {
    Steer steer;
    Camera cam;
    cam.offset_yaw = 50.0;
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{-100.0, 35.0});
    }
    REQUIRE(steer.Driving());
    CHECK(cam.angles.yaw != doctest::Approx(12.0f));
    const auto back = steer.Command(cam.angles, 3, false);
    REQUIRE(back);
    CHECK(back->yaw == doctest::Approx(12.0f));
    CHECK(back->pitch == doctest::Approx(5.0f));
    CHECK(steer.phase() == Steer::Phase::idle);
    CHECK_FALSE(steer.Command(cam.angles, 3, false));   // (once)
    // No orbit wanted (no Link): the same.
    Steer other;
    Camera cam2;
    for (int f = 0; f < 40; ++f) {
        Frame(other, cam2, Direction{60.0, 20.0});
    }
    REQUIRE(other.Driving());
    other.Want(std::nullopt);
    const auto back2 = other.Command(cam2.angles, 3, true);
    REQUIRE(back2);
    CHECK(back2->yaw == doctest::Approx(12.0f));
}

TEST_CASE("look steer: a camera that does not turn is given up, put back, and left alone until the page is left") {
    Steer steer;
    Camera cam;
    cam.frozen = true;
    bool lost_seen = false;
    for (int f = 0; f < 20; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
        lost_seen = lost_seen || steer.phase() == Steer::Phase::lost;
    }
    CHECK(lost_seen);
    CHECK(steer.phase() == Steer::Phase::lost);
    CHECK_FALSE(steer.Driving());
    const auto note = steer.TakeNote();
    REQUIRE(note);
    CHECK(note->find("does not turn") != std::string::npos);
    // Still asked: nothing more.
    CHECK_FALSE(steer.Command(cam.angles, 3, true));
    // Left and opened again: tried again.
    CHECK_FALSE(steer.Command(cam.angles, 3, false));
    Frame(steer, cam, Direction{40.0, 30.0});
    CHECK(steer.phase() == Steer::Phase::probe);
}

TEST_CASE("look steer: the camera's update is wanted only while the page is asked, a direction is wanted and the camera "
          "was not given up on") {
    Steer steer;
    Camera cam;
    CHECK_FALSE(steer.WantsCamera(true));   // (no direction yet)
    steer.Want(Direction{40.0, 30.0});
    CHECK(steer.WantsCamera(true));
    CHECK_FALSE(steer.WantsCamera(false));
    CHECK(steer.phase() == Steer::Phase::idle);   // (asking changes nothing)
    cam.frozen = true;
    for (int f = 0; f < 20 && steer.phase() != Steer::Phase::lost; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    REQUIRE(steer.phase() == Steer::Phase::lost);
    CHECK_FALSE(steer.WantsCamera(true));
}

TEST_CASE("look steer: the probe puts the camera back after it gave up on a turn it could not read") {
    Steer steer;
    Camera cam;
    cam.frozen = true;
    // The probe turns the camera (frozen here, so nothing moves); when it gives up, the original comes back.
    std::optional<Angles> last;
    for (int f = 0; f < 20 && steer.phase() != Steer::Phase::lost; ++f) {
        last = steer.Command(cam.angles, 3, true);
        cam.Take(last);
        steer.Observe(cam.Eye(), cam.target);
        steer.Want(Direction{40.0, 30.0});
    }
    REQUIRE(steer.phase() == Steer::Phase::lost);
    REQUIRE(last);
    CHECK(last->yaw == doctest::Approx(12.0f));
    CHECK(last->pitch == doctest::Approx(5.0f));
}

TEST_CASE("look steer: an eye that turns three times the camera's turn is not followed") {
    Steer steer;
    Camera cam;
    cam.sign_yaw = 3.0;
    for (int f = 0; f < 20; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    CHECK(steer.phase() == Steer::Phase::lost);
}

TEST_CASE("look steer: no view of the camera (the game's view routine does not ask): given up after kProbeWait") {
    Steer steer;
    Camera cam;
    steer.Want(Direction{40.0, 30.0});
    int frames = 0;
    while (steer.phase() != Steer::Phase::lost && frames < 100) {
        cam.Take(steer.Command(cam.angles, 3, true));
        ++frames;
    }
    CHECK(steer.phase() == Steer::Phase::lost);
    CHECK(frames == Steer::kProbeWait + 2);
    const auto note = steer.TakeNote();
    REQUIRE(note);
    CHECK(note->find("no view seen") != std::string::npos);
}

TEST_CASE("look steer: a camera that stops following is given up after kLostFrames, and put back") {
    Steer steer;
    Camera cam;
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    REQUIRE(steer.Driving());
    cam.frozen = true;
    int frames = 0;
    std::optional<Angles> last;
    while (steer.Driving() && frames < 200) {
        last = steer.Command(cam.angles, 3, true);
        cam.Take(last);
        steer.Observe(cam.Eye(), cam.target);
        steer.Want(Direction{Wrap(40.0 + 90.0), 30.0});
        ++frames;
    }
    CHECK(steer.phase() == Steer::Phase::lost);
    CHECK(frames == Steer::kLostFrames + 1);
    REQUIRE(last);
    CHECK(last->yaw == doctest::Approx(12.0f));
}

TEST_CASE("look steer: another camera has the view (state not 3 or 4): left alone, nothing put back") {
    Steer steer;
    Camera cam;
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    REQUIRE(steer.Driving());
    CHECK_FALSE(steer.Command(cam.angles, 0, true));
    CHECK(steer.phase() == Steer::Phase::idle);
    CHECK_FALSE(steer.Command(cam.angles, 3, false));   // (nothing to put back: the event's angles are its own)
    // The player's own camera (4) is turned as the following one (3).
    Steer player;
    Camera cam2;
    for (int f = 0; f < 40; ++f) {
        Frame(player, cam2, Direction{40.0, 30.0}, true, 4);
    }
    CHECK(player.Driving());
}

TEST_CASE("look steer: re-aimed from any eye up to 50 degrees above, the point is at the window's middle") {
    const Vec3 point{2.0, 1.7, -3.0};
    for (const Window w : {Square(), Window{0.0f, 0.0f, 1.0f, 1.0f, 16.0f / 9.0f}, Window{0.05f, 0.5f, 0.3f, 0.95f, 4.0f / 3.0f}}) {
        for (const double yaw : {0.0, 75.0, -160.0}) {
            for (const double elevation : {-10.0, 8.0, 35.0, 50.0}) {
                for (const double fov : {0.6, CoopLookView::kFovY, 1.1}) {
                    CAPTURE(yaw);
                    CAPTURE(elevation);
                    CAPTURE(fov);
                    const Vec3 eye = EyeAt(point, yaw, elevation, 2.5);
                    const Shot s = CoopLookView::ReAim(eye, point, w, fov);
                    CHECK(Distance(s.eye, eye) == doctest::Approx(0.0));
                    const auto at = Project(s, point, w.aspect, fov);
                    CHECK(at[0] == doctest::Approx((w.x0 + w.x1) * 0.5).epsilon(1e-6));
                    CHECK(at[1] == doctest::Approx((w.y0 + w.y1) * 0.5).epsilon(1e-6));
                }
            }
        }
    }
}

TEST_CASE("look steer: a wall beyond Aim's eye: from the eye the orbit wants, the framed picture is Aim's") {
    const Window w = Square();
    const Subject link{{12.5f, 3.0f, -40.0f}, 1.35f};
    const Vec3 game_target{12.5, 4.2, -40.0};
    const Shot aim = CoopLookView::Aim(link.feet, link.height, 0.7, w);
    const auto want = CoopLookView::DirectionOf(game_target, aim.eye);
    REQUIRE(want);
    // The game's eye in that direction, moved in by a wall from its free distance (5 m further than Aim's) to 3 m
    // further than Aim's: brought in to Aim's.
    const double aim_distance = Distance(aim.eye, game_target);
    const Vec3 far = EyeAt(game_target, want->yaw, want->elevation, aim_distance + 3.0);
    const auto shot = CoopLookView::Framed(far, game_target, aim, link, w, aim_distance + 5.0);
    REQUIRE(shot);
    CHECK(Distance(shot->eye, aim.eye) < 1e-6);
    const Vec3 middle{link.feet[0], link.feet[1] + link.height * 0.5, link.feet[2]};
    const auto a = Project(aim, middle, w.aspect);
    const auto b = Project(*shot, middle, w.aspect);
    CHECK(b[0] == doctest::Approx(a[0]).epsilon(1e-6));
    CHECK(b[1] == doctest::Approx(a[1]).epsilon(1e-6));
    const auto feet_a = Project(aim, {link.feet[0], link.feet[1], link.feet[2]}, w.aspect);
    const auto feet_b = Project(*shot, {link.feet[0], link.feet[1], link.feet[2]}, w.aspect);
    CHECK(feet_b[1] == doctest::Approx(feet_a[1]).epsilon(1e-6));
}

TEST_CASE("look steer: a wall the game's collision found keeps the eye in front of it, Link still in the middle") {
    const Window w = Square();
    const Subject link{{-119.46f, 97.92f, 54.06f}, 1.9f};
    const Vec3 game_target{-119.46, 99.5, 54.06};
    const Shot aim = CoopLookView::Aim(link.feet, link.height, -2.0, w);
    // The game placed its eye 1.2 m from its target (a cliff behind): the eye stays there.
    const Vec3 near = EyeAt(game_target, 30.0, 25.0, 1.2);
    const auto shot = CoopLookView::Framed(near, game_target, aim, link, w, 4.0);
    REQUIRE(shot);
    CHECK(Distance(shot->eye, near) < 1e-9);
    const auto at = Project(*shot, {link.feet[0], link.feet[1] + link.height * 0.5, link.feet[2]}, w.aspect);
    CHECK(at[0] == doctest::Approx((w.x0 + w.x1) * 0.5).epsilon(1e-6));
    CHECK(at[1] == doctest::Approx((w.y0 + w.y1) * 0.5).epsilon(1e-6));
    // An eye almost straight above, the window high on the screen: the camera would have to tilt past straight down.
    CHECK_FALSE(CoopLookView::Framed(EyeAt(game_target, 30.0, 85.0, 2.0), game_target, aim, link,
                                     Window{0.3f, 0.0f, 0.7f, 0.2f, 16.0f / 9.0f}, 4.0, 1.1));
    // An eye almost on the target: none (Aim's picture instead).
    CHECK_FALSE(CoopLookView::Framed(EyeAt(game_target, 30.0, 25.0, 0.1), game_target, aim, link, w, 4.0));
}

TEST_CASE("look steer: in open ground a game camera nearer than Aim's keeps Aim's picture (adult Link fits)") {
    // Review of fd5949c1: the game's own distance may be under Aim's (adult Link, 45 degrees: Aim's eye 5.27 m
    // away); framed from the game's 3 m eye his head was cut. Only an eye the collision moved in is framed.
    const Window w = Square();
    const Subject link{{40.0f, 0.0f, 12.0f}, 1.9f};
    const Vec3 game_target{40.0, 1.6, 12.0};
    for (const double fov : {0.785398, 1.047198}) {
        CAPTURE(fov);
        const Shot aim = CoopLookView::Aim(link.feet, link.height, 1.3, w, fov);
        const auto want = CoopLookView::DirectionOf(game_target, aim.eye);
        REQUIRE(want);
        const double aim_distance = Distance(aim.eye, game_target);
        for (const double free : {3.0, 4.0, aim_distance - 0.5}) {
            CAPTURE(free);
            REQUIRE(free < aim_distance);
            // The eye at its free distance, or a little in from it (a slope, the camera's own easing): Aim's.
            for (const double share : {1.0, 0.95}) {
                const Vec3 eye = EyeAt(game_target, want->yaw, want->elevation, free * share);
                CHECK_FALSE(CoopLookView::Framed(eye, game_target, aim, link, w, free, fov));
            }
            // Moved in by a wall: framed from where the game put it.
            const Vec3 walled = EyeAt(game_target, want->yaw, want->elevation, free * 0.6);
            const auto shot = CoopLookView::Framed(walled, game_target, aim, link, w, free, fov);
            REQUIRE(shot);
            CHECK(Distance(shot->eye, walled) < 1e-9);
        }
    }
}

TEST_CASE("look steer: the camera's free distance is the furthest its eye was seen since the probe began") {
    Steer steer;
    Camera cam;
    cam.dist = 3.0;
    CHECK(steer.FreeDistance() == 0.0);
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    REQUIRE(steer.Driving());
    CHECK(steer.FreeDistance() == doctest::Approx(3.0));
    // A wall moves the eye in: the free distance stays.
    cam.dist = 1.4;
    for (int f = 0; f < 5; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    CHECK(steer.FreeDistance() == doctest::Approx(3.0));
    // Open again further on (the camera's distance grows with its pitch): the furthest.
    cam.dist = 3.6;
    Frame(steer, cam, Direction{40.0, 30.0});
    CHECK(steer.FreeDistance() == doctest::Approx(3.6));
    // The page opened again at the same place (a tight start there: the probe sees only 1.4 m): the best one seen
    // there is kept.
    steer.Command(cam.angles, 3, false);
    cam.dist = 1.4;
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    REQUIRE(steer.Driving());
    CHECK(steer.FreeDistance() == doctest::Approx(3.6));
    // Opened again somewhere else (another scene, or Link walked on): learned anew.
    steer.Command(cam.angles, 3, false);
    cam.target = {cam.target[0] + 10.0, cam.target[1], cam.target[2]};
    cam.dist = 2.5;
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{40.0, 30.0});
    }
    CHECK(steer.FreeDistance() == doctest::Approx(2.5));
}

TEST_CASE("look steer: the log says the game eye's distance and Aim's, once, when the camera follows") {
    Steer steer;
    Camera cam;
    cam.dist = 3.25;
    for (int f = 0; f < 40; ++f) {
        cam.Take(steer.Command(cam.angles, 3, true));
        steer.Observe(cam.Eye(), cam.target);
        steer.Want(Direction{40.0, 30.0}, 5.27);
    }
    REQUIRE(steer.Driving());
    const auto note = steer.TakeNote();
    REQUIRE(note);
    CHECK(note->find("turns with the Customize picture") != std::string::npos);
    CHECK(note->find("its eye 3.25 m from its target, the picture's own 5.27 m") != std::string::npos);
    CHECK_FALSE(steer.TakeNote());
}

TEST_CASE("look steer: co-op switched off while the camera is turned puts its angles back, once") {
    // hos_coop_turn asks with `coop_on && the page asked` (coop_look_live.cpp Turn): co-op off is a page left.
    Steer steer;
    Camera cam;
    for (int f = 0; f < 40; ++f) {
        Frame(steer, cam, Direction{-120.0, 30.0});
    }
    REQUIRE(steer.Driving());
    const bool coop_on = false, page_asked = true;
    const auto back = steer.Command(cam.angles, 3, coop_on && page_asked);
    REQUIRE(back);
    CHECK(back->yaw == doctest::Approx(12.0f));
    CHECK(back->pitch == doctest::Approx(5.0f));
    CHECK_FALSE(steer.Command(cam.angles, 3, coop_on && page_asked));
}

namespace {

// The view hook as coop_look_live.cpp Camera runs it, around an adult Link: the orbit's shot, the camera's
// command, the eye the game placed (its distance from `wall`, by the eye's yaw: the game's collision), the
// picture, the orbit's direction for the next frame.
struct Place {
    Window w = Square();
    Subject link{{3.0f, 0.0f, -7.0f}, 1.9f};
    Camera cam;
    Steer steer;
    double orbit = 40.0;   // degrees
    std::function<double(double)> wall = [](double) { return 4.0; };
    Shot aim;
    Vec3 eye{};

    Place() { cam.target = {3.0, 1.6, -7.0}; }
    std::optional<Shot> Step() {
        aim = CoopLookView::Aim(link.feet, link.height, orbit * kRad, w);
        cam.Take(steer.Command(cam.angles, 3, true));
        cam.dist = 1.0;
        const auto dir = CoopLookView::DirectionOf(cam.target, cam.Eye());
        cam.dist = wall(dir ? dir->yaw : 0.0);
        eye = cam.Eye();
        steer.Observe(eye, cam.target);
        auto shot = steer.Picture(eye, cam.target, aim, link, w);
        steer.Want(CoopLookView::DirectionOf(cam.target, aim.eye), Distance(aim.eye, cam.target));
        return shot;
    }
    Vec3 Middle() const { return {link.feet[0], link.feet[1] + link.height * 0.5, link.feet[2]}; }
    bool IsAim(const std::optional<Shot>& s) const { return s && Distance(s->eye, aim.eye) < 1e-6; }
};

}  // namespace

TEST_CASE("look steer: a tight start (a narrow passage): never the orbit's through-wall shot, hidden until a clear view") {
    // The Bottom of the Well (camera-check-2): the passage runs along z; across it the rock pulls the game's eye in
    // to 0.9 m. The page opened with the camera across the passage, so the probe saw only 0.9 m.
    Place p;
    p.cam.angles = {90.0f, 20.0f};
    p.orbit = 90.0;
    p.wall = [](double yaw) {
        const double off = std::fabs(Wrap(yaw));
        return off < 25.0 || off > 155.0 ? 4.0 : 0.9;
    };
    int hidden = 0, clear = 0, held = 0;
    bool seen_clear = false;
    for (int f = 0; f < 600; ++f) {
        p.orbit = Wrap(90.0 + 0.75 * f);
        const auto shot = p.Step();
        if (!p.steer.Driving()) {
            continue;
        }
        CAPTURE(f);
        const double dist = Distance(p.eye, p.cam.target);
        CAPTURE(dist);
        if (dist < Steer::kBlockedMin) {
            // Blocked: the orbit's own shot (no collision: inside the rock) and the eye at the rock both never show.
            CHECK_FALSE(p.IsAim(shot));
            if (!seen_clear) {
                CHECK_FALSE(shot);
                CHECK(p.steer.Hidden());
                ++hidden;
            } else {
                REQUIRE(shot);
                CHECK(Distance(shot->eye, p.cam.target) > 3.0);   // (the last clear view, held)
                CHECK_FALSE(p.steer.Hidden());
                ++held;
            }
        } else {
            REQUIRE(shot);
            seen_clear = true;
            ++clear;
        }
    }
    CHECK(hidden > 0);
    CHECK(clear > 0);
    CHECK(held > 0);
    CHECK(p.steer.FreeDistance() == doctest::Approx(4.0));
}

TEST_CASE("look steer: an eye pulled in under max(1.2 m, 0.6 x free) is blocked, and above it framed where the game put it") {
    Place p;
    for (int f = 0; f < 40; ++f) {
        p.Step();
    }
    REQUIRE(p.steer.Driving());
    REQUIRE(p.steer.FreeDistance() == doctest::Approx(4.0));
    const auto open = p.Step();
    REQUIRE(p.IsAim(open));   // (open ground: the orbit's own picture, so he fits)
    const Vec3 good = open->eye;
    // 2.0 m and 1.0 m: under 0.6 x 4 m. The last clear view is held, not the game's eye at the wall.
    for (const double pulled : {2.0, 1.0}) {
        CAPTURE(pulled);
        p.wall = [pulled](double) { return pulled; };
        const auto shot = p.Step();
        REQUIRE(shot);
        CHECK(Distance(shot->eye, good) < 1e-6);
        CHECK(Distance(shot->eye, p.eye) > 0.5);
        const auto at = Project(*shot, p.Middle(), p.w.aspect);
        CHECK(at[0] == doctest::Approx((p.w.x0 + p.w.x1) * 0.5).epsilon(1e-6));
        CHECK(at[1] == doctest::Approx((p.w.y0 + p.w.y1) * 0.5).epsilon(1e-6));
    }
    // 2.9 m: moved in by a wall, but not to the lens: framed from where the game put it.
    p.wall = [](double) { return 2.9; };
    const auto framed = p.Step();
    REQUIRE(framed);
    CHECK(Distance(framed->eye, p.eye) < 1e-9);

    // A small free distance (a tight place all round): 1.2 m still counts.
    Place tight;
    tight.wall = [](double) { return 1.5; };
    for (int f = 0; f < 40; ++f) {
        tight.Step();
    }
    REQUIRE(tight.steer.Driving());
    tight.wall = [](double) { return 1.3; };
    const auto ok = tight.Step();   // (1.3 m: over 1.2 m, under 0.9 x 1.5 m: framed)
    REQUIRE(ok);
    CHECK(Distance(ok->eye, tight.eye) < 1e-9);
    tight.wall = [](double) { return 1.0; };
    const auto blocked = tight.Step();
    REQUIRE(blocked);
    CHECK(Distance(blocked->eye, ok->eye) < 1e-6);   // (held)
}

TEST_CASE("look steer: while the camera is driven the orbit's shot is never a fallback") {
    // Blocked from the first driven frame: nothing to hold, so no picture (the flat one), not the orbit's.
    Place p;
    p.wall = [](double) { return 0.9; };
    for (int f = 0; f < 40; ++f) {
        const auto shot = p.Step();
        if (p.steer.Driving()) {
            CHECK_FALSE(shot);
            CHECK(p.steer.Hidden());
        } else {
            CHECK(p.IsAim(shot));   // (the probe: the orbit's own, as before)
        }
    }
    REQUIRE(p.steer.Driving());
    // An eye the view cannot be re-aimed from (it would tilt past straight down): not the orbit's either.
    Place above;
    above.w = Window{0.3f, 0.0f, 0.7f, 0.2f, 16.0f / 9.0f};
    for (int f = 0; f < 40; ++f) {
        above.Step();
    }
    REQUIRE(above.steer.Driving());
    const Vec3 over = EyeAt(above.cam.target, 30.0, 85.0, 3.0);
    const Vec3 held = above.aim.eye;   // (the last clear view: open ground, the orbit's own)
    above.orbit += 90.0;                // (the orbit went on)
    above.aim = CoopLookView::Aim(above.link.feet, above.link.height, above.orbit * kRad, above.w);
    REQUIRE_FALSE(CoopLookView::Framed(over, above.cam.target, above.aim, above.link, above.w, 1e9));
    const auto kept = above.steer.Picture(over, above.cam.target, above.aim, above.link, above.w);
    REQUIRE(kept);
    CHECK_FALSE(above.IsAim(kept));
    CHECK(Distance(kept->eye, held) < 1e-6);
    // Not driven (no patch 0040, a camera that does not follow): the orbit's own picture, as before.
    Place lost;
    lost.cam.frozen = true;
    std::optional<Shot> last;
    for (int f = 0; f < 20; ++f) {
        last = lost.Step();
    }
    REQUIRE(lost.steer.phase() == Steer::Phase::lost);
    CHECK(lost.IsAim(last));
    CHECK_FALSE(lost.steer.Hidden());
}

TEST_CASE("look steer: the log says a blocked eye's distance and the free distance once per change") {
    Place p;
    for (int f = 0; f < 40; ++f) {
        p.Step();
    }
    REQUIRE(p.steer.Driving());
    while (p.steer.TakeNote()) {
    }
    p.wall = [](double) { return 0.9; };
    p.Step();
    const auto blocked = p.steer.TakeNote();
    REQUIRE(blocked);
    CHECK(blocked->find("its eye 0.90 m from its target") != std::string::npos);
    CHECK(blocked->find("free distance 4.00 m") != std::string::npos);
    CHECK(blocked->find("holds its last clear view") != std::string::npos);
    for (int f = 0; f < 10; ++f) {
        p.Step();
    }
    CHECK_FALSE(p.steer.TakeNote());   // (not every frame)
    p.wall = [](double) { return 4.0; };
    p.Step();
    const auto clear = p.steer.TakeNote();
    REQUIRE(clear);
    CHECK(clear->find("clear again") != std::string::npos);
    // At the edge (0.6 x 4 m = 2.4 m) the eye going back and forth a little says it once, not each time.
    p.wall = [](double) { return 2.35; };
    p.Step();
    CHECK(p.steer.TakeNote());
    for (int f = 0; f < 10; ++f) {
        p.wall = [f](double) { return f % 2 ? 2.35 : 2.45; };
        p.Step();
    }
    CHECK_FALSE(p.steer.TakeNote());
}

TEST_CASE("look steer: blocked with no clear view yet, the camera rises toward kLiftTo and is not given up") {
    Place p;
    p.wall = [](double) { return 0.9; };
    double first = 0, last = 0;
    for (int f = 0; f < 120; ++f) {
        p.Step();
        if (p.steer.Driving()) {
            const auto seen = Seen(p.cam);
            REQUIRE(seen);
            if (first == 0) {
                first = seen->elevation;
            }
            last = seen->elevation;
        }
    }
    CHECK(p.steer.Driving());   // (the rise is wanted: not "off by" the orbit)
    CHECK(last > first + 20.0);
    CHECK(last == doctest::Approx(Steer::kLiftTo).epsilon(0.02));
    bool said = false;
    while (const auto note = p.steer.TakeNote()) {
        said = said || note->find("no clear view yet") != std::string::npos;
    }
    CHECK(said);
    // Far enough now, but the window sits so high on the screen that from up there the view would tilt past
    // straight down: lowered again until Link can be framed.
    p.w = Window{0.6f, 0.0f, 0.95f, 0.15f, 16.0f / 9.0f};
    p.wall = [](double) { return 4.0; };
    std::optional<Shot> shot;
    int frames = 0;
    for (; frames < 60 && !shot; ++frames) {
        shot = p.Step();
    }
    REQUIRE(shot);
    CHECK(frames > 1);
    const auto lowered = Seen(p.cam);
    REQUIRE(lowered);
    CHECK(lowered->elevation < Steer::kLiftTo - 2.0);
    CHECK(Distance(shot->eye, p.eye) < 1e-9);
}

TEST_CASE("look steer: once the view is clear, a raised camera comes back down to the orbit's elevation") {
    // The orbit's own elevation, in open ground.
    Place open;
    for (int f = 0; f < 60; ++f) {
        open.Step();
    }
    REQUIRE(open.steer.Driving());
    const auto level = Seen(open.cam);
    REQUIRE(level);
    // Blocked from the start: raised to kLiftTo.
    Place p;
    p.wall = [](double) { return 0.9; };
    for (int f = 0; f < 120; ++f) {
        p.Step();
    }
    REQUIRE(Seen(p.cam)->elevation == doctest::Approx(Steer::kLiftTo).epsilon(0.02));
    // Clear (the turn reached the open side): lowered a step at a time, the picture shown all the way.
    p.wall = [](double) { return 4.0; };
    double before = Seen(p.cam)->elevation;
    for (int f = 0; f < 80; ++f) {
        CAPTURE(f);
        CHECK(p.Step());
        const double now = Seen(p.cam)->elevation;
        CHECK(now <= before + 1e-6);
        before = now;
    }
    CHECK(Seen(p.cam)->elevation == doctest::Approx(level->elevation).epsilon(0.01));
    CHECK(p.steer.Driving());
    CHECK_FALSE(p.steer.Hidden());
}

TEST_CASE("look steer: a normal-play camera state (3 or 4) with the page asked starts the probe, and says nothing else") {
    for (const int state : {3, 4}) {
        CAPTURE(state);
        Steer steer;
        steer.Want(Direction{30.0, 20.0}, 4.0);
        steer.Command({10.0f, 15.0f}, state, true);
        CHECK(steer.phase() == Steer::Phase::probe);
    }
    // The whole way in normal play (state 3, the view seen each frame): driven, and no "not turned" line.
    Place p;
    for (int f = 0; f < Steer::kGateCalls * 3; ++f) {
        p.Step();
    }
    CHECK(p.steer.Driving());
    while (const auto note = p.steer.TakeNote()) {
        CHECK(note->find("is not turned") == std::string::npos);
    }
}

TEST_CASE("look steer: while the page is asked and the camera is not turned, the log says the hook's state once per change") {
    Steer steer;
    steer.Want(Direction{30.0, 20.0}, 4.0);
    // Another camera's state: idle, said once after kGateCalls calls, not each frame.
    for (int f = 0; f < Steer::kGateCalls - 1; ++f) {
        steer.Command({10.0f, 15.0f}, 7, true);
    }
    CHECK_FALSE(steer.TakeNote());   // (not a passing frame)
    steer.Command({10.0f, 15.0f}, 7, true);
    const auto first = steer.TakeNote();
    REQUIRE(first);
    CHECK(first->find("hands in state 7") != std::string::npos);
    CHECK(first->find("a wanted direction: yes") != std::string::npos);
    CHECK(first->find("has not started (idle)") != std::string::npos);
    for (int f = 0; f < Steer::kGateCalls * 3; ++f) {
        steer.Command({10.0f, 15.0f}, 7, true);
    }
    CHECK_FALSE(steer.TakeNote());
    // A change (no wanted direction now): said again.
    steer.Want(std::nullopt);
    for (int f = 0; f < Steer::kGateCalls; ++f) {
        steer.Command({10.0f, 15.0f}, 7, true);
    }
    const auto second = steer.TakeNote();
    REQUIRE(second);
    CHECK(second->find("a wanted direction: no") != std::string::npos);
    // Not asked: nothing.
    Steer closed;
    closed.Want(Direction{30.0, 20.0}, 4.0);
    for (int f = 0; f < Steer::kGateCalls * 2; ++f) {
        closed.Command({10.0f, 15.0f}, 7, false);
    }
    CHECK_FALSE(closed.TakeNote());
    // Back to a normal-play state: the probe starts.
    steer.Want(Direction{30.0, 20.0}, 4.0);
    steer.Command({10.0f, 15.0f}, 3, true);
    CHECK(steer.phase() == Steer::Phase::probe);
}

TEST_CASE("look steer: the view hook with no camera hook call says so once") {
    Steer steer;
    for (int f = 0; f < Steer::kNoTurnFrames - 1; ++f) {
        steer.Viewed();
    }
    CHECK_FALSE(steer.TakeNote());
    steer.Viewed();
    const auto note = steer.TakeNote();
    REQUIRE(note);
    CHECK(note->find("hos_coop_turn") != std::string::npos);
    for (int f = 0; f < Steer::kNoTurnFrames * 2; ++f) {
        steer.Viewed();
    }
    CHECK_FALSE(steer.TakeNote());
    // The hook called each frame: never said.
    Steer called;
    called.Want(Direction{30.0, 20.0}, 4.0);
    for (int f = 0; f < Steer::kNoTurnFrames * 2; ++f) {
        called.Viewed();
        called.Command({10.0f, 15.0f}, 3, true);
    }
    while (const auto n = called.TakeNote()) {
        CHECK(n->find("was not called") == std::string::npos);
    }
}
