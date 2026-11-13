// Tests for starting another player's gimmick event here (lichii/hook/coop_live_events.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
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

// ---------------------------------------- deku-dungeon-2: DUNGEON_EVENT (issues 176, 196)

namespace {

constexpr std::uint64_t kSession = 0x9300000000;
constexpr std::uint64_t kEvents = 0x9400000000;
constexpr std::uint64_t kEvent = 0x9500000000;
constexpr std::uint64_t kText = 0x9600000000;
constexpr std::uint64_t kInner = 0x9700000000;
const std::string kDeku = "InsideTheDekuTree";
const std::string kPlatform = "InsideTheDekuTree_Gimmick_RoomD_PlatformUp";
const std::string kLadderEvent = "InsideTheDekuTree_Gimmick_FallingLadder";
const std::array<float, 3> kInRoomD{-22.6f, 122.6f, -3.8f};

bool ReadMem(void* context, std::uint64_t address, void* out, std::size_t size) {
    return static_cast<Mem*>(context)->Read(address, out, size);
}

void Zero(Mem& m, std::uint64_t at, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) m.bytes[at + i] = 0;
}

void PutStr(Mem& m, std::uint64_t at, const std::string& s) {
    m.Write(at, s.c_str(), s.size() + 1);
}

// A running event: the session with one event, both blocks readable and empty.
Mem Running() {
    Mem m;
    m.Put64(kMain + addr::kEventStatePtr, kManager);
    m.Put64(kManager + addr::kEventSession, kSession);
    Zero(m, kSession, addr::kScanSession);
    Zero(m, kEvent, addr::kScanEvent);
    m.Put64(kSession + addr::kSessionEvents, kEvents);
    m.Put32(kSession + addr::kSessionCount, 1);
    m.Put32(kSession + addr::kSessionCurrent, 0);
    m.Put64(kEvents, kEvent);
    return m;
}

std::optional<std::string> Find(Mem& m, const std::vector<std::string_view>& names) {
    return CoopLiveEvents::FindRunningEvent(&ReadMem, &m, kMain, names);
}

} // namespace

TEST_CASE("live events: the Deku Tree's gimmick events with a place, each once") {
    const auto deku = CoopLiveEvents::EventsIn(kDeku);
    CHECK(std::count(deku.begin(), deku.end(), std::string_view(kPlatform)) == 1);
    CHECK(std::count(deku.begin(), deku.end(), std::string_view(kLadderEvent)) == 1);
    CHECK(CoopLiveEvents::EventsIn("NoSuchScene").empty());
}

TEST_CASE("live events: the running event's name is found in its event, its session or one step on") {
    const std::vector<std::string_view> names{kLadderEvent, kPlatform};
    // Not running: no session.
    Mem none;
    none.Put64(kMain + addr::kEventStatePtr, kManager);
    none.Put64(kManager + addr::kEventSession, 0);
    CHECK_FALSE(Find(none, names));
    // Running, no name anywhere.
    Mem m = Running();
    CHECK_FALSE(Find(m, names));
    // Inline in the event.
    PutStr(m, kEvent + 0x40, kPlatform);
    CHECK(Find(m, names) == kPlatform);
    // A longer name that only starts with a known one is not it.
    Mem longer = Running();
    PutStr(longer, kEvent + 0x40, kPlatform + "_Extra");
    CHECK_FALSE(Find(longer, names));
    // A word of the event points to it.
    Mem pointed = Running();
    PutStr(pointed, kText, kLadderEvent);
    pointed.Put64(kEvent + 0x18, kText);
    CHECK(Find(pointed, names) == kLadderEvent);
    // A word of the session points to it (the string ends right before unmapped memory).
    Mem session = Running();
    PutStr(session, kText, kPlatform);
    session.Put64(kSession + 0x200, kText);
    CHECK(Find(session, names) == kPlatform);
    // One step on from the event: event word -> object -> string.
    Mem inner = Running();
    Zero(inner, kInner, addr::kScanInner);
    PutStr(inner, kText, kPlatform);
    inner.Put64(kInner + 0x28, kText);
    inner.Put64(kEvent + 0x30, kInner);
    CHECK(Find(inner, names) == kPlatform);
    // No names asked: nothing read.
    CHECK_FALSE(Find(m, {}));
}

