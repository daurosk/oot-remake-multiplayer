// Tests for the test-input script parser and stepper
// (lichii/hook/coop_testinput.cpp). No emulator code.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "coop_testinput.h"

using namespace CoopTestInput;

namespace {

Script Must(const char* text) {
    std::string error;
    auto s = Parse(text, error);
    REQUIRE_MESSAGE(s, error);
    return *s;
}

// The output at `t` seconds when only time matters.
Output At(Runner& r, double t, std::vector<std::string>* log = nullptr) {
    std::vector<std::string> ignored;
    r.Advance(t, std::nullopt, log ? *log : ignored);
    return r.Current();
}

double Tick(int n) {
    return static_cast<double>(n) / kTicksPerSecond;
}

} // namespace

TEST_CASE("testinput: wait, hold, press and marks in time") {
    Runner r(Must("# load the save\n"
                  "wait 120\n"
                  "mark go\n"
                  "hold LSTICK_UP+B 90   # run\n"
                  "press a\n"
                  "press ZR+A 10\n"));
    std::vector<std::string> log;
    CHECK(At(r, 0, &log).buttons == 0);
    CHECK(log.empty());
    const Output run = At(r, Tick(120) + 0.001, &log);
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark go");
    CHECK(run.buttons == kB);
    REQUIRE(run.left);
    CHECK(*run.left == Stick{0, kStickMax});
    CHECK(At(r, Tick(209)).buttons == kB);
    CHECK(At(r, Tick(211)).buttons == kA);
    CHECK(At(r, Tick(217)).buttons == 0);
    CHECK(At(r, Tick(223)).buttons == (kZR | kA));
    CHECK(At(r, Tick(233)).buttons == 0);
    CHECK_FALSE(r.Done());
    CHECK(At(r, Tick(243)) == Output{});
    CHECK(r.Done());
}

TEST_CASE("testinput: a late frame skips the steps it missed, in order") {
    Runner r(Must("wait 10\nmark a\nhold A 10\nmark b\nhold B 10\n"));
    std::vector<std::string> log;
    CHECK(At(r, Tick(25), &log).buttons == kB);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "mark a");
    CHECK(log[1] == "mark b");
}

TEST_CASE("testinput: sticks") {
    Runner r(Must("stick 0.5 -1 60 B\nrstick -1 0 30\nhold LSTICK_UP+LSTICK_LEFT 5\n"));
    const Output a = At(r, 0);
    REQUIRE(a.left);
    CHECK(a.left->x == (kStickMax + 1) / 2);
    CHECK(a.left->y == -kStickMax);
    CHECK(a.buttons == kB);
    CHECK_FALSE(a.right);
    const Output b = At(r, Tick(61));
    REQUIRE(b.right);
    CHECK(b.right->x == -kStickMax);
    CHECK_FALSE(b.left);
    const Output c = At(r, Tick(91));
    REQUIRE(c.left);
    CHECK(*c.left == Stick{-kStickMax, kStickMax});
}

TEST_CASE("testinput: bad lines say where") {
    std::string error;
    CHECK_FALSE(Parse("wait 10\nhold Q 5\n", error));
    CHECK(error == "line 2: unknown key Q");
    CHECK_FALSE(Parse("wait\n", error));
    CHECK(error.rfind("line 1:", 0) == 0);
    CHECK_FALSE(Parse("wait 0\n", error));
    CHECK_FALSE(Parse("wait -5\n", error));
    CHECK_FALSE(Parse("stick 2 0 10\n", error));
    CHECK_FALSE(Parse("stick 0 0 10 A Q\n", error));
    CHECK_FALSE(Parse("jump 10\n", error));
    CHECK(error == "line 1: unknown command jump");
    CHECK_FALSE(Parse("hold A+ 5\n", error));
    CHECK_FALSE(Parse("goto 1\n", error));
    CHECK_FALSE(Parse("goto 1 2 0.1\n", error));
    CHECK_FALSE(Parse("goto x 2\n", error));
    CHECK_FALSE(Parse("toward 1 2\n", error));
    CHECK_FALSE(Parse("  # only a comment\n\n", error));
    CHECK(error == "no steps");
}

