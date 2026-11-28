// SPDX-License-Identifier: GPL-3.0-or-later
// CoopEventResult (row 98(2), owner 07:40 UTC 2026-11-17): another player's room gimmick event gives our room its
// RESULT only (our asker's outputs, set with the game's set-bool), never a replay that binds our Link.
#include <doctest/doctest.h>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_event_result.h"
#include "coop_eye_switch.h"
#include "coop_live_events.h"
#include "coop_logic_probe.h"
#include "coop_logic_set.h"

namespace {
namespace p = CoopLogicProbe::addr;
namespace ls = CoopLogicSet::addr;

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
constexpr std::uint64_t kAsker = 0x20000;
constexpr std::uint64_t kBlock = 0x9000;
const std::string kLadder = "InsideTheDekuTree_Gimmick_FallingLadder";
const std::string kDeku = "InsideTheDekuTree";
const std::string kWater = "InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop";

// Our asker's live logic: two bool output plugs, EventBool (root) and EventFinish (its right child), both 0.
struct Asker {
    std::uint64_t base = kAsker + 0x1000;
    std::uint64_t ValueAt(int plug) const { return base + 0x5000 + 0x100 * static_cast<std::uint64_t>(plug); }
    void Build(FakeGuest& g) const {
        g.Put<std::uint64_t>(kAsker + p::kActorLogic, base);
        g.Put<std::uint64_t>(base + p::kLogicInner, base + 0x1000);
        g.Put<std::uint64_t>(base + 0x1000 + p::kLogicState, 5);
        g.Put<std::uint64_t>(base + 0x1000 + p::kLogicTable, base + 0x2000);
        g.Put<std::uint64_t>(base + 0x2000 + p::kTableRootHolder, base + 0x3000);
        g.Put<std::uint64_t>(base + 0x3000 + p::kRoot, base + 0x4000);
        const std::uint32_t keys[] = {coop::protocol::flag_hash("EventBool"), coop::protocol::flag_hash("EventFinish")};
        for (int i = 0; i < 2; ++i) {
            const std::uint64_t node = base + 0x4000 + 0x100 * static_cast<std::uint64_t>(i);
            g.Put<std::uint32_t>(node + p::kKey, keys[i] ^ 7u);
            g.Put<std::uint8_t>(node + p::kType, CoopLogicProbe::kBool);
            g.Put<std::uint64_t>(node + p::kValue, ValueAt(i) | 2);
            g.Put<std::uint8_t>(ValueAt(i), 0);
        }
        g.Put<std::uint64_t>(base + 0x4000 + p::kRight, base + 0x4100);
    }
    // The plug index of a key the set-bool node names (the name's key), or -1.
    static int PlugOf(std::uint32_t key) {
        if (key == coop::protocol::flag_hash("EventBool")) return 0;
        if (key == coop::protocol::flag_hash("EventFinish")) return 1;
        return -1;
    }
};

// The svc cave for one frame: Step, run each call it asks as the game would, give back the result, until it asks
// nothing more. The game's set-bool writes 1 into the plug the node names, unless `sticks` is false (the asker's own
// AI writes it back to 0 at once).
struct Cave {
    FakeGuest g;
    Asker asker;
    bool present = true;
    bool sticks = true;
    int executes = 0;
    int mallocs = 0;
    std::uint64_t malloc_size = 0;
    // Row 165: performers (name -> address) and the game's AS play: slot 0 then shows the name it was given, unless
    // `shows` is false (another AS took slot 0 at once).
    std::map<std::string, std::uint64_t, std::less<>> performers;
    bool shows = true;
    std::vector<std::pair<std::uint64_t, std::string>> plays;  // (actor, AS) per play request
    CoopEyeSwitch::Finder Finder() {
        return [this](CoopPuppet::Guest&, std::uint64_t, std::span<const std::string_view> names,
                      const std::array<float, 3>&, float) {
            const bool asked = names.front() == "EventRequestActor";
            if (const auto it = performers.find(names.front()); it != performers.end()) {
                return std::vector<std::uint64_t>{it->second};
            }
            return asked && present ? std::vector<std::uint64_t>{kAsker} : std::vector<std::uint64_t>{};
        };
    }
    // A performer at `actor` with an AS controller whose slot 0 plays `name` (kept in its own cell here).
    void Performer(const std::string& name, std::uint64_t actor) {
        namespace pa = CoopPuppet::addr;
        performers[name] = actor;
        std::uint64_t at = actor;
        for (std::size_t i = 0; i < pa::kControllerPath.size(); ++i) {
            const std::uint64_t next = actor + 0x1000 * (i + 1);
            g.Put<std::uint64_t>(at + pa::kControllerPath[i], next);
            at = next;
        }
        g.Put<std::uint64_t>(at + pa::kSlots, actor + 0x6000);
        g.Put<std::uint64_t>(actor + 0x6000, actor + 0x7000);  // slot 0
        g.Put<std::uint64_t>(actor + 0x7000 + pa::kSlotName, actor + 0x8000);
        const char wait[] = "Wait";
        g.Write(actor + 0x8000, wait, sizeof wait);
    }
    std::string Text(std::uint64_t at) {
        std::string out;
        for (char c = g.Get<char>(at); c != 0 && out.size() < 80; c = g.Get<char>(at + out.size())) out += c;
        return out;
    }
    void Frame(CoopEventResult::Setter& s, std::uint64_t now_ms) {
        auto call = s.Step(g, false, 0, kMain, now_ms);
        for (int guard = 0; call && guard < 8; ++guard) {
            std::uint64_t result = 0;
            if (call->function == kMain + ls::kMalloc) {
                ++mallocs;
                malloc_size = call->x[0];
                result = kBlock;
            } else if (call->function == kMain + CoopPuppet::addr::kPlayAs) {
                const auto actor = g.Get<std::uint64_t>(call->x[0]);
                const auto name = g.Get<std::uint64_t>(call->x[0] + 8);
                plays.emplace_back(actor, Text(name));
                CHECK(name >= kBlock);
                CHECK(name + CoopEventResult::kPlayNameCell <= kBlock + malloc_size);  // in our block
                if (shows) g.Put<std::uint64_t>(actor + 0x7000 + CoopPuppet::addr::kSlotName, name);
            } else if (call->function == kMain + ls::kExecute) {
                ++executes;
                const int plug = Asker::PlugOf(g.Get<std::uint32_t>(kBlock + ls::kNodeKey));
                if (plug >= 0 && sticks) g.Put<std::uint8_t>(asker.ValueAt(plug), 1);
            }
            call = s.Step(g, true, result, kMain, now_ms);
        }
    }
    std::uint8_t Value(int plug) { return g.Get<std::uint8_t>(asker.ValueAt(plug)); }
};

bool Noted(const std::vector<std::string>& notes, const std::string& part) {
    for (const auto& n : notes) {
        if (n.find(part) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("event result: the classes from the room graphs") {
    CHECK(CoopEventResult::IsLive(kWater));  // the basement water: its flags are its result (player test 6)
    CHECK(CoopEventResult::IsLive("InsideTheDekuTree_Gimmick_RoomH_CandleStick"));
    CHECK_FALSE(CoopEventResult::IsLive(kLadder));  // the ladder hangs on our asker's EventBool
    CHECK_FALSE(CoopEventResult::IsLive("NotAnEvent"));  // unknown: not live (held as before)
    const auto lines = CoopEventResult::AskerLines(kLadder, kDeku);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front()->outputs == "EventBool+EventFinish");
    CHECK(CoopEventResult::AskerLines(kWater, kDeku).empty());
    CHECK(CoopEventResult::AskerLines(kLadder, "ForestTemple").empty());
    for (const auto& r : CoopEventResult::Results()) {
        CAPTURE(r.event);
        CHECK_FALSE(r.event.starts_with("HavePlayedEvent."));
        CHECK_FALSE(r.scene.empty());
    }
}

TEST_CASE("event result: the outputs split in order") {
    const auto two = CoopEventResult::SplitOutputs("EventBool+EventFinish");
    REQUIRE(two.size() == 2);
    CHECK(two[0] == "EventBool");
    CHECK(two[1] == "EventFinish");
    CHECK(CoopEventResult::SplitOutputs("EventBool").size() == 1);
    CHECK(CoopEventResult::SplitOutputs("").empty());
}

TEST_CASE("event result: the hold: a result guard waits until our result is set or we played it") {
    const auto cleared = coop::protocol::flag_hash("InsideTheDekuTree_RoomC_Cleared");
    const auto played_ladder = coop::protocol::flag_hash("HavePlayedEvent." + kLadder);
    const auto none = [](const std::string&) { return false; };

    auto h = CoopEventResult::HoldFor(kDeku, {}, none);
    CHECK(h.Waits(cleared));  // it would delete our asker before our result is set
    CHECK_FALSE(h.Releases(played_ladder));

    h = CoopEventResult::HoldFor(kDeku, {kLadder}, none);
    CHECK(h.Releases(played_ladder));  // our result is set: the played flag lands in the room
    CHECK(h.Releases(cleared));
    CHECK_FALSE(h.Waits(cleared));

    h = CoopEventResult::HoldFor(kDeku, {}, [&](const std::string& e) { return e == kLadder; });
    CHECK_FALSE(h.Waits(cleared));  // our game played it: our asker has run

    h = CoopEventResult::HoldFor("KokiriForest", {}, none);
    CHECK_FALSE(h.Waits(cleared));  // only in its scene
}

TEST_CASE("event result: the eye switch path's guards wait only for a class-asker event") {
    const auto none = [](const std::string&) { return false; };
    // GTG room 03's step switch (the trigger reads it negated): its shutter event hangs on our asker.
    const auto gtg = coop::protocol::flag_hash("GerudoTrainingGround_Dungeon_Room03_StepSwitch");
    REQUIRE_FALSE(CoopEventResult::AskerLines("GerudoTrainingGround_03_ShutterOpen", "GerudoTrainingGround").empty());
    CHECK(CoopEventResult::HoldFor("GerudoTrainingGround", {}, none).Waits(gtg));
    CHECK(CoopEventResult::HoldFor("GerudoTrainingGround", {"GerudoTrainingGround_03_ShutterOpen"}, none).Releases(gtg));
    // Forest Temple room 02's gold eye: its shutter event is class live, its guard no longer waits (it waited for the
    // retired replay).
    const auto gold = coop::protocol::flag_hash("ForestTemple_Room02_GoldEyeSwitch_On");
    REQUIRE(CoopEventResult::IsLive("ForestTemple_Room02_ShutterOpen"));
    CHECK_FALSE(CoopEventResult::HoldFor("ForestTemple", {}, none).Waits(gold));
}

TEST_CASE("event result: a class-live event has nothing to set") {
    Cave cave;
    CoopEventResult::Setter s(cave.Finder());
    CHECK_FALSE(s.Want(kWater, kDeku));
    cave.Frame(s, 100);
    CHECK(cave.mallocs == 0);  // nothing asked of the game
}

namespace {
const std::string kStairs = "Dm_DodongoCave_0010";
const std::string kDodongo = "DodongoCavern";
const std::string kStair = "Obj_DodongoCave_BrokenStair_A_01";
const std::string kStairAs = "Obj_DodongoCave_BrokenStair_A_01_Demo_Anim";
constexpr std::uint64_t kStairActor = 0x100000;
}  // namespace

TEST_CASE("event result: row 165: an event whose performer's AS is its result is class asker") {
    // Run hs-dc-break: the played flag landing changed nothing in a loaded room (the room swaps the stair only at its
    // load); the event's own EventPlayAS on the stair is the live result.
    CHECK_FALSE(CoopEventResult::IsLive(kStairs));
    REQUIRE(CoopEventResult::AskerLines(kStairs, kDodongo).size() == 1);
    const auto plays = CoopEventResult::PlaysOf(kStairs, kDodongo);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0]->actor == kStair);
    CHECK(plays[0]->as == kStairAs);
    // The same pattern elsewhere: the Forest Temple's box puzzle boxes, Ganon's Castle's lift rock.
    CHECK(CoopEventResult::PlaysOf("Dm_ForestTemple_0014", "ForestTemple").size() == 4);
    CHECK(CoopEventResult::PlaysOf("Dm_InsideGanonCastle_0035", "InsideGanonCastle").size() == 1);
    CHECK_FALSE(CoopEventResult::IsLive("Dm_ForestTemple_0014"));
    CHECK_FALSE(CoopEventResult::IsLive("Dm_InsideGanonCastle_0035"));
    // The Fire Temple's hammer stair is a door whose isOpen reads the shared flag live: no play, still live.
    CHECK(CoopEventResult::PlaysOf("Dm_FireTemple_0029", "FireTemple").empty());
    CHECK(CoopEventResult::IsLive("Dm_FireTemple_0029"));
    // Its played flag waits in its scene until our result is set (a result guard).
    const auto hold = CoopEventResult::HoldFor(kDodongo, {}, [](const std::string&) { return false; });
    CHECK(hold.Waits(coop::protocol::flag_hash("HavePlayedEvent." + kStairs)));
    const auto done = CoopEventResult::HoldFor(kDodongo, {kStairs}, [](const std::string&) { return false; });
    CHECK(done.Releases(coop::protocol::flag_hash("HavePlayedEvent." + kStairs)));
}

TEST_CASE("event result: row 165: our stair plays the event's AS once, then the result counts as set") {
    Cave cave;
    cave.Performer(kStair, kStairActor);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDodongo);
    REQUIRE(s.Want(kStairs, kDodongo));
    std::uint64_t now = 1000;
    for (int f = 0; f < 20 && !s.Applied().contains(kStairs); ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kStairs));
    REQUIRE(cave.plays.size() == 1);  // once: slot 0 showed it
    CHECK(cave.plays[0].first == kStairActor);
    CHECK(cave.plays[0].second == kStairAs);
    CHECK(cave.executes == 0);  // no asker output to set
    CHECK(cave.malloc_size >= CoopLogicSet::addr::kBlockSize + 0x30 + CoopEventResult::kPlayNameCell);
    const auto notes = s.TakeNotes();
    CHECK(Noted(notes, "our " + kStair + " plays " + kStairAs));
    CHECK(Noted(notes, "set (its performers' AS)"));
    // Asked again this visit: not played again.
    CHECK(s.Want(kStairs, kDodongo));
    cave.Frame(s, now += 16);
    CHECK(cave.plays.size() == 1);
}

