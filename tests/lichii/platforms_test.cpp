// Tests for moving platforms in step (lichii/hook/coop_platforms.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_platforms.h"

using namespace CoopPlatforms;
namespace proto = coop::protocol;

namespace {

std::uint64_t Bits(float lo, float hi = 0.0f) {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::memcpy(&a, &lo, 4);
    std::memcpy(&b, &hi, 4);
    return std::uint64_t{a} | (std::uint64_t{b} << 32);
}

constexpr std::uint64_t kClockKey = Key(4, 0x30);     // the clock, low half of c+0x10+0x30
constexpr std::uint64_t kHash = 0x7f36b7404e6b5890ull;
constexpr std::uint64_t kMs = 1000;

// One game's driver: a clock word stepping at `rate` a second, a counter
// word that steps unevenly, a still word and a heap pointer.
struct Game {
    std::uint8_t self = 1;
    std::uint64_t steady_offset = 0;  // this game's steady clock = time + offset
    float clock = 0.0f;
    float rate = 1.0f;
    float cycle = 0.0f;  // the game loops its clock here (0: never)
    float uneven = 0.0f;
    int steps = 0;
    PlatformSync sync;
    std::vector<proto::Message> sent;
    std::vector<std::string> notes;
    bool probe_only = false;

    void Step(float dt) {
        clock += rate * dt;
        if (cycle > 0.0f && clock >= cycle) {
            clock -= cycle;
        }
        uneven += (++steps % 3 == 0) ? 0.5f : 0.01f;
    }

    std::map<std::uint64_t, std::uint64_t> Words() const {
        return {
            {kClockKey, Bits(clock, 0.0f)},
            {Key(4, 0x38), Bits(uneven, 7.0f)},
            {Key(0, 0x10), 0x000000214e24aae8ull},  // a pointer
        };
    }

