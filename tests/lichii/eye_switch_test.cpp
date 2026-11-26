// SPDX-License-Identifier: GPL-3.0-or-later
// CoopEyeSwitch (deku-switches, #176 #129): another player's eye-switch event starts here by our eye's FirstOn,
// set through the game's set-bool, so our room's asker asks the event itself.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <map>
#include <optional>
#include <set>

#include "coop/protocol/codec.hpp"
#include "coop_dungeon.h"
#include "coop_eye_switch.h"
#include "coop_live_events.h"
#include "coop_logic_probe.h"
#include "coop_logic_set.h"

namespace {
namespace a = CoopLogicSet::addr;
namespace p = CoopLogicProbe::addr;

class FakeGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* b = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            b[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) bytes[address + i] = b[i];
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T v) {
        Write(address, &v, sizeof v);
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kMain = 0x7100000000;
constexpr std::uint64_t kEye = 0x1000;
constexpr std::uint64_t kEye2 = 0x11000;
constexpr std::uint64_t kAsker = 0x20000;
constexpr std::uint64_t kBlock = 0x9000;
const std::string kLadder = "InsideTheDekuTree_Gimmick_FallingLadder";
const std::string kDeku = "InsideTheDekuTree";

// A live logic with one bool plug `key` (the plug keeps the name's key ^ 7) at `value`; `base` spaces the parts.
// deku-ladder: the Deku Tree's eyes start their asker by OnState (the room graph's wiring), so the fake plug is OnState.
const std::uint32_t kOnState = CoopEyeSwitch::PlugKey("OnState");
// `type`: the plug's type byte (7 an output, 0 an input); `mark`: bit 1 of its value word (the game marks changes).
void Eye(FakeGuest& g, std::uint64_t eye, std::uint8_t value = 0, std::uint32_t key = kOnState ^ 7u,
         std::uint8_t type = CoopLogicProbe::kBool, bool mark = true) {
    const std::uint64_t base = eye + 0x1000;
    g.Put<std::uint64_t>(eye + p::kActorLogic, base);
    g.Put<std::uint64_t>(base + p::kLogicInner, base + 0x1000);
    g.Put<std::uint64_t>(base + 0x1000 + p::kLogicState, 5);
    g.Put<std::uint64_t>(base + 0x1000 + p::kLogicTable, base + 0x2000);
    g.Put<std::uint64_t>(base + 0x2000 + p::kTableRootHolder, base + 0x3000);
    g.Put<std::uint64_t>(base + 0x3000 + p::kRoot, base + 0x4000);
    g.Put<std::uint32_t>(base + 0x4000 + p::kKey, key);
    g.Put<std::uint8_t>(base + 0x4000 + p::kType, type);
    g.Put<std::uint64_t>(base + 0x4000 + p::kValue, (base + 0x5000) | (mark ? 2 : 0));
    g.Put<std::uint8_t>(base + 0x5000, value);
}
std::uint8_t FirstOn(FakeGuest& g, std::uint64_t eye) { return g.Get<std::uint8_t>(eye + 0x1000 + 0x5000); }

void EventRunning(FakeGuest& g, bool on) {
    g.Put<std::uint64_t>(kMain + CoopLiveEvents::addr::kEventStatePtr, 0x40000);
    g.Put<std::uint64_t>(0x40000 + CoopLiveEvents::addr::kEventSession, on ? 0x50000 : 0);
}

// The actor list, faked: the asker and the eyes are there when set.
struct World {
    std::uint64_t asker = kAsker;
    std::vector<std::uint64_t> eyes{kEye};
    std::vector<std::string> asked;  // the names looked for
    struct Look {
        std::string name;
        std::array<float, 3> at;
        float max_m;
    };
    std::vector<Look> looks;  // where
    // The asker only at this place (unset: everywhere).
    std::optional<std::array<float, 3>> asker_only_at;
    CoopEyeSwitch::Finder Finder() {
        return [this](CoopPuppet::Guest&, std::uint64_t, std::span<const std::string_view> names,
                      const std::array<float, 3>& at, float max_m) {
            asked.emplace_back(names.front());
            looks.push_back({std::string(names.front()), at, max_m});
            if (names.front() == "EventRequestActor") {
                const bool here = asker != 0 && (!asker_only_at || *asker_only_at == at);
                return here ? std::vector<std::uint64_t>{asker} : std::vector<std::uint64_t>{};
            }
            return eyes;
        };
    }
};

float Dist(const std::array<float, 3>& v, float x, float y, float z) {
    return std::sqrt((v[0] - x) * (v[0] - x) + (v[1] - y) * (v[1] - y) + (v[2] - z) * (v[2] - z));
}

}  // namespace