namespace {
// Row 335: the room graph's fixed node output our stair's Logic_Delete input reads (one byte, the node's own memory).
constexpr std::uint64_t kLatchByte = 0x300000;

// Gives `actor` (a Cave performer) a live logic with one input plug, Logic_Delete, whose value address is `byte`.
void DeleteInput(FakeGuest& g, std::uint64_t actor, std::uint64_t byte) {
    const std::uint64_t holder = g.Get<std::uint64_t>(actor + p::kActorLogic);  // also its controller path's first hop
    const std::uint64_t base = actor + 0x10000;
    g.Put<std::uint64_t>(holder + p::kLogicInner, base);
    g.Put<std::uint64_t>(base + p::kLogicState, 5);
    g.Put<std::uint64_t>(base + p::kLogicTable, base + 0x1000);
    g.Put<std::uint64_t>(base + 0x1000 + p::kTableRootHolder, base + 0x2000);
    g.Put<std::uint64_t>(base + 0x2000 + p::kRoot, base + 0x3000);
    const std::uint64_t node = base + 0x3000;
    g.Put<std::uint32_t>(node + p::kKey, coop::protocol::flag_hash("Logic_Delete") ^ 7u);
    g.Put<std::uint8_t>(node + p::kType, CoopLogicSet::kInputType);
    g.Put<std::uint64_t>(node + p::kValue, byte);
    g.Put<std::uint8_t>(byte, 0);
}

// Runs frames from `now` (16 ms apart) until `until`.
void RunUntil(Cave& cave, CoopEventResult::Setter& s, std::uint64_t& now, std::uint64_t until) {
    while (now < until) cave.Frame(s, now += 16);
}
}  // namespace

