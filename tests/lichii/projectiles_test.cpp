// Tests for projectiles (lichii/hook/coop_projectiles.cpp) and how the spawn
// sync, its director and the objects sync use them.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_objects_sync.h"
#include "coop_projectiles.h"
#include "coop_spawn.h"

namespace proto = coop::protocol;
namespace shots = CoopProjectiles;
using CoopSpawn::Act;
using CoopSpawn::Copy;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "Spot_GraveYard";

CoopSpawn::Seen Made(std::uint64_t address, float x, std::string name,
                     std::optional<CoopPuppet::Mat3> rotation = std::nullopt) {
    CoopSpawn::Seen s;
    s.address = address;
    s.name = std::move(name);
    s.position = {x, 1.0f, 2.0f};
    s.rotation = rotation;
    return s;
}

proto::Message Wire(proto::Message m, std::uint8_t from) {
    std::visit([&](auto& v) {
        if constexpr (requires { v.player_number; }) {
            v.player_number = from;
        }
    }, m);
    const auto bytes = proto::encode(m);
    REQUIRE(bytes);
    proto::Message out;
    REQUIRE(proto::decode(*bytes, out) == proto::Error::none);
    return out;
}

template <typename T>
std::vector<T> Of(const std::vector<proto::Message>& send) {
    std::vector<T> out;
    for (const auto& m : send) {
        if (const auto* v = std::get_if<T>(&m)) {
            out.push_back(*v);
        }
    }
    return out;
}

// A turn of 90 degrees about y, as the actor holds it (rows).
constexpr CoopPuppet::Mat3 kTurned{0, 0, 1, 0, 1, 0, -1, 0, 0};

class FakeGuest : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t address, std::size_t size) {
        for (std::uint64_t p = address & ~0xfffULL; p < address + size; p += 0x1000) {
            pages_[p];
        }
    }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) return false;
            o[i] = it->second[(address + i) & 0xfff];
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) return false;
            it->second[(address + i) & 0xfff] = p[i];
        }
        return true;
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        REQUIRE(Read(address, &v, sizeof(T)));
        return v;
    }
    template <typename T>
    void Set(std::uint64_t address, const T& v) {
        REQUIRE(Write(address, &v, sizeof(T)));
    }

private:
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages_;
};

} // namespace

TEST_CASE("shots: every projectile kind is known, with its cause") {
    for (const char* name : {"Item_Bomb", "Item_Bombchu", "Item_DekuNut", "Shootable_DekuSeed",
                             "NormalArrow", "Weapon_Boomerang", "Shootable_Hook",
                             "Shootable_HookLong"}) {
        CAPTURE(name);
        REQUIRE(shots::Find(name) != nullptr);
        CHECK(CoopSpawn::IsShared(name));  // the spawn sync shares the same list
    }
    CHECK(shots::Find("Enemy_Stalchild") == nullptr);
    CHECK(shots::Find("Obj_Bomb") == nullptr);  // a pickup, not a thrown bomb
    CHECK(shots::Find("Item_Bomb")->fused);
    CHECK_FALSE(shots::Find("Item_Bombchu")->fused);
    CHECK(shots::Find("Item_Bombchu")->blasts);
    CHECK(shots::Find("NormalArrow")->ballistic);
    CHECK_FALSE(shots::Find("Weapon_Boomerang")->ballistic);  // it homes: samples only

    CHECK(shots::ElementOf("Item_Arrow_Fire") == shots::Element::fire);
    CHECK(shots::ElementOf("Item_Arrow_Ice") == shots::Element::ice);
    CHECK(shots::ElementOf("Item_Arrow_Light") == shots::Element::light);
    CHECK(shots::ElementOf("Item_Arrow_Normal") == shots::Element::none);
    CHECK(shots::ElementOf("") == shots::Element::none);
    CHECK(shots::CauseOf(shots::Kind::arrow, shots::Element::fire) == proto::DamageCause::fire_arrow);
    CHECK(shots::CauseOf(shots::Kind::arrow, shots::Element::ice) == proto::DamageCause::ice_arrow);
    CHECK(shots::CauseOf(shots::Kind::arrow, shots::Element::light) == proto::DamageCause::light_arrow);
    CHECK(shots::CauseOf(shots::Kind::arrow) == proto::DamageCause::arrow);
    CHECK(shots::CauseOf(shots::Kind::bombchu) == proto::DamageCause::bomb);
    CHECK(shots::CauseOf(shots::Kind::hook_long) == proto::DamageCause::hookshot);
    CHECK(shots::CauseOf(shots::Kind::seed) == proto::DamageCause::seed);
    CHECK(std::string(shots::NameOf(shots::Kind::boomerang)) == "boomerang");
    CHECK(std::string(shots::NameOf(shots::Element::ice)) == "ice");
}

