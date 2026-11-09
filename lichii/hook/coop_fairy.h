// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fairies: each puppet (coop_puppet.h) gets a Navi that hovers by its
// shoulder and trails it (docs/puppet.md, Fairy).
//
// The fairy is an actor made the same way as the puppet: a second
// CoopPuppet::Director, registered with the svc cave as caller 20
// (CoopSvc::AddCaller), so it runs after the puppets each frame. Its targets
// come from a second CoopPuppet::Planner fed the same inputs as the puppet's,
// so each fairy follows exactly the puppet the player sees, the debug puppet
// included. The game's own Navi AI is not used: the director writes the
// fairy's pose every game frame, like the puppet's.
//
// Follower is plain logic and is tested on its own; Publish is the emulator
// glue (built only with LICHII_COOP).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

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
    // An adult puppet (actor name ending in _Adult) is taller: the offset is
    // scaled by this, so the fairy stays by its shoulder.
    float adult_scale = 1.45f;
    float bob = 0.06f;          // up and down, units
    float bob_hz = 0.7f;
    float circle = 0.08f;       // a small loop in the horizontal plane, units
    float circle_hz = 0.35f;
    float follow_rate = 5.0f;   // 1/s: how fast it closes on where it should be
    float snap = 4.0f;          // farther than this (units) it jumps there
};

// The svc cave's order for the fairy director (puppets first, CoopSpawn 10).
inline constexpr int kCallerOrder = 20;

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

// CoopHook::OnFrame, right after CoopPuppet::Publish, with the same
// arguments. The first call reads the settings (on with the puppets;
// LICHII_COOP_FAIRY=0 off, LICHII_COOP_FAIRY_ACTOR another actor) and
// registers the fairy director with the cave.
void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, coop::host::Session* session,
             std::uint64_t server_now_us);

} // namespace CoopFairy
