// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fairies: each puppet (coop_puppet.h) gets a Navi that hovers by its
// shoulder and trails it (docs/puppet.md, Fairy). No emulator code here, so
// it is tested on its own.
//
// The fairy is an actor made the same way as the puppet: a second
// CoopPuppet::Director, asked at the same `svc #0xf4` once the puppet's
// director is done for the frame (Chain). Its targets come from a second
// CoopPuppet::Planner fed the same inputs as the puppet's, so each fairy
// follows exactly the puppet the player sees, the debug puppet included.
// The game's own Navi AI is not used: the director writes the fairy's pose
// every game frame, like the puppet's.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_puppet.h"

namespace coop::host {
class Session;
}

namespace CoopFairy {

using CoopPuppet::Vec3;

struct Settings {
    // Dm_Npc_Navi: the cutscene Navi. Its own model and glow, a simple actor
    // logic, no NPC name of its own (Npc_Navi is the player's partner, with an
    // AI that follows the player and a name the game looks up).
    std::string actor = "Dm_Npc_Navi";
    // Where it hovers, in the puppet's own frame (x right, y up, z forward),
    // in game units (about metres): by the right shoulder, a little behind.
    Vec3 offset{0.35f, 1.05f, -0.25f};
    float bob = 0.06f;          // up and down, units
    float bob_hz = 0.7f;
    float circle = 0.08f;       // a small loop in the horizontal plane, units
    float circle_hz = 0.35f;
    float follow_rate = 5.0f;   // 1/s: how fast it closes on where it should be
    float snap = 4.0f;          // farther than this (units) it jumps there
};

// Turns the puppets' targets into fairy targets. The present thread only.
class Follower {
public:
    explicit Follower(Settings settings = {}) : settings_(std::move(settings)) {}

    // `puppets` is what the puppet planner gives for this frame. Returns the
    // same input with each target a fairy beside its puppet (same numbers).
    CoopPuppet::Input Follow(const CoopPuppet::Input& puppets, std::uint64_t now_ms);

    const Settings& settings() const { return settings_; }

    // Where the fairy should be with no trailing: the puppet's position plus
    // the offset turned by its rotation, plus the bob at `t` seconds.
    Vec3 Goal(const CoopPuppet::Target& puppet, float t, std::uint8_t number) const;

private:
    struct Track {
        Vec3 at{};
        std::uint64_t ms = 0;
    };
    Settings settings_;
    std::map<std::uint8_t, Track> tracks_;
    std::string scene_;
};

// The two directors at one `svc #0xf4`. The calc thread only.
class Chain {
public:
    using Step = std::function<std::optional<CoopPuppet::Call>(bool after_call,
                                                               std::uint64_t result)>;
    // `first` runs until it says done for the frame, then `second`. Each sees
    // its own new frame once and the results of its own calls only.
    std::optional<CoopPuppet::Call> Next(const Step& first, const Step& second,
                                         bool after_call, std::uint64_t result);

private:
    enum class Who { none, first, second };
    Who last_ = Who::none;
    bool second_started_ = false;
};

// Settings and the process-wide fairy director, once, beside the puppets'
// (CoopSvc). LICHII_COOP_FAIRY=0 turns fairies off.
void Configure(const CoopPuppet::PlannerSettings& puppets, Settings settings, bool verbose,
               CoopPuppet::Director::Log log);
bool On();

// CoopSvc::Call: the puppet director, then the fairy one.
std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                     std::uint64_t result, std::uint64_t now_ms);

// CoopHook::OnFrame, right after CoopPuppet::Publish, with the same arguments.
void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, coop::host::Session* session,
             std::uint64_t server_now_us);

} // namespace CoopFairy
