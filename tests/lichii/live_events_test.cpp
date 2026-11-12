// Tests for starting another player's gimmick event here (lichii/hook/coop_live_events.cpp).
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_dungeon.h"
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

namespace {
// The Deku Tree slingshot room (romfs): the ladder's asker at -37.5 117.8 12.5,
// the door at about -30.5 110 10, the chest ledge at -54.4 112 12.2; the
// compass room's asker (PlatformUp) at -22.6 122.6 -3.8.
constexpr std::array<float, 3> kSlingDoor{-30.5f, 110.0f, 10.0f};
constexpr std::array<float, 3> kChestLedge{-54.4f, 112.0f, 12.2f};
constexpr std::array<float, 3> kCompassRoom{-22.0f, 120.0f, -3.0f};
constexpr std::array<float, 3> kFarAway{-60.0f, 82.0f, 40.0f};
} // namespace

TEST_CASE("live events: the decider starts a held event once per visit, near its place, never during an event") {
    CoopLiveEvents::Decider d;  // default: every held event with a place
    const std::vector<std::string> held{"InsideTheDekuTree_Gimmick_FallingLadder", "InsideTheDekuTree_Other"};
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", true, kSlingDoor).has_value());  // an event runs: wait
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false, kFarAway).has_value());   // not near: wait
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false).has_value());             // no position: wait
    CHECK(d.Next(held, "InsideTheDekuTree", false, kSlingDoor) ==
          std::optional<std::string>("InsideTheDekuTree_Gimmick_FallingLadder"));
    // Once; the other has no place, so it is never started unless named.
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false, kSlingDoor).has_value());
    CHECK_FALSE(d.Next(held, "KokiriForest", false, kSlingDoor).has_value());  // no place in that scene
    CHECK(d.Next(held, "InsideTheDekuTree", false, kChestLedge).has_value());  // back: a new visit; the ledge is near
    CoopLiveEvents::Decider named({"WaterTemple_X"});
    CHECK(named.Next({"WaterTemple_X"}, "WaterTemple", false) == std::optional<std::string>("WaterTemple_X"));
}

TEST_CASE("live events: the tester's Deku Tree switches each start in their own room only") {
    // Issue 176: the slingshot room eye (ladder), the 2nd eye, the compass
    // room's plate and the plate + torch that burns a web.
    const std::vector<std::string> held{
        "InsideTheDekuTree_Gimmick_FallingLadder", "InsideTheDekuTree_Gimmick_RoomF_EyeSwitch",
        "InsideTheDekuTree_Gimmick_RoomD_PlatformUp", "InsideTheDekuTree_Gimmick_CandleAndSpiderWeb"};
    CoopLiveEvents::Decider d;
    CHECK(d.Next(held, "InsideTheDekuTree", false, kSlingDoor) ==
          std::optional<std::string>("InsideTheDekuTree_Gimmick_FallingLadder"));
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false, kSlingDoor).has_value());  // the compass room is 20 m off
    CHECK(d.Next(held, "InsideTheDekuTree", false, kCompassRoom) ==
          std::optional<std::string>("InsideTheDekuTree_Gimmick_RoomD_PlatformUp"));
    CHECK(d.Next(held, "InsideTheDekuTree", false, std::array<float, 3>{-15.0f, 81.0f, 27.0f}) ==
          std::optional<std::string>("InsideTheDekuTree_Gimmick_RoomF_EyeSwitch"));
    CHECK(d.Next(held, "InsideTheDekuTree", false, std::array<float, 3>{-11.0f, 79.5f, -8.0f}) ==
          std::optional<std::string>("InsideTheDekuTree_Gimmick_CandleAndSpiderWeb"));
}

TEST_CASE("live events: every place is a shared gimmick event of a dungeon, in the scene its name says") {
    std::size_t deku = 0;
    for (const auto& p : CoopLiveEvents::Places()) {
        CHECK_FALSE(p.event.empty());
        CHECK(CoopDungeon::IsDungeonScene(p.scene));
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.y));
        CHECK(std::isfinite(p.z));
        if (p.scene == "InsideTheDekuTree") {
            ++deku;
        }
    }
    CHECK(CoopLiveEvents::Places().size() == 204);
    CHECK(deku == 6);
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