TEST_CASE("eye switch: the table, its places and FirstOn's key") {
    CHECK(CoopEyeSwitch::kFirstOn == coop::protocol::flag_hash("FirstOn"));
    CHECK(CoopEyeSwitch::Find(kLadder, kDeku) != nullptr);
    CHECK(CoopEyeSwitch::Find(kLadder, "DodongoCavern") == nullptr);
    CHECK(CoopEyeSwitch::Find("FocusOnGimmick_Basic", "DodongoCavern") == nullptr);  // the fire-wall eyes: no place
    // deku-ladder: every asker whose trigger the romfs wiring leads to a switch plug (tools/puppet_pack/eye_switches.py).
    CHECK(CoopEyeSwitch::Switches().size() == 64);
    std::set<std::string_view> scenes, events;
    int known_rooms = 0;
    for (const auto& s : CoopEyeSwitch::Switches()) {
        scenes.insert(s.scene);
        events.insert(s.event);
        CHECK(s.actor.starts_with("Obj_"));
        CHECK((s.plug == "OnState" || s.plug == "FirstOn" || s.plug == "isLightMax"));
        // Its asker stands at one of the event's live places (the decider starts it there), its switch nearby.
        const auto places = CoopLiveEvents::Places();
        CHECK(std::any_of(places.begin(), places.end(), [&](const CoopLiveEvents::Place& pl) {
            return pl.event == s.event && pl.scene == s.scene && Dist(s.asker_at, pl.x, pl.y, pl.z) < 0.2f;
        }));
        CHECK(Dist(s.at, s.asker_at[0], s.asker_at[1], s.asker_at[2]) < 30.0f);
        CHECK_FALSE(CoopLiveEvents::SpansRooms(s.event, s.scene));
        // deku-ladder: a switch asker's place has its own room, or -1 (a tie / a room Link near it is not in: the guess).
        known_rooms += std::any_of(places.begin(), places.end(), [&](const CoopLiveEvents::Place& pl) {
            return pl.event == s.event && pl.scene == s.scene && Dist(s.asker_at, pl.x, pl.y, pl.z) < 0.2f && pl.room >= 0;
        }) ? 1 : 0;
    }
    CHECK(scenes.size() == 9);
    CHECK(known_rooms == 62);  // FocusOnGimmick_JabujabuLift (a tie) and DC SwitchShutter_Room01 (room 9 voted, reached in 9%): -1
    CHECK(events.size() == 63);
    // The Deku Tree's eyes: OnState (the graph's wire), not FirstOn; the Fire Temple's room-6 eye: FirstOn.
    REQUIRE(CoopEyeSwitch::Find(kLadder, kDeku) != nullptr);
    CHECK(CoopEyeSwitch::Find(kLadder, kDeku)->plug == "OnState");
    CHECK(CoopEyeSwitch::Find("FireTemple_06_EyeSwitch", "FireTemple")->plug == "FirstOn");
    // Step, hit and sun switches (the same cause).
    CHECK(CoopEyeSwitch::Find("InsideTheDekuTree_Gimmick_RoomD_PlatformUp", kDeku)->actor == "Obj_StepSwitch_A_02");
    CHECK(CoopEyeSwitch::Find("InsideJabuJabuBellyRoom05Swich", "InsideJabuJabuBelly")->actor ==
          "Obj_InsideJabu_Jabu_SwitchHit_A_02");
    CHECK(CoopEyeSwitch::Find("SpritTemple_Room23_SunSwitch", "SpiritTemple")->plug == "isLightMax");
    // Not here: no wired trigger (Water), not shared (room spins).
    CHECK(CoopEyeSwitch::FindAll("WaterTemple_EyeSwitch_Focus", "WaterTemple").empty());
    CHECK(CoopEyeSwitch::Find("ForestTemple_Room11_RoomSpin", "ForestTemple") == nullptr);
    // Driven only where our set stays (offline hs-step-probe): eye switches and FirstOn latches, 25 of the 64 lines,
    // and the class-B lines whose switch's ON input is unwired: none since g3-objects (Jabu room 14 is class A once its
    // water flags are shared; deku-ladder had 2): 25.
    int driven = 0;
    for (const auto& s : CoopEyeSwitch::Switches()) driven += CoopEyeSwitch::Latches(s) ? 1 : 0;
    CHECK(driven == 25);
    CHECK(CoopEyeSwitch::FindAll("SpiritTemple_Level_Room_19_SwitchOn_01", "SpiritTemple").empty());  // sun: rewritten
    CHECK(CoopEyeSwitch::FindAll("InsideJabuJabuBellyRoom01StepSwitch", "InsideJabuJabuBelly").empty());  // step OnState
    CHECK(CoopEyeSwitch::FindAll("ForestTemple_Room02_ShutterOpen", "ForestTemple").size() == 1);  // step FirstOn
    CHECK(CoopEyeSwitch::FindAll("DodongoCavern_Gimmick_EyeSwitch", "DodongoCavern").size() == 1);  // one eye, not three
    CHECK(CoopEyeSwitch::PlugKey("FirstOn") == CoopEyeSwitch::kFirstOn);
    CHECK(CoopEyeSwitch::OnFromEnv("1"));
    CHECK_FALSE(CoopEyeSwitch::OnFromEnv(""));
    CHECK_FALSE(CoopEyeSwitch::OnFromEnv("0"));
}

