// Tests for the Scrub Brothers' hits shared between games (lichii/hook/coop_scrubs.cpp, issue 149).
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_scrubs.h"

namespace {

namespace addr = CoopScrubs::addr;
namespace proto = coop::protocol;
using CoopScrubs::Puzzle;
using CoopScrubs::Seen;

constexpr std::uint64_t kMain = 0x8000000000;
constexpr std::uint64_t kBlock = 0x9000000000;
constexpr std::uint64_t kTwo = 0x927ab536d2e86500ULL;    // Deku Tree, order 1
constexpr std::uint64_t kThree = 0x079a5f22c7ad91c1ULL;  // order 2
constexpr std::uint64_t kOne = 0x9b04ae25f50c7fcaULL;    // order 3
constexpr std::uint64_t kManager = 0xeb15dea71838d33dULL;
constexpr std::uint64_t kTwoAt = 0xa000000000;
constexpr std::uint64_t kThreeAt = 0xa100000000;
constexpr std::uint64_t kOneAt = 0xa200000000;
constexpr std::uint64_t kManagerAt = 0xa300000000;
const std::string kScene = "InsideTheDekuTree";

class Mem : public CoopPuppet::Guest {
public:
    std::map<std::uint64_t, std::uint8_t> bytes;
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) bytes[address + i] = p[i];
        return true;
    }
    void Put32(std::uint64_t a, std::uint32_t v) { Write(a, &v, 4); }
    std::uint32_t Get32(std::uint64_t a) {
        std::uint32_t v = 0;
        Read(a, &v, 4);
        return v;
    }
};

Seen Make(std::uint64_t address, std::uint64_t hash, std::uint32_t id, std::string anim) {
    Seen s;
    s.address = address;
    s.hash = hash;
    s.id = id;
    s.animation = std::move(anim);
    s.position = {-29.0f, 53.0f, 41.16f};
    return s;
}

std::vector<Seen> Room(const std::string& two = "Wait", const std::string& three = "Wait",
                       const std::string& one = "Wait") {
    return {Make(kTwoAt, kTwo, 11, two), Make(kThreeAt, kThree, 12, three), Make(kOneAt, kOne, 13, one),
            Make(kManagerAt, kManager, 20, "")};
}

Mem Game() {
    Mem m;
    m.Put32(kTwoAt + addr::kTransceiverId, 11);
    m.Put32(kThreeAt + addr::kTransceiverId, 12);
    m.Put32(kOneAt + addr::kTransceiverId, 13);
    m.Put32(kManagerAt + addr::kTransceiverId, 20);
    return m;
}

proto::WorldHit HitOn(std::uint64_t hash, std::uint8_t player = 2) {
    proto::WorldHit h;
    h.player_number = player;
    h.scene = kScene;
    h.hash = hash;
    return h;
}

// Runs the cave for one frame; returns the sends made (their x registers).
std::vector<CoopPuppet::Call> Frame(Puzzle& p, Mem& m, std::uint64_t now) {
    std::vector<CoopPuppet::Call> sends;
    auto c = p.Step(m, false, 0, kMain, now);
    for (int guard = 0; c && guard < 16; ++guard) {
        std::uint64_t result = 0;
        if (c->function == kMain + addr::kMalloc) {
            CHECK(c->x[0] == addr::kBlockSize);
            result = kBlock;
        } else {
            CHECK(c->function == kMain + addr::kSend);
            sends.push_back(*c);
            result = 1;
        }
        c = p.Step(m, true, result, kMain, now);
    }
    return sends;
}

} // namespace

TEST_CASE("scrubs: the brothers and managers from the Banc") {
    CHECK(CoopScrubs::Brothers().size() == 9);
    REQUIRE(CoopScrubs::FindBrother(kTwo) != nullptr);
    CHECK(CoopScrubs::FindBrother(kTwo)->order == 1);
    CHECK(CoopScrubs::FindBrother(kThree)->order == 2);
    CHECK(CoopScrubs::FindBrother(kOne)->order == 3);
    CHECK(CoopScrubs::FindBrother(kOne)->manager == kManager);
    CHECK(CoopScrubs::IsManager(kManager));
    CHECK_FALSE(CoopScrubs::IsManager(kTwo));
    CHECK(CoopScrubs::FindBrother(kManager) == nullptr);
    for (const auto& b : CoopScrubs::Brothers()) {
        CHECK(CoopScrubs::IsManager(b.manager));
        CHECK(b.order >= 1);
        CHECK(b.order <= 3);
    }
}

