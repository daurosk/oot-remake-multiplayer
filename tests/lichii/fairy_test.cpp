// Tests for the fairies (lichii/hook/coop_fairy.cpp): where a fairy goes
// beside its puppet, how it trails, and the two directors sharing one SVC.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "coop_fairy.h"

namespace {

using CoopFairy::Chain;
using CoopFairy::Follower;
using CoopFairy::Settings;
using CoopPuppet::Call;
using CoopPuppet::Input;
using CoopPuppet::Target;

Settings Still() {
    Settings s;
    s.bob = 0.0f;
    s.circle = 0.0f;
    return s;
}

Input Puppets(std::uint8_t number, CoopPuppet::Vec3 at,
              CoopPuppet::Mat3 rot = {1, 0, 0, 0, 1, 0, 0, 0, 1}, std::string scene = "S") {
    Input in;
    in.main_base = 0x80000000;
    in.scene = std::move(scene);
    Target t;
    t.number = number;
    t.actor = "CoopPuppet_Child";
    t.position = at;
    t.rotation = rot;
    in.targets.push_back(t);
    return in;
}

} // namespace

TEST_CASE("fairy: hovers at the offset beside its puppet, as the fairy actor") {
    Follower f(Still());
    const auto out = f.Follow(Puppets(2, {10, 0, 5}), 1000);
    REQUIRE(out.targets.size() == 1);
    const auto& t = out.targets[0];
    CHECK(t.number == 2);
    CHECK(t.actor == "Dm_Npc_Navi");
    CHECK(out.scene == "S");
    CHECK(out.main_base == 0x80000000);
    const auto off = Still().offset;
    CHECK(t.position[0] == doctest::Approx(10 + off[0]));
    CHECK(t.position[1] == doctest::Approx(0 + off[1]));
    CHECK(t.position[2] == doctest::Approx(5 + off[2]));
}

TEST_CASE("fairy: the offset turns with the puppet") {
    Follower f(Still());
    // Half a turn about y: x and z flip.
    const auto out = f.Follow(Puppets(1, {0, 0, 0}, {-1, 0, 0, 0, 1, 0, 0, 0, -1}), 0);
    const auto off = Still().offset;
    CHECK(out.targets[0].position[0] == doctest::Approx(-off[0]));
    CHECK(out.targets[0].position[1] == doctest::Approx(off[1]));
    CHECK(out.targets[0].position[2] == doctest::Approx(-off[2]));
    CHECK(out.targets[0].rotation[0] == doctest::Approx(-1));
}

TEST_CASE("fairy: trails a moving puppet, jumps when it is far") {
    Follower f(Still());
    f.Follow(Puppets(1, {0, 0, 0}), 0);
    // The puppet steps 1 unit; 100 ms later the fairy has closed part of it.
    auto out = f.Follow(Puppets(1, {1, 0, 0}), 100);
    const float x0 = Still().offset[0];
    const float x = out.targets[0].position[0];
    CHECK(x > x0 + 0.1f);
    CHECK(x < x0 + 1.0f);
    // A second later it is nearly there.
    out = f.Follow(Puppets(1, {1, 0, 0}), 1100);
    CHECK(std::abs(out.targets[0].position[0] - (x0 + 1)) < 0.1f);
    // A warp of 50 units: no flight across the map.
    out = f.Follow(Puppets(1, {51, 0, 0}), 1133);
    CHECK(out.targets[0].position[0] == doctest::Approx(x0 + 51));
}