TEST_CASE("eye switch: one malloc, then the plug through the game's set-bool until the asker's event runs") {
    FakeGuest g;
    World w;
    Eye(g, kEye);
    CoopEyeSwitch::Switcher s(w.Finder());
    CHECK_FALSE(s.Step(g, false, 0, kMain, 100));  // nothing wanted
    CHECK_FALSE(s.Want("InsideTheDekuTree_Gimmick_RoomH_CandleStick", kDeku));  // a torch, no switch: the old way
    REQUIRE(s.Want(kLadder, kDeku));
    CHECK(s.Want(kLadder, kDeku));  // once
    auto c = s.Step(g, false, 0, kMain, 100);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kMalloc);
    CHECK(c->x[0] == a::kBlockSize);
    c = s.Step(g, true, kBlock, kMain, 100);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kExecute);
    CHECK(c->x[0] == kBlock);
    // The node names our eye and FirstOn, value 1.
    CHECK(g.Get<std::uint32_t>(kBlock + a::kNodeKey) == kOnState);
    const auto owner = g.Get<std::uint64_t>(kBlock + a::kNodeOwner);
    CHECK(g.Get<std::uint64_t>(g.Get<std::uint64_t>(owner + 8) + a::kHolderActor) == kEye);
    CHECK(g.Get<std::uint8_t>(g.Get<std::uint64_t>(kBlock + a::kNodeValue)) == 1);
    CHECK_FALSE(s.Step(g, true, 0, kMain, 100));  // one set a frame
    // The eye's AI put 0 back (a pulse): set again the next frame, no new malloc.
    c = s.Step(g, false, 0, kMain, 116);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kExecute);
    CHECK_FALSE(s.Step(g, true, 0, kMain, 116));
    // The asker asked its event: done, not the old way.
    EventRunning(g, true);
    CHECK_FALSE(s.Step(g, false, 0, kMain, 132));
    CHECK(s.TakeFallbacks().empty());
    const auto notes = s.TakeNotes();
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].find("OnState set") != std::string::npos);
    CHECK(notes[1].find("event started") != std::string::npos);
    CHECK_FALSE(s.Step(g, false, 0, kMain, 148));  // nothing left
}

TEST_CASE("eye switch: a latched FirstOn is not set again, no event in time goes the old way") {
    FakeGuest g;
    World w;
    Eye(g, kEye);
    CoopEyeSwitch::Switcher s(w.Finder());
    REQUIRE(s.Want(kLadder, kDeku));
    REQUIRE(s.Step(g, false, 0, kMain, 1000));  // malloc
    REQUIRE(s.Step(g, true, kBlock, kMain, 1000));  // execute
    s.Step(g, true, 0, kMain, 1000);
    g.Put<std::uint8_t>(kEye + 0x1000 + 0x5000, 1);  // the execute latched it
    CHECK_FALSE(s.Step(g, false, 0, kMain, 1016));  // on: waits for the asker
    CHECK_FALSE(s.Step(g, false, 0, kMain, 1000 + CoopEyeSwitch::kTryMs));
    CHECK_FALSE(s.Step(g, false, 0, kMain, 1001 + CoopEyeSwitch::kTryMs));  // too late
    const auto f = s.TakeFallbacks();
    REQUIRE(f.size() == 1);
    CHECK(f[0] == std::make_pair(kLadder, kDeku));
    CHECK(s.TakeNotes().back().find("plan (b)") != std::string::npos);
}

