// SPDX-License-Identifier: GPL-3.0-or-later
// CoopLiftRide (jabu-lifts, player report #65): another player on our lift sets its RideOnPlayer through the
// game's set-bool.
#include <doctest/doctest.h>

#include <map>

#include "coop_fairy.h"
#include "coop_lift_ride.h"
#include "coop_logic_probe.h"

namespace {
namespace a = CoopLiftRide::addr;
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
constexpr std::uint64_t kLift = 0x1000;
constexpr std::uint64_t kBlock = 0x9000;
constexpr std::uint64_t kValue = 0x7000;

// A present lift whose logic has one plug, RideOnPlayer (bool) at `value`.
void Lift(FakeGuest& g, std::uint32_t key = CoopLiftRide::kRideOnPlayer ^ 7u, std::uint8_t value = 0) {
    g.Put<std::uint32_t>(kLift + CoopFairy::addr::kActorState, CoopFairy::addr::kStatePresent);
    g.Put<std::uint64_t>(kLift + p::kActorLogic, 0x2000);
    g.Put<std::uint64_t>(0x2000 + p::kLogicInner, 0x3000);
    g.Put<std::uint64_t>(0x3000 + p::kLogicState, 5);
    g.Put<std::uint64_t>(0x3000 + p::kLogicTable, 0x4000);
    g.Put<std::uint64_t>(0x4000 + p::kTableRootHolder, 0x5000);
    g.Put<std::uint64_t>(0x5000 + p::kRoot, 0x6000);
    g.Put<std::uint32_t>(0x6000 + p::kKey, key);
    g.Put<std::uint8_t>(0x6000 + p::kType, CoopLogicProbe::kBool);
    g.Put<std::uint64_t>(0x6000 + p::kValue, kValue | 2);
    g.Put<std::uint8_t>(kValue, value);
}

}  // namespace

TEST_CASE("lift ride: which lifts ride, and who stands on them") {
    CHECK(CoopLiftRide::IsRideLift("Obj_InsideJabu_Jabu_ElevatorLift_A_01"));
    CHECK(CoopLiftRide::IsRideLift("Obj_InsideJabu_Jabu_MoveFloor_A_01"));
    CHECK(CoopLiftRide::IsRideLift("Obj_ForestTemple_Elevator_A_01"));  // room-objects: its logic has RideOnPlayer
    CHECK_FALSE(CoopLiftRide::IsRideLift("Obj_Deku_MoveFloor_A_01"));
    CHECK_FALSE(CoopLiftRide::IsRideLift("Obj_InsideTheDekuTree_ElevatorLift_A_02"));
    CHECK_FALSE(CoopLiftRide::IsRideLift("Npc_ElevatorLift_A_01"));

    const std::vector<CoopLiftRide::Lift> lifts{{0x10, "Obj_InsideJabu_Jabu_ElevatorLift_A_01", {-32.36f, 84.5f, -93.31f}},
                                                {0x20, "Obj_InsideJabu_Jabu_MoveFloor_A_01", {0.32f, 92.5f, -42.63f}}};
    CHECK(CoopLiftRide::Ridden(lifts, {}).empty());
    CHECK(CoopLiftRide::Ridden(lifts, {{-32.0f, 84.6f, -93.0f}}) == std::vector<std::uint64_t>{0x10});
    CHECK(CoopLiftRide::Ridden(lifts, {{-29.0f, 84.5f, -93.31f}}).empty());  // 3.4 m off: stepped off
    CHECK(CoopLiftRide::Ridden(lifts, {{-32.36f, 80.0f, -93.31f}}).empty());  // under it
    CHECK(CoopLiftRide::Ridden(lifts, {{-32.36f, 86.5f, -93.31f}, {0.5f, 92.5f, -42.0f}}) ==
          std::vector<std::uint64_t>{0x10, 0x20});
}

TEST_CASE("lift ride: one malloc, then the game's set-bool on our node block naming the lift") {
    FakeGuest g;
    Lift(g);
    CoopLiftRide::Rider r;
    CHECK_FALSE(r.Step(g, false, 0, 100));  // nothing wanted
    r.Want({kLift}, kMain, 100);
    auto c = r.Step(g, false, 0, 100);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kMalloc);
    CHECK(c->x[0] == a::kBlockSize);
    c = r.Step(g, true, kBlock, 100);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kExecute);
    CHECK(c->x[0] == kBlock);
    // What the execute reads.
    CHECK(g.Get<std::uint64_t>(kBlock) == kMain + a::kVtable);
    CHECK((g.Get<std::uint8_t>(kBlock + a::kNodeFlags) & 2) != 0);
    CHECK((g.Get<std::uint32_t>(kBlock + a::kNodeKey) ^ 7u) == g.Get<std::uint32_t>(0x6000 + p::kKey));
    const auto owner = g.Get<std::uint64_t>(kBlock + a::kNodeOwner);
    const auto holder = g.Get<std::uint64_t>(owner + 8);
    CHECK(g.Get<std::uint64_t>(holder + a::kHolderActor) == kLift);
    CHECK(g.Get<std::uint8_t>(g.Get<std::uint64_t>(kBlock + a::kNodeValue)) == 1);
    CHECK(holder + a::kHolderActor + 8 <= kBlock + a::kBlockSize);
    CHECK_FALSE(r.Step(g, true, 0, 100));  // one lift: done for this frame
    // The next frame: again (the lift's AI puts its own 0 back each frame), with no new malloc.
    c = r.Step(g, false, 0, 116);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kExecute);
    CHECK(r.TakeNotes().size() == 2);  // logged once (after the caller-runs note)
}