TEST_CASE("live events: the start watch looks for a while after an event starts, one name per event") {
    CoopLiveEvents::StartWatch w;
    int looked = 0;
    auto never = [&] { ++looked; return std::optional<std::string>{}; };
    CHECK_FALSE(w.Frame(false, never));
    CHECK(looked == 0);
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(w.Frame(true, never));
    }
    CHECK(looked == CoopLiveEvents::StartWatch::kTries);  // then gives up for this event
    w.Frame(false, never);
    looked = 0;
    int found = 0;
    auto later = [&] { ++looked; return looked == 3 ? std::optional<std::string>(kPlatform) : std::nullopt; };
    for (int i = 0; i < 50; ++i) {
        if (w.Frame(true, later)) ++found;
    }
    CHECK(found == 1);
    CHECK(looked == 3);
}

TEST_CASE("live events: another player's start is offered at once near its place, our own never again") {
    // Issue 176 / dauro's play-test: the timed platforms came in B only after A's
    // cutscene ended (the played flag comes then). The start now comes first.
    CoopLiveEvents::Decider d;
    d.Started(kPlatform, kDeku);
    CHECK_FALSE(d.Next({}, kDeku, true, kInRoomD));                  // our own event runs: waits
    CHECK_FALSE(d.Next({}, kDeku, false, std::array<float, 3>{0, 0, 0}));  // far from it: waits
    CHECK(d.Next({}, kDeku, false, kInRoomD) == kPlatform);
    // Its played flag comes later: not asked twice in the visit.
    CHECK_FALSE(d.Next({kPlatform}, kDeku, false, kInRoomD));
    // Our own game started it: neither another player's start nor its flag asks it.
    CoopLiveEvents::Decider own;
    own.Ours(kPlatform, kDeku);
    own.Started(kPlatform, kDeku);
    CHECK_FALSE(own.Next({kPlatform}, kDeku, false, kInRoomD));
    // Our own start after theirs came: theirs is dropped.
    CoopLiveEvents::Decider both;
    both.Started(kPlatform, kDeku);
    both.Ours(kPlatform, kDeku);
    CHECK_FALSE(both.Next({}, kDeku, false, kInRoomD));
    // A start for another scene is forgotten when our scene is read.
    CoopLiveEvents::Decider other;
    other.Started(kPlatform, kDeku);
    CHECK_FALSE(other.Next({}, "KokiriForest", false, kInRoomD));
    CHECK_FALSE(other.Next({}, kDeku, false, kInRoomD));
    // Empty names or scenes are ignored.
    CoopLiveEvents::Decider empty;
    empty.Started("", kDeku);
    empty.Started(kPlatform, "");
    CHECK_FALSE(empty.Next({}, kDeku, false, kInRoomD));
}

TEST_CASE("live events: a waiting request is dropped when we left its scene, it is too old, or it was played") {
    CoopLiveEvents::Requester r;
    const auto none = [](const std::string&) { return false; };
    r.Want("Gimmick_A", "InsideTheDekuTree");                       // no age limit
    r.Want("Dm_Kokiri_0030", "KokiriForest", 1'000'000, 3'000'000);  // a story cutscene
    r.Want("Any");                                                   // no scene: kept
    REQUIRE(r.Waiting() == 3);

    // A load (scene not known): nothing dropped for the scene.
    r.Expire("", 2'000'000, none);
    CHECK(r.Waiting() == 3);
    // In the Kokiri Forest within 3 s: the gimmick of the Deku Tree goes.
    r.Expire("KokiriForest", 2'000'000, none);
    CHECK(r.Waiting() == 2);
    // Waited too long behind another event.
    r.Expire("KokiriForest", 4'000'000, none);
    CHECK(r.Waiting() == 1);
    // Played already (its flag came): gone too.
    r.Expire("KokiriForest", 4'000'000, [](const std::string& n) { return n == "Any"; });
    CHECK(r.Waiting() == 0);
    const auto notes = r.TakeNotes();
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].find("Gimmick_A dropped before it was asked (we left its scene)") != std::string::npos);
    CHECK(notes[1].find("Dm_Kokiri_0030") != std::string::npos);
    CHECK(notes[1].find("waited too long") != std::string::npos);
    CHECK(notes[2].find("played already") != std::string::npos);
    // The same name asked twice waits once.
    r.Want("Again", "KokiriForest");
    r.Want("Again", "KokiriForest");
    CHECK(r.Waiting() == 1);
}