TEST_CASE("eye switch: our own shot, no asker, no eye, an event running, another scene") {
    SUBCASE("FirstOn 1 already: our game did it") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 1);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(kLadder, kDeku));
        REQUIRE(s.Step(g, false, 0, kMain, 100));  // malloc
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(s.TakeFallbacks().empty());
        CHECK(s.TakeNotes().back().find("already") != std::string::npos);
    }
    SUBCASE("no asker: the room is cleared here, not started") {
        FakeGuest g;
        World w;
        w.asker = 0;
        Eye(g, kEye);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(kLadder, kDeku));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(s.TakeFallbacks().empty());
        CHECK(FirstOn(g, kEye) == 0);
    }
    SUBCASE("an eye without a live FirstOn: never set, the old way after kTryMs") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, 0x1234);  // another plug only
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(kLadder, kDeku));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        g.Put<std::uint64_t>(kEye + p::kActorLogic, 0);  // no logic: the execute would read 0+0x118
        CHECK_FALSE(s.Step(g, false, 0, kMain, 200));
        CHECK_FALSE(s.Step(g, false, 0, kMain, 101 + CoopEyeSwitch::kTryMs));
        CHECK(s.TakeFallbacks().size() == 1);
        CHECK(g.Get<std::uint64_t>(kBlock) == 0);  // no node ever written
    }
    SUBCASE("another event runs: waits, sets after it") {
        FakeGuest g;
        World w;
        Eye(g, kEye);
        EventRunning(g, true);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(kLadder, kDeku));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        EventRunning(g, false);
        CHECK(s.Step(g, false, 0, kMain, 500));
    }
    SUBCASE("we left the scene: dropped") {
        FakeGuest g;
        World w;
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(kLadder, kDeku));
        s.Expire(kDeku);
        s.Expire("DekuForest");
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(s.TakeFallbacks().empty());
        CHECK(s.TakeNotes().front().find("left its scene") != std::string::npos);
    }
}

namespace {
std::uint64_t NodeActor(FakeGuest& g) {
    return g.Get<std::uint64_t>(g.Get<std::uint64_t>(g.Get<std::uint64_t>(kBlock + a::kNodeOwner) + 8) +
                                a::kHolderActor);
}
}  // namespace

TEST_CASE("eye switch: only the eye its asker links to, at its placement (Dodongo's Cavern's fire-wall eyes stay)") {
    // deku-ladder: the old table set every A_05 eye within 25 m in Dodongo's Cavern; two of them start the fire-wall
    // graphs (FocusOnGimmick_Basic), so our walls went out. Now the eye is looked for at its own placement only.
    FakeGuest g;
    World w;
    w.eyes = {kEye, kEye2};  // a finder that (wrongly) gives two: only the first is ever used
    Eye(g, kEye);
    Eye(g, kEye2);
    CoopEyeSwitch::Switcher dc(w.Finder());
    REQUIRE(dc.Want("DodongoCavern_Gimmick_EyeSwitch", "DodongoCavern"));
    dc.Step(g, false, 0, kMain, 100);
    REQUIRE(dc.Step(g, true, kBlock, kMain, 100));
    CHECK(NodeActor(g) == kEye);
    const auto& look = w.looks.back();
    CHECK(look.name == "Obj_EyeSwitch_A_05");
    CHECK(look.at == std::array<float, 3>{80.5f, 121.0f, -6.6f});
    CHECK(look.max_m == CoopEyeSwitch::kEyeAtM);
    g.Put<std::uint8_t>(kEye + 0x1000 + 0x5000, 1);  // it latched
    dc.Step(g, true, 0, kMain, 100);
    CHECK_FALSE(dc.Step(g, false, 0, kMain, 116));  // waits for the asker; the other eye never set
    CHECK_FALSE(dc.Step(g, false, 0, kMain, 132));
    CHECK(FirstOn(g, kEye2) == 0);

    FakeGuest g2;
    Eye(g2, kEye, 1);
    Eye(g2, kEye2);
    CoopEyeSwitch::Switcher deku(w.Finder());
    REQUIRE(deku.Want(kLadder, kDeku));
    deku.Step(g2, false, 0, kMain, 100);
    CHECK_FALSE(deku.Step(g2, true, kBlock, kMain, 100));  // ours is on: our own shot; nothing else touched
    CHECK(deku.TakeNotes().back().find("already") != std::string::npos);
    CHECK(FirstOn(g2, kEye2) == 0);
}

TEST_CASE("eye switch: a switch whose own AI rewrites the plug is not driven (step OnState, sun isLightMax)") {
    // Offline hs-step-probe (2026-11-16): B's Jabu room-1 step switch OnState, set through the set-bool, read back 0
    // every frame for 2 s (its AI writes it from contact) and no event came; A's own press raised the asker's trigger.
    World w;
    CoopEyeSwitch::Switcher s(w.Finder());
    s.Expire("InsideJabuJabuBelly");
    CHECK_FALSE(s.Want("InsideJabuJabuBellyRoom01StepSwitch", "InsideJabuJabuBelly"));  // the old way at once
    CHECK_FALSE(s.Want("SpritTemple_Room23_SunSwitch", "SpiritTemple"));
    CHECK_FALSE(s.Want("InsideJabuJabuBellyRoom05Swich", "InsideJabuJabuBelly"));  // hit switch OnState
    // ... and their guard flags come as before (nothing waits for a copy we do not drive).
    CHECK_FALSE(s.HoldsGuard(coop::protocol::flag_hash("InsideJabuJabuBelly_Gimmick_Onswitch_first"), "InsideJabuJabuBelly"));
    CHECK(s.Want(kLadder, kDeku));  // an eye: driven
}