TEST_CASE("event result: row 335: once its AS is done our stair gets its room graph's own delete (the swap)") {
    Cave cave;
    cave.Performer(kStair, kStairActor);
    DeleteInput(cave.g, kStairActor, kLatchByte);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDodongo);
    REQUIRE(s.Want(kStairs, kDodongo));
    std::uint64_t now = 1000;
    for (int f = 0; f < 20 && !s.Applied().contains(kStairs); ++f) cave.Frame(s, now += 16);
    REQUIRE(s.Applied().contains(kStairs));
    const std::uint64_t shown = now;
    // Its AS holds slot 0: no swap before kSwapAfterMs (the partner sees the stair go down first).
    RunUntil(cave, s, now, shown + CoopEventResult::kSwapAfterMs - 100);
    CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 0);
    RunUntil(cave, s, now, shown + CoopEventResult::kSwapAfterMs + 100);
    CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 1);
    CHECK(cave.plays.size() == 1);  // no replay, no other game call
    CHECK(cave.executes == 0);
    CHECK(Noted(s.TakeNotes(), "its room graph's Logic_Delete source 0x300000 set 1"));
    // The game deletes it (and makes the down stair): noted once, nothing more written.
    cave.performers.erase(kStair);
    cave.Frame(s, now += 16);
    CHECK(Noted(s.TakeNotes(), "is gone after our swap: its room graph made its end actor"));
    cave.g.Put<std::uint8_t>(kLatchByte, 0);
    RunUntil(cave, s, now, now + 1000);
    CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 0);
}

TEST_CASE("event result: row 335: the swap comes as soon as slot 0 leaves the AS") {
    Cave cave;
    cave.Performer(kStair, kStairActor);
    DeleteInput(cave.g, kStairActor, kLatchByte);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDodongo);
    REQUIRE(s.Want(kStairs, kDodongo));
    std::uint64_t now = 1000;
    for (int f = 0; f < 20 && !s.Applied().contains(kStairs); ++f) cave.Frame(s, now += 16);
    REQUIRE(s.Applied().contains(kStairs));
    cave.Frame(s, now += 16);
    CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 0);
    cave.g.Put<std::uint64_t>(kStairActor + 0x7000 + CoopPuppet::addr::kSlotName, kStairActor + 0x8000);  // "Wait"
    cave.Frame(s, now += 16);
    CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 1);
}