TEST_CASE("a gimmick event whose place is near but in another room waits for our room") {
    // The slingshot room's ladder: its asker is at -37.5 117.8 12.5. A Link
    // 11 m from it but in the room next door must not get the event: that
    // room is not loaded there, and the event's camera would show black.
    const std::vector<std::string> held{"InsideTheDekuTree_Gimmick_FallingLadder"};
    const std::array<float, 3> link{-37.5f, 117.8f, 1.5f};
    int link_room = 2;
    const CoopLiveEvents::RoomOf room_of = [&](const std::array<float, 3>& at) -> std::optional<int> {
        return at == link ? link_room : 5;  // the place is in room 5
    };
    CoopLiveEvents::Decider d;
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false, link, &room_of).has_value());
    CHECK_FALSE(d.Next(held, "InsideTheDekuTree", false, link, &room_of).has_value());  // it waits
    link_room = 5;  // we walked in
    CHECK(d.Next(held, "InsideTheDekuTree", false, link, &room_of) ==
          std::optional<std::string>("InsideTheDekuTree_Gimmick_FallingLadder"));
}

TEST_CASE("a gimmick event starts by distance alone where a room is not known") {
    const std::vector<std::string> held{"InsideTheDekuTree_Gimmick_FallingLadder"};
    const std::array<float, 3> link{-37.5f, 117.8f, 1.5f};
    {
        const CoopLiveEvents::RoomOf none = [](const std::array<float, 3>&) { return std::optional<int>(); };
        CoopLiveEvents::Decider d;
        CHECK(d.Next(held, "InsideTheDekuTree", false, link, &none).has_value());
    }
    {
        // Our room known, the place's not: as before.
        const CoopLiveEvents::RoomOf half = [&](const std::array<float, 3>& at) {
            return at == link ? std::optional<int>(2) : std::optional<int>();
        };
        CoopLiveEvents::Decider d;
        CHECK(d.Next(held, "InsideTheDekuTree", false, link, &half).has_value());
    }
    {
        const CoopLiveEvents::RoomOf empty;  // no function at all
        CoopLiveEvents::Decider d;
        CHECK(d.Next(held, "InsideTheDekuTree", false, link, &empty).has_value());
    }
}
TEST_CASE("live events: the game's own requests reach every reader once, the newest kept") {
    CoopLiveEvents::GameRequests requests;
    std::uint64_t a = 0, b = 0;
    CHECK_FALSE(requests.Live());
    CHECK(requests.Since(a).empty());
    requests.Push("Dm_DekuTree_Ladder", true);
    requests.Push("Dm_SY_BancChangeGo", false);
    CHECK(requests.Live());
    auto got = requests.Since(a);
    REQUIRE(got.size() == 2);
    CHECK(std::string(got[0].name) == "Dm_DekuTree_Ladder");
    CHECK(got[0].taken);
    CHECK(std::string(got[1].name) == "Dm_SY_BancChangeGo");
    CHECK_FALSE(got[1].taken);
    CHECK(requests.Since(a).empty());  // each once
    requests.Push("Third", true);
    got = requests.Since(b);  // the second reader gets all three
    REQUIRE(got.size() == 3);
    CHECK(std::string(got[2].name) == "Third");
    got = requests.Since(a);
    REQUIRE(got.size() == 1);
    CHECK(std::string(got[0].name) == "Third");

    // A name longer than the buffer is cut, and still ends.
    requests.Push(std::string(300, 'x'), true);
    got = requests.Since(a);
    REQUIRE(got.size() == 1);
    CHECK(std::string(got[0].name).size() == sizeof got[0].name - 1);
    // A reader far behind gets the newest kKept only.
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 50; ++i) requests.Push("n" + std::to_string(round * 50 + i), true);
        requests.Since(a);
    }
    got = requests.Since(b);
    REQUIRE(got.size() == CoopLiveEvents::GameRequests::kKept);
    CHECK(std::string(got.back().name) == "n149");
    CHECK(std::string(got.front().name) == "n" + std::to_string(150 - CoopLiveEvents::GameRequests::kKept));
}

