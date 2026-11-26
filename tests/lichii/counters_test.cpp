// Tests for looping platforms' pause counters (lichii/hook/coop_counters.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "coop_counters.h"

using namespace CoopCounters;
namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kMs = 1000;
constexpr std::uint64_t kHash = 0x51802f1732232227ull;

// One game's lift: a pause of `pause` frames, then a move of `move` s, again.
// `phase_s` is where in the cycle it is at time 0.
struct Lift {
    std::uint8_t self = 1;
    float pause = 60.0f;  // frames (1 s)
    float move = 2.0f;    // seconds
    double t_in = 0.0;    // seconds into the cycle
    float remaining = 0.0f;
    std::uint64_t node = 0x2140001000;
    std::uint64_t graph = 0x2140000f00;
    std::string name = "Obj_DodongoCave_MoveFloor_A_01";
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;
    bool frozen = false;  // the game paused
    bool probe = false;   // LICHII_COOP_PLATFORMS=probe

    double Cycle() const { return pause / 60.0 + move; }
    // Where the lift is (0 at the bottom pause, 1 at the top), for the check.
    double Phase() const { return std::fmod(t_in, Cycle()) / Cycle(); }

    void Step(double dt) {
        if (frozen) {
            return;
        }
        t_in += dt;
        const double into = std::fmod(t_in, Cycle());
        remaining = into < pause / 60.0 ? pause - static_cast<float>(into * 60.0) : 0.0f;
    }

    void Tick(std::uint64_t time_us, const std::string& scene = "DodongoCavern") {
        Seen s;
        s.node = node;
        s.owner_hash = kHash;
        s.owner_name = name;
        s.remaining = remaining;
        s.duration = pause;
        s.key = KeyOf(kHash, pause, node, graph);
        CounterSync::Out out;
        sync.Tick(scene, self, time_us + 50'000'000, {s}, probe, out);
        for (auto& m : out.send) {
            sent.push_back(std::get<proto::PlatformCounter>(m));
        }
        for (const auto& w : out.writes) {
            CHECK(w.node == node);
            // The game's pause is that much shorter or longer: the cycle moves.
            t_in += static_cast<double>(w.was - w.remaining) / 60.0;
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
};

void Play(Lift& a, Lift& b, std::uint64_t& time_us, std::uint64_t ms, std::uint64_t delay_ms = 80) {
    struct InFlight {
        std::uint64_t at_us;
        proto::PlatformCounter m;
        bool to_b;
    };
    std::vector<InFlight> flight;
    for (std::uint64_t t = 0; t < ms; t += 33) {
        time_us += 33 * kMs;
        a.Step(0.033);
        b.Step(0.033);
        a.Tick(time_us);
        b.Tick(time_us);
        for (auto& m : a.sent) {
            m.player_number = a.self;
            flight.push_back({time_us + delay_ms * kMs, m, true});
        }
        for (auto& m : b.sent) {
            m.player_number = b.self;
            flight.push_back({time_us + delay_ms * kMs, m, false});
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

double PhaseApart(const Lift& a, const Lift& b) {
    double d = std::fabs(a.Phase() - b.Phase());
    return std::min(d, 1.0 - d) * a.Cycle();  // seconds
}

}  // namespace

TEST_CASE("counters: the cave's jump and its bytes") {
    CHECK(HookWord() == 0x14d97fe8u);  // b 0x3c46700 at main+0x5e6760
    CHECK(kCave[0] == 0x7f);           // pacibsp first, as the game had it
    CHECK(kCave.size() == 40);
    CHECK(kRingPointer >= kCaveOffset + kCave.size());
}

TEST_CASE("counters: ring entries, keys and owners") {
    std::array<std::uint64_t, kRingNodes> e{};
    e[0] = 5;
    e[1] = 7;
    e[2] = 5;
    CHECK(RingNodes(e) == std::vector<std::uint64_t>{5, 7});
    // The same placement, length and place in the graph: the same key, whatever the addresses.
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) == KeyOf(kHash, 60.0f, 0x3170001000, 0x3170000f00));
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) != KeyOf(kHash, 90.0f, 0x2140001000, 0x2140000f00));
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) != KeyOf(kHash, 60.0f, 0x2140001100, 0x2140000f00));
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) != KeyOf(kHash + 1, 60.0f, 0x2140001000, 0x2140000f00));
    CHECK(IsPlatformOwner("Obj_DodongoCave_MoveFloor_A_01"));
    CHECK_FALSE(IsPlatformOwner("Enemy_Dodongo"));
    CHECK_FALSE(IsPlatformOwner("Npc_Kokiri"));
    CHECK_FALSE(IsPlatformOwner("Player_Child"));
}

