// Tests for another player's horse (lichii/hook/coop_ride.cpp, issue 199):
// what the rider sends and the copy the others draw under its puppet.
#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_objects_sync.h"
#include "coop_ride.h"

namespace proto = coop::protocol;
using CoopRide::HorseSeen;
using CoopRide::Mat3;
using CoopRide::Quat;
using CoopRide::Vec3;

namespace {

Quat Yaw(float degrees) {
    const float h = degrees * 3.14159265f / 360.0f;
    return {0.0f, std::sin(h), 0.0f, std::cos(h)};
}

bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f) {
    return std::fabs(a[0] - b[0]) < eps && std::fabs(a[1] - b[1]) < eps && std::fabs(a[2] - b[2]) < eps;
}

bool Near(const Mat3& a, const Mat3& b, float eps = 1e-3f) {
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(a[i] - b[i]) >= eps) {
            return false;
        }
    }
    return true;
}

Vec3 Add(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// Through the codec, as the server relays it: player 2's.
proto::PlayerRide Relayed(proto::PlayerRide m) {
    m.player_number = 2;
    std::vector<std::uint8_t> bytes;
    REQUIRE(proto::encode(proto::Message{m}, bytes) == proto::Error::none);
    proto::Message back;
    REQUIRE(proto::decode(bytes, back) == proto::Error::none);
    return std::get<proto::PlayerRide>(back);
}

CoopPuppet::Remote Rider(const Vec3& at, const Quat& turn, const char* anim = "HorseMove") {
    CoopPuppet::Remote r;
    r.number = 2;
    r.scene = "HyruleField";
    r.position = at;
    r.rotation = turn;
    r.adult = true;
    r.anim = CoopPuppet::Anim{anim, 0.0f, 1.0f};
    return r;
}

constexpr std::uint32_t kP2Live = 1u << 2;

} // namespace

TEST_CASE("ride: a turn and a place in the rider's frame come back exactly") {
    const Mat3 frame = CoopPuppet::QuatToMatrix(Yaw(70.0f));
    const Mat3 horse = CoopPuppet::QuatToMatrix(Yaw(95.0f));
    CHECK(Near(CoopRide::Absolute(frame, CoopRide::Relative(frame, horse)), horse));
    const Vec3 d{0.3f, -1.2f, 0.8f};
    CHECK(Near(CoopRide::FromFrame(frame, CoopRide::ToFrame(frame, d)), d));
    // Turned with the rider, the offset turns too.
    const Mat3 other = CoopPuppet::QuatToMatrix(Yaw(160.0f));
    const Vec3 local = CoopRide::ToFrame(frame, d);
    const Vec3 turned = CoopRide::FromFrame(other, local);
    CHECK(std::fabs(std::hypot(turned[0], turned[2]) - std::hypot(d[0], d[2])) < 1e-3f);
    CHECK(std::fabs(turned[1] - d[1]) < 1e-3f);
}