TEST_CASE("live events: the request that the manager took and that is one of ours names the start") {
    std::vector<CoopLiveEvents::GameRequest> requests(3);
    std::memcpy(requests[0].name, "Dm_Door", sizeof "Dm_Door");
    requests[0].taken = true;
    std::memcpy(requests[1].name, "Dm_DekuTree_Ladder", sizeof "Dm_DekuTree_Ladder");
    requests[1].taken = false;  // refused: an event was running
    std::memcpy(requests[2].name, "Dm_DekuTree_Web", sizeof "Dm_DekuTree_Web");
    requests[2].taken = true;
    const std::vector<std::string_view> names{"Dm_DekuTree_Ladder", "Dm_DekuTree_Web"};
    CHECK(CoopLiveEvents::RequestedOf(requests, names) == "Dm_DekuTree_Web");
    requests[1].taken = true;
    CHECK(CoopLiveEvents::RequestedOf(requests, names) == "Dm_DekuTree_Ladder");
    CHECK_FALSE(CoopLiveEvents::RequestedOf(requests, {"Dm_Other"}));
    CHECK_FALSE(CoopLiveEvents::RequestedOf({}, names));
}

TEST_CASE("live events: a start is given once, by the request when the wrap is there, else by the search") {
    using CoopLiveEvents::StartSource;
    int looked = 0;
    std::optional<std::string> in_session;
    const auto find = [&] {
        ++looked;
        return in_session;
    };
    // No wrap (the emulator, an older Hookshot): the search, as StartWatch alone.
    StartSource plain;
    CHECK_FALSE(plain.Frame(std::nullopt, false, false, find));
    in_session = "Ladder";
    CHECK(plain.Frame(std::nullopt, false, true, find) == "Ladder");
    CHECK_FALSE(plain.Frame(std::nullopt, false, true, find));
    CHECK(plain.ScanOnly() == 0);

    // The wrap: the request names it before the event runs; the search finding it then adds nothing.
    StartSource s;
    looked = 0;
    in_session.reset();
    CHECK(s.Frame(std::string("Ladder"), true, false, find) == "Ladder");
    CHECK_FALSE(s.Frame(std::nullopt, true, false, find));
    in_session = "Ladder";
    for (int i = 0; i < 40; ++i) CHECK_FALSE(s.Frame(std::nullopt, true, true, find));
    CHECK(looked >= 1);
    CHECK(s.ScanOnly() == 0);
    // The event ends; the same event asked again later is a new start.
    CHECK_FALSE(s.Frame(std::nullopt, true, false, find));
    CHECK(s.Frame(std::string("Ladder"), true, false, find) == "Ladder");
    for (int i = 0; i < 5; ++i) CHECK_FALSE(s.Frame(std::nullopt, true, true, find));
    CHECK_FALSE(s.Frame(std::nullopt, true, false, find));

    // An event that started some other way: no request, the search finds it; given, and counted.
    in_session = "Web";
    CHECK(s.Frame(std::nullopt, true, true, find) == "Web");
    CHECK(s.ScanOnly() == 1);
    CHECK_FALSE(s.Frame(std::nullopt, true, true, find));
    CHECK_FALSE(s.Frame(std::nullopt, true, false, find));

    // A request whose event never runs is forgotten, so a later start found by the search is given.
    CHECK(s.Frame(std::string("Web"), true, false, find) == "Web");
    for (int i = 0; i <= StartSource::kForgetFrames; ++i) CHECK_FALSE(s.Frame(std::nullopt, true, false, find));
    CHECK(s.Frame(std::nullopt, true, true, find) == "Web");
    CHECK(s.ScanOnly() == 2);
}

