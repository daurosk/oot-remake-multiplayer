// Tests for moving platforms in step (lichii/hook/coop_platforms.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <optional>
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

constexpr std::uint64_t kAsKey = Key(kRegionAsFrame, 0);
constexpr std::uint64_t kClockKey = kAsKey;  // the clock: the AS slot 0 frame (room-objects: no guessed word)
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
    bool paused = false;  // a pause menu: the game's clocks stand still
    // Issue 194: the driver and our Link (by default a controller 1 m away, nobody riding).
    std::string driver_name = "GroundControlActor_WaveLoop";
    proto::Vec3 driver_at{1.0f, 0.0f, 0.0f};
    std::optional<bool> ride_plug;
    proto::Vec3 link{0.0f, 0.0f, 0.0f};
    // #196: where the clock is (the AS frame by default; a GroundControl node's phase for the wave tests).
    std::uint64_t clock_key = kClockKey;

    void Step(float dt) {
        if (paused) {
            return;
        }
        clock += rate * dt;
        if (cycle > 0.0f && clock >= cycle) {
            clock -= cycle;
        }
        uneven += (++steps % 3 == 0) ? 0.5f : 0.01f;
    }

    std::map<std::uint64_t, std::uint64_t> Words() const {
        if (clock_key >> 32 == kRegionWaveNode) {
            // The node's block: its vtable, the linear cycle 18 s on x (0xc8), the phases (0xf8: x, y).
            return {
                {Key(kRegionWaveNode, 0xc0), 0x0000007104b6ca28ull},
                {Key(kRegionWaveNode, 0xc8), Bits(18.0f, 0.0f)},
                {Key(kRegionWaveNode, 0xd0), Bits(0.0f, 4.0f)},
                {clock_key, Bits(clock, 0.0f)},
                {Key(4, 0x38), Bits(uneven, 7.0f)},
            };
        }
        return {
            {kClockKey, Bits(clock, 0.0f)},
            {Key(4, 0x38), Bits(uneven, 7.0f)},
            {Key(0, 0x10), 0x000000214e24aae8ull},  // a pointer
        };
    }

    // `time_us`: the one true time of the test; the server is time + 10 s.
    void Tick(std::uint64_t time_us, const std::string& scene = "DodongoCavern") {
        const std::vector<PlatformSync::Driver> drivers{{0x1000, kHash, driver_name, driver_at, ride_plug}};
        PlatformSync::Out out;
        sync.Tick(scene, self, time_us + steady_offset, time_us + 10'000'000, drivers, link,
                  [this](const PlatformSync::Driver&) { return Words(); }, probe_only, out);
        for (auto& m : out.send) {
            sent.push_back(std::move(m));
        }
        for (const auto& w : out.writes) {
            CHECK(w.key == clock_key);
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

TEST_CASE("platforms: the finder takes the AS frame, never a guessed word that steps with time (room-objects)") {
    ClockFinder f;
    ClockFinder guessed;  // the same words without the AS frame: a seconds word, a frame counter, a slow place
    float clock = 2.0f;
    float frames = 100.0f;
    float uneven = 0.0f;
    float slow_linear = 0.0f;  // steps steadily but not like a clock (0.37 a second)
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t t = 1'000'000 + static_cast<std::uint64_t>(i) * 250'000;
        Floats look{{Key(2, 0x0), clock},     {Key(2, 0x4), 5.0f},
                    {Key(2, 0x8), uneven},    {Key(2, 0xc), frames},
                    {Key(2, 0x10), slow_linear}};
        guessed.Add(t, look);
        look[kAsKey] = frames;
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
    CHECK(found->key == kAsKey);
    CHECK(found->rate == doctest::Approx(30.0f).epsilon(0.01));
    CHECK(found->cycle == 0.0f);
    // What the clock path took in runs (the Deku lifts' frame counter, the water-room controller's place) is never
    // the clock: nothing to send or write.
    CHECK_FALSE(guessed.Found());
    CHECK_FALSE(ClockFinder::Trusted(Key(2, 0xc), false));
    CHECK(ClockFinder::Trusted(Key(2, 0xc), true));  // a wave phase
    CHECK(ClockFinder::Trusted(kAsKey, false));
}

TEST_CASE("platforms: a looping clock gives its cycle and counts as a jump") {
    ClockFinder f;
    float clock = 0.0f;
    bool jumped = false;
    for (int i = 0; i < 20; ++i) {
        f.Add(1'000'000 + static_cast<std::uint64_t>(i) * 250'000, Floats{{kAsKey, clock}});
        jumped = jumped || f.Jumped();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;  // loops every 3 s
        }
    }
    const auto found = f.Found();
    REQUIRE(found);
    CHECK(found->key == kAsKey);
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
        followed = followed || n.find("following player 1's clock as-frame") != std::string::npos;
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

TEST_CASE("platforms: when the owner goes quiet the next game takes over, and a lower number that comes back leads") {
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
    // A comes back with its clock far off: the lowest number leads in every game (as PR 279's pause leader),
    // so B follows A and A writes nothing.
    Game a2;
    a2.self = 1;
    a2.clock = 50.0f;
    Play(a2, b, time, 4000);
    CHECK(a2.sync.OwnerOf(kHash, 1, time + 10'000'000) == 1);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    CHECK(std::fabs(a2.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(a2.sync.Writes() == 0);
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
        f.Add(t, Floats{{kAsKey, clock}});
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

TEST_CASE("platforms: a wave mover's phase is taken before seconds and frames, with its loop 2 pi at once") {
    // The Deku Tree water room's floor: GroundControlActor_WaveLoop, WaveCycle_Linear 18 s (Banc), so its phase
    // steps 2 pi / 18 = 0.349 a second. Its wave block is at component + 192: the cycle at +8 (+0xc8), the phase
    // at +56 (+0xf8). A seconds word and a frames word step beside it.
    ClockFinder f;
    const std::uint64_t cycle_key = Key(3, 0xc8);
    const std::uint64_t phase_key = Key(3, 0xc8 + kWavePhaseFromCycle);
    float phase = 1.0f;
    float seconds = 50.0f;
    float frames = 3000.0f;
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t t = 1'000'000 + static_cast<std::uint64_t>(i) * 250'000;
        f.Add(t, Floats{{Key(3, 0x0), seconds}, {Key(3, 0x4), frames}, {cycle_key, 18.0f},
                        {cycle_key + 12, 4.0f}, {phase_key, phase}});
        seconds += 0.25f;
        frames += 15.0f;
        phase += kTwoPi / 18.0f * 0.25f;
    }
    const auto found = f.Found();
    REQUIRE(found);
    CHECK(found->key == phase_key);
    CHECK(found->rate == doctest::Approx(kTwoPi / 18.0f).epsilon(0.01));
    CHECK(found->cycle == doctest::Approx(kTwoPi));
}

namespace {
// #196 (room-actor-replay-0116 run): one look every 250 ms at a wave block whose cycle word is `cycle_s` and whose
// "phase" word takes `values` in turn.
std::vector<ClockFinder::Dropped> LookAtWave(ClockFinder& f, float cycle_s, const std::vector<float>& values) {
    const std::uint64_t cycle_key = Key(3, 0x1a8);
    const std::uint64_t phase_key = Key(3, 0x1a8 + kWavePhaseFromCycle);
    std::vector<ClockFinder::Dropped> dropped;
    for (std::size_t i = 0; i < values.size(); ++i) {
        f.Add(1'000'000 + i * 250'000, Floats{{cycle_key, cycle_s}, {phase_key, values[i]}});
        for (const auto& d : f.TakeDropped()) {
            CHECK(d.key == phase_key);
            dropped.push_back(d);
        }
    }
    return dropped;
}
}  // namespace

TEST_CASE("platforms: a wave word outside 0 to 2 pi is no clock (#196, the water room floor at -26.4)") {
    // bigrun-0115 / room-actor-replay-0116-2: Obj_Deku_MoveFloor_A_01's c+0x60+0x1d8 matched the cycle test (1.177 a
    // second) at -26.403 .. -27.098; the game worked it out again every frame, so no write ever held.
    ClockFinder f;
    std::vector<float> v;
    for (int i = 0; i < 12; ++i) {
        v.push_back(-26.403f + 1.177f * 0.25f * static_cast<float>(i));
    }
    const auto dropped = LookAtWave(f, kTwoPi / 1.177f, v);
    CHECK_FALSE(f.Found());
    REQUIRE(dropped.size() == 1);
    CHECK(std::string(dropped[0].why).find("outside [0, 2 pi)") != std::string::npos);
}

TEST_CASE("platforms: a wave word that swings back is no clock (#196, the WaveLoop's actor+0x3a8)") {
    // The same run: GroundControlActor_WaveLoop actor+0x3a8 rose at +0.137 a second as the owner's, and fell
    // 0.461 -> -0.032 in the other game: a wave's offset (it swings), not its phase.
    ClockFinder f;
    std::vector<float> v;
    float x = 0.30f;
    for (int i = 0; i < 6; ++i, x += 0.137f * 0.25f) {
        v.push_back(x);
    }
    for (int i = 0; i < 6; ++i, x -= 0.137f * 0.25f) {
        v.push_back(x);
    }
    const auto dropped = LookAtWave(f, kTwoPi / 0.137f, v);
    CHECK_FALSE(f.Found());
    REQUIRE(dropped.size() == 1);
    CHECK(std::string(dropped[0].why).find("went back") != std::string::npos);
}

TEST_CASE("platforms: a wave phase set back to its start mid-loop is a jump, not a swing (#196 review)") {
    // A switch or a reload can set a wave's phase to 0 again from the middle of its loop: no offset, still the clock.
    ClockFinder f;
    std::vector<float> v;
    float x = 2.0f;
    for (int i = 0; i < 8; ++i, x += 1.177f * 0.25f) {
        v.push_back(x);
    }
    for (x = 0.0f; v.size() < 16; x += 1.177f * 0.25f) {
        v.push_back(x);
    }
    const auto dropped = LookAtWave(f, kTwoPi / 1.177f, v);
    CHECK(dropped.empty());
    CHECK(f.Found());
}

TEST_CASE("platforms: a real wave phase that loops 2 pi -> 0 stays the clock (#196 guard)") {
    ClockFinder f;
    std::vector<float> v;
    float x = kTwoPi - 1.0f;
    for (int i = 0; i < 16; ++i) {
        v.push_back(x);
        x += 1.177f * 0.25f;
        if (x >= kTwoPi) {
            x -= kTwoPi;
        }
    }
    const auto dropped = LookAtWave(f, kTwoPi / 1.177f, v);
    CHECK(dropped.empty());
    REQUIRE(f.Found());
    CHECK(f.Found()->cycle == doctest::Approx(kTwoPi));
}

TEST_CASE("platforms: a follower whose own word stands outside the owner's loop does not write it (#196)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock_key = b.clock_key = Key(kRegionWaveNode, 0xf8);  // a wave phase by the node's layout
    a.cycle = b.cycle = 6.2831853f;
    a.clock = 4.5f;    // loops within 2 s, so the owner's ticks carry the loop 2 pi
    b.clock = -26.4f;  // the floor's word in the other game: never in the loop
    std::uint64_t time = 0;
    std::size_t writes_when_seen = 0;
    bool seen = false;
    for (int i = 0; i < 400; ++i) {
        time += 50 * kMs;
        a.Step(0.05f);
        b.Step(0.05f);
        if (b.clock > -20.0f) {
            b.clock = -26.4f;  // the game works it out again
        }
        a.Tick(time);
        b.Tick(time);
        for (auto& m : a.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = 1;
            if (i > 60) {  // B comes into the room after 3 s
                b.sync.Receive(pc);
            }
        }
        a.sent.clear();
        b.sent.clear();
        if (!seen) {
            for (const auto& n : b.notes) {
                if (n.find("is no clock here") != std::string::npos) {
                    seen = true;
                    writes_when_seen = b.sync.Writes();
                }
            }
        }
    }
    CHECK(seen);
    CHECK(writes_when_seen == 0);
    CHECK(b.sync.Writes() == 0);  // never written
}

TEST_CASE("platforms: a non-wave frame clock a step past the owner's estimated loop keeps following (#196 review)") {
    // A frame lift's loop is only an estimate from the one jump the owner saw; the follower's own clock can stand a
    // little past it near the top. Not a wave phase by any layout, so the follower never gives up over it.
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.cycle = 10.0f;
    b.cycle = 10.3f;  // B's top lies a step past A's
    b.clock = 3.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 40000);
    CHECK(b.sync.Writes() > 0);
    for (const auto& n : b.notes) {
        CHECK(n.find("is no clock here") == std::string::npos);
    }
}

TEST_CASE("platforms: a word that steps but not at 2 pi over the word before it is no wave phase") {
    const Floats look{{Key(3, 0x10), 18.0f}, {Key(3, 0x40), 1.0f}, {Key(3, 0x20), 1.0f}};
    CHECK(WavePhaseCycle(look, Key(3, 0x40), kTwoPi / 18.0f).has_value());
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x40), 1.0f).has_value());   // a seconds clock
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x20), kTwoPi / 18.0f).has_value());  // no cycle 48 bytes before
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x10), kTwoPi / 18.0f).has_value());  // first words of a region
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x40), -kTwoPi / 18.0f).has_value());
}