// The upper Jabu-Jabu room 14 line as it was (deku-ladder, A #39): the input path's own tests use it.
constexpr CoopEyeSwitch::Input kOldUpper[] = {
    {"InsideJabuJabuBellyRoom14StepSwitch", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_04", {29.2f, 77.5f, -66.7f}, "FirstOn"},
};

TEST_CASE("eye switch: no input lines (Jabu-Jabu room 14 is class A once its water flags are shared), not GTG room 6") {
    // tools/puppet_pack/eye_switches.py --classify (g3-objects): 38 class A, 1 class B (GTG room 6, no settable input).
    // Jabu room 14's events write the shared two-way water flags: our asker run again would be a second, late writer.
    CHECK(CoopEyeSwitch::Inputs().empty());
    std::set<std::string_view> shared;
    for (const auto& f : CoopDungeon::Flags()) shared.insert(f.name);
    CHECK(shared.contains("Gimmick_Insidejabujabu_Updown_MoveWater"));
    CHECK(shared.contains("Gimmick_Insidejabujabu_down_MoveWater"));
    for (const char* event : {"InsideJabuJabuBellyRoom14StepSwitch", "InsideJabuJabuBellyRoom14StepSwitch_Under"}) {
        const auto* line = CoopEyeSwitch::Find(event, "InsideJabuJabuBelly");
        REQUIRE(line != nullptr);
        CHECK(CoopEyeSwitch::InputOf(*line) == nullptr);
        CHECK_FALSE(CoopEyeSwitch::Latches(*line));                                 // a step switch's OnState
        CHECK(CoopEyeSwitch::FindAll(event, "InsideJabuJabuBelly").empty());       // the old way at once
    }
    CHECK(CoopEyeSwitch::FindAll("GerudoTrainingGround_06_DisappearFire", "GerudoTrainingGround").empty());
    CHECK(CoopEyeSwitch::FindAll("SpritTemple_Room08_OpenShutter", "SpiritTemple").empty());
    CoopEyeSwitch::Switcher s;
    s.Expire("InsideJabuJabuBelly");
    // nothing of ours is tried, so the asker-delete flags come as they arrive
    CHECK_FALSE(s.HoldsGuard(coop::protocol::flag_hash("InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Upper"), "InsideJabuJabuBelly"));
    CHECK_FALSE(s.HoldsGuard(coop::protocol::flag_hash("InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Lower"), "InsideJabuJabuBelly"));
}

TEST_CASE("eye switch: an input line (a test line) names one table line and holds its asker-delete flag while tried") {
    CoopEyeSwitch::ScopedInputsForTest lines(kOldUpper);
    REQUIRE(CoopEyeSwitch::Inputs().size() == 1);
    const auto& in = CoopEyeSwitch::Inputs()[0];
    int found = 0;
    for (const auto& s : CoopEyeSwitch::Switches()) {
        if (CoopEyeSwitch::InputOf(s) == &in) {
            ++found;
            CHECK(s.plug == "OnState");
            CHECK(CoopEyeSwitch::Latches(s));
        }
    }
    CHECK(found == 1);  // the upper and lower switches are one kind, told apart by placement
    CHECK(CoopEyeSwitch::FindAll(in.event, in.scene).size() == 1);
    CoopEyeSwitch::Switcher s;
    s.Expire("InsideJabuJabuBelly");
    CHECK(s.HoldsGuard(coop::protocol::flag_hash("InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Upper"), "InsideJabuJabuBelly"));
    CHECK_FALSE(s.HoldsGuard(coop::protocol::flag_hash("Insidejabujabu_Room5_Cleared"), "InsideJabuJabuBelly"));
}

TEST_CASE("eye switch: the held guards follow the input lines (no cache of the first call's lines)") {
    // The full host run (2436/2437, eye_switch_test:397): HoldsGuard's hashes were a function-local static built on
    // its first call, so a test with no input lines that ran first left the later input-line test with "not held".
    const auto upper = coop::protocol::flag_hash("InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Upper");
    CoopEyeSwitch::Switcher s;
    s.Expire("InsideJabuJabuBelly");
    CHECK_FALSE(s.HoldsGuard(upper, "InsideJabuJabuBelly"));  // no input lines: the flag comes as it arrives
    {
        CoopEyeSwitch::ScopedInputsForTest lines(kOldUpper);
        CHECK(s.HoldsGuard(upper, "InsideJabuJabuBelly"));    // the same Switcher, now with the line: held
    }
    CHECK_FALSE(s.HoldsGuard(upper, "InsideJabuJabuBelly"));  // the lines back: not held again
}

TEST_CASE("eye switch: an input line stores 1 into the switch's unwired input (no call) until the asker's event runs") {
    const std::string jabu = "InsideJabuJabuBelly";
    const std::string upper = "InsideJabuJabuBellyRoom14StepSwitch";
    const auto first_on = CoopEyeSwitch::PlugKey("FirstOn") ^ 7u;
    CoopEyeSwitch::ScopedInputsForTest lines(kOldUpper);  // no generated input lines since g3-objects
    SUBCASE("set, held, then the event") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, false);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(upper, jabu));
        REQUIRE(s.Step(g, false, 0, kMain, 100));             // the malloc (shared with the output path)
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));     // stored, no execute call
        CHECK(FirstOn(g, kEye) == 1);
        CHECK(g.Get<std::uint64_t>(kBlock) == 0);             // our node block untouched
        CHECK_FALSE(s.Step(g, false, 0, kMain, 116));         // it stays: waits for the asker
        EventRunning(g, true);
        CHECK_FALSE(s.Step(g, false, 0, kMain, 132));
        CHECK(s.TakeFallbacks().empty());
        const auto notes = s.TakeNotes();
        REQUIRE(notes.size() >= 2);
        CHECK(notes.front().find("FirstOn set (its unwired input") != std::string::npos);
        CHECK(notes[1].find("old byte 0") != std::string::npos);  // the address and the old byte are logged
        CHECK(notes.back().find("event started") != std::string::npos);
        // review Round 8: the old byte goes back once the try ended (the switch is re-armed by its own event).
        CHECK(FirstOn(g, kEye) == 1);
        CHECK_FALSE(s.Step(g, false, 0, kMain, 148));
        CHECK(FirstOn(g, kEye) == 0);
        CHECK(s.TakeNotes().back().find("taken back to 0") != std::string::npos);
    }
    SUBCASE("gave up (no event in time): the old way, and the old byte back") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, false);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(upper, jabu));
        s.Step(g, false, 0, kMain, 100);
        s.Step(g, true, kBlock, kMain, 100);
        CHECK(FirstOn(g, kEye) == 1);
        CHECK_FALSE(s.Step(g, false, 0, kMain, 101 + CoopEyeSwitch::kTryMs));
        CHECK(s.TakeFallbacks().size() == 1);
        CHECK_FALSE(s.Step(g, false, 0, kMain, 120 + CoopEyeSwitch::kTryMs));
        CHECK(FirstOn(g, kEye) == 0);
    }
    SUBCASE("we left the scene before the next Step: nothing written into the gone switch") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, false);
        CoopEyeSwitch::Switcher s(w.Finder());
        s.Expire(jabu);
        REQUIRE(s.Want(upper, jabu));
        s.Step(g, false, 0, kMain, 100);
        s.Step(g, true, kBlock, kMain, 100);
        EventRunning(g, true);
        s.Step(g, false, 0, kMain, 116);  // ended: the take-back waits for the next Step
        s.Expire("KokiriForest");
        s.Step(g, false, 0, kMain, 132);
        CHECK(FirstOn(g, kEye) == 1);  // not written (the real one is gone with the scene)
        CHECK(s.TakeNotes().back().find("not taken back") != std::string::npos);
    }
    SUBCASE("an input whose changes the game marks: not set, the old way") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, true);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(upper, jabu));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(FirstOn(g, kEye) == 0);
        CHECK(s.TakeFallbacks().size() == 1);
        CHECK(s.TakeNotes().back().find("marks its changes") != std::string::npos);
    }
    // review Round 8: the value address must be this plug's own on this instance, or nothing is written.
    SUBCASE("another plug of the switch shares the value address (a shared default): not set") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, false);
        const std::uint64_t base = kEye + 0x1000;
        g.Put<std::uint64_t>(base + 0x4000 + p::kLeft, base + 0x6000);  // a second plug, the same value address
        g.Put<std::uint32_t>(base + 0x6000 + p::kKey, 0x1234u);
        g.Put<std::uint8_t>(base + 0x6000 + p::kType, CoopLogicSet::kInputType);
        g.Put<std::uint64_t>(base + 0x6000 + p::kValue, base + 0x5000);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(upper, jabu));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(FirstOn(g, kEye) == 0);
        CHECK(s.TakeFallbacks().size() == 1);
        CHECK(s.TakeNotes().back().find("another plug of the switch shares it") != std::string::npos);
    }
    SUBCASE("the other switch of its kind has the same value address: not set") {
        FakeGuest g;
        World w;
        w.eyes = {kEye, kEye2};
        Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, false);
        Eye(g, kEye2, 0, first_on, CoopLogicSet::kInputType, false);
        g.Put<std::uint64_t>(kEye2 + 0x1000 + 0x4000 + p::kValue, kEye + 0x1000 + 0x5000);
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(upper, jabu));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(FirstOn(g, kEye) == 0);
        CHECK(s.TakeNotes().back().find("another switch of its kind shares it") != std::string::npos);
    }
    SUBCASE("a value in the game's static image, or far from its plug: not set") {
        for (const std::uint64_t at : {kMain + 0x4e85000, std::uint64_t{0x900000}}) {
            FakeGuest g;
            World w;
            Eye(g, kEye, 0, first_on, CoopLogicSet::kInputType, false);
            g.Put<std::uint64_t>(kEye + 0x1000 + 0x4000 + p::kValue, at);
            CoopEyeSwitch::Switcher s(w.Finder());
            REQUIRE(s.Want(upper, jabu));
            s.Step(g, false, 0, kMain, 100);
            CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
            CHECK(g.Get<std::uint8_t>(at) == 0);
            CHECK(s.TakeFallbacks().size() == 1);
        }
    }
    SUBCASE("only an output of that name: never written as the input") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 0, first_on);  // type 7
        CoopEyeSwitch::Switcher s(w.Finder());
        REQUIRE(s.Want(upper, jabu));
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK_FALSE(s.Step(g, false, 0, kMain, 101 + CoopEyeSwitch::kTryMs));
        CHECK(FirstOn(g, kEye) == 0);
        CHECK(s.TakeFallbacks().size() == 1);
    }
}