TEST_CASE("shots: a track is carried on along the velocity, with the fall the samples show") {
    shots::Track t;
    CHECK(t.Empty());
    REQUIRE(t.Add({1'000'000, {0, 10, 0}, {20, 0, 0}}));
    // No gravity known from one sample.
    CHECK(t.At(1'100'000, true)[0] == doctest::Approx(2.0f));
    CHECK(t.At(1'100'000, true)[1] == doctest::Approx(10.0f));
    // Before the sample's time: where the sample was.
    CHECK(t.At(900'000, true)[0] == doctest::Approx(0.0f));

    // Velocity y fell by 1 m/s in 100 ms: 10 m/s^2.
    REQUIRE(t.Add({1'100'000, {2, 10, 0}, {20, -1, 0}}));
    CHECK(t.Gravity() == doctest::Approx(10.0f));
    const auto at = t.At(1'200'000, true);
    CHECK(at[0] == doctest::Approx(4.0f));
    CHECK(at[1] == doctest::Approx(10.0f - 0.1f - 0.05f));
    // Not ballistic: a straight line.
    CHECK(t.At(1'200'000, false)[1] == doctest::Approx(9.9f));
    // Carried at most 250 ms: then held.
    CHECK(t.At(3'000'000, false)[0] == doctest::Approx(2.0f + 20.0f * 0.25f));

    // Older, equal and broken samples are dropped.
    CHECK_FALSE(t.Add({1'100'000, {9, 9, 9}, {}}));
    CHECK_FALSE(t.Add({1'000'000, {9, 9, 9}, {}}));
    CHECK_FALSE(t.Add({1'300'000, {NAN, 0, 0}, {}}));
    CHECK(t.Last().position[0] == doctest::Approx(2.0f));

    // Thrown upwards it slows the same way; a rise never counts as negative
    // gravity, and a huge change is capped.
    shots::Track up;
    up.Add({0, {}, {0, -5, 0}});
    up.Add({50'000, {}, {0, 5, 0}});
    CHECK(up.Gravity() == doctest::Approx(0.0f));
    up.Add({100'000, {}, {0, 1, 0}});  // 80 m/s^2
    CHECK(up.Gravity() == doctest::Approx(shots::Track::kGravityMax));
    // Two samples too close in time leave the gravity as it was.
    up.Add({110'000, {}, {0, 0, 0}});
    CHECK(up.Gravity() == doctest::Approx(shots::Track::kGravityMax));

    // Stopped where the owner said it ended: no more carrying on.
    t.Stop({7, 8, 9});
    CHECK(t.Stopped());
    CHECK(t.At(5'000'000, true)[0] == doctest::Approx(7.0f));
    CHECK(t.At(5'000'000, true)[1] == doctest::Approx(8.0f));
}

TEST_CASE("shots: the fuse is learnt from our own bombs") {
    shots::FuseClock f;
    CHECK_FALSE(f.Fuse());
    CHECK_FALSE(f.Left(0));
    f.Lived(100'000);      // the help-box bomb: not a fuse
    f.Lived(60'000'000);   // something odd
    CHECK_FALSE(f.Fuse());
    f.Lived(3'000'000);
    CHECK(*f.Fuse() == 3'000'000);
    f.Lived(3'200'000);
    f.Lived(9'000'000);  // held under water? one odd life does not move it far
    CHECK(*f.Fuse() == 3'200'000);
    CHECK(*f.Left(1'200'000) == doctest::Approx(2.0f));
    CHECK_FALSE(f.Left(3'200'000));
    for (int i = 0; i < 10; ++i) {
        f.Lived(4'000'000);
    }
    CHECK(*f.Fuse() == 4'000'000);  // only the last kKeep count
}

TEST_CASE("shots: why an owner's projectile left the list") {
    using R = proto::RemoveReason;
    CHECK(shots::GoneReason(*shots::Find("Item_Bomb"), 3'000'000, 0) == R::exploded);
    CHECK(shots::GoneReason(*shots::Find("Item_Bomb"), 100'000, 0) == R::gone);
    CHECK(shots::GoneReason(*shots::Find("Item_Bombchu"), 2'000'000, 8) == R::exploded);
    CHECK(shots::GoneReason(*shots::Find("NormalArrow"), 400'000, 30) == R::hit);
    CHECK(shots::GoneReason(*shots::Find("NormalArrow"), 9'000'000, 0) == R::gone);  // stuck, then gone
    CHECK(shots::GoneReason(*shots::Find("Shootable_DekuSeed"), 300'000, 20) == R::hit);
    CHECK(shots::GoneReason(*shots::Find("Weapon_Boomerang"), 2'000'000, 15) == R::gone);  // caught
    CHECK(shots::GoneReason(*shots::Find("Shootable_Hook"), 900'000, 10) == R::gone);
}

TEST_CASE("shots: a bomb's moment on the server clock") {
    CHECK(*shots::BlowAt(1'000'000, 2.5f) == 3'500'000);
    CHECK_FALSE(shots::BlowAt(1'000'000, std::nullopt));
    CHECK_FALSE(shots::BlowAt(1'000'000, 0.0f));
    CHECK_FALSE(shots::BlowAt(1'000'000, -1.0f));
    CHECK_FALSE(shots::BlowAt(1'000'000, NAN));
    CHECK_FALSE(shots::BlowAt(1'000'000, 60.0f));
}

TEST_CASE("shots: a copy's blast covers drops near it in place and time") {
    shots::Blasts b;
    b.Add({0, 0, 0}, 10'000'000);
    CHECK(b.Near({3, 0, 3}, 10'500'000));
    CHECK(b.Near({0, 0, 0}, 9'900'000));  // read just before: clocks and walks
    CHECK_FALSE(b.Near({0, 0, 0}, 9'000'000));
    CHECK_FALSE(b.Near({0, 0, 0}, 11'300'000));
    CHECK_FALSE(b.Near({7, 0, 0}, 10'100'000));
    for (int i = 0; i < 40; ++i) {
        b.Add({100.0f + i, 0, 0}, 20'000'000);
    }
    CHECK(b.Size() == shots::Blasts::kKeep);
    CHECK_FALSE(b.Near({0, 0, 0}, 10'100'000));  // the oldest went
    b.Reset();
    CHECK(b.Size() == 0);
}

TEST_CASE("shots: our arrow is sent turned, a fast one gone was a hit") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> none;
    std::uint64_t now = 1'000'000;
    sync.Tick(kScene, 1, now, {}, none, send, copies);
    now += 33 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x6000, 0.0f, "NormalArrow", kTurned)}, none, send, copies);
    auto spawns = Of<proto::ObjectSpawn>(send);
    REQUIRE(spawns.size() == 1);
    REQUIRE(spawns[0].state.rotation);
    CHECK_FALSE(spawns[0].state.fuse);  // not a bomb
    // The quaternion goes back to the same turn.
    const auto m = CoopPuppet::QuatToMatrix(*spawns[0].state.rotation);
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(m[i] == doctest::Approx(kTurned[i]).epsilon(1e-4));
    }
    CHECK(proto::encode(spawns[0]));

    send.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x6000, 1.0f, "NormalArrow", kTurned)}, none, send, copies);
    auto samples = Of<proto::ObjectSample>(send);
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].state.rotation);
    CHECK(proto::encode(samples[0]));

    send.clear();
    sync.Tick(kScene, 1, now + 33 * kMs, {}, none, send, copies);
    sync.Tick(kScene, 1, now + 66 * kMs, {}, none, send, copies);
    const auto removes = Of<proto::ObjectRemove>(send);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].reason == proto::RemoveReason::hit);  // 30 m/s when it went
}

TEST_CASE("shots: once our bomb went off, the next one carries its fuse") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> none;
    std::uint64_t now = 1'000'000;
    sync.Tick(kScene, 1, now, {}, none, send, copies);
    // The first bomb: no fuse known.
    now += 33 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x6000, 0.0f, "Item_Bomb")}, none, send, copies);
    REQUIRE(Of<proto::ObjectSpawn>(send).size() == 1);
    CHECK_FALSE(Of<proto::ObjectSpawn>(send)[0].state.fuse);
    // It lasts 3 s and goes off.
    now += 3'000 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x6000, 0.0f, "Item_Bomb")}, none, send, copies);
    sync.Tick(kScene, 1, now + 33 * kMs, {}, none, send, copies);
    sync.Tick(kScene, 1, now + 66 * kMs, {}, none, send, copies);
    REQUIRE(sync.Fuse().Fuse());
    CHECK(*sync.Fuse().Fuse() == 3'066'000);

    // The second one says how long it has left, at the spawn and in samples.
    send.clear();
    now += 1'000 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x7000, 0.0f, "Item_Bomb")}, none, send, copies);
    auto spawns = Of<proto::ObjectSpawn>(send);
    REQUIRE(spawns.size() == 1);
    REQUIRE(spawns[0].state.fuse);
    CHECK(*spawns[0].state.fuse == doctest::Approx(3.066f));
    send.clear();
    now += 1'000 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x7000, 2.0f, "Item_Bomb")}, none, send, copies);
    auto samples = Of<proto::ObjectSample>(send);
    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0].state.fuse);
    CHECK(*samples[0].state.fuse == doctest::Approx(2.066f));
    CHECK(proto::encode(samples[0]));
}