TEST_CASE("platforms: a follower's pause menu does not stop it following, and it is in step after (#35)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 4000);
    REQUIRE(std::fabs(b.clock - a.clock) < PlatformSync::kDriftS + 0.06f);
    const std::size_t before = b.sync.Writes();
    b.paused = true;
    Play(a, b, time, 4000);
    CHECK(b.sync.Writes() == before);  // nothing written into a game that stands still
    b.paused = false;
    Play(a, b, time, 3000);
    CHECK(std::fabs(b.clock - a.clock) < PlatformSync::kDriftS + 0.06f);
    for (const auto& n : b.notes) {
        CHECK(n.find("do not hold") == std::string::npos);
    }
}

TEST_CASE("platforms: an owner's short pause menu does not pull the others back (#35)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 4000);
    REQUIRE(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    const float shared = a.clock;  // where the shared clock is now; it goes on at 1 a second
    a.paused = true;
    Play(a, b, time, 1500);  // A stays the owner (it was quiet less than kOwnerQuietUs)
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    a.paused = false;
    Play(a, b, time, 3000);
    CHECK(std::fabs(a.clock - (shared + 4.5f)) < PlatformSync::kDriftS + 0.06f);
    CHECK(std::fabs(b.clock - (shared + 4.5f)) < PlatformSync::kDriftS + 0.06f);
    bool caught_up = false;
    for (const auto& n : a.notes) {
        caught_up = caught_up || n.find("stood still") != std::string::npos;
    }
    CHECK(caught_up);
    for (const auto& n : b.notes) {
        CHECK(n.find("do not hold") == std::string::npos);
    }
}

TEST_CASE("platforms: our Link rides by the game's RideOnPlayer and the platform's footprint (#194)") {
    PlatformSync::Driver lift{0x1000, kHash, "Obj_InsideTheDekuTree_ElevatorLift_A_02", {10.0f, 5.0f, 0.0f}, {}};
    const proto::Vec3 on{10.5f, 5.2f, 0.5f};
    const proto::Vec3 beside{13.0f, 5.0f, 0.0f};
    const proto::Vec3 under{10.0f, 3.0f, 0.0f};
    CHECK(PlatformSync::OurLinkRides(lift, on));  // no plug: the footprint
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, beside));
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, under));
    lift.ride_plug = true;
    CHECK(PlatformSync::OurLinkRides(lift, on));
    CHECK(PlatformSync::OurLinkRides(lift, beside));  // a wide platform: our Link on its far edge (review)
    lift.other_on = true;
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, beside));  // another player's puppet on our copy
    CHECK(PlatformSync::OurLinkRides(lift, on));            // both of us on it
    lift.other_on = false;
    lift.ride_plug = false;
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, on));  // the game says nobody is on it
    PlatformSync::Driver mover{0x2000, kHash, "GroundControlActor_ResidentMoveTo", {10.0f, 5.0f, 0.0f}, {}};
    CHECK_FALSE(PlatformSync::OurLinkRides(mover, on));  // a controller: no footprint, only its plug
    mover.ride_plug = true;
    CHECK(PlatformSync::OurLinkRides(mover, beside));
}