TEST_CASE("event result: row 335: no swap through a constant, without a logic, or after the room loads again") {
    {
        Cave cave;  // its Logic_Delete reads the game's static image (a constant): never written
        cave.Performer(kStair, kStairActor);
        const std::uint64_t constant = kMain + 0x4000000;
        DeleteInput(cave.g, kStairActor, constant);
        CoopEventResult::Setter s(cave.Finder());
        s.Visit(kDodongo);
        REQUIRE(s.Want(kStairs, kDodongo));
        std::uint64_t now = 1000;
        RunUntil(cave, s, now, now + CoopEventResult::kSwapAfterMs + 500);
        CHECK(cave.g.Get<std::uint8_t>(constant) == 0);
        CHECK(Noted(s.TakeNotes(), "static image"));
    }
    {
        Cave cave;  // a change-marked value word (bit 1): not ours to store into
        cave.Performer(kStair, kStairActor);
        DeleteInput(cave.g, kStairActor, kLatchByte | 2);
        CoopEventResult::Setter s(cave.Finder());
        s.Visit(kDodongo);
        REQUIRE(s.Want(kStairs, kDodongo));
        std::uint64_t now = 1000;
        RunUntil(cave, s, now, now + CoopEventResult::kSwapAfterMs + 500);
        CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 0);
        CHECK(Noted(s.TakeNotes(), "not swapped"));
    }
    {
        Cave cave;  // no logic read: given up after the wait, nothing written
        cave.Performer(kStair, kStairActor);
        CoopEventResult::Setter s(cave.Finder());
        s.Visit(kDodongo);
        REQUIRE(s.Want(kStairs, kDodongo));
        std::uint64_t now = 1000;
        RunUntil(cave, s, now, now + CoopEventResult::kSwapAfterMs + CoopEventResult::kSwapWaitMs + 500);
        CHECK(Noted(s.TakeNotes(), "has no Logic_Delete input we can read"));
    }
    {
        Cave cave;  // the room loads again before the swap: its load makes the end state, we write nothing
        cave.Performer(kStair, kStairActor);
        DeleteInput(cave.g, kStairActor, kLatchByte);
        CoopEventResult::Setter s(cave.Finder());
        s.Visit(kDodongo);
        REQUIRE(s.Want(kStairs, kDodongo));
        std::uint64_t now = 1000;
        for (int f = 0; f < 20 && !s.Applied().contains(kStairs); ++f) cave.Frame(s, now += 16);
        s.Visit("DodongoCavernBoss");
        s.Visit(kDodongo);
        RunUntil(cave, s, now, now + CoopEventResult::kSwapAfterMs + 500);
        CHECK(cave.g.Get<std::uint8_t>(kLatchByte) == 0);
    }
}

TEST_CASE("event result: row 335: SwapByteNot") {
    CoopLogicProbe::Plug plug;
    plug.value = 0;
    plug.value_word = kLatchByte;
    CHECK(CoopEventResult::SwapByteNot(plug, kMain) == nullptr);
    plug.value_word = kLatchByte | 2;
    CHECK(CoopEventResult::SwapByteNot(plug, kMain) != nullptr);
    plug.value_word = 0;
    CHECK(CoopEventResult::SwapByteNot(plug, kMain) != nullptr);
    plug.value_word = kMain + 0x100;
    CHECK(CoopEventResult::SwapByteNot(plug, kMain) != nullptr);
    plug.value_word = kLatchByte;
    plug.value.reset();
    CHECK(CoopEventResult::SwapByteNot(plug, kMain) != nullptr);
}

TEST_CASE("event result: row 165: a performer not at its place is skipped, one that never shows is played twice") {
    {
        Cave cave;  // no stair here (our room shows its end already)
        CoopEventResult::Setter s(cave.Finder());
        s.Visit(kDodongo);
        REQUIRE(s.Want(kStairs, kDodongo));
        std::uint64_t now = 1000;
        for (int f = 0; f < 10; ++f) cave.Frame(s, now += 16);
        CHECK(s.Applied().contains(kStairs));
        CHECK(cave.plays.empty());
        CHECK(Noted(s.TakeNotes(), "is not at its place: skipped"));
    }
    {
        Cave cave;
        cave.Performer(kStair, kStairActor);
        cave.shows = false;
        CoopEventResult::Setter s(cave.Finder());
        s.Visit(kDodongo);
        REQUIRE(s.Want(kStairs, kDodongo));
        std::uint64_t now = 1000;
        for (int f = 0; f < 20; ++f) cave.Frame(s, now += 16);
        CHECK(cave.plays.size() == static_cast<std::size_t>(CoopEventResult::kMaxPlayCalls));
        CHECK(s.Applied().contains(kStairs));
        CHECK(Noted(s.TakeNotes(), "did not show " + kStairAs));
    }
}

TEST_CASE("event result: our asker's EventBool then EventFinish, each read 1 for 3 frames, then set") {
    Cave cave;
    cave.asker.Build(cave.g);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 20 && !s.Applied().contains(kLadder); ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kLadder));
    CHECK(cave.mallocs == 1);  // one block, kept
    CHECK(cave.executes == 2);  // one set-bool per output
    CHECK(cave.Value(0) == 1);
    CHECK(cave.Value(1) == 1);
    const auto notes = s.TakeNotes();
    CHECK(Noted(notes, "no replay"));
    CHECK(Noted(notes, "set (EventBool+EventFinish)"));
    // Asked again this visit: nothing more is set.
    CHECK(s.Want(kLadder, kDeku));
    cave.Frame(s, now += 16);
    CHECK(cave.executes == 2);
}

TEST_CASE("event result: an output the asker writes back to 0 is not set, its flags stay held") {
    Cave cave;
    cave.asker.Build(cave.g);
    cave.sticks = false;
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 300; ++f) cave.Frame(s, now += 16);  // past kTryMs
    CHECK_FALSE(s.Applied().contains(kLadder));
    CHECK(Noted(s.TakeNotes(), "an output did not stay 1"));
    // Not tried again this visit; a new visit tries again.
    const int before = cave.executes;
    s.Want(kLadder, kDeku);
    cave.Frame(s, now += 16);
    CHECK(cave.executes == before);
    s.Visit("KokiriForest");
    s.Visit(kDeku);
    CHECK(s.Want(kLadder, kDeku));
    cave.Frame(s, now += 16);
    CHECK(cave.executes == before + 1);
}

TEST_CASE("event result: no asker here: nothing set, the flags stay held") {
    Cave cave;
    cave.present = false;
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 300; ++f) cave.Frame(s, now += 16);
    CHECK_FALSE(s.Applied().contains(kLadder));
    CHECK(cave.executes == 0);
    CHECK(Noted(s.TakeNotes(), "our asker not found"));
}

