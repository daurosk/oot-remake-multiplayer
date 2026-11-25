// SPDX-License-Identifier: GPL-3.0-or-later
// CoopMoveProbe (row 287): a boss's CCT move state, read as the game's QueryPhysicsCctGetCurrentMoveStateType reads it.
#include <doctest/doctest.h>

#include <map>
#include <string>

#include "coop_move_probe.h"

namespace {
namespace a = CoopMoveProbe::addr;

struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;
    CoopMoveProbe::ReadWord reader() const {
        return [this](std::uint64_t at) -> std::optional<std::uint64_t> {
            const auto it = words.find(at);
            if (it == words.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

constexpr std::uint64_t kActor = 0x1000, kTable = 0x2000, kPhysics = 0x3000, kInner = 0x4000, kHolder = 0x5000,
                        kCct = 0x6000, kMachine = 0x7000, kState = 0x8000;

void Put(Memory& m, std::uint32_t current, std::uint32_t before) {
    m.words[kMachine + a::kMachineCurrent] = std::uint64_t{current} | std::uint64_t{before} << 32;
}

// State 2 now, 1 before; the states' types are 10, 11, 12 (the high half of the word is not the type).
Memory Boss() {
    Memory m;
    m.words[kActor + a::kActorComponents] = kTable;
    m.words[kTable + a::kComponentPhysics] = kPhysics;
    m.words[kPhysics + a::kPhysicsInner] = kInner;
    m.words[kInner + a::kInnerHolder] = kHolder;
    m.words[kHolder + a::kHolderCct] = kCct;
    m.words[kCct + a::kCctMachine] = kMachine;
    for (std::uint64_t i = 0; i < 3; ++i) {
        m.words[kMachine + a::kMachineStates + 8 * i] = kState + 0x100 * i;
        m.words[kState + 0x100 * i + a::kStateType] = (std::uint64_t{7} << 32) | (10 + i);
    }
    Put(m, 2, 1);
    return m;
}

CoopMoveProbe::Seen Sight(bool ours, float y) {
    return {kActor, "Enemy_DungeonBoss_PhantomGanon", ours, ours ? std::uint8_t{1} : std::uint8_t{2},
            {139, -1, -1, -1, -1}, "Down", {0.0f, y, 0.0f}};
}
}  // namespace

TEST_CASE("move probe: the state, the one before and its type") {
    Memory m = Boss();
    const auto move = CoopMoveProbe::Read(m.reader(), kActor);
    REQUIRE(move);
    CHECK(move->cct == kCct);
    CHECK(move->current == 2);
    CHECK(move->before == 1);
    CHECK(move->type == 12);
    Put(m, 3, 2);  // past the three: the game reads the first
    CHECK(CoopMoveProbe::Read(m.reader(), kActor)->type == 10);
}

TEST_CASE("move probe: nothing when a link of the chain is not there") {
    CHECK_FALSE(CoopMoveProbe::Read(Boss().reader(), 0));
    for (const std::uint64_t at : {kActor + a::kActorComponents, kTable + a::kComponentPhysics,
                                   kPhysics + a::kPhysicsInner, kInner + a::kInnerHolder, kHolder + a::kHolderCct,
                                   kCct + a::kCctMachine, kMachine + a::kMachineCurrent,
                                   kMachine + a::kMachineStates + 16, kState + 0x200 + a::kStateType}) {
        Memory m = Boss();
        m.words.erase(at);
        CHECK_FALSE(CoopMoveProbe::Read(m.reader(), kActor));
        m = Boss();
        if (at != kMachine + a::kMachineCurrent && at != kState + 0x200 + a::kStateType) {
            m.words[at] = 0;
            CHECK_FALSE(CoopMoveProbe::Read(m.reader(), kActor));
        }
    }
}

TEST_CASE("move probe: a line at first sight, then one a change") {
    Memory m = Boss();
    CoopMoveProbe::Watch watch;
    auto lines = watch.Look(m.reader(), Sight(false, 0.0f));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("a copy of player 2: first seen, move state 2 (type 12, before 1), verbs 139") !=
          std::string::npos);
    CHECK(watch.Look(m.reader(), Sight(false, 1.0f)).empty());
    Put(m, 1, 2);
    lines = watch.Look(m.reader(), Sight(false, 1.0f));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("move state 2 (type 12, before 1) -> 1 (type 11, before 2)") != std::string::npos);
    lines = watch.Look(m.reader(), Sight(true, 1.0f));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("ours: HANDED OVER (was a copy of player 2) move state 1") != std::string::npos);
    lines = watch.Look(m.reader(), Sight(true, -40.0f));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("at 0.0 -40.0 0.0") != std::string::npos);
}

TEST_CASE("move probe: a cct class is named once, with the code of its two gravity calls") {
    Memory m = Boss();
    m.words[kActor] = 0x10000000 + a::kActorVtable;
    m.words[kCct] = 0x10000000 + 0x4b00000;
    m.words[0x10000000 + 0x4b00000 + a::kSlotSetGravityScale] = 0x9000;
    m.words[0x9000] = 0xbd000000;
    CoopMoveProbe::Watch watch;
    auto lines = watch.Look(m.reader(), Sight(true, 0.0f));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("main+0x4b00000, +0x1f0 code 0xbd000000 ?") != std::string::npos);
    CHECK(lines[0].find("+0x1f8 code ?") != std::string::npos);
    Put(m, 1, 2);
    CHECK(watch.Look(m.reader(), Sight(true, 0.0f)).size() == 1);
}