TEST_CASE("shots: a bomb copy goes off at the owner's fuse, before its remove") {
    shots::TheBlasts().Reset();
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t now = 10'000'000;
    sync.Tick(kScene, 2, now, {}, live, send, copies);

    proto::ObjectSpawn spawn;
    spawn.id = {proto::ObjectKind::made, 1, 5};
    spawn.actor = "Item_Bomb";
    spawn.flags = proto::object_spawn::projectile;
    spawn.state.time = {now, 0};
    spawn.state.position = proto::Vec3{4.0f, 0.0f, 0.0f};
    spawn.state.velocity = proto::Vec3{};
    spawn.state.rotation = proto::Quat{0, 0, 0, 1};
    spawn.state.fuse = 3.0f;
    sync.Receive(Wire(spawn, 1));
    now += 50 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::show);
    CHECK(copies[0].setter);
    REQUIRE(copies[0].rotation);
    CHECK((*copies[0].rotation)[0] == doctest::Approx(1.0f));
    live[copies[0].key] = 0x9000;

    // A sample says 1 s is left (a held bomb's fuse still burns).
    proto::ObjectSample sample;
    sample.id = spawn.id;
    sample.state.time = {now + 1'950 * kMs, 1};
    sample.state.position = proto::Vec3{6.0f, 0.0f, 0.0f};
    sample.state.fuse = 1.0f;
    sync.Receive(Wire(sample, 1));
    copies.clear();
    now += 1'960 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 6.0f, "Item_Bomb")}, live, send, copies);
    CHECK(copies[0].act == Act::show);

    // At the owner's moment, with no remove yet: it goes off here too, and
    // its blast is noted for the objects sync.
    copies.clear();
    now += 1'000 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 6.0f, "Item_Bomb")}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::explode);
    CHECK(sync.BlownOnTime() == 1);
    CHECK(shots::TheBlasts().Near({6.0f, 0.0f, 0.0f}, now + 100 * kMs));

    // The owner's remove comes later: still exploded, counted once.
    proto::ObjectRemove remove;
    remove.id = spawn.id;
    remove.reason = proto::RemoveReason::exploded;
    remove.position = {6.0f, 0.0f, 0.0f};
    sync.Receive(Wire(remove, 1));
    copies.clear();
    now += 60 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 6.0f, "Item_Bomb")}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::explode);
    CHECK(sync.BlownOnTime() == 1);
    CHECK(shots::TheBlasts().Size() == 1);
    CHECK(send.empty());
    shots::TheBlasts().Reset();
}