TEST_CASE("lift ride: not set when on already, gone, without the plug, or no longer ridden") {
    FakeGuest g;
    CoopLiftRide::Rider r;
    r.Want({kLift}, kMain, 100);
    REQUIRE(r.Step(g, false, 0, 100));  // the malloc
    CHECK_FALSE(r.Step(g, true, kBlock, 100));  // no lift there: not present
    Lift(g, CoopLiftRide::kRideOnPlayer ^ 7u, 1);
    CHECK_FALSE(r.Step(g, false, 0, 110));  // our Link is on it: 1 already
    Lift(g, 0xa19eba9c);  // only RideOnCharacter
    CHECK_FALSE(r.Step(g, false, 0, 120));
    Lift(g);
    g.Put<std::uint64_t>(kLift + p::kActorLogic, 0);  // no logic: the execute would read 0+0x118
    CHECK_FALSE(r.Step(g, false, 0, 130));
    Lift(g);
    CHECK(r.Step(g, false, 0, 140));
    CHECK_FALSE(r.Step(g, true, 0, 140));
    CHECK_FALSE(r.Step(g, false, 0, 100 + CoopLiftRide::kHoldMs + 1));  // the rider's samples left the lift
}

TEST_CASE("lift ride: a failed malloc turns it off") {
    FakeGuest g;
    Lift(g);
    CoopLiftRide::Rider r;
    r.Want({kLift}, kMain, 100);
    REQUIRE(r.Step(g, false, 0, 100));
    CHECK_FALSE(r.Step(g, true, 0, 100));
    r.Want({kLift}, kMain, 200);
    CHECK_FALSE(r.Step(g, false, 0, 200));
    CHECK(r.TakeNotes().size() == 2);
}

TEST_CASE("switch stand: another player on our step switch sets its contact bool, and leaving lets go") {
    CHECK(CoopLiftRide::IsStandSwitch("Obj_StepSwitch_A_01"));
    CHECK(CoopLiftRide::IsStandSwitch("Obj_DodongoCave_StepSwitch_A_02"));
    CHECK_FALSE(CoopLiftRide::IsStandSwitch("Obj_SwitchShutter_A_06"));
    CHECK_FALSE(CoopLiftRide::IsStandSwitch("Npc_StepSwitch_A_01"));
    // Every step switch actor of the game (romfs Pack/Actor, 2026-11-24): some are spelt "Stepswitch".
    for (const char* name : {"Obj_StepSwitch_A_01", "Obj_StepSwitch_A_02", "Obj_StepSwitch_A_04", "Obj_StepSwitch_A_05",
                             "Obj_Stepswitch_A_03", "Obj_Stepswitch_A_03_Amos", "Obj_WaterTemple_Stepswitch_A_03",
                             "Obj_InsideJabu_Jabu_Stepswitch_A_02", "Obj_InsideJabu_Jabu_StepSwitch_A_03",
                             "Obj_InsideJabu_Jabu_StepSwitch_A_04"}) {
        CAPTURE(name);
        CHECK(CoopLiftRide::IsStandSwitch(name));
    }
    CHECK_FALSE(CoopLiftRide::IsStandSwitch("SwitchStep"));  // no Obj_ prefix
    // Dodongo's held switch (coop_room_objects.cpp): a puppet on it, beside it, above it.
    const std::vector<CoopLiftRide::Lift> sw{{kLift, "Obj_StepSwitch_A_01", {-49.0f, 114.2f, -8.0f}}};
    CHECK(CoopLiftRide::Stood(sw, {{-49.3f, 114.4f, -7.6f}}) == std::vector<std::uint64_t>{kLift});
    CHECK(CoopLiftRide::Stood(sw, {{-49.0f, 114.6f, -3.0f}}).empty());  // 5 m off
    CHECK(CoopLiftRide::Stood(sw, {{-49.0f, 117.0f, -8.0f}}).empty());  // on the ledge above
    FakeGuest g;
    Lift(g, CoopLiftRide::kStepContact ^ 7u);
    CHECK(g.Get<std::uint32_t>(0x6000 + p::kKey) == 0x9cdaf491u);  // the probe's plug key
    CoopLiftRide::Rider r(CoopLiftRide::kStepContact, "switch stand");
    r.Want(CoopLiftRide::Stood(sw, {{-49.0f, 114.3f, -8.0f}}), kMain, 100);
    REQUIRE(r.Step(g, false, 0, 100));  // the malloc
    auto c = r.Step(g, true, kBlock, 100);
    REQUIRE(c);
    CHECK(c->function == kMain + a::kExecute);
    CHECK((g.Get<std::uint32_t>(kBlock + a::kNodeKey) ^ 7u) == 0x9cdaf491u);
    // The holder leaves the scene or the session: OthersHere no longer has them, nothing is wanted, and after the
    // hold nothing of ours sets the bool (the switch's AI puts its 0 back).
    r.Want(CoopLiftRide::Stood(sw, {}), kMain, 200);
    CHECK(r.Step(g, false, 0, 300));  // still within the hold
    CHECK_FALSE(r.Step(g, true, 0, 300));
    CHECK_FALSE(r.Step(g, false, 0, 100 + CoopLiftRide::kHoldMs + 1));
    CHECK(r.TakeNotes().front().starts_with("switch stand:"));
}

TEST_CASE("switch stand: the stander sets OnState (the contact bool reads 1 already in the other game), contact only on ask") {
    CHECK(CoopLiftRide::StandKey(nullptr) == CoopLiftRide::kStepOnState);
    CHECK(CoopLiftRide::StandKey("1") == CoopLiftRide::kStepOnState);
    CHECK(CoopLiftRide::StandKey("contact") == CoopLiftRide::kStepContact);
}
