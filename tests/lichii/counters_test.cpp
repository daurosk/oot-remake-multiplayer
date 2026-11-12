// Tests for looping platforms' pause counters (lichii/hook/coop_counters.cpp).
#include <doctest/doctest.h>

#include <cmath>
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

TEST_CASE("counters: a pause menu puts one out of phase; the next pause puts it back") {
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

TEST_CASE("counters: a late or other scene's line is not used; a scene change forgets") {
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