TEST_CASE("event result: our asker gone after our set (its graph took the result) counts as set") {
    Cave cave;
    cave.asker.Build(cave.g);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    cave.Frame(s, now += 16);  // malloc + the first set-bool
    REQUIRE(cave.executes == 1);
    cave.present = false;  // EventBool -> Logic_Delete: the room graph deleted our asker
    for (int f = 0; f < 5; ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kLadder));
    CHECK(Noted(s.TakeNotes(), "our asker is gone after our set"));
}

TEST_CASE("event result: nothing is set while an event of ours runs") {
    Cave cave;
    cave.asker.Build(cave.g);
    cave.g.Put<std::uint64_t>(kMain + CoopLiveEvents::addr::kEventStatePtr, 0x40000);
    cave.g.Put<std::uint64_t>(0x40000 + CoopLiveEvents::addr::kEventSession, 0x50000);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 10; ++f) cave.Frame(s, now += 16);
    CHECK(cave.executes == 0);
    cave.g.Put<std::uint64_t>(0x40000 + CoopLiveEvents::addr::kEventSession, 0);  // it ended
    for (int f = 0; f < 20; ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kLadder));
}

TEST_CASE("event result: a wanted event is dropped once our game played it or we left its scene") {
    Cave cave;
    cave.asker.Build(cave.g);
    cave.present = false;  // not loaded yet: it waits
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    cave.Frame(s, now += 16);
    s.Visit(kDeku, [&](const std::string& e) { return e == kLadder; });  // our own asker ran it
    cave.present = true;
    for (int f = 0; f < 5; ++f) cave.Frame(s, now += 16);
    CHECK(cave.executes == 0);
    CHECK_FALSE(s.Applied().contains(kLadder));
}

namespace {
constexpr std::uint64_t kGtg02Counter = 0xb1f683aa91daf209ULL;
const std::vector<std::uint64_t> kGtg02Tokens = {0x0e741bc26e2c0697ULL, 0x1da71f60ac02ec12ULL, 0x4ab44ec9fa62841eULL,
                                                 0x77f9feaac7f28534ULL, 0xe09a166c192d8995ULL};
const std::vector<std::uint64_t> kFire15Slugs = {0x524d8f690c11944aULL, 0x7955892af6fcf0a2ULL, 0x8515bd766b65e010ULL,
                                                 0xa36a08d01e93953cULL, 0xf8119818b5e22a95ULL, 0xfc367bdaeb7524efULL};
}  // namespace

TEST_CASE("event result: the trigger table covers silver rupees, kills and shared flags (both classes)") {
    std::set<std::uint64_t> tokens;
    bool counter = false, flag = false, live_kill = false, jabu14 = false;
    for (const auto& t : CoopEventResult::Triggers()) {
        if (t.event == "GerudoTrainingGround_02_SilverToken_Unlock") {
            if (t.kind == CoopEventResult::Trigger::key) counter = counter || t.id == kGtg02Counter;
            if (t.kind == CoopEventResult::Trigger::token && t.group == kGtg02Counter) tokens.insert(t.id);
        }
        if (t.kind == CoopEventResult::Trigger::flag && t.name == "ForestTemple_DefeatAmy") flag = true;
        // g3-objects shares Jabu room 14's water flags: the upper switch's trigger reads one, so it waits too
        if (t.kind == CoopEventResult::Trigger::flag && t.name == "Gimmick_Insidejabujabu_Updown_MoveWater" &&
            t.event == "InsideJabuJabuBellyRoom14StepSwitch")
            jabu14 = true;
        // a class-live event's kills too (its played flag lands live, but our asker asks at the kill)
        if (t.kind == CoopEventResult::Trigger::kill && t.event == "GerudoTrainingGround_10_AppearTreasureBox") live_kill = true;
    }
    CHECK(counter);
    CHECK(tokens == std::set<std::uint64_t>(kGtg02Tokens.begin(), kGtg02Tokens.end()));
    CHECK(flag);
    CHECK(jabu14);
    CHECK(live_kill);
    CHECK(CoopEventResult::IsLive("GerudoTrainingGround_10_AppearTreasureBox"));
    // the silver rupee line is class asker: its result is set, and its trigger is covered too
    CHECK_FALSE(CoopEventResult::AskerLines("GerudoTrainingGround_02_SilverToken_Unlock", "GerudoTrainingGround").empty());
}

TEST_CASE("event result: shared silver rupees never complete our counter before our game played the event") {
    std::set<std::uint64_t> have;
    bool played = false;
    const auto is_played = [&](const std::string& e) {
        return played && e == "GerudoTrainingGround_02_SilverToken_Unlock";
    };
    const auto have_key = [&](std::uint64_t k) { return have.contains(k); };
    const auto hold = [&] { return CoopEventResult::HoldFor("GerudoTrainingGround", {}, is_played, have_key); };
    // nothing taken here: the counter's key and only the largest token wait; the other four land (a split collection)
    auto h = hold();
    CHECK(h.WaitsKey(kGtg02Counter));
    CHECK(h.WaitsKey(0xe09a166c192d8995ULL));
    for (std::size_t i = 0; i + 1 < kGtg02Tokens.size(); ++i) CHECK_FALSE(h.WaitsKey(kGtg02Tokens[i]));
    // the other player took four (they landed), ours never took the largest: it still waits, the counter is one short
    have.insert(kGtg02Tokens.begin(), kGtg02Tokens.end() - 1);
    CHECK(hold().WaitsKey(0xe09a166c192d8995ULL));
    // the largest landed first (our Link took it: our own key), then the last one missing waits instead
    have = {0xe09a166c192d8995ULL, 0x0e741bc26e2c0697ULL, 0x1da71f60ac02ec12ULL, 0x4ab44ec9fa62841eULL};
    h = hold();
    CHECK(h.WaitsKey(0x77f9feaac7f28534ULL));
    CHECK_FALSE(h.WaitsKey(0x0e741bc26e2c0697ULL));
    // our game played it (the played flag landed: our asker is disarmed): everything lands
    played = true;
    h = hold();
    CHECK_FALSE(h.WaitsKey(kGtg02Counter));
    CHECK_FALSE(h.WaitsKey(0x77f9feaac7f28534ULL));
    // another scene: nothing of it waits
    played = false;
    CHECK_FALSE(CoopEventResult::HoldFor("IceCavern", {}, is_played, have_key).WaitsKey(kGtg02Counter));
}

