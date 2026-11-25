// Tests for the rule that moves a copy's body (lichii/hook/coop_puppet_body.h, native puppets phase 1).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "coop_puppet_body.h"

using CoopPuppetBody::Capsule;
using CoopPuppetBody::Carry;
using CoopPuppetBody::Command;
using CoopPuppetBody::Drive;
using CoopPuppetBody::Input;
using CoopPuppetBody::Kind;
using CoopPuppetBody::Pose;
using CoopPuppetBody::PoseHistory;
using CoopPuppetBody::Vec3;

namespace {

constexpr float kDt = 1.0f / 30.0f;

Input At(const Vec3& body, const Vec3& place, const Vec3& velocity = {}, bool channel = true) {
    Input in;
    in.body = body;
    in.place = place;
    in.velocity = velocity;
    in.dt = kDt;
    in.speed_channel = channel;
    return in;
}

}  // namespace

TEST_CASE("puppet body: which of the pack's actors have a body") {
    using CoopPuppetBody::PackBodyActors;
    using Names = std::vector<std::string>;
    // --puppet-body: every copy has one.
    CHECK(PackBodyActors(R"({"actors": [{"actor": "CoopPuppet_Child", "from": "Player_Child", "body": {"a": 1}},
        {"actor": "CoopPuppet_Adult", "body": {"b": 2}}], "body": true})") ==
          Names{"CoopPuppet_Child", "CoopPuppet_Adult"});
    // --puppet-pvp alone: the pack's own "body" is false and only the _Pvp copies have one (build.py gives an
    // actor without a body {}); the same slot places them, so they are moved by the rule too.
    CHECK(PackBodyActors(R"({"actors": [{"actor": "CoopPuppet_Child", "body": {}},
        {"actor": "CoopPuppet_Child_Pvp", "body": {"Phive/x": 40}}, {"actor": "CoopPuppet_Adult", "body": { }}],
        "perception": true, "body": false})") == Names{"CoopPuppet_Child_Pvp"});
    // The pack's own switch after the last actor is no actor's body; a pack from before bodies has none.
    CHECK(PackBodyActors(R"({"actors": [{"actor": "CoopPuppet_Child", "members": []}], "body": true})").empty());
    CHECK(PackBodyActors(R"({"actors": [{"actor": "CoopPuppet_Child", "body": null}], "body": false})").empty());
    CHECK(PackBodyActors(R"({"tool": "tools/puppet_pack"})").empty());
    CHECK(PackBodyActors(R"({"actors": [{"actor": )").empty());
}

TEST_CASE("puppet body: a body where it should be is left alone") {
    Drive d;
    const Command c = d.Step(At({1, 2, 3}, {1, 2, 3}));
    CHECK(c.kind == Kind::rest);
    // Within the rest distance and no turn: still rest, with or without a speed channel.
    CHECK(d.Step(At({1, 2, 3}, {1.001f, 2, 3})).kind == Kind::rest);
    CHECK(d.Step(At({1, 2, 3}, {1.001f, 2, 3}, {}, false)).kind == Kind::rest);
    // Turned on the spot: the physics is told.
    Input turn = At({1, 2, 3}, {1, 2, 3}, {}, false);
    turn.turned = 0.1f;
    CHECK(d.Step(turn).kind == Kind::warp);
    // About to move (the samples have a speed): not rest.
    CHECK(d.Step(At({1, 2, 3}, {1, 2, 3}, {3, 0, 0})).kind == Kind::drive);
}

TEST_CASE("puppet body: without a speed channel every move is the game's own setter") {
    Drive d;
    const Command c = d.Step(At({0, 0, 0}, {0.2f, 0, 0}, {6, 0, 0}, false));
    CHECK(c.kind == Kind::warp);
    CHECK(c.place[0] == doctest::Approx(0.2f));
    CHECK(c.velocity[0] == 0.0f);
}

TEST_CASE("puppet body: the speed takes the body to the place and never past the cap") {
    Drive d;
    // Following a walk at 6 m/s with a 0.1 m gap: the samples' speed plus the gain's share of the gap.
    const Command c = d.Step(At({0, 0, 0}, {0.1f, 0, 0}, {6, 0, 0}));
    REQUIRE(c.kind == Kind::drive);
    CHECK(c.velocity[0] == doctest::Approx(6.0f + 0.1f * CoopPuppetBody::kGain / kDt));
    CHECK(c.velocity[1] == 0.0f);
    // A body driven so closes the gap and holds it closed (no growing overshoot).
    Drive e;
    Vec3 body{0, 0, 0};
    Vec3 place{0.3f, 0, 0};
    for (int i = 0; i < 60; ++i) {
        const Command s = e.Step(At(body, place, {6, 0, 0}));
        REQUIRE(s.kind == Kind::drive);
        body[0] += s.velocity[0] * kDt;  // the solver moves it freely
        place[0] += 6.0f * kDt;          // the samples walk on
        CHECK(body[0] <= place[0] + 1e-3f);  // never ahead of the samples
    }
    CHECK(place[0] - body[0] < 0.005f);
    CHECK(place[0] - body[0] > -0.005f);
    // The cap.
    const Command fast = Drive{}.Step(At({0, 0, 0}, {1.4f, 0, 0}, {30, 0, 0}));
    REQUIRE(fast.kind == Kind::drive);
    CHECK(CoopPuppetBody::Length(fast.velocity) == doctest::Approx(CoopPuppetBody::kMaxSpeed));
}