TEST_CASE("live events: the names list from the environment") {
    CHECK(CoopLiveEvents::ParseNames("a, b ,,c") == std::set<std::string>{"a", "b", "c"});
    const auto all = CoopLiveEvents::ParseNames(" all ");  // every held gimmick event
    REQUIRE(all.has_value());
    CHECK(all->empty());
    CHECK(CoopLiveEvents::ParseNames("InsideTheDekuTree_Gimmick_FallingLadder") ==
          std::set<std::string>{"InsideTheDekuTree_Gimmick_FallingLadder"});
    // A value that names nothing is no event at all, never "all".
    CHECK_FALSE(CoopLiveEvents::ParseNames("").has_value());
    CHECK_FALSE(CoopLiveEvents::ParseNames(" , ").has_value());
    CHECK_FALSE(CoopLiveEvents::ParseNames(",,").has_value());
    CHECK(CoopLiveEvents::ParseNames("all, a") == std::set<std::string>{"all", "a"});  // only "all" alone
}

TEST_CASE("live events: a build without a name buffer still frees its members before the block is reused") {
    Mem m;
    m.Put64(kMain + addr::kManagerPtr, kHolder);
    m.Put64(kHolder, kManager);
    CoopLiveEvents::Requester r;
    r.Want("E");
    std::vector<std::uint64_t> order;
    auto call = r.Step(m, false, 0, kMain);
    while (call) {
        order.push_back(call->function - kMain);
        // The build leaves no name buffer this time.
        const std::uint64_t ret = call->function == kMain + addr::kArgBuild ? 0 : Run(m, *call);
        call = r.Step(m, true, ret, kMain);
    }
    CHECK(order == std::vector<std::uint64_t>{addr::kMalloc, addr::kArgBuild, addr::kStringFree, addr::kStringFree});
}

TEST_CASE("live events: a queued event waits while an event runs in the game") {
    Mem m;
    constexpr std::uint64_t kState = 0x9300000000;
    m.Put64(kMain + addr::kEventStatePtr, kState);
    m.Put64(kState + addr::kEventSession, 0x1234);  // an event runs
    m.Put64(kMain + addr::kManagerPtr, kHolder);
    m.Put64(kHolder, kManager);
    CoopLiveEvents::Requester r;
    r.Want("E");
    auto call = r.Step(m, false, 0, kMain);
    REQUIRE(call);  // the malloc may go first
    CHECK(call->function == kMain + addr::kMalloc);
    CHECK_FALSE(r.Step(m, true, Run(m, *call), kMain).has_value());  // no build while the event runs
    CHECK_FALSE(r.Step(m, false, 0, kMain).has_value());
    m.Put64(kState + addr::kEventSession, 0);  // it ended
    call = r.Step(m, false, 0, kMain);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kArgBuild);
}

TEST_CASE("live events: no scene read is the same visit, and an empty name is logged plainly") {
    CoopLiveEvents::Decider d;
    const std::vector<std::string> held{"InsideTheDekuTree_Gimmick_FallingLadder"};
    CHECK(d.Next(held, "InsideTheDekuTree", false, kSlingDoor).has_value());
    CHECK_FALSE(d.Next(held, "", false, kSlingDoor).has_value());
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false, kSlingDoor).has_value());  // no second ask
    CoopLiveEvents::Requester r;
    r.Want("");
    const auto notes = r.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "live event with no name not asked");
}

namespace {

CoopLiveEvents::OtherPlayer Other(std::uint8_t n, const char* scene, std::optional<std::string> cutscene,
                                  float x = 0.0f) {
    CoopLiveEvents::OtherPlayer o;
    o.number = n;
    o.scene = scene;
    o.cutscene = std::move(cutscene);
    o.place = std::array<float, 3>{x, 0.0f, 0.0f};
    return o;
}

const std::array<float, 3> kHere{5.0f, 0.0f, 0.0f};

}  // namespace

TEST_CASE("scene sync: a cutscene another player starts beside us starts here, once") {
    CoopLiveEvents::SceneSync s;
    CHECK_FALSE(s.Next({Other(2, "KokiriForest", std::nullopt)}, "KokiriForest", false, kHere));
    const auto a = s.Next({Other(2, "KokiriForest", "KokiriForest_Saria")}, "KokiriForest", false, kHere);
    CHECK(a == std::optional<std::string>("KokiriForest_Saria"));
    // While it plays (theirs and then ours), nothing more.
    CHECK_FALSE(s.Next({Other(2, "KokiriForest", "KokiriForest_Saria")}, "KokiriForest", true, kHere));
    CHECK_FALSE(s.Next({Other(2, "KokiriForest", std::nullopt)}, "KokiriForest", false, kHere));
    // The same name again in this visit: not asked twice.
    CHECK_FALSE(s.Next({Other(2, "KokiriForest", "KokiriForest_Saria")}, "KokiriForest", false, kHere));
}