TEST_CASE("scrubs: hit animations and their override") {
    const auto names = CoopScrubs::DefaultHitAnimations();
    CHECK(CoopScrubs::IsHitAnimation("Paralysis_Start", names));
    CHECK(CoopScrubs::IsHitAnimation("SmallHit", names));
    CHECK_FALSE(CoopScrubs::IsHitAnimation("Escape", names));
    CHECK(CoopScrubs::IsIdleAnimation("Dig"));
    CHECK_FALSE(CoopScrubs::IsIdleAnimation("Paralysis_Start"));
    CHECK_FALSE(CoopScrubs::IsHitAnimation("Wait", names));
    CHECK_FALSE(CoopScrubs::IsHitAnimation("", names));
    const auto parsed = CoopScrubs::ParseAnimations(" SmallHit , ,Paralysis");
    CHECK(parsed == std::set<std::string>{"SmallHit", "Paralysis"});
    CHECK(CoopScrubs::IsHitAnimation("Paralysis_Hit_Left", parsed));
    CHECK(CoopScrubs::ParseAnimations("").empty());
}

TEST_CASE("scrubs: our hit goes out once, the first look is no hit") {
    Puzzle p;
    Puzzle::Out out;
    p.Look(kScene, Room("Paralysis_Start"), std::nullopt, 1'000'000, out);  // first look: baseline
    CHECK(out.send.empty());
    out = {};
    p.Look(kScene, Room("Wait"), std::nullopt, 1'100'000, out);
    p.Look(kScene, Room("Paralysis_Start"), proto::Vec3{1, 2, 3}, 1'200'000, out);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].hash == kTwo);
    CHECK(out.send[0].scene == kScene);
    CHECK(out.send[0].from == proto::Vec3{1, 2, 3});
    out = {};
    p.Look(kScene, Room("Paralysis_Start"), std::nullopt, 1'300'000, out);  // still in it
    p.Look(kScene, Room("Paralysis_Start_Loop"), std::nullopt, 1'400'000, out);  // hit to hit
    CHECK(out.send.empty());
}

TEST_CASE("scrubs: another game's hit is sent to our manager as our brother") {
    Puzzle p;
    Mem m = Game();
    Puzzle::Out out;
    p.Look(kScene, Room(), std::nullopt, 1'000'000, out);
    p.Look(kScene, Room(), std::nullopt, 5'000'000, out);  // settled
    CHECK(p.Receive(HitOn(kThree), 5'000'000));
    CHECK(p.Pending() == 1);
    const auto sends = Frame(p, m, 5'010'000);
    REQUIRE(sends.size() == 1);
    CHECK(p.Block() == kBlock);
    const auto& c = sends[0];
    CHECK(c.x[0] == kThreeAt + addr::kTransceiver);  // the sender: our brother Three
    CHECK(c.x[2] == kBlock + addr::kHeader);
    CHECK(c.x[3] == kBlock + addr::kPayload);
    CHECK(m.Get32(kBlock + addr::kHeader) == 20);  // our manager's id
    CHECK(m.Get32(kBlock + addr::kHeader + 4) == addr::kDefeatedOrderMessage);
    CHECK(m.bytes[kBlock + addr::kHeader + 8] == 0);
    CHECK(m.Get32(kBlock + addr::kPayload) == 2);  // Three's DefeatedOrder
    for (std::uint64_t i = 4; i < addr::kPayloadSize; ++i) {
        CHECK(m.bytes[kBlock + addr::kPayload + i] == 0);
    }
    CHECK(p.Pending() == 0);
    // Malloc once: the next hit reuses the block.
    CHECK(p.Receive(HitOn(kOne), 5'500'000));
    const auto again = Frame(p, m, 5'510'000);
    REQUIRE(again.size() == 1);
    CHECK(again[0].x[0] == kOneAt + addr::kTransceiver);
    CHECK(m.Get32(kBlock + addr::kPayload) == 3);
}