TEST_CASE("puppet body: falls and knock-back are followed, never simulated") {
    // The samples fall at 12 m/s: the speed handed on is theirs (down), with no gravity of ours added.
    Drive d;
    const Command c = d.Step(At({0, 10, 0}, {0, 10, 0}, {2, -12, 0}));
    REQUIRE(c.kind == Kind::drive);
    CHECK(c.velocity[0] == doctest::Approx(2.0f));
    CHECK(c.velocity[1] == doctest::Approx(-12.0f));
}

TEST_CASE("puppet body: warps") {
    Drive d;
    // A scene's spawn point / a teleport (the director says the place jumped).
    Input jump = At({0, 0, 0}, {0.5f, 0, 0}, {1, 0, 0});
    jump.jump = true;
    CHECK(d.Step(jump).kind == Kind::warp);
    // A long gap is never closed by speed.
    CHECK(d.Step(At({0, 0, 0}, {5, 0, 0}, {1, 0, 0})).kind == Kind::warp);
    // The body's place not read: put, not driven.
    Input unread = At({0, 0, 0}, {0.1f, 0, 0});
    unread.body.reset();
    CHECK(d.Step(unread).kind == Kind::warp);
    // A stalled frame: no speed is made from its time.
    Input stall = At({0, 0, 0}, {0.1f, 0, 0}, {3, 0, 0});
    stall.dt = 0.5f;
    CHECK(d.Step(stall).kind == Kind::warp);
    // A place that is no number: nothing is sent.
    Input bad = At({0, 0, 0}, {std::nanf(""), 0, 0});
    CHECK(d.Step(bad).kind == Kind::rest);
}

TEST_CASE("puppet body: a body held back by this game alone is warped past it") {
    Drive d;
    const Vec3 body{0, 0, 0};  // the solver never lets it move (a block only we have)
    int warped_at = -1;
    for (int i = 0; i < 20 && warped_at < 0; ++i) {
        const Command c = d.Step(At(body, {0.8f, 0, 0}));
        if (c.kind == Kind::warp) {
            warped_at = i;
        } else {
            CHECK(c.kind == Kind::drive);
        }
    }
    CHECK(warped_at == CoopPuppetBody::kHeldFrames - 1);
    // A small steady lag (a contact it leans on) is not that: never warped.
    Drive e;
    for (int i = 0; i < 60; ++i) {
        CHECK(e.Step(At({0, 0, 0}, {0.3f, 0, 0})).kind == Kind::drive);
    }
}

TEST_CASE("puppet body: bodies that only touch are left to the game") {
    // Our Link stands on a copy (his feet a few millimetres in its top) or leans on it: no place of ours
    // moves the copy from under him, or it would sink a little every frame.
    const Capsule ours = CoopPuppetBody::LinkBody({0, 1.16f, 0}, true);
    float moved = 1.0f;
    Vec3 p = CoopPuppetBody::Yield(CoopPuppetBody::LinkBody({0, 0, 0}, false), ours, &moved);
    CHECK(p[1] == 0.0f);
    CHECK(moved == 0.0f);
    p = CoopPuppetBody::Yield(CoopPuppetBody::LinkBody({0.56f, 1.16f, 0}, false), ours, &moved);
    CHECK(p[0] == doctest::Approx(0.56f));
    CHECK(moved == 0.0f);
    // The sizes are the game's own (child 0.23 x 1.17, adult 0.35 x 1.68), by the actor's name.
    CHECK(CoopPuppetBody::AdultName("CoopPuppet_Adult_Red_Pvp"));
    CHECK_FALSE(CoopPuppetBody::AdultName("CoopPuppet_Child"));
    CHECK(CoopPuppetBody::LinkBody({}, false).height == doctest::Approx(1.17f));
    // Deep by lag (their Link walked where ours stands): out sideways until they touch.
    p = CoopPuppetBody::Yield(CoopPuppetBody::LinkBody({0.3f, 1.16f, 0}, false), ours, &moved);
    CHECK(p[0] == doctest::Approx(0.58f));
    CHECK(moved == doctest::Approx(0.28f));
}