TEST_CASE("event result: a shared trigger flag waits until our game played the event, also once our result is set") {
    const auto amy = coop::protocol::flag_hash("ForestTemple_DefeatAmy");
    bool played = false;
    const auto is_played = [&](const std::string& e) {
        return played && e == "ForestTemple_Room25_CandleLight_ShutterOpen";
    };
    CHECK(CoopEventResult::HoldFor("ForestTemple", {}, is_played).Waits(amy));
    CHECK_FALSE(CoopEventResult::HoldFor("DodongoCavern", {}, is_played).Waits(amy));
    played = true;
    CHECK_FALSE(CoopEventResult::HoldFor("ForestTemple", {}, is_played).Waits(amy));
    // a class-asker event's counter: our result set this visit but the played flag not landed yet: still waits
    const auto h = CoopEventResult::HoldFor("GerudoTrainingGround", {"GerudoTrainingGround_02_SilverToken_Unlock"},
                                            [](const std::string&) { return false; });
    CHECK(h.Releases(coop::protocol::flag_hash("HavePlayedEvent.GerudoTrainingGround_02_SilverToken_Unlock")));
    CHECK(h.WaitsKey(kGtg02Counter));
}

TEST_CASE("event result: the kill keeper: the largest live enemy of an unplayed event waits, the others die") {
    std::set<std::uint64_t> alive(kFire15Slugs.begin(), kFire15Slugs.end());
    bool played = false;
    const auto is_played = [&](const std::string& e) { return played && e == "FireTemple_15_OpenShutter"; };
    const auto keepers = [&] {
        return CoopEventResult::KillKeepers("FireTemple", [&](std::uint64_t h) { return alive.contains(h); }, is_played);
    };
    auto k = keepers();
    CHECK(k.contains(0xfc367bdaeb7524efULL));
    for (std::size_t i = 0; i + 1 < kFire15Slugs.size(); ++i) CHECK_FALSE(k.contains(kFire15Slugs[i]));
    // our Link killed the largest itself: the next largest live one keeps the event
    alive.erase(0xfc367bdaeb7524efULL);
    k = keepers();
    CHECK(k.contains(0xf8119818b5e22a95ULL));
    CHECK_FALSE(k.contains(0xfc367bdaeb7524efULL));
    // played here: nothing is kept; another scene: nothing either
    played = true;
    CHECK(keepers().empty());
    played = false;
    CHECK(CoopEventResult::KillKeepers("WaterTemple", [&](std::uint64_t h) { return alive.contains(h); }, is_played)
              .count(0xf8119818b5e22a95ULL) == 0);
}

TEST_CASE("event result: the kill keeper: an enemy our game owns is never kept, and no copy while one of ours lives") {
    // Rows 160/161 (E'tykalu's 0.11.6 logs): Dodongo's Cavern's second Lizalfos room, the two Lizalfos owned by two
    // games (the host 0x2345..., player 2 0x155c...). Before, each game kept the other owner's kill: nobody played.
    constexpr std::uint64_t kA = 0x23456e4b5b005315ULL;  // the larger: owned by the host
    constexpr std::uint64_t kB = 0x155ce1b2ba5028cfULL;  // owned by player 2
    std::set<std::uint64_t> alive{kA, kB};
    std::set<std::uint64_t> mine;
    bool played = false;
    const auto keepers = [&] {
        return CoopEventResult::KillKeepers(
            "DodongoCavern", [&](std::uint64_t h) { return alive.contains(h); },
            [&](const std::string& e) { return played && e == "DodongoCavern_Gimmick_DefeatLizalfos02"; },
            [&](std::uint64_t h) { return mine.contains(h); });
    };
    // Player 2's game: B is ours, A a copy. The host's kill of A lands (B, ours, still lives).
    mine = {kB};
    CHECK(keepers().empty());
    alive.erase(kA);
    CHECK(keepers().empty());  // B's death (a remote hit too) is our own game's: never kept; our asker plays it
    // The host's game: A dead (ours), B a copy and the last one: its kill waits until the event's flags come.
    mine = {kA};
    CHECK(keepers() == std::set<std::uint64_t>{kB});
    played = true;
    CHECK(keepers().empty());
    // A game that owns neither (a third player): the largest live copy waits, as before.
    played = false;
    mine.clear();
    alive = {kA, kB};
    CHECK(keepers() == std::set<std::uint64_t>{kA});
    // Both ours (the owner of the whole fight): nothing is kept, a teammate's last hit lands here.
    mine = {kA, kB};
    CHECK(keepers().empty());
    // No owner function (as before): the largest live one.
    CHECK(CoopEventResult::KillKeepers("DodongoCavern", [&](std::uint64_t h) { return alive.contains(h); },
                                       [](const std::string&) { return false; }) == std::set<std::uint64_t>{kA});
}

TEST_CASE("event result: crossing last kills: only the lowest-numbered owner lets the other's kill land, after a wait") {
    // review-dc-0118: P1 owns A, P2 owns B; each kills its own within one round trip, so each keeps the other's kill.
    constexpr std::uint64_t kA = 0x23456e4b5b005315ULL;
    constexpr std::uint64_t kB = 0x155ce1b2ba5028cfULL;
    constexpr auto kLong = CoopEventResult::kCrossingUs;
    const auto died = [](std::uint64_t own) { return [own](std::uint64_t h) { return h == own; }; };
    // P1's game keeps B's kill (B owned by 2), its own A died: lands once kept long enough.
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong - 1, 1, 2, died(kA)));
    CHECK(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 1, 2, died(kA)));
    // P2's game keeps A's kill (A owned by 1): never by this rule; P1's flags end it.
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kA, kLong * 4, 2, 1, died(kB)));
    // A third player who owned neither (or whose own died long before: the caller's gap check says false): waits.
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong * 4, 1, 2,
                                                   [](std::uint64_t) { return false; }));
    // Our own "died" enemy must be another kill actor of the same event, in the same scene.
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 1, 2, died(kB)));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 1, 2, died(0x9cbd9c524b24df87ULL)));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("FireTemple", kB, kLong, 1, 2, died(kA)));
    // Unknown numbers or owner: no tiebreak.
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 0, 2, died(kA)));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 1, std::nullopt, died(kA)));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 1, 0, died(kA)));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, kLong, 2, 2, died(kA)));
    // The first room's pair goes the same way (every kill line shares the rule).
    CHECK(CoopEventResult::CrossingKillLands("DodongoCavern", 0x9cbd9c524b24df87ULL, kLong, 1, 3,
                                             died(0x01fa0074fddbc182ULL)));
    // review-dc-0118 MUST 2: the lowest owner (1) left mid-wait. Player 2 keeps 1's kill of A: with 1 gone from our
    // scene, the scene's decider (now 2) lets it land; a third player who does not decide still waits.
    const auto none = [](std::uint64_t) { return false; };
    CHECK(CoopEventResult::CrossingKillLands("DodongoCavern", kA, kLong, 2, 1, died(kB), false, true));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kA, kLong - 1, 2, 1, died(kB), false, true));
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kA, kLong, 3, 1, none, false, false));
    CHECK(CoopEventResult::CrossingKillLands("DodongoCavern", kA, kLong, 3, std::nullopt, none, false, true));
    // ... but only for a kill line of our scene.
    CHECK_FALSE(CoopEventResult::CrossingKillLands("FireTemple", kA, kLong, 2, 1, died(kB), false, true));
}