TEST_CASE("shots: an arrow copy flies the owner's arc on the server clock") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t now = 10'000'000;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    proto::ObjectSpawn spawn;
    spawn.id = {proto::ObjectKind::made, 1, 9};
    spawn.actor = "NormalArrow";
    spawn.state.time = {now, 0};
    spawn.state.position = proto::Vec3{0.0f, 5.0f, 0.0f};
    spawn.state.velocity = proto::Vec3{30.0f, 0.0f, 0.0f};
    sync.Receive(Wire(spawn, 1));
    proto::ObjectSample sample;
    sample.id = spawn.id;
    sample.state.time = {now + 100 * kMs, 1};
    sample.state.position = proto::Vec3{3.0f, 5.0f, 0.0f};
    sample.state.velocity = proto::Vec3{30.0f, -2.0f, 0.0f};  // 20 m/s^2 down
    sync.Receive(Wire(sample, 1));
    // Our clock is 60 ms past the sample: the copy is where the owner's
    // arrow is now, not where it was when the sample was taken.
    now += 160 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].position[0] == doctest::Approx(3.0f + 30.0f * 0.06f));
    CHECK(copies[0].position[1] == doctest::Approx(5.0f - 2.0f * 0.06f - 0.5f * 20.0f * 0.06f * 0.06f));
    CHECK_FALSE(copies[0].rotation);  // none sent: the create's default

    // It hit something: the copy stops there, sticks a moment (issue 166)
    // and goes.
    proto::ObjectRemove remove;
    remove.id = spawn.id;
    remove.reason = proto::RemoveReason::hit;
    remove.position = {5.0f, 4.8f, 0.0f};
    sync.Receive(Wire(remove, 1));
    copies.clear();
    sync.Tick(kScene, 2, now + 33 * kMs, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::show);
    CHECK(copies[0].position[0] == doctest::Approx(5.0f));
    CHECK(sync.Landed() == 1);
    copies.clear();
    sync.Tick(kScene, 2, now + 33 * kMs + shots::LandHoldUs(shots::Kind::arrow), {}, live, send,
              copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::remove);
    CHECK(copies[0].position[0] == doctest::Approx(5.0f));
    CHECK(sync.Landed() == 1);  // counted once
}

