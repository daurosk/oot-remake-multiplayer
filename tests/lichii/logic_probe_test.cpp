// SPDX-License-Identifier: GPL-3.0-or-later
// CoopLogicProbe (jabu-lifts): an actor's logic plugs, walked as the game's ExecuteLogicSetBool walks them.
#include <doctest/doctest.h>

#include <map>

#include "coop_logic_probe.h"

namespace {
namespace a = CoopLogicProbe::addr;

struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;
    CoopLogicProbe::ReadWord reader() const {
        return [this](std::uint64_t at) -> std::optional<std::uint64_t> {
            const auto it = words.find(at);
            if (it == words.end()) return std::nullopt;
            return it->second;
        };
    }
};

constexpr std::uint64_t kActor = 0x1000, kOuter = 0x2000, kLogic = 0x3000, kTable = 0x4000, kHolder = 0x5000;

// A live logic with a three-plug tree: root 0x20 (key 0x500), left 0x500 is at 0x6100 (key 0x100), right at
// 0x6200 (key 0x900). Values: bytes at 0x7000 + n.
Memory Lift() {
    Memory m;
    m.words[kActor + a::kActorLogic] = kOuter;
    m.words[kOuter + a::kLogicInner] = kLogic;
    m.words[kLogic + a::kLogicState] = 5;
    m.words[kLogic + a::kLogicFlags] = 0;
    m.words[kLogic + a::kLogicTable] = kTable;
    m.words[kTable + a::kTableRootHolder] = kHolder;
    m.words[kHolder + a::kRoot] = 0x6000;
    const auto plug = [&m](std::uint64_t at, std::uint32_t key, std::uint64_t left, std::uint64_t right,
                           std::uint64_t value_at, std::uint8_t value) {
        m.words[at + a::kKey] = key;
        m.words[at + a::kType] = CoopLogicProbe::kBool;
        m.words[at + a::kLeft] = left;
        m.words[at + a::kRight] = right;
        m.words[at + a::kValue] = value_at | 2;  // the low bits are flags
        m.words[value_at] = value;
    };
    plug(0x6000, 0x500, 0x6100, 0x6200, 0x7000, 0);
    plug(0x6100, 0x100, 0, 0, 0x7010, 1);
    plug(0x6200, 0x900, 0, 0, 0x7020, 0);
    return m;
}

}  // namespace

TEST_CASE("logic probe: an actor's plugs come in key order with their values") {
    const Memory m = Lift();
    const auto plugs = CoopLogicProbe::Read(m.reader(), kActor);
    REQUIRE(plugs);
    REQUIRE(plugs->size() == 3);
    CHECK((*plugs)[0].key == 0x100);
    CHECK((*plugs)[0].value == std::optional<std::uint8_t>(1));
    CHECK((*plugs)[1].key == 0x500);
    CHECK((*plugs)[2].key == 0x900);
    CHECK((*plugs)[2].type == CoopLogicProbe::kBool);
}

TEST_CASE("logic probe: a logic that is not live, or not there, gives nothing") {
    Memory m = Lift();
    m.words[kLogic + a::kLogicState] = ~std::uint64_t{0};
    CHECK_FALSE(CoopLogicProbe::Read(m.reader(), kActor));
    m = Lift();
    m.words[kLogic + a::kLogicFlags] = 1;
    CHECK_FALSE(CoopLogicProbe::Read(m.reader(), kActor));
    m = Lift();
    m.words.erase(kActor + a::kActorLogic);
    CHECK_FALSE(CoopLogicProbe::Read(m.reader(), kActor));
}

TEST_CASE("logic probe: a tree that loops back ends the walk") {
    Memory m = Lift();
    m.words[0x6100 + a::kLeft] = 0x6000;  // a bad read: back to the root
    const auto plugs = CoopLogicProbe::Read(m.reader(), kActor);
    REQUIRE(plugs);
    CHECK(plugs->size() < 3);
}

TEST_CASE("logic probe: the watch logs the plugs once, then each change") {
    Memory m = Lift();
    CoopLogicProbe::Watch w;
    const std::vector<std::pair<std::uint64_t, std::string>> lifts{{kActor, "Obj_InsideJabu_Jabu_ElevatorLift_A_01"}};
    auto lines = w.Look(m.reader(), lifts);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("00000100:7=1") != std::string::npos);
    CHECK(w.Look(m.reader(), lifts).empty());
    m.words[0x7000] = 1;  // RideOnCharacter-like: the root plug goes to 1
    lines = w.Look(m.reader(), lifts);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("plug 00000500 (type 7) 0 -> 1") != std::string::npos);
}