TEST_CASE("scrubs: the stun the given hit makes here is not sent back") {
    Puzzle p;
    Mem m = Game();
    Puzzle::Out out;
    p.Look(kScene, Room(), std::nullopt, 1'000'000, out);
    p.Look(kScene, Room(), std::nullopt, 5'000'000, out);
    p.Receive(HitOn(kTwo), 5'000'000);
    Frame(p, m, 5'000'000);
    out = {};
    p.Look(kScene, Room("Paralysis_Start"), std::nullopt, 5'500'000, out);
    CHECK(out.send.empty());
    // Later, a hit of our own on it goes out again.
    p.Look(kScene, Room("Wait"), std::nullopt, 12'000'000, out);
    p.Look(kScene, Room("Paralysis_Start"), std::nullopt, 12'100'000, out);
    CHECK(out.send.size() == 1);
}

TEST_CASE("scrubs: hits from another game that are not taken") {
    Puzzle p;
    Mem m = Game();
    Puzzle::Out out;
    // Not a brother: for the world sync.
    CHECK_FALSE(p.Receive(HitOn(0x1234), 0));
    // No brothers here yet.
    p.Look(kScene, {}, std::nullopt, 1'000'000, out);
    CHECK(p.Receive(HitOn(kTwo), 1'000'000));
    CHECK(p.Pending() == 0);
    // Just came: a kept hit of an earlier try.
    p.Look(kScene, Room(), std::nullopt, 2'000'000, out);
    CHECK(p.Receive(HitOn(kTwo), 2'500'000));
    CHECK(p.Pending() == 0);
    // Another scene.
    p.Look(kScene, Room(), std::nullopt, 6'000'000, out);
    auto other = HitOn(kTwo);
    other.scene = "KokiriForest_Child";
    CHECK(p.Receive(other, 6'000'000));
    CHECK(p.Pending() == 0);
    // Settled: taken.
    CHECK(p.Receive(HitOn(kTwo), 6'000'000));
    CHECK(p.Pending() == 1);
    // Waited too long for the cave: dropped, nothing sent.
    CHECK(Frame(p, m, 6'000'000 + Puzzle::kWaitUs + 1).empty());
    CHECK(p.Pending() == 0);
}

TEST_CASE("scrubs: a changed or missing actor gets no call") {
    Puzzle p;
    Mem m = Game();
    Puzzle::Out out;
    p.Look(kScene, Room(), std::nullopt, 1'000'000, out);
    p.Look(kScene, Room(), std::nullopt, 5'000'000, out);
    // The manager's id is another now (the address was reused).
    m.Put32(kManagerAt + addr::kTransceiverId, 99);
    p.Receive(HitOn(kTwo), 5'000'000);
    CHECK(Frame(p, m, 5'000'000).empty());
    m.Put32(kManagerAt + addr::kTransceiverId, 20);
    // The walk is old.
    p.Receive(HitOn(kTwo), 5'000'000);
    CHECK(Frame(p, m, 5'000'000 + Puzzle::kFreshUs + 1).empty());
    // No manager in the walk.
    auto room = Room();
    room.pop_back();
    p.Look(kScene, room, std::nullopt, 9'000'000, out);
    p.Receive(HitOn(kTwo), 9'000'000);
    CHECK(Frame(p, m, 9'000'000).empty());
    const auto notes = p.TakeNotes();
    CHECK_FALSE(notes.empty());
}

TEST_CASE("scrubs: leaving the scene drops the waiting hits") {
    Puzzle p;
    Puzzle::Out out;
    p.Look(kScene, Room(), std::nullopt, 1'000'000, out);
    p.Look(kScene, Room(), std::nullopt, 5'000'000, out);
    p.Receive(HitOn(kTwo), 5'000'000);
    CHECK(p.Pending() == 1);
    p.Look("KokiriForest_Child", {}, std::nullopt, 5'100'000, out);
    CHECK(p.Pending() == 0);
}

TEST_CASE("scrubs: a failed malloc turns the giving off") {
    Puzzle p;
    Mem m = Game();
    Puzzle::Out out;
    p.Look(kScene, Room(), std::nullopt, 1'000'000, out);
    p.Look(kScene, Room(), std::nullopt, 5'000'000, out);
    p.Receive(HitOn(kTwo), 5'000'000);
    auto c = p.Step(m, false, 0, kMain, 5'000'000);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    CHECK_FALSE(p.Step(m, true, 0, kMain, 5'000'000));
    p.Receive(HitOn(kTwo), 5'100'000);
    CHECK_FALSE(p.Step(m, false, 0, kMain, 5'100'000));
}