    // `time_us`: the one true time of the test; the server is time + 10 s.
    void Tick(std::uint64_t time_us, const std::string& scene = "DodongoCavern") {
        const std::vector<PlatformSync::Driver> drivers{
            {0x1000, kHash, "GroundControlActor_WaveLoop", {1.0f, 0.0f, 0.0f}}};
        PlatformSync::Out out;
        sync.Tick(scene, self, time_us + steady_offset, time_us + 10'000'000, drivers,
                  proto::Vec3{0.0f, 0.0f, 0.0f},
                  [this](const PlatformSync::Driver&) { return Words(); }, probe_only, out);
        for (auto& m : out.send) {
            sent.push_back(std::move(m));
        }
        for (const auto& w : out.writes) {
            CHECK(w.key == kClockKey);
            CHECK(w.hash == kHash);
            clock = w.value;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
};

// Plays two games for `ms`, 50 ms a step; what A sends reaches B `delay_ms` later (and B's A).
void Play(Game& a, Game& b, std::uint64_t& time_us, std::uint64_t ms, std::uint64_t delay_ms = 80) {
    struct InFlight {
        std::uint64_t at_us;
        proto::PlatformClock m;
        bool to_b;
    };
    std::vector<InFlight> flight;
    for (std::uint64_t t = 0; t < ms; t += 50) {
        time_us += 50 * kMs;
        a.Step(0.05f);
        b.Step(0.05f);
        a.Tick(time_us);
        b.Tick(time_us);
        for (auto& m : a.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = a.self;
            flight.push_back({time_us + delay_ms * kMs, pc, true});
        }
        for (auto& m : b.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = b.self;
            flight.push_back({time_us + delay_ms * kMs, pc, false});
        }
        a.sent.clear();
        b.sent.clear();
        for (auto it = flight.begin(); it != flight.end();) {
            if (it->at_us <= time_us) {
                (it->to_b ? b : a).sync.Receive(it->m);
                it = flight.erase(it);
            } else {
                ++it;
            }
        }
    }
}

}  // namespace

TEST_CASE("platforms: the drivers and platform actors, not other actors") {
    CHECK(IsDriver("GroundControlActor_WaveLoop"));
    CHECK(IsDriver("GroundControlActor_Rotate"));
    CHECK(IsDriver("GroundControlActor_ResidentMoveTo"));
    CHECK(IsDriver("GrouondControlActor_DiffentSpeedMoveTo"));  // the game's spelling
    CHECK(IsDriver("Obj_DodongoCave_MoveFloor_A_01"));
    CHECK(IsDriver("Obj_InsideTheDekuTree_ElevatorLift_A_02"));
    CHECK(IsDriver("Obj_InsideJabu_Jabu_ElevatorLift_A_01"));
    CHECK(IsDriver("Obj_ForestTemple_Elevator_A_01"));
    CHECK(IsDriver("Obj_Gerudo_RotateFloor_A_01"));
    CHECK(IsDriver("Land_GerudoValley_lift_A_01"));
    CHECK_FALSE(IsDriver("Obj_LiftRock_A_01"));  // a rock Link lifts
    CHECK_FALSE(IsDriver("Enemy_Dodongo"));
    CHECK_FALSE(IsDriver("Player_Child"));
}

TEST_CASE("platforms: a look's floats leave out pointers, the table and the actor's own place") {
    const std::map<std::uint64_t, std::uint64_t> words{
        {Key(0, 0x10), Bits(1.5f, 2.5f)},
        {Key(0, 0x320), Bits(9.0f, 9.0f)},       // the place: left out
        {Key(0, 0x350), Bits(3.0f, 0.0f)},       // after it
        {Key(1, 0x0), Bits(4.0f, 4.0f)},         // the table
        {Key(3, 0x8), 0x000000214e24aae8ull},    // a heap pointer
        {Key(kRegionAsFrame, 0), Bits(12.0f, 99.0f)},
    };
    const Floats f = FloatsOf(words);
    CHECK(f.at(Key(0, 0x10)) == 1.5f);
    CHECK(f.at(Key(0, 0x14)) == 2.5f);
    CHECK(f.at(Key(0, 0x350)) == 3.0f);
    CHECK_FALSE(f.contains(Key(0, 0x320)));
    CHECK_FALSE(f.contains(Key(0, 0x324)));
    CHECK_FALSE(f.contains(Key(1, 0x0)));
    CHECK_FALSE(f.contains(Key(3, 0x8)));
    CHECK_FALSE(f.contains(Key(3, 0xc)));
    CHECK(f.at(Key(kRegionAsFrame, 0)) == 12.0f);
    CHECK_FALSE(f.contains(Key(kRegionAsFrame, 4)));
    CHECK(Where(Key(4, 0x30)) == "c+0x10+0x30");
    CHECK(Where(Key(0, 0x44)) == "actor+0x44");
    CHECK(Where(Key(kRegionAsFrame, 0)) == "as-frame");
}

TEST_CASE("platforms: the finder takes the word that steps with time, and its rate") {
    ClockFinder f;
    float clock = 2.0f;
    float frames = 100.0f;
    float uneven = 0.0f;
    float slow_linear = 0.0f;  // steps steadily but not like a clock (0.37 a second)
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t t = 1'000'000 + static_cast<std::uint64_t>(i) * 250'000;
        Floats look{{Key(2, 0x0), clock},     {Key(2, 0x4), 5.0f},
                    {Key(2, 0x8), uneven},    {Key(2, 0xc), frames},
                    {Key(2, 0x10), slow_linear}};
        f.Add(t, look);
        if (i < 4) {
            CHECK_FALSE(f.Found());  // three agreeing checks first
        }
        clock += 0.25f;
        frames += 7.5f;  // 30 a second
        slow_linear += 0.0925f;
        uneven += (i % 2 == 0) ? 1.0f : 0.1f;
    }
    const auto found = f.Found();
    REQUIRE(found);
    // Both the seconds and the frames words are clock-like; the first by key wins.
    CHECK((found->key == Key(2, 0x0) || found->key == Key(2, 0xc)));
    if (found->key == Key(2, 0x0)) {
        CHECK(found->rate == doctest::Approx(1.0f).epsilon(0.01));
    } else {
        CHECK(found->rate == doctest::Approx(30.0f).epsilon(0.01));
    }
    CHECK(found->cycle == 0.0f);
}

TEST_CASE("platforms: a looping clock gives its cycle and counts as a jump") {
    ClockFinder f;
    float clock = 0.0f;
    bool jumped = false;
    for (int i = 0; i < 20; ++i) {
        f.Add(1'000'000 + static_cast<std::uint64_t>(i) * 250'000, Floats{{Key(0, 0x40), clock}});
        jumped = jumped || f.Jumped();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;  // loops every 3 s
        }
    }
    const auto found = f.Found();
    REQUIRE(found);
    CHECK(found->key == Key(0, 0x40));
    CHECK(found->cycle == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(jumped);
}

TEST_CASE("platforms: the advance by server time, looped, and the drift the short way") {
    CHECK(Advance(2.0f, 1.0f, 0.0f, 10'000'000, 10'500'000) == doctest::Approx(2.5f));
    CHECK(Advance(2.0f, 30.0f, 0.0f, 10'000'000, 10'100'000) == doctest::Approx(5.0f));
    CHECK(Advance(2.5f, 1.0f, 3.0f, 10'000'000, 11'000'000) == doctest::Approx(0.5f));  // looped
    CHECK(Advance(1.0f, -1.0f, 3.0f, 10'000'000, 12'000'000) == doctest::Approx(2.0f));  // backwards, looped
    CHECK(Advance(2.0f, 1.0f, 0.0f, 10'000'000, 9'990'000) == doctest::Approx(2.0f));  // from the future: 0
    CHECK(DriftSeconds(0.1f, 2.9f, 1.0f, 3.0f) == doctest::Approx(0.2f));  // round the loop
    CHECK(DriftSeconds(1.0f, 2.0f, 1.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(DriftSeconds(0.0f, 15.0f, 30.0f, 0.0f) == doctest::Approx(0.5f));  // frames: half a second
}

TEST_CASE("platforms: the owner's clock reaches the other game in step, whatever its own clock says") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock = 1.0f;
    b.clock = 40.0f;               // far out of step
    b.steady_offset = 777'000'000;  // B's own clock is far from A's: only server time counts
    b.sync.Receive({});             // empty message: ignored
    std::uint64_t time = 0;
    // B has nothing from A yet and would send its own; A begins first.
    a.Tick(time);
    Play(a, b, time, 1500);
    Play(a, b, time, 3000, 120);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(b.sync.Writes() >= 1);
    CHECK(a.sync.Writes() == 0);
    bool followed = false;
    for (const auto& n : b.notes) {
        followed = followed || n.find("following player 1's clock c+0x10+0x30") != std::string::npos;
    }
    CHECK(followed);
    // In step: no more writes.
    const std::size_t writes = b.sync.Writes();
    Play(a, b, time, 3000);
    CHECK(b.sync.Writes() == writes);
}

TEST_CASE("platforms: a small drift is left alone, and probe mode never writes") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock = 5.0f;
    b.clock = 5.03f;  // 30 ms off: under the limit
    std::uint64_t time = 0;
    Play(a, b, time, 4000, 30);
    CHECK(b.sync.Writes() == 0);

    Game c;
    Game d;
    c.self = 1;
    d.self = 2;
    d.clock = 20.0f;
    d.probe_only = true;
    time = 0;
    Play(c, d, time, 4000);
    CHECK(d.sync.Writes() == 0);
    CHECK(std::fabs(c.clock - d.clock) > 10.0f);
}

TEST_CASE("platforms: a late tick older than one we have is not used") {
    PlatformSync s;
    // Join the scene first (a Tick sets it).
    PlatformSync::Out out;
    s.Tick("DodongoCavern", 2, 1, 20'000'000, {}, proto::Vec3{}, {}, false, out);
    proto::PlatformClock newer;
    newer.player_number = 1;
    newer.scene = "DodongoCavern";
    newer.server_time_us = 20'000'000;
    newer.ticks.push_back({kHash, kClockKey, 9.0f, 1.0f, 0.0f});
    proto::PlatformClock older = newer;
    older.server_time_us = 19'000'000;
    older.ticks[0].value = 1.0f;
    s.Receive(newer);
    s.Receive(older);  // came late: dropped
    // B's clock at 9.2 (in step with the newer one): no write.
    Game b;
    b.self = 2;
    b.sync = s;
    b.clock = 9.0f;
    std::uint64_t time = 10'000'000;  // server time = time + 10 s = 20 s
    for (int i = 0; i < 30; ++i) {
        time += 50 * kMs;
        b.Step(0.05f);
        b.Tick(time);
        if (i % 10 == 0) {
            newer.server_time_us = time + 10'000'000;
            newer.ticks[0].value = b.clock;
            b.sync.Receive(newer);
        }
    }
    CHECK(b.sync.Writes() == 0);
    // Another scene's tick is not taken.
    proto::PlatformClock other = newer;
    other.scene = "KokiriForest";
    other.player_number = 3;
    b.sync.Receive(other);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
}

TEST_CASE("platforms: when the owner goes quiet the next game takes over, and a later lower number follows") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    std::uint64_t time = 0;
    Play(a, b, time, 3000);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    // A leaves (nothing from it any more): after kOwnerQuietUs B sends.
    for (int i = 0; i < 100; ++i) {
        time += 50 * kMs;
        b.Step(0.05f);
        b.Tick(time);
    }
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 2);
    CHECK_FALSE(b.sent.empty());
    b.sent.clear();
    // A comes back with its clock far off: B has sent the longest, A follows B.
    Game a2;
    a2.self = 1;
    a2.clock = 50.0f;
    Play(a2, b, time, 4000);
    CHECK(a2.sync.OwnerOf(kHash, 1, time + 10'000'000) == 2);
    CHECK(std::fabs(a2.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(b.sync.Writes() == 0);
}

TEST_CASE("platforms: two that begin together: the lower number keeps it") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock = 3.0f;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 5000);
    CHECK(a.sync.OwnerOf(kHash, 1, time + 10'000'000) == 1);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
}