TEST_CASE("ride: the rider sends its horse 30 times a second, and riding off when it gets down") {
    CoopRide::Rider rider;
    const Vec3 me{100.0f, 5.0f, -40.0f};
    const Quat turn = Yaw(30.0f);
    HorseSeen horse;
    horse.address = 0x1234;
    horse.position = Add(me, {0.0f, -1.1f, 0.2f});
    horse.rotation = CoopPuppet::QuatToMatrix(Yaw(35.0f));
    horse.animation = proto::Animation{"Gallop", 7.0f, 1.25f};
    HorseSeen far = horse;
    far.address = 0x9999;
    far.position = Add(me, {30.0f, 0.0f, 0.0f});

    // On foot: nothing, and no walk.
    CHECK_FALSE(rider.Wants(false, 1'000'000));
    CHECK_FALSE(rider.Tick(false, me, turn, {horse}, 1'000'000));

    // Riding, the horse not seen (getting on): nothing yet.
    CHECK(rider.Wants(true, 1'000'000));
    CHECK_FALSE(rider.Tick(true, me, turn, {far}, 1'000'000));
    CHECK_FALSE(rider.Riding());
    CHECK_FALSE(rider.Wants(true, 1'010'000));  // a walk at most every 33 ms

    auto m = rider.Tick(true, me, turn, {far, horse}, 1'040'000);
    REQUIRE(m);
    CHECK(rider.Riding());
    CHECK(rider.Horse() == 0x1234);
    CHECK(m->flags == proto::kRideRiding);
    CHECK(m->horse == "Animal_Epona_Adult");
    REQUIRE(m->animation);
    CHECK(m->animation->name == "Gallop");
    // The offset is in the rider's frame: back in the world it is the horse's.
    const Mat3 frame = CoopPuppet::QuatToMatrix(turn);
    CHECK(Near(Add(me, CoopRide::FromFrame(frame, m->offset)), horse.position));
    CHECK(Near(CoopRide::Absolute(frame, CoopPuppet::QuatToMatrix(m->rotation)), horse.rotation));

    CHECK_FALSE(rider.Tick(true, me, turn, {horse}, 1'050'000));  // too soon
    CHECK(rider.Tick(true, me, turn, {horse}, 1'080'000));

    // Off: at once, then twice more, then nothing.
    CHECK(rider.Wants(false, 1'081'000));
    auto off = rider.Tick(false, me, turn, {}, 1'081'000);
    REQUIRE(off);
    CHECK(off->flags == 0);
    CHECK(off->horse.empty());
    CHECK_FALSE(rider.Riding());
    CHECK_FALSE(rider.Tick(false, me, turn, {}, 1'090'000));
    CHECK(rider.Tick(false, me, turn, {}, 1'120'000));
    CHECK(rider.Tick(false, me, turn, {}, 1'160'000));
    CHECK_FALSE(rider.Tick(false, me, turn, {}, 1'200'000));
    CHECK_FALSE(rider.Wants(false, 1'300'000));
    std::vector<std::uint8_t> bytes;
    CHECK(proto::encode(proto::Message{*off}, bytes) == proto::Error::none);
}

TEST_CASE("ride: the others draw the horse under the riding puppet, turned with it") {
    // The rider's game: its Link at `there`, its horse under it.
    CoopRide::Rider rider;
    const Vec3 there{10.0f, 0.0f, 10.0f};
    const Quat turn = Yaw(120.0f);
    HorseSeen horse;
    horse.address = 0x1;
    horse.position = Add(there, CoopRide::FromFrame(CoopPuppet::QuatToMatrix(turn), {0.0f, -1.0f, 0.3f}));
    horse.rotation = CoopPuppet::QuatToMatrix(Yaw(125.0f));
    horse.animation = proto::Animation{"Run", 10.0f, 1.0f};
    auto sent = rider.Tick(true, there, turn, {horse}, 5'000'000);
    REQUIRE(sent);

    CoopRide::Rides rides;
    rides.Receive(Relayed(*sent), 5'000'000);
    const std::optional<std::string> scene = std::string("HyruleField");

    // The puppet shows a little behind (interpolation): the horse goes where
    // the puppet is, not where the rider is now.
    const Vec3 puppet{9.5f, 0.0f, 9.8f};
    auto copies = rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live, 5'100'000);
    REQUIRE(copies.size() == 1);
    const auto& c = copies[0];
    CHECK(c.actor == "Animal_Epona_Adult");
    CHECK((c.key >> 61) == 1);
    CHECK(c.place);
    CHECK(Near(c.position, Add(puppet, CoopRide::FromFrame(CoopPuppet::QuatToMatrix(turn), {0.0f, -1.0f, 0.3f}))));
    REQUIRE(c.rotation);
    CHECK(Near(*c.rotation, horse.rotation));
    REQUIRE(c.animation);
    CHECK(c.animation->name == "Run");
    CHECK(c.animation->frame > 10.0f);  // moved on by the 100 ms since it was sent
    const auto changes = rides.Changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0] == "P2 rides Animal_Epona_Adult (horse copy CoopRide_Epona)");
    CHECK(rides.Changes().empty());

    // Same key while the ride goes on.
    rides.Receive(Relayed(*rider.Tick(true, there, turn, {horse}, 5'040'000)), 5'140'000);
    auto again = rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live, 5'150'000);
    REQUIRE(again.size() == 1);
    CHECK(again[0].key == c.key);

    // No horse while: their puppet is not live, they are elsewhere, our cutscene runs.
    CHECK(rides.Plan(scene, false, {Rider(puppet, turn)}, 0, 5'160'000).empty());
    CHECK(rides.Plan(std::string("LonLonRanch"), false, {Rider(puppet, turn)}, kP2Live, 5'170'000).empty());
    CHECK(rides.Plan(scene, true, {Rider(puppet, turn)}, kP2Live, 5'180'000).empty());
    // Getting off: their command says so before riding off comes.
    CHECK(rides.Plan(scene, false, {Rider(puppet, turn, "DismountHorse")}, kP2Live, 5'190'000).empty());
    CHECK(rides.Changes().size() == 1);  // "P2 off the horse" once, not per frame
    // Stale (riding off lost): gone after kStaleUs.
    auto back_on = rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live, 5'200'000);
    CHECK(back_on.size() == 1);
    CHECK(rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live,
                     5'140'000 + CoopRide::Rides::kStaleUs + 1)
              .empty());

    // Riding off, then on again: a new key, so a new copy is made.
    proto::PlayerRide off;
    rides.Receive(Relayed(off), 6'000'000);
    CHECK(rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live, 6'010'000).empty());
    rides.Receive(Relayed(*sent), 6'100'000);
    auto remount = rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live, 6'110'000);
    REQUIRE(remount.size() == 1);
    CHECK(remount[0].key != c.key);

    // The player left: nothing kept.
    rides.Forget(2);
    CHECK(rides.Plan(scene, false, {Rider(puppet, turn)}, kP2Live, 6'120'000).empty());
}