TEST_CASE("shots: a bounce is a turn of over 45 degrees at speed") {
    CHECK(shots::Bounced({20, 0, 0}, {-15, 2, 0}));   // off a wall
    CHECK(shots::Bounced({10, -10, 0}, {10, 8, 0}));  // off the floor
    CHECK_FALSE(shots::Bounced({20, 0, 0}, {19, -3, 0}));  // its arc
    CHECK_FALSE(shots::Bounced({20, 0, 0}, {0, 0, 0}));    // stopped: no bounce
    CHECK_FALSE(shots::Bounced({1, 0, 0}, {-1, 0, 0}));    // too slow to tell
    CHECK_FALSE(shots::Bounced({0, 0, 0}, {30, 0, 0}));    // the shot itself
    CHECK(shots::LandHoldUs(shots::Kind::seed) > 0);
    CHECK(shots::LandHoldUs(shots::Kind::arrow) > shots::LandHoldUs(shots::Kind::seed));
    CHECK(shots::LandHoldUs(shots::Kind::nut) == 0);
    CHECK(shots::LandHoldUs(shots::Kind::bomb) == 0);

    // A copy's track keeps its fall across a bounce: the turn is no gravity.
    shots::Track t;
    t.Add({0, {0, 5, 0}, {20, 0, 0}});
    t.Add({100'000, {2, 5, 0}, {20, -1, 0}});
    CHECK(t.Gravity() == doctest::Approx(10.0f));
    t.Add({150'000, {3, 5, 0}, {-15, -1.5f, 0}});  // off a wall
    CHECK(t.Gravity() == doctest::Approx(10.0f));
}

TEST_CASE("shots: our seed's bounce is sent at once, and the log counts it") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> none;
    std::uint64_t now = 1'000'000;
    sync.Tick(kScene, 1, now, {}, none, send, copies);
    now += 33 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x7000, 0.0f, "Shootable_DekuSeed")}, none, send, copies);
    REQUIRE(Of<proto::ObjectSpawn>(send).size() == 1);
    // Flying at 30 m/s along x, one sample each 33 ms.
    float x = 0.0f;
    for (int i = 0; i < 2; ++i) {
        send.clear();
        now += 33 * kMs;
        x += 1.0f;
        sync.Tick(kScene, 1, now, {Made(0x7000, x, "Shootable_DekuSeed")}, none, send, copies);
        CHECK(Of<proto::ObjectSample>(send).size() == 1);
    }
    // Next frame (16 ms, under the 33 ms gap) it came back off a wall: sent
    // now, not a sample later.
    send.clear();
    now += 16 * kMs;
    x -= 0.4f;
    sync.Tick(kScene, 1, now, {Made(0x7000, x, "Shootable_DekuSeed")}, none, send, copies);
    auto samples = Of<proto::ObjectSample>(send);
    REQUIRE(samples.size() == 1);
    CHECK((*samples[0].state.velocity)[0] < 0.0f);
    // Straight on again 16 ms later: the usual gap holds.
    send.clear();
    now += 16 * kMs;
    x -= 0.4f;
    sync.Tick(kScene, 1, now, {Made(0x7000, x, "Shootable_DekuSeed")}, none, send, copies);
    CHECK(Of<proto::ObjectSample>(send).empty());

    send.clear();
    sync.Tick(kScene, 1, now + 33 * kMs, {}, none, send, copies);
    sync.Tick(kScene, 1, now + 66 * kMs, {}, none, send, copies);
    REQUIRE(sync.Flights().size() == 1);
    CHECK(sync.Flights()[0].bounces == 1);
    CHECK(Of<proto::ObjectRemove>(send).size() == 1);
}