TEST_CASE("platforms: a looping clock follows round the loop") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.cycle = 4.0f;
    b.cycle = 4.0f;
    a.clock = 0.5f;
    b.clock = 2.5f;
    std::uint64_t time = 0;
    Play(a, b, time, 8000);
    CHECK(DriftSeconds(b.clock, a.clock, 1.0f, 4.0f) < PlatformSync::kDriftS + 0.06f);
}

TEST_CASE("platforms: writes the game takes back each time are stopped") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    // B's game puts its own clock back after every write (a word that only
    // looks like the clock).
    for (int i = 0; i < 400; ++i) {
        time += 50 * kMs;
        a.Step(0.05f);
        b.Step(0.05f);
        a.Tick(time);
        const float before = b.clock;
        b.Tick(time);
        if (b.clock != before) {
            b.clock = before;  // taken back
        }
        for (auto& m : a.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = 1;
            b.sync.Receive(pc);
        }
        a.sent.clear();
        b.sent.clear();
    }
    CHECK(b.sync.Writes() == PlatformSync::kMaxNoHold);
    bool stopped = false;
    for (const auto& n : b.notes) {
        stopped = stopped || n.find("do not hold") != std::string::npos;
    }
    CHECK(stopped);
}

TEST_CASE("platforms: a new scene forgets the clocks and owners") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    std::uint64_t time = 0;
    Play(a, b, time, 3000);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    b.Tick(time + 50 * kMs, "KokiriForest");
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 2);
}

