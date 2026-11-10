// Tests for the test-input script parser and stepper
// (lichii/hook/coop_testinput.cpp). No emulator code.
#include <doctest/doctest.h>

#include <algorithm>
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
// `camera` radians and, with `mirrored`, flipped left for right. Like the
// game's, he turns at most 8 radians a second and walks only once he faces
// within 1 radian of the stick.
struct FakeLink {
    double camera = 0.0;
    bool mirrored = false;
    std::array<float, 2> pos{0.0f, 0.0f};
    double facing = 0.0;

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
        a += camera;  // the world angle the stick asks for
        double turn = std::remainder(a - facing, 2 * 3.14159265358979);
        const double most = 8.0 * dt;
        turn = std::clamp(turn, -most, most);
        facing += turn;
        if (std::fabs(std::remainder(a - facing, 2 * 3.14159265358979)) > 1.0) {
            return;
        }
        const float nx = pos[0] + static_cast<float>(std::cos(facing) * 5.0 * mag * dt);
        const float nz = pos[1] + static_cast<float>(std::sin(facing) * 5.0 * mag * dt);
        if (wall && nx >= 2.0f && nx <= 2.5f && std::fabs(nz) <= 1.5f) {
            return;  // a short wall across the way: x 2 to 2.5, z -1.5 to 1.5
        }
        pos = {nx, nz};
    }
    bool wall = false;
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
    for (int i = 0; i < 15; ++i) {  // he turns first, then walks
        link.Step(r.Current(), dt);
    }
    const float dx = link.pos[0] - before[0];
    const float dz = link.pos[1] - before[1];
    CHECK(dx > 0.3f);
    CHECK(std::fabs(dz) < 0.3f * dx);
}

TEST_CASE("testinput: goto gets round a short wall in the way") {
    for (const double camera : {0.0, 1.2}) {
        CAPTURE(camera);
        Runner r(Must("goto 5 0 0.6 1200\nmark there\n"));
        FakeLink link{camera, false};
        link.wall = true;
        std::vector<std::string> log;
        REQUIRE(Play(r, link, 25.0, log));
        REQUIRE(log.size() == 1);
        CHECK(log[0] == "mark there");
    }
}

TEST_CASE("testinput: goto with the game's camera, which turns behind Link as he walks") {
    for (const bool mirrored : {false, true}) {
        CAPTURE(mirrored);
        Runner r(Must("goto 8 -6 0.6\nmark there\ngoto -3 4 0.6\n"));
        FakeLink link{0.4, mirrored};
        std::vector<std::string> log;
        const double dt = 1.0 / 30;
        bool done = false;
        for (double t = 0; t < 20.0 && !done; t += dt) {
            const double c = link.camera;
            const double side = mirrored ? 1.0 : -1.0;  // where stick right walks
            Runner::Camera cam{static_cast<float>(std::cos(c)), static_cast<float>(std::sin(c)),
                               static_cast<float>(std::cos(c + side * 1.5707963)),
                               static_cast<float>(std::sin(c + side * 1.5707963))};
            r.Advance(t, link.pos, log, cam);
            done = r.Done();
            link.Step(r.Current(), dt);
            // The camera swings round behind Link at 1.5 radians a second.
            const double behind = std::remainder(link.facing - link.camera, 2 * 3.14159265358979);
            link.camera += std::clamp(behind, -1.5 * dt, 1.5 * dt);
        }
        REQUIRE(done);
        REQUIRE(log.size() == 1);
        CHECK(std::hypot(link.pos[0] + 3.0f, link.pos[1] - 4.0f) <= 0.6f);
    }
}

TEST_CASE("testinput: warp and give are handed to the hook once, in order") {
    std::string error;
    CHECK_FALSE(Parse("warp\n", error));
    CHECK_FALSE(Parse("warp A B C\n", error));
    CHECK_FALSE(Parse("give\n", error));
    Runner r(Must("warp KakarikoVillage_Child Kakariko_WellSide\ngive bombs\nwait 10\n"
                  "warp WaterTemple\nmark after\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::warp);
    CHECK(requests[0].name == "KakarikoVillage_Child");
    CHECK(requests[0].start == "Kakariko_WellSide");
    CHECK(requests[1].kind == Kind::give);
    CHECK(requests[1].name == "bombs");
    CHECK(r.TakeRequests().empty());
    r.Advance(Tick(11), std::nullopt, log);
    requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].name == "WaterTemple");
    CHECK(requests[0].start.empty());
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark after");
}

TEST_CASE("testinput: wear names a gear slot and a piece") {
    std::string error;
    CHECK_FALSE(Parse("wear head\n", error));
    CHECK_FALSE(Parse("wear hat Armor_Child_Mask_Keaton\n", error));
    CHECK(WearFlag("head") == std::optional<std::string>("EquipHeadArmor"));
    CHECK(WearFlag("Boots") == std::optional<std::string>("EquipBoot"));
    CHECK_FALSE(WearFlag("hand"));
    Runner r(Must("wear head Armor_Child_Mask_Keaton\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::wear);
    CHECK(requests[0].start == "head");
    CHECK(requests[0].name == "Armor_Child_Mask_Keaton");
}