TEST_CASE("shots: a seed copy is seen landing where the owner's hit, a gone one goes at once") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t now = 10'000'000;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    auto shoot = [&](std::uint64_t value, proto::RemoveReason reason) {
        proto::ObjectSpawn spawn;
        spawn.id = {proto::ObjectKind::made, 1, value};
        spawn.actor = "Shootable_DekuSeed";
        spawn.state.time = {now, 0};
        spawn.state.position = proto::Vec3{0.0f, 5.0f, 0.0f};
        spawn.state.velocity = proto::Vec3{40.0f, 0.0f, 0.0f};
        sync.Receive(Wire(spawn, 1));
        proto::ObjectRemove remove;
        remove.id = spawn.id;
        remove.reason = reason;
        remove.position = {4.0f, 5.0f, 0.0f};
        sync.Receive(Wire(remove, 1));
    };
    shoot(1, proto::RemoveReason::hit);
    shoot(2, proto::RemoveReason::gone);
    copies.clear();
    now += 100 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 2);
    // Keyed in the order made: the hit one first.
    CHECK(copies[0].act == Act::show);
    CHECK(copies[0].position[0] == doctest::Approx(4.0f));  // not carried on past it
    CHECK(copies[1].act == Act::remove);
    CHECK(sync.Landed() == 1);

    copies.clear();
    sync.Tick(kScene, 2, now + shots::LandHoldUs(shots::Kind::seed), {}, live, send, copies);
    REQUIRE(copies.size() == 2);
    CHECK(copies[0].act == Act::remove);
    CHECK(copies[0].position[0] == doctest::Approx(4.0f));
}

TEST_CASE("shots: a bomb copy that went off by itself is noted as a blast") {
    shots::TheBlasts().Reset();
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t now = 10'000'000;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    proto::ObjectSpawn spawn;
    spawn.id = {proto::ObjectKind::made, 1, 6};
    spawn.actor = "Item_Bombchu";
    spawn.state.time = {now, 0};
    spawn.state.position = proto::Vec3{1.0f, 0.0f, 0.0f};
    sync.Receive(Wire(spawn, 1));
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    live[copies[0].key] = 0x9000;
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    CHECK(shots::TheBlasts().Size() == 0);
    // The director lost it: our bombchu hit something here.
    live.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    CHECK(shots::TheBlasts().Size() == 1);
    CHECK(shots::TheBlasts().Near({1.0f, 0.0f, 0.0f}, now));
    // Once only.
    sync.Tick(kScene, 2, now + 33 * kMs, {}, live, send, copies);
    CHECK(shots::TheBlasts().Size() == 1);
    shots::TheBlasts().Reset();
}

TEST_CASE("shots: the director writes a copy's turn and calls the game's setter") {
    namespace addr = CoopPuppet::addr;
    constexpr std::uint64_t base = 0x8000000;
    constexpr std::uint64_t manager = 0x20000000;
    constexpr std::uint64_t block = 0x30000000;
    constexpr std::uint64_t actor = 0x40000000;
    constexpr std::uint64_t request = 0x50000000;
    constexpr std::uint64_t vtable = 0x9000000;  // inside main
    FakeGuest guest;
    guest.Map(base + addr::kActorManagerPtr, 8);
    guest.Map(base + addr::kGotCreateDefault, 8);
    guest.Set(base + addr::kActorManagerPtr, manager);
    guest.Set(base + addr::kGotCreateDefault, std::uint64_t{0x1234});
    guest.Map(block, CoopSpawn::Director::kSlots * CoopSpawn::Director::kSlotSize);
    guest.Map(actor, 0x600);
    guest.Map(request, 0x10);
    guest.Map(vtable, 0x400);
    guest.Set(actor, vtable);
    guest.Set(vtable + 0x218, base + 0x4c6580);
    guest.Set(actor + addr::kActorState, std::uint32_t{addr::kStatePresent});

    CoopSpawn::Director d;
    Copy copy;
    copy.key = 1;
    copy.actor = "Item_DekuNut";  // seeds, arrows and hook tips are stand-ins (no setter)
    copy.position = {1.0f, 2.0f, 3.0f};
    copy.rotation = kTurned;
    copy.setter = true;
    d.SetInput(base, {copy});

    auto c = d.Step(guest, false, 0, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kMalloc);
    c = d.Step(guest, true, block, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kCreate);
    // Created facing the owner's way.
    std::array<float, 9> made{};
    REQUIRE(guest.Read(c->x[2] + 0x0c, made.data(), sizeof(made)));
    CHECK(made == kTurned);
    const std::uint64_t handle = c->x[3];
    CHECK_FALSE(d.Step(guest, true, 0, 0));
    guest.Set(handle, request);
    guest.Set(request, std::uint32_t{addr::kRequestReady});
    c = d.Step(guest, false, 0, 16);
    REQUIRE(c);
    CHECK(c->function == base + addr::kTakeCreated);
    CHECK_FALSE(d.Step(guest, true, actor, 16));

    // Live: place and turn written as one block, then the setter.
    copy.position = {5.0f, 6.0f, 7.0f};
    d.SetInput(base, {copy});
    c = d.Step(guest, false, 0, 33);
    REQUIRE(c);
    CHECK(c->function == base + 0x4c6580);
    CHECK(c->x[0] == actor);
    CHECK(c->x[1] == 0);
    CHECK(guest.Get<float>(actor + addr::kActorPosition) == doctest::Approx(5.0f));
    std::array<float, 9> turn{};
    REQUIRE(guest.Read(actor + addr::kActorRotation, turn.data(), sizeof(turn)));
    CHECK(turn == kTurned);
    CHECK_FALSE(d.Step(guest, true, 0, 33));  // one setter a copy a frame

    // A vtable whose slot points outside main gets no call.
    guest.Set(vtable + 0x218, std::uint64_t{0x1000});
    CHECK_FALSE(d.Step(guest, false, 0, 50));
    // Setter off: none either.
    guest.Set(vtable + 0x218, base + 0x4c6580);
    copy.setter = false;
    d.SetInput(base, {copy});
    CHECK_FALSE(d.Step(guest, false, 0, 66));
}