TEST_CASE("scene sync: apart, in another scene or with our own event, the cutscene is theirs only") {
    CoopLiveEvents::SceneSync far;
    CHECK_FALSE(far.Next({Other(2, "KokiriForest", "X", 40.0f)}, "KokiriForest", false, kHere));  // 35 m away
    CoopLiveEvents::SceneSync other_scene;
    CHECK_FALSE(other_scene.Next({Other(2, "HyruleField", "X")}, "KokiriForest", false, kHere));
    CoopLiveEvents::SceneSync busy;
    CHECK_FALSE(busy.Next({Other(2, "KokiriForest", "X")}, "KokiriForest", true, kHere));
    // A start we could not join is not joined halfway when we become free.
    CHECK_FALSE(busy.Next({Other(2, "KokiriForest", "X")}, "KokiriForest", false, kHere));
    CoopLiveEvents::SceneSync no_link;
    CHECK_FALSE(no_link.Next({Other(2, "KokiriForest", "X")}, "KokiriForest", false, std::nullopt));
}

TEST_CASE("scene sync: two players starting at once give one start, the other is not lost later") {
    CoopLiveEvents::SceneSync s;
    s.Next({Other(2, "Kakariko", std::nullopt), Other(3, "Kakariko", std::nullopt)}, "Kakariko", false, kHere);
    const auto a =
        s.Next({Other(2, "Kakariko", "A"), Other(3, "Kakariko", "B")}, "Kakariko", false, kHere);
    CHECK(a == std::optional<std::string>("A"));
    // B's began in the same frame: seen, not started later out of step.
    CHECK_FALSE(s.Next({Other(2, "Kakariko", "A"), Other(3, "Kakariko", "B")}, "Kakariko", false, kHere));
    // A new visit of the scene may start the same name again.
    s.Next({Other(2, "Kakariko", std::nullopt)}, "HyruleField", false, kHere);
    const auto again = s.Next({Other(2, "Kakariko", "A")}, "Kakariko", false, kHere);
    CHECK(again == std::optional<std::string>("A"));
}

TEST_CASE("scene sync: a cutscene our game played itself in this visit is not played again") {
    CoopLiveEvents::SceneSync s;
    // We came in first and our game played the room's entry scene.
    CHECK_FALSE(s.Next({Other(2, "Deku", std::nullopt)}, "Deku", true, kHere, std::string("Deku_ShutIn")));
    CHECK_FALSE(s.Next({Other(2, "Deku", std::nullopt)}, "Deku", false, kHere));
    // The other player comes in later and plays it: not replayed here.
    CHECK_FALSE(s.Next({Other(2, "Deku", "Deku_ShutIn")}, "Deku", false, kHere));
    // A new scene of theirs still is.
    CHECK(s.Next({Other(2, "Deku", "Deku_Other")}, "Deku", false, kHere) == std::optional<std::string>("Deku_Other"));
}

TEST_CASE("scene sync: in a boss room nothing is started here (each game plays the boss's scenes itself)") {
    CoopLiveEvents::SceneSync s;
    const std::array<float, 3> under_gohma{6.7f, 47.0f, 67.6f};
    const auto there = [&](std::optional<std::string> cutscene) {
        CoopLiveEvents::OtherPlayer o;
        o.number = 2;
        o.scene = "InsideTheDekuTree";
        o.cutscene = std::move(cutscene);
        o.place = std::array<float, 3>{7.5f, 47.0f, 80.0f};
        return o;
    };
    CHECK_FALSE(s.Next({there(std::nullopt)}, "InsideTheDekuTree", false, under_gohma));
    CHECK_FALSE(s.Next({there("InsideTheDekuTree_BossAppear")}, "InsideTheDekuTree", false, under_gohma));
    // Not joined later either, and the same dungeon outside the boss room is as everywhere.
    CHECK_FALSE(s.Next({there("InsideTheDekuTree_BossAppear")}, "InsideTheDekuTree", false, under_gohma));
    const std::array<float, 3> upstairs{7.5f, 120.0f, 80.0f};
    auto o = there("InsideTheDekuTree_Other");
    o.place = upstairs;
    CHECK(s.Next({o}, "InsideTheDekuTree", false, upstairs) == std::optional<std::string>("InsideTheDekuTree_Other"));
}
