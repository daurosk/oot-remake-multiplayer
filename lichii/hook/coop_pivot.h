// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Forest Temple basement's turning floor (docs/dungeon.md, "The turning floor"): Obj_ForestTemple_PivotedFloor_A_01
// at 0 59.5 -3.5, room 27, a capstan a player grabs by one of its two bars and pushes; each push turns it an eighth of
// a lap. Its direction is the int flag ForestTemple_SageOfForest_PivotedFloor_A_01 (InitialDirIndexGameDataPath),
// which co-op shares, but the floor reads it only when the room loads: a turn in one game left the other games' floor
// where it was (run st-pivot, 2026-11-21: the flag set under a loaded floor did nothing). Run st-pivot3 measured a
// push: the actor's own 3x3 turns (yaw 0 to -45 in about 1.4 s, even steps) while the other game's stays at 0, and at
// the turn's end the flag reads 7 (0 before): the yaw is 45 x the direction.
//
// So each game turns its own floor to the shared direction: when the flag's direction and the floor's yaw differ and
// the floor has rested at a direction (a multiple of 45 degrees) for kOwnQuietMs (never during our own player's push,
// which starts slower than a look can tell from one look to the next), our floor is turned the shortest way at the game's own speed (45 degrees in
// 1.4 s) through the game's set-matrix call, which moves its collision too. A player standing on the ring is carried
// with it or shut in by its walls (the design: "blocking them in is better, since it's close and an easy unblock").
// Plain logic: tested in tests/lichii/pivot_test.cpp; coop_objects.cpp reads the floor and drives it.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace CoopPivot {

inline constexpr std::string_view kActor = "Obj_ForestTemple_PivotedFloor_A_01";
inline constexpr std::string_view kFlag = "ForestTemple_SageOfForest_PivotedFloor_A_01";
inline constexpr std::uint32_t kFlagHash = 0x4037fff8;

inline constexpr float kStepDeg = 45.0f;             // yaw = 45 x direction (st-pivot3: a push to yaw -45, flag 7)
inline constexpr std::uint64_t kStepMs = 1400;        // one push's turn
inline constexpr std::uint64_t kOwnQuietMs = 600;     // turned only once it has rested this long
inline constexpr float kSameDeg = 1.5f;               // closer than this: at the direction (or a resting place)
inline constexpr float kStillDeg = 0.05f;             // a look to look change bigger than this: it is moving

// The yaw (degrees, -180..180) of direction `dir` (any whole number; 8 a lap).
float YawOfDir(int dir);
// b - a the short way round (degrees, -180..180).
float Short(float a, float b);

// A turn we drive: from `from` to `to` (the short way), started at `start_ms`, `ms` long.
struct Turn {
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t start_ms = 0;
    std::uint64_t ms = 0;
    // The yaw at `now_ms` (the end's once done).
    float At(std::uint64_t now_ms) const;
    bool Done(std::uint64_t now_ms) const { return now_ms >= start_ms + ms; }
};

// One floor's follower: Look() each scan with our floor's yaw and the shared direction; a turn to drive comes back
// when one starts. The driver writes Turn::At every game frame until Done.
class Follower {
public:
    // `yaw`: our floor's yaw now; `dir`: the shared flag (none: not known yet). Returns a new turn to drive.
    std::optional<Turn> Look(std::uint64_t now_ms, float yaw, std::optional<int> dir);
    // The turn being driven (none: idle or our game's own).
    const std::optional<Turn>& Driving() const { return driving_; }
    // A new floor (the room loaded again): what was seen is forgotten.
    void Reset();

private:
    std::optional<float> last_yaw_;
    std::uint64_t still_ms_ = 0;  // since when our floor has not moved (0: not seen still yet)
    std::optional<Turn> driving_;
};

} // namespace CoopPivot