namespace {

CoopObjects::Seen Enemy(std::uint64_t address, std::int32_t life, float x = 0.0f) {
    CoopObjects::Seen s;
    s.address = address;
    s.name = "Enemy_Stalchild";
    s.hash = 0x11;
    s.life = proto::ObjectLife{life, 8};
    s.position = {x, 0.0f, 0.0f};
    return s;
}

struct ObjStep {
    std::vector<proto::Message> send;
    std::vector<CoopObjects::LifeWrite> writes;
};

ObjStep Tick(CoopObjects::Sync& sync, std::uint64_t now, std::int32_t life, std::uint8_t self) {
    ObjStep step;
    sync.Tick(kScene, self, now, {Enemy(0x100, life)}, step.send, step.writes);
    return step;
}

} // namespace

TEST_CASE("shots: a copy bomb's blast on an enemy copy is not sent to the owner") {
    shots::Blasts blasts;
    CoopObjects::Sync sync;
    sync.SetBlastNear([&](const proto::Vec3& at, std::uint64_t now) { return blasts.Near(at, now); });
    Tick(sync, 0, 8, 2);
    const proto::ObjectId id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, 8, 2);

    // Player 1's bomb copy went off here beside it: the drop is the owner's
    // own bomb's hit, which its game deals itself.
    blasts.Add({2.0f, 0.0f, 0.0f}, 20 * kMs);
    Tick(sync, 40 * kMs, 6, 2);
    auto step = Tick(sync, 200 * kMs, 6, 2);
    CHECK(Of<proto::Damage>(step.send).empty());
    CHECK(sync.BlastDrops() == 1);

    // A sword hit later, with no blast near: sent as before.
    Tick(sync, 3'000 * kMs, 5, 2);
    step = Tick(sync, 3'200 * kMs, 5, 2);
    const auto damage = Of<proto::Damage>(step.send);
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 1);
}

TEST_CASE("shots: the owner takes a hit once when a copy bomb and the DAMAGE both bring it") {
    shots::Blasts blasts;
    CoopObjects::Sync sync;
    sync.SetBlastNear([&](const proto::Vec3& at, std::uint64_t now) { return blasts.Near(at, now); });
    Tick(sync, 0, 8, 1);
    const proto::ObjectId id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, 8, 1);

    // Player 2 threw a bomb at our enemy: our copy of it went off and took 2.
    blasts.Add({0.0f, 0.0f, 0.0f}, 20 * kMs);
    Tick(sync, 40 * kMs, 6, 1);
    CHECK(sync.BlastDrops() == 1);

    // Then player 2's DAMAGE for the same bomb comes: already taken.
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 2;
    sync.Receive(d);
    auto step = Tick(sync, 120 * kMs, 6, 1);
    CHECK(step.writes.empty());

    // A bigger DAMAGE than the credit left: only the rest is taken.
    blasts.Add({0.0f, 0.0f, 0.0f}, 500 * kMs);
    Tick(sync, 520 * kMs, 5, 1);
    d.amount = 3;
    sync.Receive(d);
    step = Tick(sync, 600 * kMs, 5, 1);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 3);

    // The credit runs out after 2 s: a later DAMAGE is taken whole.
    blasts.Add({0.0f, 0.0f, 0.0f}, 3'000 * kMs);
    Tick(sync, 3'020 * kMs, 2, 1);
    d.amount = 1;
    sync.Receive(d);
    step = Tick(sync, 6'000 * kMs, 2, 1);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 1);
}

TEST_CASE("shots: a copy blast after the thrower's DAMAGE is put back, so the hit counts once") {
    shots::Blasts blasts;
    CoopObjects::Sync sync;
    sync.SetBlastNear([&](const proto::Vec3& at, std::uint64_t now) { return blasts.Near(at, now); });
    Tick(sync, 0, 8, 1);
    const proto::ObjectId id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, 8, 1);

    // Player 2's first bomb (no fuse known): its DAMAGE comes first.
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 2;
    sync.Receive(d);
    auto step = Tick(sync, 50 * kMs, 8, 1);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 6);
    Tick(sync, 60 * kMs, 6, 1);

    // Then our copy goes off at the remove and the game takes 2 again.
    blasts.Add({0.0f, 0.0f, 0.0f}, 120 * kMs);
    step = Tick(sync, 140 * kMs, 4, 1);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 6);  // written back up
    CHECK(sync.BlastDrops() == 1);
    // Nothing is left as credit: the next DAMAGE is taken whole.
    Tick(sync, 160 * kMs, 6, 1);
    d.amount = 1;
    sync.Receive(d);
    step = Tick(sync, 200 * kMs, 6, 1);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 5);
}