TEST_CASE("testinput: windows line ends") {
    Runner r(Must("wait 5\r\npress B\r\n"));
    CHECK(At(r, Tick(6)).buttons == kB);
    CHECK(At(r, Tick(18)) == Output{});
    CHECK(r.Done());
}

namespace {

// A Link that walks where the stick says, through a camera turned by
// `camera` radians and, with `mirrored`, flipped left for right.
struct FakeLink {
    double camera = 0.0;
    bool mirrored = false;
    std::array<float, 2> pos{0.0f, 0.0f};

    void Step(const Output& out, double dt) {
        if (!out.left || (out.left->x == 0 && out.left->y == 0)) {
            return;
        }
        const double sx = static_cast<double>(out.left->x) / kStickMax;
        const double sy = static_cast<double>(out.left->y) / kStickMax;
        const double mag = std::min(1.0, std::hypot(sx, sy));
        double a = std::atan2(sy, sx) - 3.14159265358979 / 2;  // 0 = stick up
        if (mirrored) {
            a = -a;
        }
        a += camera;  // world angle of stick up
        pos[0] += static_cast<float>(std::cos(a) * 5.0 * mag * dt);
        pos[1] += static_cast<float>(std::sin(a) * 5.0 * mag * dt);
    }
};

// Plays `script` at 30 frames a second for up to `seconds`.
bool Play(Runner& r, FakeLink& link, double seconds, std::vector<std::string>& log) {
    const double dt = 1.0 / 30;
    for (double t = 0; t < seconds; t += dt) {
        r.Advance(t, link.pos, log);
        if (r.Done()) {
            return true;
        }
        link.Step(r.Current(), dt);
    }
    return false;
}

} // namespace

TEST_CASE("testinput: goto reaches the point whatever the camera") {
    for (const double camera : {0.0, 1.0, 2.5, -2.0}) {
        for (const bool mirrored : {false, true}) {
            CAPTURE(camera);
            CAPTURE(mirrored);
            Runner r(Must("goto 8 -6\nmark there\ngoto -3 4 0.5\n"));
            FakeLink link{camera, mirrored};
            std::vector<std::string> log;
            REQUIRE(Play(r, link, 20.0, log));
            REQUIRE(log.size() == 1);
            CHECK(log[0] == "mark there");
            CHECK(std::hypot(link.pos[0] + 3.0f, link.pos[1] - 4.0f) <= 0.5f);
        }
    }
}

TEST_CASE("testinput: a goto that cannot get there gives up and goes on") {
    Runner r(Must("goto 100 0 1 60\nmark after\n"));
    FakeLink link;
    std::vector<std::string> log;
    for (double t = 0; t < 1.5; t += 1.0 / 30) {
        r.Advance(t, link.pos, log);  // Link never moves
    }
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "goto timed out at line 1");
    CHECK(log[1] == "mark after");
    CHECK(r.Done());
}

TEST_CASE("testinput: toward turns Link to the point for its ticks, with buttons") {
    Runner r(Must("goto 0 5\ntoward 5 5 6 Y\n"));
    FakeLink link{0.7, false};
    std::vector<std::string> log;
    const double dt = 1.0 / 30;
    double t = 0;
    for (; t < 10 && !r.Done(); t += dt) {
        r.Advance(t, link.pos, log);
        if (r.Current().buttons == kY) {
            break;
        }
        link.Step(r.Current(), dt);
    }
    REQUIRE(r.Current().buttons == kY);
    // The stick now walks Link along +x in the world, toward the point.
    const auto before = link.pos;
    link.Step(r.Current(), dt);
    CHECK(link.pos[0] - before[0] > 0.1f);
    CHECK(std::fabs(link.pos[1] - before[1]) < 0.05f);
}