TEST_CASE("eye switch: the plug the graph reads, never another (an eye with only FirstOn is not set for OnState)") {
    FakeGuest g;
    World w;
    Eye(g, kEye, 0, CoopEyeSwitch::kFirstOn ^ 7u);  // the Deku ladder reads OnState: this eye has no such plug
    CoopEyeSwitch::Switcher s(w.Finder());
    REQUIRE(s.Want(kLadder, kDeku));
    s.Step(g, false, 0, kMain, 100);
    CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
    CHECK_FALSE(s.Step(g, false, 0, kMain, 101 + CoopEyeSwitch::kTryMs));
    CHECK(s.TakeFallbacks().size() == 1);  // the old way, nothing written
    CHECK(g.Get<std::uint64_t>(kBlock) == 0);
}

TEST_CASE("eye switch: dropped once our game has the event played (our asker ran it)") {
    FakeGuest g;
    World w;
    Eye(g, kEye);
    CoopEyeSwitch::Switcher s(w.Finder());
    REQUIRE(s.Want(kLadder, kDeku));
    s.Expire(kDeku, [](const std::string&) { return false; });
    s.Expire(kDeku, nullptr);  // no flags read: kept
    s.Expire(kDeku, [&](const std::string& n) { return n == kLadder; });
    s.Step(g, false, 0, kMain, 100);
    CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
    CHECK(FirstOn(g, kEye) == 0);
    CHECK(s.TakeFallbacks().empty());
    CHECK(s.TakeNotes().front().find("has it played") != std::string::npos);
}