TEST_CASE("shots: a bomb copy the owner removed as gone is no blast") {
    shots::TheBlasts().Reset();
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t now = 10'000'000;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    proto::ObjectSpawn spawn;
    spawn.id = {proto::ObjectKind::made, 1, 7};
    spawn.actor = "Item_Bomb";
    spawn.state.time = {now, 0};
    spawn.state.position = proto::Vec3{1.0f, 0.0f, 0.0f};
    sync.Receive(Wire(spawn, 1));
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    live[copies[0].key] = 0x9000;
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    // The help-box bomb: gone within a second; the director deletes ours.
    proto::ObjectRemove remove;
    remove.id = spawn.id;
    remove.reason = proto::RemoveReason::gone;
    remove.position = {1.0f, 0.0f, 0.0f};
    sync.Receive(Wire(remove, 1));
    copies.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::remove);
    live.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    CHECK(shots::TheBlasts().Size() == 0);
}

// Issue 169: a bomb in its owner's hands.
TEST_CASE("shots: a bomb over Link, moving with him, is in his hands") {
    const proto::Vec3 link{10.0f, 0.0f, 5.0f};
    CHECK(shots::InHands({10.2f, 1.3f, 5.1f}, link, 0.0f));    // over a child's head
    CHECK(shots::InHands({10.0f, 2.2f, 5.0f}, link, 1.0f));    // an adult's raised arms
    CHECK_FALSE(shots::InHands({10.0f, 0.1f, 5.0f}, link, 0.0f));  // at his feet
    CHECK_FALSE(shots::InHands({11.5f, 1.3f, 5.0f}, link, 0.0f));  // beside him
    CHECK_FALSE(shots::InHands({10.0f, 3.5f, 5.0f}, link, 0.0f));  // thrown up high
    CHECK_FALSE(shots::InHands({10.2f, 1.3f, 5.1f}, link, 8.0f));  // flying past
    CHECK_FALSE(shots::InHands({NAN, 1.3f, 5.1f}, link, 0.0f));
    CHECK(shots::HeldByAnimation("Lifted") == true);
    CHECK(shots::HeldByAnimation("Thrown") == false);
    CHECK_FALSE(shots::HeldByAnimation("Bomb_Blink").has_value());
}

TEST_CASE("shots: a held bomb's path is read between two samples, never carried on") {
    shots::HeldPath path;
    CHECK(path.Empty());
    path.Add({1'000'000, {0.0f, 1.0f, 0.0f}, {}});
    path.Add({1'100'000, {1.0f, 1.0f, 0.0f}, {}});
    path.Add({1'050'000, {9.0f, 9.0f, 9.0f}, {}});  // older: dropped
    path.Add({1'200'000, {NAN, 1.0f, 0.0f}, {}});   // not finite: dropped
    CHECK(path.At(900'000)[0] == doctest::Approx(0.0f));
    CHECK(path.At(1'050'000)[0] == doctest::Approx(0.5f));
    CHECK(path.At(1'500'000)[0] == doctest::Approx(1.0f));
    for (std::uint64_t t = 2'000'000; t < 2'000'000 + 20 * 33'000; t += 33'000) {
        path.Add({t, {float(t) / 1e6f, 1.0f, 0.0f}, {}});
    }
    CHECK(path.At(0)[0] > 2.0f);  // only the newest kKeep are kept
    path.Clear();
    CHECK(path.Empty());
}