TEST_CASE("puppet body: a copy is never put inside our own Link") {
    const Capsule ours{{0, 0, 0}, 0.3f, 1.2f};
    float moved = 0.0f;
    // Apart: untouched.
    Vec3 p = CoopPuppetBody::Yield({{1, 0, 0}, 0.3f, 1.2f}, ours, &moved);
    CHECK(p[0] == doctest::Approx(1.0f));
    CHECK(moved == 0.0f);
    // Side by side and overlapping by lag: moved out along the line between them, no further.
    p = CoopPuppetBody::Yield({{0.4f, 0, 0}, 0.3f, 1.2f}, ours, &moved);
    CHECK(p[0] == doctest::Approx(0.6f));
    CHECK(p[1] == 0.0f);
    CHECK(moved == doctest::Approx(0.2f));
    p = CoopPuppetBody::Yield({{0, 0, -0.4f}, 0.3f, 1.2f}, ours);
    CHECK(p[2] == doctest::Approx(-0.6f));
    // Our Link stands on the copy and it rises a little into him: it stays under his feet (he is
    // not thrown off sideways).
    const Capsule on_top{{0.05f, 1.15f, 0}, 0.3f, 1.2f};
    p = CoopPuppetBody::Yield({{0, 0, 0}, 0.3f, 1.2f}, on_top, &moved);
    CHECK(p[0] == 0.0f);
    CHECK(p[1] == doctest::Approx(-0.05f));
    CHECK(moved == doctest::Approx(0.05f));
    // The copy stands on our Link's head and sinks a little: it stays on his head.
    p = CoopPuppetBody::Yield({{0, 1.1f, 0}, 0.3f, 1.2f}, ours);
    CHECK(p[1] == doctest::Approx(1.2f));
    // Exactly the same spot: one fixed side, a full step out.
    p = CoopPuppetBody::Yield({{0, 0, 0}, 0.3f, 1.2f}, ours);
    CHECK(p[0] == doctest::Approx(0.6f));
    // Our Link standing cleanly on the copy's head: untouched (he can stand there).
    p = CoopPuppetBody::Yield({{0, 0, 0}, 0.3f, 1.2f}, {{0, 1.2f, 0}, 0.3f, 1.2f}, &moved);
    CHECK(p[1] == 0.0f);
    CHECK(moved == 0.0f);
}

TEST_CASE("puppet body: a place on a platform is carried by our own platform") {
    // The platform slid 2 m and rose 1 m since the sample: the place went with it.
    Vec3 p = Carry({{10, 0, 10}, 0.0f}, {{12, 1, 10}, 0.0f}, {10.5f, 0.2f, 9.5f});
    CHECK(p[0] == doctest::Approx(12.5f));
    CHECK(p[1] == doctest::Approx(1.2f));
    CHECK(p[2] == doctest::Approx(9.5f));
    // A turning floor: a quarter turn about its middle (x to -z for a positive yaw).
    const float quarter = 1.5707963f;
    p = Carry({{0, 0, 0}, 0.0f}, {{0, 0, 0}, quarter}, {1, 0.5f, 0});
    CHECK(p[0] == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(p[1] == doctest::Approx(0.5f));
    CHECK(p[2] == doctest::Approx(-1.0f));
    // A still platform carries nothing.
    p = Carry({{3, 3, 3}, 1.0f}, {{3, 3, 3}, 1.0f}, {4, 3, 5});
    CHECK(p[0] == doctest::Approx(4.0f));
    CHECK(p[2] == doctest::Approx(5.0f));
    // The rider's own walk: the platform's slide and swing taken out of their speed.
    const Vec3 v = CoopPuppetBody::RiderSpeed({2.5f, 1.0f, 0.0f}, {2, 1, 0}, {{0, 0, 0}, 0.0f}, 0.0f, {1, 0, 0});
    CHECK(v[0] == doctest::Approx(0.5f));
    CHECK(v[1] == doctest::Approx(0.0f));
    // Standing still 1 m out on a floor turning 1 rad/s: their world speed is all swing, none is walk.
    const Vec3 w = CoopPuppetBody::RiderSpeed({0, 0, -1.0f}, {}, {{0, 0, 0}, 0.0f}, 1.0f, {1, 0, 0});
    CHECK(CoopPuppetBody::Length(w) == doctest::Approx(0.0f));
}

TEST_CASE("puppet body: where a platform was when a sample was made") {
    PoseHistory<4> h;
    CHECK_FALSE(h.At(100).has_value());
    h.Push(100, {{0, 0, 0}, 0.0f});
    h.Push(200, {{2, 0, 0}, 1.0f});
    h.Push(200, {{9, 9, 9}, 9.0f});  // not newer: dropped
    REQUIRE(h.At(150).has_value());
    CHECK(h.At(150)->at[0] == doctest::Approx(1.0f));
    CHECK(h.At(150)->yaw == doctest::Approx(0.5f));
    CHECK(h.At(200)->at[0] == doctest::Approx(2.0f));
    CHECK(h.At(500)->at[0] == doctest::Approx(2.0f));  // newer than the record: the last pose
    CHECK_FALSE(h.At(50).has_value());                 // older than the record: not known
    // The ring keeps the newest four.
    for (std::uint64_t t = 300; t <= 600; t += 100) {
        h.Push(t, {{static_cast<float>(t) / 100.0f, 0, 0}, 0.0f});
    }
    CHECK_FALSE(h.At(200).has_value());
    CHECK(h.At(450)->at[0] == doctest::Approx(4.5f));
    h.Clear();
    CHECK_FALSE(h.At(450).has_value());
}