TEST_CASE("platforms: no clock write while our Link rides it, in step once he is off (#194)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;  // far out of step: a write would make the lift jump
    for (Game* g : {&a, &b}) {
        g->driver_name = "Obj_Deku_MoveFloor_A_01";  // a free floor (not in this scene's table): its footprint
        g->driver_at = {1.0f, 0.0f, 0.0f};
    }
    a.link = {6.0f, 0.0f, 0.0f};  // A beside its floor
    b.link = {1.2f, 0.3f, 0.0f};  // B stands on its floor
    std::uint64_t time = 0;
    a.Tick(time);
    Play(a, b, time, 4000);
    CHECK(b.sync.Writes() == 0);
    CHECK(std::fabs(a.clock - b.clock) > 10.0f);
    const auto has = [&b](const char* text) {
        for (const auto& n : b.notes) {
            if (n.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("our Link rides it (footprint)"));
    // Off the floor: put in step, and the held writes count as none the game took back.
    b.link = {6.0f, 0.0f, 0.0f};
    Play(a, b, time, 3000);
    CHECK(b.sync.Writes() >= 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(has("our Link is off it"));
    CHECK_FALSE(has("do not hold"));
}

TEST_CASE("platforms: a jump on the platform keeps the hold, a longer time off it does not (#194 review)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    for (Game* g : {&a, &b}) {
        g->driver_name = "Obj_Deku_MoveFloor_A_01";
    }
    a.link = {6.0f, 0.0f, 0.0f};
    b.link = {1.2f, 0.3f, 0.0f};
    std::uint64_t time = 0;
    a.Tick(time);
    Play(a, b, time, 3000);
    REQUIRE(b.sync.Writes() == 0);
    // A jump: Link is over the footprint's top for half a second.
    b.link = {1.2f, 3.0f, 0.0f};
    Play(a, b, time, 500);
    CHECK(b.sync.Writes() == 0);
    // Landed again: still held.
    b.link = {1.2f, 0.3f, 0.0f};
    Play(a, b, time, 1000);
    CHECK(b.sync.Writes() == 0);
    // Off it for good: in step after the grace.
    b.link = {6.0f, 0.0f, 0.0f};
    Play(a, b, time, 3000);
    CHECK(b.sync.Writes() >= 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
}

TEST_CASE("platforms: an owner riding its platform does not put its clock back after a pause (#194 review)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    for (Game* g : {&a, &b}) {
        g->driver_name = "Obj_Deku_MoveFloor_A_01";
        g->link = {6.0f, 0.0f, 0.0f};
    }
    std::uint64_t time = 0;
    Play(a, b, time, 4000);
    REQUIRE(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    a.link = {1.2f, 0.3f, 0.0f};  // A stands on its floor
    a.paused = true;
    Play(a, b, time, 1500);
    a.paused = false;
    Play(a, b, time, 3000);
    CHECK(a.sync.Writes() == 0);
    for (const auto& n : a.notes) {
        CHECK(n.find("stood still") == std::string::npos);
    }
}

TEST_CASE("platforms: an object its own logic drives is neither sent nor written (room-objects)") {
    struct Result {
        std::size_t sent = 0;
        std::size_t writes = 0;
        bool told = false;
    };
    const auto run = [](const char* name, const char* scene, proto::Vec3 at, std::optional<bool> plug) {
        Game a;
        Game b;
        a.self = 1;
        b.self = 2;
        b.clock = 30.0f;
        for (Game* g : {&a, &b}) {
            g->driver_name = name;
            g->driver_at = at;
            g->ride_plug = plug;
            g->link = {at[0] + 6.0f, at[1], at[2]};
        }
        Result r;
        std::uint64_t time = 0;
        for (std::uint64_t t = 0; t < 4000; t += 50) {
            time += 50 * kMs;
            a.Step(0.05f);
            b.Step(0.05f);
            a.Tick(time, scene);
            b.Tick(time, scene);
            r.sent += a.sent.size() + b.sent.size();
            for (auto& m : a.sent) {
                auto pc = std::get<proto::PlatformClock>(m);
                pc.player_number = a.self;
                b.sync.Receive(pc);
            }
            a.sent.clear();
            b.sent.clear();
        }
        r.writes = a.sync.Writes() + b.sync.Writes();
        for (const auto& n : b.notes) {
            r.told = r.told || n.find("driven by its own logic") != std::string::npos;
        }
        return r;
    };
    // The Deku Tree compass-room lift: IsOn from its plate (CoopEyeSwitch sets the plate's plug).
    const Result lift = run("Obj_InsideTheDekuTree_ElevatorLift_A_02", "InsideTheDekuTree", {-29.9f, 116.0f, 3.2f}, {});
    CHECK(lift.sent == 0);
    CHECK(lift.writes == 0);
    CHECK(lift.told);
    // Its limit part placed on it moves with it.
    const Result lim = run("Obj_InsideTheDekuTree_ElevatorLiftLim_A_02", "InsideTheDekuTree", {-29.9f, 118.0f, 3.2f}, {});
    CHECK(lim.sent == 0);
    // A rider lift by its live plug (CoopLiftRide sets it for the other players), in any scene.
    const Result rider = run("Obj_InsideJabu_Jabu_MoveFloor_A_01", "DodongoCavern", {0.0f, 0.0f, 0.0f}, false);
    CHECK(rider.sent == 0);
    CHECK(rider.told);
    // The water room's wave runs free: shared and followed.
    const Result wave = run("GroundControlActor_WaveLoop", "InsideTheDekuTree", {-26.2f, 82.3f, 38.0f}, {});
    CHECK(wave.sent > 0);
    CHECK(wave.writes > 0);
    CHECK_FALSE(wave.told);
}

// ---- #196: the GroundControl node (coop_platforms.h, GroundNode) ----

namespace {
constexpr std::uint64_t kMain = 0x0000007100000000ull;
constexpr std::uint64_t kActor = 0x0000021000001000ull;

// Guest memory for the node search: listed words, 0 elsewhere in the heap, nothing outside it.
struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;
    std::optional<std::uint64_t> operator()(std::uint64_t a) const {
        const auto i = words.find(a);
        if (i == words.end()) {
            return a >= 0x0000021000000000ull && a < 0x0000021100000000ull ? std::optional<std::uint64_t>(0)
                                                                            : std::nullopt;
        }
        return i->second;
    }
};
}  // namespace

TEST_CASE("platforms: the WaveLoop's node is found from its actor, not a node of another actor (#196)") {
    Memory m;
    const std::uint64_t table = 0x0000021000010000ull;
    const std::uint64_t logic = 0x0000021000020000ull;
    const std::uint64_t node = 0x0000021000030000ull;
    const std::uint64_t other = 0x0000021000040000ull;
    m.words[kActor + 0x270] = table;
    m.words[table + 0x118] = logic;
    m.words[logic + 0x10] = other;  // another actor's wave node, met first
    m.words[logic + 0x40] = node;
    m.words[other] = kMain + GroundNode::kWaveVtable;
    m.words[other + 8] = 0x0000021000005000ull;
    m.words[node] = kMain + GroundNode::kWaveVtable;
    m.words[node + 8] = kActor;
    const GroundNode::ReadWord read = [&m](std::uint64_t a) { return m(a); };
    std::size_t looked = 0;
    const auto hit = GroundNode::Find(read, kActor, kMain, &looked);
    REQUIRE(hit);
    CHECK(hit->node == node);
    CHECK(hit->region == kRegionWaveNode);
    CHECK(hit->path == "actor+0x270 > +0x118 > +0x40");
    CHECK(looked > 0);
    CHECK(GroundNode::RegionOf(read, node, kActor, kMain) == kRegionWaveNode);
    CHECK(GroundNode::RegionOf(read, other, kActor, kMain) == 0);
    // A rotate node the same way; nothing for an actor with neither.
    m.words[node] = kMain + GroundNode::kRotateVtable;
    const auto rotate = GroundNode::Find(read, kActor, kMain);
    REQUIRE(rotate);
    CHECK(rotate->region == kRegionRotateNode);
    m.words[node] = 0;
    CHECK_FALSE(GroundNode::Find(read, kActor, kMain));
    // The words handed to the finder: the node's block, keyed by the offset in the node.
    m.words[node] = kMain + GroundNode::kWaveVtable;
    m.words[node + 0xf8] = Bits(1.25f, 0.0f);
    const auto again = GroundNode::Find(read, kActor, kMain);
    REQUIRE(again);
    const auto words = GroundNode::Words(read, *again);
    CHECK(words.size() == (GroundNode::kTo - GroundNode::kFrom) / 8);
    CHECK(words.at(Key(kRegionWaveNode, 0xf8)) == Bits(1.25f, 0.0f));
}

TEST_CASE("platforms: the node search is spread over looks, at most kReadsPerStep words each (B #29 N1)") {
    // A wide heap: the actor points to 150 objects, each to 60 more (over kMaxObjects in all), the node far in.
    Memory m;
    std::uint64_t next = 0x0000021000100000ull;
    std::uint64_t node = 0;
    for (std::uint64_t i = 0; i < 150; ++i) {
        const std::uint64_t a = next;
        next += 0x1000;
        m.words[kActor + 8 * i] = a;
        for (std::uint64_t k = 0; k < 60; ++k) {
            m.words[a + 8 * k] = next;
            if (i == 60 && k == 30) {
                node = next;
            }
            next += 0x1000;
        }
    }
    m.words[node] = kMain + GroundNode::kWaveVtable;
    m.words[node + 8] = kActor;
    std::size_t reads = 0;
    const GroundNode::ReadWord read = [&m, &reads](std::uint64_t a) {
        ++reads;
        return m(a);
    };
    const auto whole = GroundNode::Find(read, kActor, kMain);
    REQUIRE(whole);
    const std::size_t whole_reads = reads;
    CHECK(whole_reads > 4 * GroundNode::kReadsPerStep);  // too much for one frame: the case N1 is about

    GroundNode::Search search(kActor, kMain);
    std::optional<GroundNode::Hit> hit;
    int steps = 0;
    while (!search.Done() && steps < 1000) {
        reads = 0;
        hit = search.Step(read, GroundNode::kReadsPerStep);
        CHECK(reads <= GroundNode::kReadsPerStep + 1);
        ++steps;
    }
    REQUIRE(hit);
    CHECK(hit->node == whole->node);
    CHECK(hit->path == whole->path);
    CHECK(search.Reads() == whole_reads);  // the same words as one whole search, no more
    CHECK(steps > 1);
    // Done: later steps read nothing and keep the hit.
    reads = 0;
    CHECK(search.Step(read, GroundNode::kReadsPerStep)->node == node);
    CHECK(reads == 0);

    // Nothing to find: it ends (Done, no hit) within kMaxObjects objects, each step bounded the same.
    m.words[node] = 0;
    GroundNode::Search none(kActor, kMain);
    steps = 0;
    while (!none.Done() && steps < 1000) {
        reads = 0;
        CHECK_FALSE(none.Step(read, GroundNode::kReadsPerStep));
        CHECK(reads <= GroundNode::kReadsPerStep + 1);
        ++steps;
    }
    CHECK(none.Done());
    CHECK(none.Looked() <= GroundNode::kMaxObjects);
}

TEST_CASE("platforms: the node search goes kMaxDepth objects deep, no further (#196)") {
    Memory m;
    std::uint64_t at = kActor;
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t next = 0x0000021000100000ull + static_cast<std::uint64_t>(i) * 0x1000;
        m.words[at] = next;
        at = next;
    }
    m.words[at] = kMain + GroundNode::kWaveVtable;  // 8 objects down: past the search
    m.words[at + 8] = kActor;
    const GroundNode::ReadWord read = [&m](std::uint64_t a) { return m(a); };
    std::size_t looked = 0;
    CHECK_FALSE(GroundNode::Find(read, kActor, kMain, &looked));
    CHECK(looked == GroundNode::kMaxDepth + 1);
}

TEST_CASE("platforms: a node's wave phase is the clock by its layout, whatever unit the frame time is in (#196)") {
    // The node steps phase += frame time x 2 pi / cycle. A rate that is not 2 pi / the cycle a second (frame time
    // in another unit) would fail the component words' rate test; the node's layout says what the word is.
    ClockFinder f;
    const std::uint64_t phase_key = Key(kRegionWaveNode, 0xf8);
    float phase = 0.5f;
    const float rate = 1.5f * kTwoPi / 18.0f;
    for (int i = 0; i < 12; ++i) {
        f.Add(1'000'000 + static_cast<std::uint64_t>(i) * 250'000,
              Floats{{Key(kRegionWaveNode, 0xc8), 18.0f}, {Key(kRegionWaveNode, 0xd4), 4.0f}, {phase_key, phase}});
        phase = std::fmod(phase + rate * 0.25f, kTwoPi);
    }
    CHECK(f.TakeDropped().empty());
    REQUIRE(f.Found());
    CHECK(f.Found()->key == phase_key);
    CHECK(f.Found()->cycle == doctest::Approx(kTwoPi));
    CHECK(ClockFinder::Trusted(phase_key, true));
    // The angular phases' cycles are 36 bytes before them; a phase whose cycle is 0 (no wave on that axis) is none.
    const Floats look{{Key(kRegionWaveNode, 0xe0), 9.0f}, {Key(kRegionWaveNode, 0xcc), 0.0f}};
    CHECK(NodePhaseCycle(look, Key(kRegionWaveNode, 0x104)).has_value());
    CHECK_FALSE(NodePhaseCycle(look, Key(kRegionWaveNode, 0xfc)).has_value());
    CHECK_FALSE(NodePhaseCycle(look, Key(kRegionWaveNode, 0xc8)).has_value());  // a cycle is no phase
    CHECK(Where(phase_key) == "wave-node+0xf8");
}

TEST_CASE("platforms: a node phase worked out again outside 0 to 2 pi is still dropped (#196 guard)") {
    ClockFinder f;
    const std::uint64_t phase_key = Key(kRegionWaveNode, 0xf8);
    for (int i = 0; i < 8; ++i) {
        f.Add(1'000'000 + static_cast<std::uint64_t>(i) * 250'000,
              Floats{{Key(kRegionWaveNode, 0xc8), 18.0f},
                     {phase_key, -26.4f + 0.349f * 0.25f * static_cast<float>(i)}});
    }
    CHECK_FALSE(f.Found());
    CHECK(f.Banned(phase_key));
}

TEST_CASE("platforms: a rotate node's angle turning backwards stays the clock across its loop (#196)") {
    ClockFinder f;
    const std::uint64_t angle_key = Key(kRegionRotateNode, GroundNode::kRotateAngle);
    float angle = 0.4f;
    const float speed = -0.8f;
    for (int i = 0; i < 16; ++i) {
        f.Add(1'000'000 + static_cast<std::uint64_t>(i) * 250'000,
              Floats{{Key(kRegionRotateNode, GroundNode::kRotateSpeed), speed}, {angle_key, angle}});
        CHECK(f.TakeDropped().empty());
        angle += speed * 0.25f;
        if (angle < 0.0f) {
            angle += kTwoPi;
        }
    }
    REQUIRE(f.Found());
    CHECK(f.Found()->key == angle_key);
    CHECK(f.Found()->rate == doctest::Approx(speed).epsilon(0.02));
    CHECK(Where(angle_key) == "rotate-node+0xd4");
    // Standing still (speed 0): no clock to share.
    CHECK_FALSE(NodePhaseCycle(Floats{{Key(kRegionRotateNode, GroundNode::kRotateSpeed), 0.0f}}, angle_key));
}

TEST_CASE("platforms: the other game follows the owner's wave-node phase into the node's own word (#196)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock_key = b.clock_key = Key(kRegionWaveNode, 0xf8);
    a.rate = b.rate = kTwoPi / 18.0f;
    a.cycle = b.cycle = kTwoPi;
    a.clock = 1.0f;
    b.clock = 4.0f;  // B loaded the room later: its floor is 3 rad (8.6 s) on
    std::uint64_t time = 0;
    Play(a, b, time, 8000);
    CHECK(b.sync.Writes() > 0);  // each write's key is checked in Game::Tick
    CHECK(a.sync.Writes() == 0);
    CHECK(DriftSeconds(b.clock, a.clock, a.rate, kTwoPi) < PlatformSync::kDriftS + 0.05f);
}
