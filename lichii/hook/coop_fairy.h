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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

// The game's live actor list (coop_objects.cpp reads the same, build
// e0f3ccfaf50065ae): main + kManagerPtr -> manager; manager + kLists ->
// lists; the list head is lists + kListHead, a ring of nodes, each node's
// owner at node + kNodeOwner, and an actor's own node at actor + kActorNode.
namespace addr {
inline constexpr std::uint64_t kManagerPtr = 0x4e91888;
inline constexpr std::uint64_t kLists = 0x118;
inline constexpr std::uint64_t kListHead = 0x670;
inline constexpr std::uint64_t kNodeOwner = 0x10;
inline constexpr std::uint64_t kActorNode = 0x538;
inline constexpr std::uint64_t kActorName = 0x260;  // -> the actor's name, a C string
inline constexpr std::uint64_t kActorState = 0x2c;  // 4: present
inline constexpr std::uint32_t kStatePresent = 4;
inline constexpr std::size_t kMaxWalk = 8192;
}  // namespace addr

// The first present actor called `name` on the actor list; 0 none. The calc
// thread (or a test).
std::uint64_t FindActor(CoopPuppet::Guest& guest, std::uint64_t main_base, std::string_view name);

// The wing beat: the fairy actor (Dm_Npc_Navi) plays no animation of its own,
// so each frame the local player's own Navi's skeleton pose is copied onto
// every fairy (CoopPose::CopySkeleton). The fairies so flap and turn as the
// local Navi does. The calc thread only.
class Wings {
public:
    using Log = std::function<void(const std::string&)>;
    explicit Wings(std::string source = "Npc_Navi", Log log = {})
        : source_(std::move(source)), log_(std::move(log)) {}

    // Once a frame with the live fairy actors. Looks for the local Navi at
    // most every kSearchMs while she is not found.
    void Frame(CoopPuppet::Guest& guest, std::uint64_t main_base,
               const std::vector<std::uint64_t>& fairies, std::uint64_t now_ms);

    std::uint64_t navi() const { return navi_; }
    unsigned copies() const { return copies_; }  // fairy frames copied, all told

    static constexpr std::uint64_t kSearchMs = 500;

private:
    bool StillNavi(CoopPuppet::Guest& guest);
    void Say(const std::string& text) const {
        if (log_) log_(text);
    }

    std::string source_;
    Log log_;
    std::uint64_t navi_ = 0;
    std::uint64_t search_ms_ = 0;
    bool searched_ = false;
    unsigned copies_ = 0;
    std::map<std::uint64_t, std::string> notes_;  // per fairy actor: the last note logged
};

// CoopHook::OnFrame, right after CoopPuppet::Publish, with the same
// arguments. The first call reads the settings (on with the puppets;
// LICHII_COOP_FAIRY=0 off, LICHII_COOP_FAIRY_ACTOR another actor) and
// registers the fairy director with the cave. Each Navi shows, holds or
// hides as her puppet does (CoopPuppet::Collect, the same `local_event`).
void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us);

} // namespace CoopFairy