TEST_CASE("fairy: a new scene or a returning player starts at the puppet") {
    Follower f(Still());
    f.Follow(Puppets(1, {0, 0, 0}), 0);
    auto out = f.Follow(Puppets(1, {2, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "T"), 33);
    CHECK(out.targets[0].position[0] == doctest::Approx(2 + Still().offset[0]));
    // Player 1 leaves for a frame and comes back elsewhere.
    Input none = Puppets(1, {0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "T");
    none.targets.clear();
    CHECK(f.Follow(none, 66).targets.empty());
    out = f.Follow(Puppets(1, {3, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, "T"), 99);
    CHECK(out.targets[0].position[0] == doctest::Approx(3 + Still().offset[0]));
}

TEST_CASE("fairy: bobs within its range") {
    Follower f;  // default bob and loop
    const auto& s = f.settings();
    Target p;
    p.position = {0, 0, 0};
    float lo = 1e9f, hi = -1e9f;
    for (int ms = 0; ms < 3000; ms += 50) {
        const auto g = f.Goal(p, static_cast<float>(ms) / 1000.0f, 1);
        lo = std::min(lo, g[1]);
        hi = std::max(hi, g[1]);
        CHECK(std::abs(g[0] - s.offset[0]) <= s.circle + 1e-4f);
        CHECK(std::abs(g[2] - s.offset[2]) <= s.circle + 1e-4f);
    }
    CHECK(hi - lo > s.bob);
    CHECK(hi <= s.offset[1] + s.bob + 1e-4f);
    CHECK(lo >= s.offset[1] - s.bob - 1e-4f);
}

namespace {

// A director stand-in: hands out `calls` calls a frame and records what it saw.
struct FakeStep {
    int calls = 0;
    std::uint64_t id = 0;
    int given = 0;
    std::vector<std::string> seen;
    std::optional<Call> operator()(bool after, std::uint64_t result) {
        if (!after) {
            given = 0;
            seen.push_back("frame");
        } else {
            seen.push_back("result " + std::to_string(result));
        }
        if (given >= calls) {
            return std::nullopt;
        }
        ++given;
        Call c;
        c.function = id * 100 + static_cast<std::uint64_t>(given);
        return c;
    }
};

// Runs one frame as the cave does: SVC, call, SVC with its result, ...
std::vector<std::uint64_t> Frame(Chain& chain, FakeStep& a, FakeStep& b) {
    std::vector<std::uint64_t> made;
    Chain::Step sa = [&](bool af, std::uint64_t r) { return a(af, r); };
    Chain::Step sb = [&](bool af, std::uint64_t r) { return b(af, r); };
    auto call = chain.Next(sa, sb, false, 0);
    while (call) {
        made.push_back(call->function);
        call = chain.Next(sa, sb, true, call->function + 1000);  // the "result"
    }
    return made;
}

} // namespace

TEST_CASE("fairy chain: the puppets first, then the fairies, each with its own results") {
    Chain chain;
    FakeStep a{2, 1};
    FakeStep b{1, 2};
    CHECK(Frame(chain, a, b) == std::vector<std::uint64_t>{101, 102, 201});
    CHECK(a.seen == std::vector<std::string>{"frame", "result 1101", "result 1102"});
    CHECK(b.seen == std::vector<std::string>{"frame", "result 1201"});
    // The next frame starts both again.
    a.seen.clear();
    b.seen.clear();
    CHECK(Frame(chain, a, b) == std::vector<std::uint64_t>{101, 102, 201});
    CHECK(a.seen.front() == "frame");
    CHECK(b.seen.front() == "frame");
}

TEST_CASE("fairy chain: nothing from either, or only the fairies") {
    Chain chain;
    FakeStep a{0, 1};
    FakeStep b{0, 2};
    CHECK(Frame(chain, a, b).empty());
    CHECK(a.seen == std::vector<std::string>{"frame"});
    CHECK(b.seen == std::vector<std::string>{"frame"});
    FakeStep c{0, 1};
    FakeStep d{2, 2};
    CHECK(Frame(chain, c, d) == std::vector<std::uint64_t>{201, 202});
    CHECK(c.seen == std::vector<std::string>{"frame"});
    CHECK(d.seen == std::vector<std::string>{"frame", "result 1201", "result 1202"});
}

TEST_CASE("fairy chain: a stray result with no call out is ignored") {
    Chain chain;
    FakeStep a{1, 1};
    FakeStep b{1, 2};
    Chain::Step sa = [&](bool af, std::uint64_t r) { return a(af, r); };
    Chain::Step sb = [&](bool af, std::uint64_t r) { return b(af, r); };
    CHECK_FALSE(chain.Next(sa, sb, true, 5));
    CHECK(a.seen.empty());
    CHECK(b.seen.empty());
}