TEST_CASE("platforms: a stall right after a loop is no cycle and no jump (review)") {
    ClockFinder f;
    float clock = 0.0f;
    std::uint64_t t = 1'000'000;
    const auto look = [&] {
        f.Add(t, Floats{{Key(0, 0x40), clock}});
        t += 250'000;
    };
    // Steps at 1 a second and loops at 3: the cycle is learned.
    for (int i = 0; i < 14; ++i) {
        look();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;
        }
    }
    REQUIRE(f.Found());
    REQUIRE(f.Found()->cycle == doctest::Approx(3.0f).epsilon(0.01));
    // Loop again, then the game stalls (a pause) just after it.
    while (clock > 0.3f) {
        look();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;
        }
    }
    look();  // just past the loop
    clock += 0.25f;
    look();  // one step on, then it stands still
    bool jumped = false;
    for (int i = 0; i < 3; ++i) {  // three looks with the clock standing still
        look();
        jumped = jumped || f.Jumped();
        CHECK_FALSE(f.Found());  // standing still: nothing frozen to send
    }
    CHECK_FALSE(jumped);
    // It goes on: found again with the same cycle.
    for (int i = 0; i < 8; ++i) {
        clock += 0.25f;
        look();
    }
    REQUIRE(f.Found());
    CHECK(f.Found()->cycle == doctest::Approx(3.0f).epsilon(0.01));
}