TEST_CASE("counters: Hookshot's world hook ring (patch 0005) reads as the guest ring") {
    HostRing ring;
    std::array<std::uint64_t, kRingNodes> e{};
    CHECK(ring.Read(e) == 0);
    ring.Push(0);  // (no node: nothing)
    CHECK(ring.Count() == 0);
    ring.Push(5);
    ring.Push(7);
    ring.Push(5);
    CHECK(ring.Read(e) == 3);
    CHECK(e[1] == 5);  // the count goes up, then the node goes at count % kRingNodes, as the cave
    CHECK(e[2] == 7);
    CHECK(RingNodes(e) == std::vector<std::uint64_t>{5, 7});
    for (std::uint64_t i = 0; i < kRingNodes + 10; ++i) {
        ring.Push(0x1000 + i);
    }
    CHECK(ring.Read(e) == 3 + kRingNodes + 10);
    CHECK(e[(3 + kRingNodes + 10) % kRingNodes] == 0x1000 + kRingNodes + 9);  // the newest
    ring.Clear();
    ring.Read(e);
    CHECK(RingNodes(e).empty());
    CHECK(ring.Count() == 3 + kRingNodes + 10);  // (the count stays: the reader goes on from it)
}

TEST_CASE("counters: the gap the short way round the period") {
    CHECK(PhaseGap(10'500'000, 10'000'000, 3'000'000) == doctest::Approx(0.5f));
    CHECK(PhaseGap(10'000'000, 10'500'000, 3'000'000) == doctest::Approx(-0.5f));
    CHECK(PhaseGap(12'800'000, 10'000'000, 3'000'000) == doctest::Approx(-0.2f));  // round the loop
    CHECK(PhaseGap(16'100'000, 10'000'000, 3'000'000) == doctest::Approx(0.1f));   // two loops on
}

TEST_CASE("counters: two lifts out of phase come into phase once and stay") {
    Lift a;
    Lift b;
    a.self = 1;
    b.self = 2;
    b.t_in = 1.6;  // 1.6 s out of a 3 s cycle
    std::uint64_t time = 0;
    Play(a, b, time, 3000);  // a cycle: both start counting
    a.Step(0.0);
    CHECK(PhaseApart(a, b) > 1.0);
    Play(a, b, time, 15000);
    CHECK(PhaseApart(a, b) < 0.1);
    CHECK(a.sync.Writes() + b.sync.Writes() == 1);  // one write, by one game
    CHECK(b.sync.Writes() == 1);                     // a tie of starts: the higher number moves
    CHECK(a.sync.Looping(KeyOf(kHash, 60.0f, a.node, a.graph)));
    bool logged = false;
    for (const auto& n : b.notes) {
        logged = logged || n.find("off player 1's: frames left") != std::string::npos;
    }
    CHECK(logged);
    const std::size_t writes = b.sync.Writes();
    Play(a, b, time, 20000);
    CHECK(b.sync.Writes() == writes);  // in phase: nothing more
    CHECK(PhaseApart(a, b) < 0.1);
    CHECK(a.sync.LoopingOwners().contains(kHash));
}

TEST_CASE("counters: a game that came later moves, not the one with more starts") {
    Lift a;
    Lift b;
    a.self = 2;  // the higher number, but here first
    b.self = 1;
    b.t_in = 0.9;
    std::uint64_t time = 0;
    // A alone for 12 s: many starts.
    for (int i = 0; i < 360; ++i) {
        time += 33 * kMs;
        a.Step(0.033);
        a.Tick(time);
        a.sent.clear();
    }
    Play(a, b, time, 20000);
    CHECK(a.sync.Writes() == 0);
    CHECK(b.sync.Writes() == 1);
    CHECK(PhaseApart(a, b) < 0.1);
}

TEST_CASE("counters: a pause menu puts one out of phase, and the next pause puts it back") {
    Lift a;
    Lift b;
    a.self = 1;
    b.self = 2;
    std::uint64_t time = 0;
    Play(a, b, time, 12000);
    CHECK(PhaseApart(a, b) < 0.1);
    CHECK(a.sync.Writes() + b.sync.Writes() == 0);  // in phase from the start
    b.frozen = true;  // B's player opens the menu for 0.8 s
    Play(a, b, time, 800);
    b.frozen = false;
    CHECK(PhaseApart(a, b) > 0.5);
    Play(a, b, time, 15000);
    CHECK(PhaseApart(a, b) < 0.1);
}

TEST_CASE("counters: a counter that never loops (a switch's one move) is never sent or written") {
    Lift a;
    a.self = 1;
    std::uint64_t time = 0;
    a.remaining = 30.0f;  // one pause, then it stays still
    a.frozen = true;
    for (int i = 0; i < 300; ++i) {
        time += 33 * kMs;
        a.remaining = std::max(0.0f, a.remaining - 2.0f);
        a.Tick(time);
    }
    CHECK(a.sent.empty());
    CHECK(a.sync.LoopingOwners().empty());
}