TEST_CASE("eye switch: guard flags (a negated trigger flag, an asker's delete flag) wait while the switch path works") {
    // deku-ladder round 3: generated with the table; only flags coop_dungeon.cpp shares, each for a table event.
    CHECK(CoopEyeSwitch::Guards().size() == 43);  // g3-objects: + Jabu room 14's Updown_MoveWater (now shared)
    for (const auto& g : CoopEyeSwitch::Guards()) {
        // read once at the room's load (UpdateOnce 1): it deletes the compass asker only at a load, never live
        CHECK(g.flag != "InsideTheDekuTree_RoomD_StepSwitch_On_01");
    }
    std::set<std::string_view> shared;
    for (const auto& f : CoopDungeon::Flags()) shared.insert(f.name);
    for (const auto& g : CoopEyeSwitch::Guards()) {
        CHECK(shared.contains(g.flag));
        CHECK(CoopEyeSwitch::Find(g.event, g.scene) != nullptr);
        CHECK_FALSE(g.flag.starts_with("HavePlayedEvent."));  // those are held already (coop_dungeon.h HeldIn)
    }
    const auto cleared = coop::protocol::flag_hash("InsideTheDekuTree_RoomC_Cleared");  // deletes the ladder's asker
    const auto fire = coop::protocol::flag_hash("GerudoTrainingGround_Dungeon_Room06_Fire");  // read negated
    FakeGuest g;
    World w;
    Eye(g, kEye, 0, 0x1234);  // no live plug: the switch path gives up after kTryMs
    CoopEyeSwitch::Switcher s(w.Finder());
    s.Expire(kDeku);
    CHECK(s.HoldsGuard(cleared, kDeku));
    CHECK_FALSE(s.HoldsGuard(cleared, "GerudoTrainingGround"));  // its own dungeon only
    CHECK_FALSE(s.HoldsGuard(fire, "GerudoTrainingGround"));  // a step switch's OnState event: not driven, not held
    CHECK_FALSE(s.HoldsGuard(coop::protocol::flag_hash("ForestTemple_After_Appearance_TreasureChest"), "ForestTemple"));
    // The path gave up on the ladder this visit: its guard comes as before (the old way); others still wait.
    REQUIRE(s.Want(kLadder, kDeku));
    s.Step(g, false, 0, kMain, 100);
    s.Step(g, true, kBlock, kMain, 100);
    s.Step(g, false, 0, kMain, 101 + CoopEyeSwitch::kTryMs);
    REQUIRE(s.TakeFallbacks().size() == 1);
    CHECK_FALSE(s.HoldsGuard(cleared, kDeku));
    CHECK(s.HoldsGuard(coop::protocol::flag_hash("InsideTheDekuTree_RoomF_Cleared"), kDeku));
    // A new visit: tried again, so it waits again.
    s.Expire("KokiriForest");
    s.Expire(kDeku);
    CHECK(s.HoldsGuard(cleared, kDeku));
}

