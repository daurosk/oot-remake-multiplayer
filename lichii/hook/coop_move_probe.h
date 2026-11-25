// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Row 287 (Phantom Ganon falls through the floor after a hand-over): read-only probe of a boss's CCT (the physics
// character controller) move state, in every game, as the owner's and as a copy's.
//
// Read from the game's own AI nodes (main of build e0f3cc, dump/main.plain):
//   QueryPhysicsCctGetCurrentMoveStateType's execute (main+0x2aa97a0) and OneShotPhysicsCctChangeMoveState's
//   (main+0x28fff10) walk the same chain from the actor ([[[node + 8] + 8] + 0x1f0]):
//     physics = [[actor + 0x270] + 0x50], cct = [[[physics + 0x20] + 0x10] + 8], machine = [cct + 0x48]
//     the current state's index: u32 at machine + 0x58, the one before: +0x5c, the states: [machine + 0x40 + 8 * i],
//     i < 3 (else [machine + 0x40]), a state's type: u32 at state + 0x10
//   ChangeMoveState to NextMoveState N ends the current state (vtable + 0x38), then writes N to +0x58.
//   OneShotPhysicsCctSetGravityScale's execute (main+0x29015a0) calls cct's vtable + 0x1f0 with the scale in s0
//   (vtable + 0x1f8 is QueryPhysicsCctGetGravity's). Which class a boss's cct is, is not known: its vtable and the
//   first words of both are logged once a class, so the field can be read offline.
// LICHII_COOP_MOVE_PROBE=1 turns it on (off by default).
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace CoopMoveProbe {

namespace addr {
constexpr std::uint64_t kActorComponents = 0x270;
constexpr std::uint64_t kComponentPhysics = 0x50;
constexpr std::uint64_t kPhysicsInner = 0x20;
constexpr std::uint64_t kInnerHolder = 0x10;
constexpr std::uint64_t kHolderCct = 8;
constexpr std::uint64_t kCctMachine = 0x48;
constexpr std::uint64_t kMachineStates = 0x40;
constexpr std::uint64_t kMachineCurrent = 0x58;  // u32, and the one before it in the next u32
constexpr std::uint64_t kStateType = 0x10;
constexpr std::uint64_t kSlotSetGravityScale = 0x1f0;
constexpr std::uint64_t kSlotGetGravity = 0x1f8;
constexpr std::uint64_t kActorVtable = 0x4b25e18;  // main+: every actor's (coop_objects.cpp)
}  // namespace addr

using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

struct Move {
    std::uint64_t cct = 0;
    std::uint32_t current = 0;  // the state's index (NextMoveState's value)
    std::uint32_t before = 0;
    std::uint32_t type = 0;  // what QueryPhysicsCctGetCurrentMoveStateType answers
    friend bool operator==(const Move&, const Move&) = default;
};

// The actor's move state, or nothing when a link of the chain is not there.
std::optional<Move> Read(const ReadWord& read, std::uint64_t actor);

// One boss as a walk saw it.
struct Seen {
    std::uint64_t actor = 0;
    std::string name;
    bool ours = false;
    std::uint8_t owner = 0;
    std::array<std::int32_t, 5> verbs{};
    std::string animation;
    std::array<float, 3> at{};
};

// Logs each boss's first sight, then every change of its move state, owner or verbs, and every move up or down of
// kRise metres. Reads only.
class Watch {
public:
    static constexpr float kRise = 5.0f;
    static constexpr int kMaxLines = 2000;  // per session
    static constexpr std::size_t kMaxActors = 64;
    std::vector<std::string> Look(const ReadWord& read, const Seen& seen);

private:
    struct Last {
        std::optional<Move> move;
        bool ours = false;
        std::uint8_t owner = 0;
        std::array<std::int32_t, 5> verbs{};
        float y = 0.0f;
    };
    void Class(const ReadWord& read, const Seen& seen, std::uint64_t cct, std::vector<std::string>& lines);
    std::map<std::uint64_t, Last> last_;
    std::set<std::uint64_t> classes_;
    int lines_ = 0;
};

}  // namespace CoopMoveProbe