TEST_CASE("counters: a late or other scene's line is not used, and a scene change forgets") {
    CounterSync s;
    CounterSync::Out out;
    s.Tick("DodongoCavern", 2, 1'000'000, {}, false, out);
    proto::PlatformCounter m;
    m.player_number = 1;
    m.scene = "KokiriForest";
    m.server_time_us = 900'000;
    m.ticks.push_back({1234, 3'000'000, 100'000, 5});
    s.Receive(m);  // another scene: dropped
    Lift b;
    b.self = 2;
    b.sync = s;
    b.t_in = 1.0;
    std::uint64_t time = 0;
    for (int i = 0; i < 400; ++i) {
        time += 33 * kMs;
        b.Step(0.033);
        b.Tick(time);
    }
    CHECK(b.sync.Writes() == 0);
    CHECK_FALSE(b.sent.empty());
    b.Tick(time + 33 * kMs, "KokiriForest");
    CHECK(b.sync.LoopingOwners().empty());
}

TEST_CASE("counters: two of our nodes with one key are neither sent nor followed") {
    CounterSync s;
    std::uint64_t time = 50'000'000;
    float r = 0.0f;
    for (int i = 0; i < 600; ++i) {
        time += 33 * kMs;
        r = (i % 90 == 0) ? 60.0f : std::max(0.0f, r - 2.0f);
        Seen x{0x10, 99, kHash, "Obj_X", r, 60.0f};
        Seen y{0x20, 99, kHash, "Obj_X", r, 60.0f};
        CounterSync::Out out;
        s.Tick("DodongoCavern", 1, time, {x, y}, false, out);
        CHECK(out.send.empty());
    }
}

TEST_CASE("counters: probe mode says what it would write, writes nothing, keeps its own loop (review)") {
    Lift a;
    Lift b;
    a.self = 1;
    b.self = 2;
    b.probe = true;
    b.t_in = 1.6;
    std::uint64_t time = 0;
    Play(a, b, time, 20000);
    CHECK(b.sync.Writes() == 0);
    CHECK(PhaseApart(a, b) > 1.0);  // still out of phase
    int said = 0;
    for (const auto& n : b.notes) {
        said += n.find("(probe: not written)") != std::string::npos ? 1 : 0;
    }
    CHECK(said >= 2);  // once a pause, every pause: its loop is still seen
    CHECK(b.sync.Looping(KeyOf(kHash, 60.0f, b.node, b.graph)));
}

TEST_CASE("counters: other actors' nodes are skipped only while they are still theirs (review)") {
    OtherNodes o;
    o.Add(0x100, 0xa0);  // an enemy's counter
    o.Add(0x200, 0xb0);  // an NPC's
    CHECK(o.Skip(0x100, 0xa0));
    CHECK_FALSE(o.Skip(0x300, 0xa0));  // never kept
    // The heap gave the enemy's node memory to a lift's node: read again.
    CHECK_FALSE(o.Skip(0x100, 0xc0));
    CHECK_FALSE(o.Contains(0x100));
    CHECK_FALSE(o.Skip(0x100, 0xa0));  // and forgotten for good
    // The NPC leaves the walk: forgotten.
    o.Keep([](std::uint64_t actor) { return actor != 0xb0; });
    CHECK_FALSE(o.Contains(0x200));
    // A scene change clears all; a full set starts again.
    o.Add(0x400, 0xd0);
    o.Clear();
    CHECK(o.Size() == 0);
    for (std::uint64_t i = 0; i < OtherNodes::kMax; ++i) {
        o.Add(0x1000 + 8 * i, 0xe0);
    }
    CHECK(o.Size() == OtherNodes::kMax);
    o.Add(0x9, 0xe0);
    CHECK(o.Size() == 1);
}

TEST_CASE("counters: the loop from the periods: one, or two that alternate (2026-11-12 run)") {
    using D = std::deque<std::uint64_t>;
    CHECK(LoopOf(D{3'000'000, 3'050'000}, 0.08f) == 3'050'000);
    CHECK(LoopOf(D{3'000'000}, 0.08f) == 0);
    // One node times both waits; up 2 s, down 4 s, 1 s waits: 3 s, 5 s, 3 s, 5 s.
    CHECK(LoopOf(D{3'000'000, 5'000'000, 3'000'000, 5'000'000}, 0.08f) == 8'000'000);
    CHECK(LoopOf(D{5'000'000, 3'000'000, 5'000'000, 3'000'000}, 0.08f) == 8'000'000);
    CHECK(LoopOf(D{3'000'000, 5'000'000, 3'000'000}, 0.08f) == 0);  // not yet
    CHECK(LoopOf(D{3'000'000, 5'000'000, 4'000'000, 6'500'000}, 0.08f) == 0);
}

namespace {

// A lift whose one counter times both of its waits: wait, up, wait, down.
struct TwoWaitLift {
    std::uint8_t self = 1;
    float pause = 60.0f;   // frames, both waits
    double up = 2.0;       // seconds
    double down = 4.0;
    double t_in = 0.0;
    float remaining = 0.0f;
    std::uint64_t node = 0x2140002000;
    std::uint64_t graph = 0x2140001f00;
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;

    double Cycle() const { return 2 * pause / 60.0 + up + down; }
    void Step(double dt) {
        t_in += dt;
        const double into = std::fmod(t_in, Cycle());
        const double w = pause / 60.0;
        if (into < w) {
            remaining = pause - static_cast<float>(into * 60.0);
        } else if (into >= w + up && into < 2 * w + up) {
            remaining = pause - static_cast<float>((into - w - up) * 60.0);
        } else {
            remaining = 0.0f;
        }
    }
    void Tick(std::uint64_t time_us) {
        Seen s{node, KeyOf(kHash, pause, node, graph), kHash, "Obj_DodongoCave_MoveFloor_A_01", remaining, pause};
        CounterSync::Out out;
        sync.Tick("DodongoCavern", self, time_us + 50'000'000, {s}, false, out);
        for (auto& m : out.send) {
            sent.push_back(std::get<proto::PlatformCounter>(m));
        }
        for (const auto& w : out.writes) {
            t_in += static_cast<double>(w.was - w.remaining) / 60.0;
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
    double Apart(const TwoWaitLift& o) const {
        double d = std::fabs(std::fmod(t_in, Cycle()) - std::fmod(o.t_in, Cycle()));
        return std::min(d, Cycle() - d);
    }
};

}  // namespace

TEST_CASE("counters: a lift timing both waits with one counter loops, and comes into phase") {
  for (const double offset : {0.5, 1.5, 3.3, 4.2, 5.0, 7.2}) {
    CAPTURE(offset);
    TwoWaitLift a;
    TwoWaitLift b;
    a.self = 1;
    b.self = 2;
    b.t_in = offset;  // seconds into its 8 s loop
    std::uint64_t time = 0;
    std::vector<std::pair<std::uint64_t, proto::PlatformCounter>> to_a, to_b;
    for (int i = 0; i < 1500; ++i) {  // 50 s
        time += 33 * kMs;
        a.Step(0.033);
        b.Step(0.033);
        a.Tick(time);
        b.Tick(time);
        for (auto& m : a.sent) {
            m.player_number = 1;
            b.sync.Receive(m);
        }
        for (auto& m : b.sent) {
            m.player_number = 2;
            a.sync.Receive(m);
        }
        a.sent.clear();
        b.sent.clear();
    }
    const auto key = KeyOf(kHash, 60.0f, a.node, a.graph);
    CHECK(a.sync.CycleOf(key) == doctest::Approx(8'000'000).epsilon(0.02));
    CHECK(a.Apart(b) < 0.1);
    CHECK(a.sync.Writes() + b.sync.Writes() <= 1);
    bool two = false;
    for (const auto& n : a.notes) {
        two = two || n.find("(two waits)") != std::string::npos;
    }
    CHECK(two);
  }
}

TEST_CASE("counters: every platform actor with a counter leaves the clock path at once, looping or not") {
    CounterSync s;
    CounterSync::Out out;
    Seen x{0x10, 7, kHash, "Obj_DodongoCave_MoveFloor_A_01", 40.0f, 60.0f};
    Seen enemy{0x20, 8, 0x99, "Enemy_Dodongo", 40.0f, 60.0f};
    s.Tick("DodongoCavern", 1, 1'000'000, {x, enemy}, false, out);
    CHECK(s.CounterOwners().contains(kHash));
    CHECK_FALSE(s.CounterOwners().contains(0x99));
    CHECK(s.LoopingOwners().empty());
    // Still kept when its node is out of sight a while (the walk lost it), until the scene changes.
    s.Tick("DodongoCavern", 1, 2'000'000, {}, false, out);
    CHECK(s.CounterOwners().contains(kHash));
    s.Tick("KokiriForest", 1, 3'000'000, {}, false, out);
    CHECK(s.CounterOwners().empty());
}

TEST_CASE("counters: one odd period does not stop a loop (hysteresis)") {
    Lift a;
    a.self = 1;
    std::uint64_t time = 0;
    for (int i = 0; i < 400; ++i) {
        time += 33 * kMs;
        a.Step(0.033);
        a.Tick(time);
    }
    const auto key = KeyOf(kHash, 60.0f, a.node, a.graph);
    REQUIRE(a.sync.Looping(key));
    a.t_in += 0.7;  // a hitch: one period 0.7 s short
    for (int i = 0; i < 120; ++i) {
        time += 33 * kMs;
        a.Step(0.033);
        a.Tick(time);
        CHECK(a.sync.Looping(key));
    }
}

namespace {

// One game's lift node as the 2026-11-12 10:50 run logged it
// (coop-kit/shots/platforms-dodongo/20261112-105044, the first 180-frame
// Obj_DodongoCave_MoveFloor_A_01 node of each game): its starts (seconds,
// from the 10 s "pause counters:" lines: time - (180 - left) / 60), and
// below zero (-0.6) between pauses, as the game leaves it.
struct LogLift {
    std::uint8_t self = 1;
    std::vector<double> starts;
    double shift = 0.0;  // our writes: the pause ends this much sooner (s)
    float remaining = -0.6f;
    std::uint64_t node = 0x2140001000;
    std::uint64_t graph = 0x2140000f00;
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;

    void At(double t) {
        remaining = -0.6f;
        for (const double s : starts) {
            const double into = t - (s - shift);
            if (into >= 0.0 && into * 60.0 < 180.0) {
                remaining = 180.0f - static_cast<float>(into * 60.0);
            }
        }
    }
    void Tick(double t) {
        Seen s;
        s.node = node;
        s.owner_hash = kHash;
        s.owner_name = "Obj_DodongoCave_MoveFloor_A_01";
        s.remaining = remaining;
        s.duration = 180.0f;
        s.key = KeyOf(kHash, 180.0f, node, graph);
        CounterSync::Out out;
        sync.Tick("DodongoCavern", self, static_cast<std::uint64_t>(t * 1e6), {s}, false, out);
        for (auto& m : out.send) {
            auto pc = std::get<proto::PlatformCounter>(m);
            pc.player_number = self;
            sent.push_back(pc);
        }
        for (const auto& w : out.writes) {
            shift += static_cast<double>(w.was - w.remaining) / 60.0;
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
    // Our next start after t (s), with our writes.
    double NextStart(double t) const {
        for (const double s : starts) {
            if (s - shift > t) {
                return s - shift;
            }
        }
        return 0.0;
    }
};

bool Said(const std::vector<std::string>& notes, const char* what) {
    return std::any_of(notes.begin(), notes.end(),
                       [&](const std::string& n) { return n.find(what) != std::string::npos; });
}

}  // namespace

TEST_CASE("counters: the Dodongo lift starts of the 2026-11-12 run loop, though the counter goes below zero") {
    LogLift a;
    LogLift b;
    a.self = 1;
    b.self = 2;
    // From the log (A at 79.85 10.7 left, 99.92 65.9, 119.99 117.0, 140.01
    // 172.6; B at 79.17 9.6, 99.21 67.6, 119.24 122.9, 139.29 177.3), then
    // on at the same period so the test can see that it stays in phase.
    a.starts = {77.03, 98.02, 118.94, 139.88, 160.81, 181.74, 202.67};
    b.starts = {76.33, 97.34, 118.29, 139.24, 160.17, 181.10, 202.03};
    struct InFlight {
        double at;
        proto::PlatformCounter m;
        bool to_b;
    };
    std::vector<InFlight> flight;
    for (double t = 70.0; t < 215.0; t += 0.033) {
        a.At(t);
        b.At(t);
        a.Tick(t);
        b.Tick(t);
        for (auto& m : a.sent) {
            flight.push_back({t + 0.08, m, true});
        }
        for (auto& m : b.sent) {
            flight.push_back({t + 0.08, m, false});
        }
        a.sent.clear();
        b.sent.clear();
        for (auto it = flight.begin(); it != flight.end();) {
            if (it->at <= t) {
                (it->to_b ? b : a).sync.Receive(it->m);
                it = flight.erase(it);
            } else {
                ++it;
            }
        }
    }
    const auto key = KeyOf(kHash, 180.0f, a.node, a.graph);
    CHECK(a.sync.Looping(key));  // the old code: "0 looping" the whole run
    CHECK(b.sync.Looping(key));
    CHECK(std::llabs(static_cast<long long>(a.sync.CycleOf(key)) - 20'930'000) < 100'000);
    CHECK(Said(a.notes, "loops every 20.9"));
    CHECK(Said(b.notes, "loops every 20.9"));
    CHECK(a.sync.Writes() + b.sync.Writes() == 1);  // one write, by one game
    CHECK(b.sync.Writes() == 1);                     // a tie of starts: the higher number moves
    CHECK(Said(b.notes, "s off player 1's: frames left"));
    // In phase from then on: the next starts within 0.3 s of each other.
    CHECK(std::fabs(a.NextStart(185.0) - b.NextStart(185.0)) < 0.3);
}


namespace {

// Report 138: a lift that waits at home (its placement), moves up, waits at the far end, moves down: one
// counter times both waits. Its moves do not take the same time each cycle (a physics slider: a rider, a
// slower frame): each move takes the next of `moves` seconds. It says which wait it is at (Seen::wait, from
// its placement, the same in every game).
struct WaitLift {
    std::uint8_t self = 1;
    float pause = 180.0f;  // 3 s
    std::vector<double> moves{7.5};
    std::size_t move_index = 0;
    int wait = 0;           // the wait it is at, or comes to next
    float remaining = 180.0f;
    double move_left = 0.0;
    bool driven = false;
    bool switched = false;
    bool frozen = false;                     // its game stands still (a pause menu)
    const std::set<std::uint8_t>* away = nullptr;  // the players away, as PLAYER_STATUS says
    std::uint64_t node = 0x2140001000;
    std::uint64_t graph = 0x2140000f00;
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;
    std::vector<std::pair<std::uint64_t, int>> pause_ends;  // server time, wait

    void Step(double dt, std::uint64_t now_us) {
        if (frozen) {
            return;
        }
        if (remaining > 0.0f) {
            remaining -= static_cast<float>(dt * 60.0);
            if (remaining <= 0.0f) {
                remaining = 0.0f;
                pause_ends.push_back({now_us, wait});
                move_left = moves[move_index++ % moves.size()];
            }
            return;
        }
        move_left -= dt;
        if (move_left <= 0.0) {
            wait = 1 - wait;
            remaining = pause;  // the next wait's pause starts
        }
    }
    // Starts its loop `s` seconds in (its pauses and moves as they would have run).
    void Advance(double s) {
        for (double t = 0.0; t < s; t += 1.0 / 60.0) {
            Step(1.0 / 60.0, 0);
        }
        pause_ends.clear();
    }

    void Tick(std::uint64_t time_us) {
        Seen s;
        s.node = node;
        s.owner_hash = kHash;
        s.owner_name = "Obj_DodongoCave_MoveFloor_A_01";
        s.remaining = remaining;
        s.duration = pause;
        s.key = KeyOf(kHash, pause, node, graph);
        s.wait = wait;
        s.driven = driven;
        s.switched = switched;
        CounterSync::Out out;
        static const std::set<std::uint8_t> none;
        sync.Tick("DodongoCavern", self, time_us, {s}, false, out, away != nullptr ? *away : none);
        for (auto& m : out.send) {
            sent.push_back(std::get<proto::PlatformCounter>(m));
        }
        for (const auto& w : out.writes) {
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
    bool Said(const char* what) const {
        return std::any_of(notes.begin(), notes.end(),
                           [what](const std::string& n) { return n.find(what) != std::string::npos; });
    }
};

// Runs the lifts together for `seconds` at 60 frames a second, every message 80 ms late.
void Run(const std::vector<WaitLift*>& lifts, std::uint64_t& time, double seconds) {
    std::vector<std::pair<std::uint64_t, proto::PlatformCounter>> flight;
    for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) {
        time += 16'667;
        for (WaitLift* l : lifts) {
            l->Step(1.0 / 60.0, time);
            l->Tick(time);
            for (auto& m : l->sent) {
                m.player_number = l->self;
                flight.push_back({time + 80'000, m});
            }
            l->sent.clear();
        }
        for (auto it = flight.begin(); it != flight.end();) {
            if (it->first <= time) {
                for (WaitLift* l : lifts) {
                    if (l->self != it->second.player_number) {
                        l->sync.Receive(it->second);
                    }
                }
                it = flight.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// Each of b's last `last` pause ends against a's nearest end of the same wait: the largest gap, in seconds.
double WorstEndGap(const WaitLift& a, const WaitLift& b, std::size_t last) {
    double worst = 0.0;
    const std::size_t from = b.pause_ends.size() > last ? b.pause_ends.size() - last : 0;
    for (std::size_t i = from; i < b.pause_ends.size(); ++i) {
        double best = 1e9;
        for (const auto& [t, w] : a.pause_ends) {
            if (w == b.pause_ends[i].second) {
                best = std::min(best, std::fabs(static_cast<double>(t) - static_cast<double>(b.pause_ends[i].first)) / 1e6);
            }
        }
        worst = std::max(worst, best);
    }
    return worst;
}

}  // namespace

TEST_CASE("counters: a lift whose moves take different times each cycle ends each pause with the leader's (report 138)") {
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    b.moves = {6.0, 9.4, 6.5, 10.1};  // a rider, a slower frame: periods 15 to 26 s, far outside kPeriodSlack
    b.Advance(1.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 240.0);
    REQUIRE(b.pause_ends.size() > 12);
    CHECK(b.sync.Writes() > 0);           // the follower moved its pauses
    CHECK(a.sync.Writes() == 0);          // the leader never does
    CHECK(b.Said("(this pause, "));       // by its pauses, not the period rule
    CHECK(WorstEndGap(a, b, 8) < 0.15);   // every recent pause ends with the leader's of the same wait
}

TEST_CASE("counters: a lift far out of step (beyond one pause) comes back in step") {
    for (const double off : {5.0, 8.0, 12.0, 16.0}) {  // a 21 s cycle with 3 s pauses: most of it
        CAPTURE(off);
        WaitLift a;
        WaitLift b;
        a.self = 1;
        b.self = 2;
        b.moves = {7.5, 8.6};
        b.Advance(off);
        std::uint64_t time = 0;
        Run({&a, &b}, time, 300.0);
        CHECK(b.sync.Writes() > 0);
        CHECK(WorstEndGap(a, b, 6) < 0.15);
    }
}

TEST_CASE("counters: a leader whose lift stops starting does not hold the others (PR 279 review M1)") {
    // The leader's lift stands still (a pause menu, it left the room) while its game keeps resending its old starts:
    // a follower is held at most until the leader's next start was due, and the leader goes stale after two of its
    // periods. Before: every follower's lift was held a flat 60 s at each wait.
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    b.Advance(5.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 120.0);
    REQUIRE(b.sync.Writes() > 0);  // b followed a
    a.remaining = 1.0e9f;          // a's lift stops in a pause; a still sends
    b.pause_ends.clear();
    Run({&a, &b}, time, 150.0);
    CHECK(b.pause_ends.size() >= 8);  // b's lift keeps its own pace (a 21 s cycle, two waits)
}

TEST_CASE("counters: three games follow one leader, even when two of them agree on a period") {
    WaitLift a;  // the host: here first, most starts, its moves change
    WaitLift b;
    WaitLift c;
    a.self = 1;
    b.self = 2;
    c.self = 3;
    a.moves = {7.5, 9.8};
    b.moves = {7.5};  // b and c agree with each other, never with a
    c.moves = {7.5};
    std::uint64_t time = 0;
    Run({&a}, time, 60.0);  // a alone first
    b.Advance(4.0);
    c.Advance(11.0);
    Run({&a, &b, &c}, time, 300.0);
    CHECK(a.sync.Writes() == 0);
    CHECK(WorstEndGap(a, b, 6) < 0.15);
    CHECK(WorstEndGap(a, c, 6) < 0.15);
}

TEST_CASE("counters: one counter timing both waits is never lined up top on bottom") {
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    b.moves = {7.5, 8.2};
    b.Advance(10.5);  // half a cycle: b waits at the far end while a waits at home
    std::uint64_t time = 0;
    Run({&a, &b}, time, 300.0);
    // Each of b's pauses ends with a's pause of the same wait (WorstEndGap matches waits): not the other one.
    CHECK(WorstEndGap(a, b, 6) < 0.15);
}

TEST_CASE("counters: a mover its own player drives (a rider, a player sensor) is never sent or written") {
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    a.driven = true;
    b.driven = true;
    b.Advance(6.0);
    std::uint64_t time = 0;
    std::size_t sends = 0;
    for (int i = 0; i < 60 * 120; ++i) {
        time += 16'667;
        for (WaitLift* l : {&a, &b}) {
            l->Step(1.0 / 60.0, time);
            l->Tick(time);
            sends += l->sent.size();
            l->sent.clear();
        }
    }
    CHECK(sends == 0);
    CHECK(a.sync.Writes() + b.sync.Writes() == 0);
}

TEST_CASE("counters: a counter that restarts every few frames sends at most one message every 250 ms") {
    WaitLift a;
    a.self = 1;
    a.pause = 6.0f;  // 0.1 s
    a.remaining = 6.0f;
    a.moves = {0.05};
    std::uint64_t time = 0;
    std::size_t sends = 0;
    for (int i = 0; i < 60 * 10; ++i) {  // 10 s
        time += 16'667;
        a.Step(1.0 / 60.0, time);
        a.Tick(time);
        sends += a.sent.size();
        a.sent.clear();
    }
    CHECK(sends > 0);
    CHECK(sends <= 10 * 4 + 1);  // at most one every 250 ms (the 3 s ones fall within them)
}

TEST_CASE("counters: a leader whose moves vary irregularly is followed from its actual starts, not a guess") {
    WaitLift a;  // the leader (the lower number): a rider, slower frames, its periods change by seconds
    WaitLift b;
    a.self = 1;
    b.self = 2;
    a.moves = {7.5, 9.8, 6.1, 11.3, 8.0, 6.6};
    b.moves = {7.5};
    b.Advance(2.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 300.0);
    CHECK(a.sync.Writes() == 0);
    CHECK(b.sync.Writes() > 0);
    CHECK(WorstEndGap(a, b, 8) < 0.15);  // every recent pause ends with the leader's of the same wait
    // No pause of b's was held for a whole period by a guess: each ended within one cycle of its start.
    for (std::size_t i = 1; i < b.pause_ends.size(); ++i) {
        CHECK(static_cast<double>(b.pause_ends[i].first - b.pause_ends[i - 1].first) / 1e6 < 40.0);
    }
}

TEST_CASE("counters: two games with equal counts, the higher number more than a pause ahead, do not deadlock") {
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;  // the higher number, ahead by more than one 3 s pause, as many starts as a
    b.Advance(8.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 300.0);
    CHECK(a.sync.Writes() == 0);  // one leader in both games: the lower number
    CHECK(b.sync.Writes() > 0);
    CHECK(WorstEndGap(a, b, 6) < 0.15);
}

TEST_CASE("counters: a counter with no wait known sends no wait keys") {
    WaitLift a;
    a.self = 1;
    a.moves = {7.5};
    std::uint64_t time = 0;
    std::vector<proto::PlatformCounter> all;
    for (int i = 0; i < 60 * 120; ++i) {
        time += 16'667;
        a.Step(1.0 / 60.0, time);
        Seen s;
        s.node = a.node;
        s.owner_hash = kHash;
        s.owner_name = "Obj_DodongoCave_MoveFloor_A_01";
        s.remaining = a.remaining;
        s.duration = a.pause;
        s.key = KeyOf(kHash, a.pause, a.node, a.graph);
        s.wait = -1;  // no placement known
        CounterSync::Out out;
        a.sync.Tick("DodongoCavern", 1, time, {s}, false, out);
        for (auto& m : out.send) {
            all.push_back(std::get<proto::PlatformCounter>(m));
        }
        for (const auto& w : out.writes) {
            a.remaining = w.remaining;
        }
    }
    const std::uint64_t key = KeyOf(kHash, a.pause, a.node, a.graph);
    bool any = false;
    for (const auto& m : all) {
        for (const auto& t : m.ticks) {
            CHECK(t.key != WaitKey(key, 0));
            CHECK(t.key != WaitKey(key, 1));
            any = true;
        }
    }
    CHECK(any);  // its loop is still sent, under its own key
}

TEST_CASE("counters: more entries than one message holds go in more messages, none left out") {
    // Twelve lifts with their waits known: up to 3 entries each (the loop and two waits), 36 in all.
    std::vector<WaitLift> lifts(12);
    for (std::size_t k = 0; k < lifts.size(); ++k) {
        lifts[k].node = 0x2140001000 + 0x1000 * k;
        lifts[k].graph = 0x2140000f00 + 0x1000 * k;
        lifts[k].Advance(0.7 * static_cast<double>(k));
    }
    CounterSync sync;
    std::uint64_t time = 0;
    std::vector<proto::PlatformCounter> last;
    for (int i = 0; i < 60 * 120; ++i) {
        time += 16'667;
        std::vector<Seen> seen;
        for (std::size_t k = 0; k < lifts.size(); ++k) {
            lifts[k].Step(1.0 / 60.0, time);
            Seen s;
            s.node = lifts[k].node;
            s.owner_hash = kHash + k;
            s.owner_name = "Obj_DodongoCave_MoveFloor_A_01";
            s.remaining = lifts[k].remaining;
            s.duration = lifts[k].pause;
            s.key = KeyOf(kHash + k, lifts[k].pause, lifts[k].node, lifts[k].graph);
            s.wait = lifts[k].wait;
            seen.push_back(s);
        }
        CounterSync::Out out;
        sync.Tick("DodongoCavern", 1, time, seen, false, out);
        if (!out.send.empty()) {
            last.clear();
            for (auto& m : out.send) {
                last.push_back(std::get<proto::PlatformCounter>(m));
            }
        }
    }
    REQUIRE(last.size() >= 2);
    std::size_t entries = 0;
    for (const auto& m : last) {
        CHECK(m.ticks.size() <= proto::kMaxPlatformCounters);
        entries += m.ticks.size();
    }
    CHECK(entries > proto::kMaxPlatformCounters);  // all of them went, in more than one message
}

TEST_CASE("counters: row 164: the shortcut lift (a shared switch's input) is lined up once it loops") {
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    a.switched = true;  // Dodongo's 10.5 96.5 -23.0 lift: IsShortCutMove, from the shared step switch
    b.switched = true;
    b.moves = {7.5, 8.1};
    b.Advance(6.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 300.0);
    CHECK(b.sync.Writes() > 0);
    CHECK(a.sync.Writes() == 0);
    CHECK(WorstEndGap(a, b, 6) < 0.15);
}

TEST_CASE("counters: row 164: a switched mover that does not loop (a switch's moves) is never sent or written") {
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    a.switched = true;
    b.switched = true;
    a.moves = {4.0, 19.0, 7.0, 31.0, 12.0};  // pressed at no steady rate: never two periods alike
    b.moves = {9.0, 5.0, 23.0, 6.0, 15.0};
    b.Advance(3.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 240.0);
    CHECK(a.sync.Writes() + b.sync.Writes() == 0);
}

TEST_CASE("counters: row 164: the leader in a pause menu holds nobody, and back it follows, not pulls") {
    std::set<std::uint8_t> away;
    WaitLift a;  // the host: player 1, the leader
    WaitLift b;
    a.self = 1;
    b.self = 2;
    a.away = &away;
    b.away = &away;
    b.Advance(5.0);
    std::uint64_t time = 0;
    Run({&a, &b}, time, 150.0);  // both settled, b follows a
    REQUIRE(WorstEndGap(a, b, 4) < 0.15);
    // a's player opens the pause menu for 25 s: its lift stands still, its game still sends.
    a.frozen = true;
    away.insert(1);
    b.pause_ends.clear();
    b.notes.clear();
    Run({&a, &b}, time, 25.0);
    CHECK_FALSE(b.Said("held for player 1"));
    CHECK(b.pause_ends.size() >= 2);  // b's lift kept its own pace (a 21 s cycle, two waits)
    a.frozen = false;
    away.erase(1);
    // Back: a is 25 s behind. It follows b (settled) until it settles again, so b's lift is never pulled.
    const std::size_t b_writes = b.sync.Writes();
    a.pause_ends.clear();
    b.pause_ends.clear();
    Run({&a, &b}, time, 120.0);
    CHECK(a.sync.Writes() > 0);
    CHECK(b.sync.Writes() == b_writes);
    CHECK(WorstEndGap(b, a, 4) < 0.15);
    // Once a has settled again it leads; in step already, so nothing moves.
    Run({&a, &b}, time, 120.0);
    CHECK(WorstEndGap(a, b, 4) < 0.15);
}

TEST_CASE("counters: row 164: our own game in a pause menu writes nothing") {
    std::set<std::uint8_t> away;
    WaitLift a;
    WaitLift b;
    a.self = 1;
    b.self = 2;
    a.away = &away;
    b.away = &away;
    b.Advance(8.0);
    std::uint64_t time = 0;
    Run({&a}, time, 30.0);
    away.insert(2);  // b is paused from the start
    b.frozen = true;
    Run({&a, &b}, time, 60.0);
    CHECK(b.sync.Writes() == 0);
}

TEST_CASE("counters: row 164: a game that comes into the room follows at once and never pulls the others") {
    WaitLift a;  // player 1, the host, comes back into the room late
    WaitLift b;  // player 2, there all along
    a.self = 1;
    b.self = 2;
    a.Advance(9.0);
    std::uint64_t time = 0;
    Run({&b}, time, 120.0);  // b alone: settled
    Run({&a, &b}, time, 45.0);
    CHECK(a.sync.Writes() > 0);           // a lined up on b from its first pauses
    CHECK(b.sync.Writes() == 0);          // b never moved for the lower number
    CHECK(WorstEndGap(b, a, 2) < 0.15);
    Run({&a, &b}, time, 150.0);           // a settles and leads: in step, so b is not pulled
    CHECK(WorstEndGap(a, b, 4) < 0.15);
}