TEST_CASE("event result: the follow keepers: blocks on press switches wait, the box puzzle's largest corner waits") {
    std::set<std::string> played;
    const auto is_played = [&](const std::string& e) { return played.contains(e); };
    // Forest Temple Dm_ForestTemple_0014 (ForestTemple_MovableBox_Puzzle_Manager.CorrectPos): its four corners; only
    // the largest waits, so another game's pushes alone never complete our puzzle.
    constexpr std::array<std::uint64_t, 4> kCorners{0x1f0b64dac1bb1499ULL, 0x794f8e0396597b84ULL,
                                                    0x9340d4ba68f5feccULL, 0xebddc3cef6e36708ULL};
    auto k = CoopEventResult::FollowKeepers("ForestTemple", is_played);
    CHECK(k.contains(0xebddc3cef6e36708ULL));
    for (std::size_t i = 0; i + 1 < kCorners.size(); ++i) CHECK_FALSE(k.contains(kCorners[i]));
    // Dodongo's Cavern room 19: every block in the press switch's room waits (any of them can rest on it).
    constexpr std::array<std::uint64_t, 4> kRoom19{0x0f6a348ed7daaef4ULL, 0x244fd0ef8c5e5e01ULL,
                                                   0x641bc828409e0c24ULL, 0xfc81551012767634ULL};
    k = CoopEventResult::FollowKeepers("DodongoCavern", is_played);
    for (const auto h : kRoom19) CHECK(k.contains(h));
    CHECK(k.contains(0x8342f68da2a61311ULL));  // room 12's statue
    // A hammer switch (Spirit Temple room 8): the switch itself.
    CHECK(CoopEventResult::FollowKeepers("SpiritTemple", is_played).contains(0xcb61a9de736c9e1cULL));
    // Played here: that event's keepers go, the others stay; another scene: none of them.
    played.insert("DodongoCavern_Gimmick_SwitchShutter_Room19");
    k = CoopEventResult::FollowKeepers("DodongoCavern", is_played);
    for (const auto h : kRoom19) CHECK_FALSE(k.contains(h));
    CHECK(k.contains(0x8342f68da2a61311ULL));
    CHECK_FALSE(CoopEventResult::FollowKeepers("WaterTemple", is_played).contains(0xebddc3cef6e36708ULL));
    // The flag hold never takes a follow line (the world sync holds them).
    const auto h = CoopEventResult::HoldFor("ForestTemple", {}, is_played);
    for (const auto c : kCorners) CHECK_FALSE(h.WaitsKey(c));
    std::size_t follows = 0;
    for (const auto& t : CoopEventResult::Triggers()) follows += t.kind == CoopEventResult::Trigger::follow;
    CHECK(follows == 22);
}

TEST_CASE("event result: row 160: an owner downed, out, spectating, dead or loading cannot play the event") {
    namespace st = coop::protocol::status;
    CHECK(CoopEventResult::AbleToPlay(true, true, 0));
    CHECK(CoopEventResult::AbleToPlay(true, true, st::in_cutscene));  // the room event itself
    CHECK(CoopEventResult::AbleToPlay(true, true, st::paused | st::in_menu));
    CHECK_FALSE(CoopEventResult::AbleToPlay(false, true, 0));          // gone from our scene, as before
    CHECK_FALSE(CoopEventResult::AbleToPlay(true, false, 0));          // DBNO downed / spectating / out
    CHECK_FALSE(CoopEventResult::AbleToPlay(true, true, st::dead));
    CHECK_FALSE(CoopEventResult::AbleToPlay(true, true, st::loading)); // a void-out's respawn, a Try Again
    // Run 20261120-132403: B (2) owned and killed the last Lizalfos, then went down; A (1) kept its kill. With B
    // not able, A (the lowest able game) decides: the kept kill lands there after kCrossingUs.
    constexpr std::uint64_t kA = 0x23456e4b5b005315ULL;
    constexpr std::uint64_t kB = 0x155ce1b2ba5028cfULL;
    const auto none = [](std::uint64_t) { return false; };
    const std::array<std::uint8_t, 1> two{2};
    const auto able_but = [](std::uint8_t down) { return [down](std::uint8_t p) { return p != down; }; };
    CHECK(CoopEventResult::DeciderAmongAble(1, two, able_but(2)) == 1);
    CHECK(CoopEventResult::CrossingKillLands("DodongoCavern", kB, CoopEventResult::kCrossingUs, 1, 2, none,
                                             CoopEventResult::AbleToPlay(true, false, 0), true));
    // Player 1 is the one down: player 2 decides for the scene, though 1 is lower.
    CHECK(CoopEventResult::DeciderAmongAble(2, std::array<std::uint8_t, 1>{1}, able_but(1)) == 2);
    CHECK(CoopEventResult::DeciderAmongAble(1, two, able_but(1)) == 2);
    // Nobody able: nobody decides (the first game up again does).
    CHECK(CoopEventResult::DeciderAmongAble(1, two, [](std::uint8_t) { return false; }) == 0);
    // Everyone up: the lowest number, as Sync::DeciderOf.
    CHECK(CoopEventResult::DeciderAmongAble(3, std::array<std::uint8_t, 2>{2, 5}, [](std::uint8_t) { return true; }) == 2);
    CHECK(CoopEventResult::DeciderAmongAble(0, two, [](std::uint8_t) { return true; }) == 2);
    // An owner that is up and here still plays it: the other games only wait (no landing before its flags).
    CHECK_FALSE(CoopEventResult::CrossingKillLands("DodongoCavern", kB, CoopEventResult::kCrossingUs * 4, 1, 2, none,
                                                   CoopEventResult::AbleToPlay(true, true, 0), true));
    (void)kA;
}

