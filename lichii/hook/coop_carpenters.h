// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 333 (player report: a client freed a Gerudo Fortress carpenter; "it doesn't count for the host"): the
// fortress counts its rescued carpenters in a saved Int of each game, not in the shared rescue flags.
//
// Read from the game's event flows (romfs Event/EventFlow, GameDataList):
// - SageOfSpirit_RescueCarpenter, played inside each cell's rescue (Common_Dungeon_OpenDoorChainLockMove's
//   GerudoFortress_OpenDoorChainLock_MainFlow, subflow Check_NumOfCarpenterHelped), adds 1 to
//   GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped (Int 0x290a6222, default 0) and -1 to
//   ..._NumOfCarpenterHelped_02 (Int 0x6a466d6b, default 4), then (StepCheck_Clear) plays
//   Meet_FourCarpenter_ClearEvent only when Helped > 3: the card scene (SageOfSpirit.evfc entry
//   Meet_FourCarpenter_ClearEvent: Dm_Gerudo_0025 and SageOfSpirit Step02); below 4, Navi's "carpenters left".
// - Dm_Gerudo_0020 (each cell's thank-you, entries 0..3) picks its lines by Helped and, at Helped >= 4
//   (Npc_Gerudo_001_CreateCheck), sets GerudoFortress_Npc_Gerudo_001_IsInFirstPlace (Bool, ResetTypeValue 9,
//   not saved): the card Gerudo stands at her first place for the card scene.
// Co-op shares every cell's rescue (coop_story.cpp, report 99: the guard, the key, Saved_*, IsAfterHelpCarpenter_*,
// WarpPoint0N_ClearEvent), so a game whose carpenter another player freed never plays that rescue and never adds
// its 1: with the rescues split between players no game reaches 4, nobody gets the card scene, and SageOfSpirit
// never reaches Step02 (row 150's rule raises Step02 only from the shared played Meet_FourCarpenter_ClearEvent).
//
// The game's own invariant, outside an event: Helped = the cells rescued and Helped_02 = 4 - Helped (each rescue
// adds and sets its cell's flags in one event; Helped is the only reader, _02 is never read). So each game's two
// Ints FOLLOW the shared rescue flags (both ways: a save with a count above its rescues, a rescue done twice at
// once, is set right too), never while our game plays an event (our own rescue adds before its flags are set).
// The 4th rescue then plays the card scene in the rescuer's game, as in a solo game.
//
// A save that is ALREADY stuck (all four cells rescued, the card scene never played: before this fix, or two
// players freeing the last two carpenters at once) has no carpenter left to free. There our game is given what its
// own 4th rescue does: IsInFirstPlace and the card scene Meet_FourCarpenter_ClearEvent, asked by name (the
// live-event requester, as the shared story scenes are joined), once our Link is in the fortress, no event runs and
// the state has held kStuckUs (the rescuer's own scene sets WarpPoint0N_ClearEvent before it plays the card scene).
// Pure: no game memory here (tests/lichii/carpenters_test.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace CoopCarpenters {

inline constexpr std::uint32_t kHelped = 0x290a6222u;        // GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped
inline constexpr std::uint32_t kHelpedLeft = 0x6a466d6bu;    // GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped_02
inline constexpr std::uint32_t kFirstPlace = 0xb749c6cbu;    // GerudoFortress_Npc_Gerudo_001_IsInFirstPlace
inline constexpr std::uint32_t kCardScene = 0x754f6228u;     // HavePlayedEvent.Meet_FourCarpenter_ClearEvent
inline constexpr std::string_view kCardSceneName = "Meet_FourCarpenter_ClearEvent";
inline constexpr int kCells = 4;
inline constexpr std::uint64_t kStuckUs = 90'000'000;  // the rescuer's thank-you and card scene take under this

// One cell (N = 1..4 holds Npc_Hylia_01(2+N)_Adult): rescued when either of its shared end flags is set.
struct Cell {
    std::uint32_t after_help;  // GerudoFortress_IsAfterHelpCarpenter_Npc_Hylia_01x_Adult
    std::uint32_t warp_clear;  // GerudoFortress_WarpPoint0N_ClearEvent
};
inline constexpr std::array<Cell, kCells> kCellFlags{{
    {0xf1eb5977u, 0x89c70062u},  // 013, WarpPoint01
    {0x06d7f245u, 0x231f663au},  // 014, WarpPoint02
    {0xf717e3dau, 0x0e18ee0du},  // 015, WarpPoint03
    {0x73dce2fau, 0xa4b1efb9u},  // 016, WarpPoint04
}};

using ReadBool = std::function<std::optional<bool>(std::uint32_t)>;
using ReadInt = std::function<std::optional<std::int32_t>(std::uint32_t)>;

// The cells rescued in our game's flags; nothing while one of them cannot be read (no save loaded).
inline std::optional<int> Rescued(const ReadBool& flag) {
    int n = 0;
    for (const Cell& c : kCellFlags) {
        const auto a = flag(c.after_help);
        const auto w = flag(c.warp_clear);
        if (!a || !w) {
            return std::nullopt;
        }
        n += (*a || *w) ? 1 : 0;
    }
    return n;
}

// The Ints to write so they follow the rescues (nothing when they already do, or in an event).
struct Follow {
    std::optional<std::int32_t> helped;
    std::optional<std::int32_t> left;
    int rescued = 0;
};
inline Follow FollowRescues(const ReadBool& flag, const ReadInt& value, bool in_event) {
    Follow f;
    const auto n = Rescued(flag);
    if (!n || in_event) {
        return f;
    }
    f.rescued = *n;
    const auto helped = value(kHelped);
    const auto left = value(kHelpedLeft);
    if (helped && *helped != *n) {
        f.helped = *n;
    }
    if (left && *left != kCells - *n) {
        f.left = kCells - *n;
    }
    return f;
}

// The stuck save: all four rescued, the card scene not played. Asked once the state held kStuckUs with our Link in
// the fortress and no event running; the wait starts again whenever one of those is not so, and after each ask: the
// requester drops an ask that could not start in time (Requester::Expire: kJoinUs behind another event, a teleport
// pause, a door), so while the scene is still unplayed it is asked again every kStuckUs (review-c123-21).
class Mender {
public:
    // True on the tick our game should get IsInFirstPlace and the card scene.
    bool Tick(const ReadBool& flag, std::string_view scene, bool in_event, std::uint64_t now_us) {
        const auto n = Rescued(flag);
        const auto played = flag(kCardScene);
        const bool stuck = n == kCells && played == false;
        if (!stuck || in_event || !scene.starts_with("GerudoFortress")) {
            since_us_ = 0;
            return false;
        }
        if (since_us_ == 0) {
            since_us_ = now_us;
        }
        if (now_us - since_us_ < kStuckUs) {
            return false;
        }
        since_us_ = now_us;  // the next ask only after another full wait (the scene, once it starts, is an event)
        return true;
    }
    void Reset() { since_us_ = 0; }

private:
    std::uint64_t since_us_ = 0;
};

}  // namespace CoopCarpenters