TEST_CASE("eye switch: guard flags after a try: released when our copy did not run, held until played when it ran") {
    const auto cleared = coop::protocol::flag_hash("InsideTheDekuTree_RoomC_Cleared");
    SUBCASE("no asker") {
        FakeGuest g;
        World w;
        w.asker = 0;
        Eye(g, kEye);
        CoopEyeSwitch::Switcher s(w.Finder());
        s.Expire(kDeku);
        REQUIRE(s.Want(kLadder, kDeku));
        CHECK(s.HoldsGuard(cleared, kDeku));  // waiting for its try
        s.Step(g, false, 0, kMain, 100);
        CHECK_FALSE(s.Step(g, true, kBlock, kMain, 100));
        CHECK(s.TakeNotes().back().find("no room asker") != std::string::npos);
        CHECK_FALSE(s.HoldsGuard(cleared, kDeku));
    }
    SUBCASE("our own hit") {
        FakeGuest g;
        World w;
        Eye(g, kEye, 1);
        CoopEyeSwitch::Switcher s(w.Finder());
        s.Expire(kDeku);
        REQUIRE(s.Want(kLadder, kDeku));
        s.Step(g, false, 0, kMain, 100);
        s.Step(g, true, kBlock, kMain, 100);
        CHECK(s.HoldsGuard(cleared, kDeku));  // our own event runs: the other player's value must not land mid-event
        s.Expire(kDeku, [](const std::string&) { return false; });
        CHECK(s.HoldsGuard(cleared, kDeku));
        s.Expire(kDeku, [](const std::string& n) { return n == kLadder; });
        CHECK_FALSE(s.HoldsGuard(cleared, kDeku));
    }
    SUBCASE("our copy ran") {
        FakeGuest g;
        World w;
        Eye(g, kEye);
        CoopEyeSwitch::Switcher s(w.Finder());
        s.Expire(kDeku);
        REQUIRE(s.Want(kLadder, kDeku));
        s.Step(g, false, 0, kMain, 100);
        REQUIRE(s.Step(g, true, kBlock, kMain, 100));
        s.Step(g, true, 0, kMain, 100);
        EventRunning(g, true);
        s.Step(g, false, 0, kMain, 116);
        CHECK(s.TakeNotes().back().find("event started") != std::string::npos);
        // Held while our event runs (most guards delete the asker), until our game has it played.
        CHECK(s.HoldsGuard(cleared, kDeku));
        s.Expire(kDeku, nullptr);  // no flags read: still held
        CHECK(s.HoldsGuard(cleared, kDeku));
        s.Expire(kDeku, [](const std::string&) { return false; });
        CHECK(s.HoldsGuard(cleared, kDeku));
        s.Expire(kDeku, [](const std::string& n) { return n == kLadder; });
        CHECK_FALSE(s.HoldsGuard(cleared, kDeku));
        // A new visit after it played: the try may come again (the decider asks once a visit), so it waits again.
        s.Expire("KokiriForest");
        s.Expire(kDeku);
        CHECK(s.HoldsGuard(cleared, kDeku));
    }
    SUBCASE("ran, then we left before it was played: a new visit starts clean") {
        FakeGuest g;
        World w;
        Eye(g, kEye);
        CoopEyeSwitch::Switcher s(w.Finder());
        s.Expire(kDeku);
        REQUIRE(s.Want(kLadder, kDeku));
        s.Step(g, false, 0, kMain, 100);
        REQUIRE(s.Step(g, true, kBlock, kMain, 100));
        s.Step(g, true, 0, kMain, 100);
        EventRunning(g, true);
        s.Step(g, false, 0, kMain, 116);
        s.Expire("KokiriForest");
        CHECK_FALSE(s.HoldsGuard(cleared, "KokiriForest"));
    }
    SUBCASE("dropped: our game has it played") {
        World w;
        CoopEyeSwitch::Switcher s(w.Finder());
        s.Expire(kDeku);
        REQUIRE(s.Want(kLadder, kDeku));
        s.Expire(kDeku, [](const std::string&) { return true; });
        CHECK_FALSE(s.HoldsGuard(cleared, kDeku));
    }
}