TEST_CASE("event result: row 160: a whole room killed in this visit is killed again when a reload makes it") {
    constexpr std::uint64_t kA = 0x23456e4b5b005315ULL;
    constexpr std::uint64_t kB = 0x155ce1b2ba5028cfULL;
    const auto never = [](const std::string&) { return false; };
    CHECK(CoopEventResult::KillEventOf("DodongoCavern", kA) == "DodongoCavern_Gimmick_DefeatLizalfos02");
    CHECK_FALSE(CoopEventResult::KillEventOf("FireTemple", kA));
    CHECK_FALSE(CoopEventResult::KillEventOf("DodongoCavern", 0x1234ULL));
    // A's game: its own A seen dead, B's kill kept: both of the event are dead for the group.
    CHECK(CoopEventResult::KillsToRedo("DodongoCavern", {kA}, {kB}, never) == std::set<std::uint64_t>{kA, kB});
    // Only one killed: the re-made room is a native refight.
    CHECK(CoopEventResult::KillsToRedo("DodongoCavern", {kA}, {}, never).empty());
    CHECK(CoopEventResult::KillsToRedo("DodongoCavern", {}, {kB}, never).empty());
    // Co-op idle (C #49): a host in a session, alone in the room, killed both itself and reloads before its event
    // played (no other game's kill kept here): no redo, the game's own refight, as solo.
    CHECK(CoopEventResult::KillsToRedo("DodongoCavern", {kA, kB}, {}, never).empty());
    // The event played here: nothing to redo.
    CHECK(CoopEventResult::KillsToRedo("DodongoCavern", {kA}, {kB},
                                       [](const std::string& e) { return e == "DodongoCavern_Gimmick_DefeatLizalfos02"; })
              .empty());
    // Another scene's hashes do not count.
    CHECK(CoopEventResult::KillsToRedo("FireTemple", {kA}, {kB}, never).empty());
    // Every 'last enemy -> event' line (kind kill) of every room: all its actors dead with one kept -> all of them;
    // all dead by our own game only -> none; one missing -> none of that event's.
    std::map<std::pair<std::string_view, std::string_view>, std::set<std::uint64_t>> events;
    for (const auto& t : CoopEventResult::Triggers()) {
        if (t.kind == CoopEventResult::Trigger::kill) events[{t.scene, t.event}].insert(t.id);
    }
    CHECK(events.size() >= 44);  // 44 kill events in 11 dungeons (ff54edf6)
    for (const auto& [key, ids] : events) {
        CAPTURE(key.second);
        const std::set<std::uint64_t> one_kept{*ids.rbegin()};
        const auto redo = CoopEventResult::KillsToRedo(key.first, ids, one_kept, never);
        for (const auto id : ids) CHECK(redo.contains(id));
        const auto own_only = CoopEventResult::KillsToRedo(key.first, ids, {}, never);
        for (const auto id : ids) {
            bool shared = false;  // an id another event of the scene also names may come from that one
            for (const auto& [k2, ids2] : events) {
                if (k2 != key && k2.first == key.first && ids2.contains(id)) shared = true;
            }
            if (!shared) CHECK_FALSE(own_only.contains(id));
        }
        if (ids.size() > 1) {
            std::set<std::uint64_t> one_short = ids;
            const std::uint64_t missing = *one_short.begin();
            one_short.erase(one_short.begin());
            const auto partial = CoopEventResult::KillsToRedo(key.first, one_short, {*ids.rbegin()}, never);
            for (const auto id : ids) {
                // an id shared with another event of the scene may still come from that one
                bool shared = false;
                for (const auto& [k2, ids2] : events) {
                    if (k2 != key && k2.first == key.first && ids2.contains(id)) shared = true;
                }
                if (!shared) CHECK_FALSE(partial.contains(id));
            }
            (void)missing;
        }
    }
}

TEST_CASE("event result: row 160 (b): a kept kill's copy holds our key while kept, the key comes out before it lands") {
    CoopEventResult::KeptShield shield;
    using Step = CoopEventResult::KeptShield::Step;
    const std::set<std::uint64_t> live{0x100, 0x200};
    // Kept: our key goes in, one component at a time.
    auto next = shield.Next({0x100}, live);
    REQUIRE(next);
    CHECK(*next == Step{0x100, true});
    shield.Called(*next, true);
    CHECK(shield.On(0x100));
    CHECK_FALSE(shield.Next({0x100}, live));
    // Its kill is to land (no longer wanted): the key comes out first.
    next = shield.Next({}, live);
    REQUIRE(next);
    CHECK(*next == Step{0x100, false});
    shield.Called(*next, true);
    CHECK_FALSE(shield.On(0x100));
    // An actor that went (a reload): never called on, forgotten.
    shield.Called(Step{0x200, true}, true);
    CHECK(shield.On(0x200));
    CHECK_FALSE(shield.Next({}, {0x100}));
    CHECK_FALSE(shield.On(0x200));
    // Not in the walk: never asked.
    CHECK_FALSE(shield.Next({0x300}, live));
    // Failed calls are given up after kKeptShieldTries.
    for (unsigned i = 0; i < CoopEventResult::kKeptShieldTries; ++i) {
        next = shield.Next({0x100}, live);
        REQUIRE(next);
        shield.Called(*next, false);
    }
    CHECK(shield.GivenUp(Step{0x100, true}));
    CHECK_FALSE(shield.Next({0x100}, live));
    CHECK(CoopEventResult::kKeptShieldKey != 0xC0DB0D01u);  // not dbno's
    CHECK(CoopEventResult::kKeptShieldKey != 0xC0DB0D02u);  // not the PvP copies'
}
