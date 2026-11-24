// SPDX-License-Identifier: GPL-3.0-or-later
// A research probe for the other players' blocks under the hearts as the game's own panes (reports/menu-chat-01113.md,
// "Item 18 plan"). Test builds of the module only switch it on: COOP_TEST_HUD_PROBE=1 (scenario hs-hud-probe); with it
// off nothing here runs. It changes nothing in the game but the look of Link's own first heart, for a minute.
//
// The HUD is the game's PlayerStatus_00 (UIPlayerStatusScreen). Its layout holds one heart part, Pa_HeartGauge_00
// (PaHeartGauge_00: Pa_NoGauge_00, Pa_Gauge_00, Pa_Born_00), whose layouts name HeartGauge, HeartState, BaseState,
// GlowState, GlowGauge, ControlType, Pinch: read from the romfs, not known to be animations. The title's BtnNum is a
// layout state that is an animation, its frame the look, set with the game's animation finder (main+0x43d870) and its
// stop-at-frame (main+0x2d2d90), as co-op has done since 0.11.6. The probe asks the same of the heart:
//   1. each path below: an animation or not, and its last frame; copies of the heart by name (Pa_HeartGauge_01...);
//   2. then holds each found kHoldPaths animation at its frames in turn (HeartGauge at 0, 1/4, 1/2, 3/4, the last; a
//      state at each frame), kHoldMs each, set again every frame, logging each change: the scenario's screenshots every
//      few seconds show what each frame looks like and whether the game's own update undoes it.
// Run 1 (2026-11-24, reports/menu-chat-01113.md) proved the animations (HeartGauge 0..100, HeartState 0..1, ...) but
// Link had whole hearts, which Pa_NoGauge_00 draws, so the part heart's frames showed nothing; the scenario now hurts
// Link to half a heart first, and the NoGauge heart's state is held too. (Run 1's walk of the screen object for panes
// was noise, a pane's first word is no vtable here: gone.)
// Every line starts "coop: hud probe: ". Split for tests (tests/hookshot/hud_probe_test.cpp): it goes through Game.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_native_ui.h"

namespace CoopHudProbe {

// main+ offsets of build e0f3ccfaf50065ae (game 1.1.1).
namespace addr {
inline constexpr std::uint64_t kStatusVtable = 0x84c0b710 - 0x80020000;   // UIPlayerStatusScreen (names file, checked)
inline constexpr std::uint64_t kAnimFind = 0x43d870;       // (layout, "name or part/part/name") -> animation, 0 none
inline constexpr std::uint64_t kAnimPlayer = 0x20;         // animation+: its player
inline constexpr std::uint64_t kAnimLastFrame = 0x2d1340;  // (player) -> s32 its last frame
inline constexpr std::uint64_t kAnimStopAt = 0x2d2d90;     // (animation, s0 frame): stopped at that frame
}  // namespace addr

// What the probe asks; paths from PlayerStatus_00's layout.
inline constexpr std::array<const char*, 16> kPaths = {
    "Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge",   "Pa_HeartGauge_00/Pa_Gauge_00/HeartState",
    "Pa_HeartGauge_00/Pa_Gauge_00/BaseState",    "Pa_HeartGauge_00/Pa_Gauge_00/GlowState",
    "Pa_HeartGauge_00/Pa_Gauge_00/Pinch",        "Pa_HeartGauge_00/Pa_NoGauge_00/HeartState",
    "Pa_HeartGauge_00/Pa_NoGauge_00/BaseState",  "Pa_HeartGauge_00/Pa_NoGauge_00/Pinch",
    "Pa_HeartGauge_01/Pa_Gauge_00/HeartGauge",   "Pa_HeartGauge_02/Pa_Gauge_00/HeartGauge",
    "Pa_HeartGauge_03/Pa_Gauge_00/HeartGauge",   "Pa_HeartGauge_19/Pa_Gauge_00/HeartGauge",
    "Pa_HeartGauge_01/Pa_NoGauge_00/HeartState", "HeartPos",
    "InGauge",                                   "IsMpGauge",
};
// Held in this order when found.
inline constexpr std::array<const char*, 3> kHoldPaths = {
    "Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge", "Pa_HeartGauge_00/Pa_NoGauge_00/HeartState",
    "Pa_HeartGauge_00/Pa_Gauge_00/HeartState"};

inline constexpr std::uint64_t kStartMs = 30000;   // after the HUD is open: the scenario's hurt and "before" shot first
inline constexpr std::uint64_t kHoldMs = 6000;

// The module's game, with what the probe needs beyond the chat's: a call with a float in s0, and a guest C string.
class Game : public CoopNativeUi::Game {
public:
    virtual std::optional<std::uint64_t> CallFloat(std::uint64_t function, std::uint64_t x0, float s0) = 0;
    virtual std::uint64_t String(std::string_view text) = 0;   // a 0-ended copy in guest memory, 0 when none
};

// One held frame: an animation, the frame, and which hold of how many.
struct Hold {
    std::string path;
    std::uint64_t anim = 0;
    float frame = 0.0f;
};
// The holds for an animation with this last frame: HeartGauge (a gauge) at its quarters, any other at each frame
// (at most 8).
std::vector<Hold> HoldsFor(const std::string& path, std::uint64_t anim, std::int32_t last);

class Probe {
public:
    // Each frame on the game's thread: the lines to log (none most frames).
    std::vector<std::string> Step(Game& game, std::uint64_t now_ms);
    bool done() const { return phase_ == Phase::kDone; }

private:
    enum class Phase { kFind, kHold, kDone };
    void Look(Game& game, std::uint64_t layout, std::vector<std::string>& out);
    Phase phase_ = Phase::kFind;
    bool said_wait_ = false;
    std::uint64_t found_ms_ = 0;
    std::vector<Hold> holds_;
    std::size_t shown_ = static_cast<std::size_t>(-1);
};

}  // namespace CoopHudProbe