namespace {

constexpr std::uint64_t kEntries = 0x9300000000;
constexpr std::uint64_t kNames = 0x9400000000;

// The build, with room for `capacity` arguments as the game's makes it: each entry's name buffer of 0x40 bytes.
std::uint64_t RunWithArgs(Mem& m, const CoopPuppet::Call& c, std::uint32_t capacity) {
    const std::uint64_t result = Run(m, c);
    if (c.function == kMain + addr::kArgBuild) {
        m.Put32(c.x[0] + addr::kArgCapacity, capacity);
        m.Put64(c.x[0] + addr::kArgEntries, kEntries);
        m.Put32(c.x[0] + addr::kArgCount, 0);
        for (std::uint32_t i = 0; i < capacity; ++i) {
            const std::uint64_t entry = kEntries + addr::kArgEntrySize * i;
            for (std::uint64_t b = 0; b < addr::kArgEntrySize; ++b) m.bytes[entry + b] = 0;
            for (std::uint64_t b = 0; b < 0x40; ++b) m.bytes[kNames + 0x40 * i + b] = 0;
            m.Put64(entry + addr::kEntryName, kNames + 0x40 * i);
            m.Put32(entry + addr::kEntryNameCap, 0x40);
        }
    }
    return result;
}

std::uint32_t Get32(Mem& m, std::uint64_t a) {
    std::uint32_t v = 0;
    m.Read(a, &v, 4);
    return v;
}

} // namespace

