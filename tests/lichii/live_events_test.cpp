// Tests for starting another player's gimmick event here (lichii/hook/coop_live_events.cpp).
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_live_events.h"

namespace {

namespace addr = CoopLiveEvents::addr;
constexpr std::uint64_t kMain = 0x8000000000;
constexpr std::uint64_t kBlock = 0x9000000000;
constexpr std::uint64_t kManager = 0x9100000000;
constexpr std::uint64_t kHolder = 0x9200000000;

// Guest memory as bytes.
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
    void Put64(std::uint64_t a, std::uint64_t v) { Write(a, &v, 8); }
    void Put32(std::uint64_t a, std::uint32_t v) { Write(a, &v, 4); }
    std::uint64_t Get64(std::uint64_t a) {
        std::uint64_t v = 0;
        Read(a, &v, 8);
        return v;
    }
    std::string Str(std::uint64_t a) {
        std::string s;
        for (char ch = 0; Read(a + s.size(), &ch, 1) && ch != 0;) s.push_back(ch);
        return s;
    }
};

// The game as the cave would run it: what each call does to memory.
std::uint64_t Run(Mem& m, const CoopPuppet::Call& c) {
    if (c.function == kMain + addr::kMalloc) {
        for (std::uint64_t i = 0; i < c.x[0]; ++i) m.bytes[kBlock + i] = 0;
        return kBlock;
    }
    if (c.function == kMain + addr::kArgBuild) {
        m.Put64(c.x[0] + addr::kArgName, c.x[0] + 0x1a9c);
        m.Put32(c.x[0] + addr::kArgNameCap, 0x80);
        return 0;
    }
    return 1;
}

} // namespace

TEST_CASE("live events: the decider starts an allowed held event once per visit, never during an event") {
    CoopLiveEvents::Decider d;
    const std::vector<std::string> held{"InsideTheDekuTree_Gimmick_FallingLadder", "InsideTheDekuTree_Other"};
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", true).has_value());  // an event runs: wait
    CHECK(d.Next(held, "InsideTheDekuTree", false) == std::optional<std::string>("InsideTheDekuTree_Gimmick_FallingLadder"));
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false).has_value());  // once; the other is not allowed
    CHECK_FALSE(d.Next({"InsideTheDekuTree_Other"}, "KokiriForest", false).has_value());  // only the ladder by default
    CHECK(d.Next(held, "InsideTheDekuTree", false).has_value());  // back in the Deku Tree: a new visit
    CoopLiveEvents::Decider all({});
    CHECK(all.Next({"WaterTemple_X"}, "WaterTemple", false) == std::optional<std::string>("WaterTemple_X"));
}

TEST_CASE("live events: the requester makes the node's calls in order, with the name in the arg") {
    Mem m;
    m.Put64(kMain + addr::kManagerPtr, kHolder);
    m.Put64(kHolder, kManager);
    CoopLiveEvents::Requester r;
    CHECK_FALSE(r.Step(m, false, 0, kMain).has_value());  // nothing wanted
    r.Want("InsideTheDekuTree_Gimmick_FallingLadder");

    std::vector<std::uint64_t> order;
    auto call = r.Step(m, false, 0, kMain);
    while (call) {
        order.push_back(call->function - kMain);
        if (call->function == kMain + addr::kRequest) {
            CHECK(call->x[0] == kManager);
            CHECK(call->x[1] == kBlock);
            CHECK(m.Str(m.Get64(kBlock + addr::kArgName)) == "InsideTheDekuTree_Gimmick_FallingLadder");
        }
        if (call->function == kMain + addr::kStringFree) {
            CHECK(m.Get64(call->x[0]) == kMain + addr::kStringVtable);  // base vtable first
        }
        call = r.Step(m, true, Run(m, *call), kMain);
    }
    CHECK(order == std::vector<std::uint64_t>{addr::kMalloc, addr::kArgBuild, addr::kRequest, addr::kStringFree,
                                              addr::kStringFree});
    CHECK(r.Block() == kBlock);
    bool asked = false;
    for (const auto& n : r.TakeNotes()) asked |= n.find("asked of the game") != std::string::npos;
    CHECK(asked);

    // The next event reuses the block: no second malloc.
    r.Want("InsideTheDekuTree_Gimmick_FallingLadder");
    call = r.Step(m, false, 0, kMain);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kArgBuild);
}

TEST_CASE("live events: no manager or no malloc asks nothing and frees what was built") {
    Mem m;  // no event manager in memory
    CoopLiveEvents::Requester r;
    r.Want("E");
    std::vector<std::uint64_t> order;
    auto call = r.Step(m, false, 0, kMain);
    while (call) {
        order.push_back(call->function - kMain);
        call = r.Step(m, true, Run(m, *call), kMain);
    }
    CHECK(order == std::vector<std::uint64_t>{addr::kMalloc, addr::kArgBuild, addr::kStringFree, addr::kStringFree});

    CoopLiveEvents::Requester q;
    q.Want("E");
    call = q.Step(m, false, 0, kMain);
    REQUIRE(call);
    CHECK_FALSE(q.Step(m, true, 0, kMain).has_value());  // malloc gave nothing
    q.Want("F");
    CHECK_FALSE(q.Step(m, false, 0, kMain).has_value());  // off for the run
    CHECK_FALSE(q.Step(m, false, 0, 0).has_value());
}