TEST_CASE("ride: a player who never sent a ride gets no horse (an old build): the puppet still shows") {
    CoopRide::Rides rides;
    CHECK(rides.Plan(std::string("HyruleField"), false, {Rider({0, 0, 0}, Yaw(0))}, kP2Live, 1'000'000).empty());
    CoopPuppet::Planner planner;
    auto in = planner.Plan(0x1000, std::string("HyruleField"), std::nullopt, false,
                           {Rider({0, 0, 0}, Yaw(0))}, 0);
    REQUIRE(in.targets.size() == 1);
    CHECK(in.targets[0].show == CoopPuppet::Show::show);
    CHECK(std::string(in.targets[0].why) == "riding");
}

TEST_CASE("ride: the ridable horse is never a shared object (one horse per game)") {
    CHECK(CoopRide::IsHorse(CoopRide::kHorse));
    CHECK_FALSE(CoopObjects::IsShared(CoopRide::kHorse));
}

TEST_CASE("ride: the highest player number rides too (16 players); one past it is ignored") {
    CoopRide::Rider rider;
    HorseSeen horse;
    horse.address = 0x1;
    horse.position = {0.0f, -1.0f, 0.0f};
    horse.rotation = CoopPuppet::QuatToMatrix(Yaw(0.0f));
    auto sent = rider.Tick(true, {0.0f, 0.0f, 0.0f}, Yaw(0.0f), {horse}, 5'000'000);
    REQUIRE(sent);
    const std::uint8_t top = proto::kMaxPlayers;
    for (const std::uint8_t n : {top, static_cast<std::uint8_t>(top + 1)}) {
        CoopRide::Rides rides;
        proto::PlayerRide m = Relayed(*sent);
        m.player_number = n;
        rides.Receive(m, 5'000'000);
        CoopPuppet::Remote r = Rider({0.0f, 0.0f, 0.0f}, Yaw(0.0f));
        r.number = n;
        const auto copies = rides.Plan(std::string("HyruleField"), false, {r}, 1u << n, 5'010'000);
        CHECK(copies.size() == (n == top ? 1u : 0u));
    }
}