TEST_CASE("live events: an event with arguments and a starter, as the game's map change is asked") {
    using Arg = CoopLiveEvents::Requester::Arg;
    Mem m;
    m.Put64(kMain + addr::kManagerPtr, kHolder);
    m.Put64(kHolder, kManager);
    CoopLiveEvents::Requester r;
    constexpr std::uint64_t kLink = 0x9500000000;
    r.WantWith("Dm_SY_BancChangeGo",
               {Arg{"NextBanc", std::string("HyruleField_Child"), false},
                Arg{"NextStartPos", std::string("HyruleField_FrontOfLostWoodBridge"), false},
                Arg{"BootEventName", std::string("Dm_SY_BancChangeArrive"), false},
                Arg{"IsCheckAir", std::nullopt, false}},
               kLink);
    // Asked again before it ran: one request, with the newer arguments.
    r.WantWith("Dm_SY_BancChangeGo",
               {Arg{"NextBanc", std::string("KokiriForest_Child"), false},
                Arg{"NextStartPos", std::string("LostWoodBridge_FrontOfHyruleField"), false},
                Arg{"BootEventName", std::string("Dm_SY_BancChangeArrive"), false},
                Arg{"IsCheckAir", std::nullopt, true}},
               kLink);
    CHECK(r.Waiting() == 1);

    std::vector<std::uint64_t> order;
    auto call = r.Step(m, false, 0, kMain);
    while (call) {
        order.push_back(call->function - kMain);
        if (call->function == kMain + addr::kMalloc) {
            CHECK(call->x[0] == addr::kArgSize + addr::kStringsSize);  // room for the strings after the arg
        }
        if (call->function == kMain + addr::kSetLink) {
            CHECK(call->x[0] == kBlock + addr::kArgMember2);
            CHECK(call->x[1] == kLink);
            CHECK(call->x[2] == 0);
        }
        if (call->function == kMain + addr::kRequest) {
            CHECK(m.Str(m.Get64(kBlock + addr::kArgName)) == "Dm_SY_BancChangeGo");
            REQUIRE(Get32(m, kBlock + addr::kArgCount) == 4);
            const char* names[] = {"NextBanc", "NextStartPos", "BootEventName", "IsCheckAir"};
            const char* texts[] = {"KokiriForest_Child", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive"};
            for (std::uint32_t i = 0; i < 4; ++i) {
                const std::uint64_t entry = kEntries + addr::kArgEntrySize * i;
                CHECK(m.Str(kNames + 0x40 * i) == names[i]);
                if (i < 3) {
                    CHECK(Get32(m, entry + addr::kEntryType) == addr::kTypeString);
                    const std::uint64_t text = m.Get64(entry + addr::kEntryValue);
                    CHECK(text >= kBlock + addr::kArgStrings);  // in our block, after the arg: it stays
                    CHECK(text < kBlock + addr::kArgStrings + addr::kStringsSize);
                    CHECK(m.Str(text) == texts[i]);
                } else {
                    CHECK(Get32(m, entry + addr::kEntryType) == addr::kTypeBool);
                    CHECK(m.bytes[entry + addr::kEntryValue] == 1);
                }
            }
        }
        call = r.Step(m, true, RunWithArgs(m, *call, 8), kMain);
    }
    CHECK(order == std::vector<std::uint64_t>{addr::kMalloc, addr::kArgBuild, addr::kSetLink, addr::kRequest,
                                              addr::kStringFree, addr::kStringFree});

    // The next plain event carries nothing over: no link, no arguments.
    r.Want("InsideTheDekuTree_Gimmick_FallingLadder");
    order.clear();
    call = r.Step(m, false, 0, kMain);
    while (call) {
        order.push_back(call->function - kMain);
        if (call->function == kMain + addr::kRequest) CHECK(Get32(m, kBlock + addr::kArgCount) == 0);
        call = r.Step(m, true, RunWithArgs(m, *call, 8), kMain);
    }
    CHECK(order == std::vector<std::uint64_t>{addr::kArgBuild, addr::kRequest, addr::kStringFree, addr::kStringFree});
}

TEST_CASE("live events: arguments that do not fit the request are not asked, and what was built is freed") {
    using Arg = CoopLiveEvents::Requester::Arg;
    Mem m;
    m.Put64(kMain + addr::kManagerPtr, kHolder);
    m.Put64(kHolder, kManager);
    for (int which = 0; which < 3; ++which) {
        CoopLiveEvents::Requester r;
        std::vector<Arg> args{Arg{"NextBanc", std::string("A"), false}, Arg{"NextStartPos", std::string("B"), false}};
        if (which == 1) args[0].name = std::string(0x40, 'n');                 // a name past the entry's buffer
        if (which == 2) args[1].text = std::string(addr::kStringsSize, 't');   // a text past our strings' room
        r.WantWith("Dm_SY_BancChangeGo", args, 0);
        std::vector<std::uint64_t> order;
        auto call = r.Step(m, false, 0, kMain);
        while (call) {
            order.push_back(call->function - kMain);
            call = r.Step(m, true, RunWithArgs(m, *call, which == 0 ? 1 : 8), kMain);  // 0: room for one argument only
        }
        CHECK(order == std::vector<std::uint64_t>{addr::kMalloc, addr::kArgBuild, addr::kStringFree, addr::kStringFree});
        bool said = false;
        for (const auto& n : r.TakeNotes()) said |= n.find("do not fit") != std::string::npos;
        CHECK(said);
    }
}

TEST_CASE("live events: with no reader the ring keeps the newest requests and never stays full") {
    CoopLiveEvents::GameRequests requests;
    for (int i = 0; i < 1000; ++i) requests.Push("e" + std::to_string(i), true);  // nobody calls Since
    CHECK(requests.Seen() == 1000);
    CHECK(requests.Dropped() == 1000 - 64);
    std::uint64_t cursor = 0;
    const auto got = requests.Since(cursor);
    REQUIRE(got.size() == 64);
    CHECK(std::string(got.front().name) == "e936");
    CHECK(std::string(got.back().name) == "e999");
    requests.Push("after", true);
    const auto more = requests.Since(cursor);
    REQUIRE(more.size() == 1);
    CHECK(std::string(more[0].name) == "after");
}
