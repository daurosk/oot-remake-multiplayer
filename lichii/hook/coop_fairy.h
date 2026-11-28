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
// Row 316: each fairy is where her player's own Navi is in their game. That
// game sends NAVI_SAMPLE (NaviSender: its Npc_Navi's place and turn in its
// Link's own frame, and whether she is out); the others put the fairy at the
// puppet plus that offset, turned with the puppet (NaviSamples, Follower). A
// player with no fresh sample (an older build or server) gets the shoulder
// place below, as before.
//
// Follower, NaviSender and NaviSamples are plain logic and are tested on
// their own; Publish and ShareNavi are the emulator glue (built only with
// LICHII_COOP).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"
#include "coop_caller_orders.h"
#include "coop_rooms.h"

namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}
namespace Core {
class System;
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
    // Issue 145: in a dungeon, a fairy is parked while her player is in
    // another room than ours (coop_rooms.h), unless the two Links are within
    // room_near units (a doorway, or a room guessed wrong). Off: false.
    bool rooms = true;
    float room_near = 8.0f;
    // A call (/here, CoopEmote::Def::heard): the caller's Navi flies up over
    // their head and bobs there quickly for call_ms, as the game's own Navi
    // hovers over what she points at. That is the ping's marker: the game's
    // own fairy, nothing drawn. Offset in the puppet's frame, scaled for an
    // adult as `offset` is.
    Vec3 call_offset{0.0f, 2.1f, 0.1f};
    float call_bob = 0.14f;
    float call_bob_hz = 2.2f;
    std::uint64_t call_ms = 5000;
};

// The svc cave's order for the fairy director (puppets first, CoopSpawn 10).
inline constexpr int kCallerOrder = CoopCallerOrders::kFairy;

// Row 316, our side: our own Navi (the game's Npc_Navi) as read this frame.
struct NaviSeen {
    bool present = false;  // on the actor list and present (state 4): she is out
    Vec3 position{};
    CoopPuppet::Mat3 rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// What to send of her (NAVI_SAMPLE): about 30 a second while she or our Link
// moves, about 7 while both are still, and the same while she is not out (so
// the others' fallback, after NaviSamples::kStaleMs, never shows a fairy we
// do not have). A change of out/not out is sent at once.
class NaviSender {
public:
    static constexpr std::uint64_t kEveryMs = 33;
    static constexpr std::uint64_t kStillEveryMs = 150;
    static constexpr float kStillM = 0.01f;

    // `link_rotation` unset: identity. Our Link's place unread: nothing sent
    // (her place would be a guess). Returns the message to send now, if any.
    std::optional<coop::protocol::NaviSample> Tick(const NaviSeen& navi,
                                                   const std::optional<Vec3>& link_position,
                                                   const std::optional<CoopPuppet::Quat>& link_rotation,
                                                   std::uint64_t now_ms);
    void Reset() { *this = NaviSender{}; }

private:
    bool sent_ = false;
    std::uint64_t last_ms_ = 0;
    coop::protocol::NaviSample last_{};
    Vec3 link_at_{};
};

// Theirs: the last NAVI_SAMPLE of each player. Receive and Forget on the
// network thread, Fresh on the present thread.
class NaviSamples {
public:
    // Older than this, a player's sample is not used (the samples channel is
    // unreliable, and a player whose build sends none must still get a fairy).
    static constexpr std::uint64_t kStaleMs = 600;

    void Receive(const coop::protocol::NaviSample& m, std::uint64_t now_ms);
    void Forget(std::uint8_t number);
    std::map<std::uint8_t, coop::protocol::NaviSample> Fresh(std::uint64_t now_ms) const;

private:
    struct Kept {
        coop::protocol::NaviSample m;
        std::uint64_t ms = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::uint8_t, Kept> kept_;  // guarded
};

// Turns the puppets' targets into fairy targets. The present thread only.
class Follower {
public:
    using Log = std::function<void(const std::string&)>;
    // `rooms`: the dungeon room table (coop_rooms.txt from the puppet pack);
    // empty: no room rule. `log`: one line when a fairy is parked or shown
    // again for the room rule, naming both rooms.
    explicit Follower(Settings settings = {}, CoopRooms::Table rooms = {}, Log log = {})
        : settings_(std::move(settings)), rooms_(std::move(rooms)), log_(std::move(log)) {}

    // `puppets` is what the puppet planner gives for this frame. Returns the
    // same input with each target a fairy beside its puppet (same numbers).
    // `live`: CoopPuppet::LivePuppets(), bit n while puppet n's actor is live
    // and not parked; a fairy whose puppet is not (still being made, refused,
    // gone) is parked under the floor (issue 145).
    // `local`: our Link's position; with it, a fairy whose player is in
    // another room of our dungeon is parked too (Settings::rooms).
    // `navis` (row 316): each player's fresh NAVI_SAMPLE (NaviSamples::Fresh).
    // A player in it gets her place and turn from their game, at the puppet
    // (no bob, circle or adult scale: their game's Navi already sits by an
    // adult's shoulder), and is parked while their Navi is not out. A player
    // not in it gets Goal below (one log line each time that changes).
    CoopPuppet::Input Follow(const CoopPuppet::Input& puppets, std::uint64_t now_ms,
                             std::uint32_t live = ~0u,
                             const std::optional<Vec3>& local = std::nullopt,
                             const std::map<std::uint8_t, coop::protocol::NaviSample>& navis = {});

    const Settings& settings() const { return settings_; }

    // Player `number` called (CoopEmote::TakeCalls) at `now_ms`: their fairy
    // hovers over their head until now_ms + call_ms (a new call restarts it).
    void Call(std::uint8_t number, std::uint64_t now_ms);
    bool Calling(std::uint8_t number, std::uint64_t now_ms) const;

    // Where the fairy should be with no trailing: the puppet's position plus
    // the offset turned by its rotation, plus the bob at `t` seconds. While
    // `calling`, call_offset and the call's quicker bob instead.
    Vec3 Goal(const CoopPuppet::Target& puppet, float t, std::uint8_t number, bool calling = false) const;

    // Row 316: where her player's game has her: the puppet's position plus the
    // sample's offset turned by the puppet's rotation; her turn the puppet's
    // turned by the sample's.
    static Vec3 SampledGoal(const CoopPuppet::Target& puppet, const coop::protocol::NaviSample& m);
    static CoopPuppet::Mat3 SampledTurn(const CoopPuppet::Target& puppet, const coop::protocol::NaviSample& m);

private:
    struct Track {
        Vec3 at{};
        std::uint64_t ms = 0;
    };
    Settings settings_;
    std::map<std::uint8_t, Track> tracks_;
    std::map<std::uint8_t, std::uint64_t> calls_;  // per player: the call's end (steady ms)
    std::string scene_;
    CoopRooms::Tracker rooms_;
    Log log_;
    std::map<std::uint8_t, bool> room_parked_;  // per player: parked by the room rule (for the log)
    std::set<std::uint8_t> called_logged_;      // per player: this call's log line written
    std::map<std::uint8_t, bool> sampled_logged_;  // row 316, per player: placed from a sample, as last logged
    static constexpr std::uint32_t kLocalKey = 0x100;  // our Link in rooms_
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

// The game's live actor list walked once: `visit` gets each actor the list owns (owner + kActorNode is its node), in
// list order, and returns true to stop there. Returns the actor it stopped at, or 0. The calc thread (or a test).
std::uint64_t WalkActors(CoopPuppet::Guest& guest, std::uint64_t main_base,
                         const std::function<bool(std::uint64_t actor)>& visit);
// The actor is present (state kStatePresent) / its name is exactly `name` (read at that length and its 0).
bool ActorPresent(CoopPuppet::Guest& guest, std::uint64_t actor);
bool ActorNamed(CoopPuppet::Guest& guest, std::uint64_t actor, std::string_view name);

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
// The dungeon room table from the puppet pack (coop_rooms.txt), read now;
// empty without one. Also for /teleport (coop_teleport.h CheckMove).
CoopRooms::Table RoomTable();

// The calc thread: `number`'s live fairy in our game (0: none).
std::uint64_t FairyActor(std::uint8_t number);

void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us);

// Row 316, CoopHook::OnFrame after Publish: our own Npc_Navi read and sent as
// NAVI_SAMPLE (NaviSender) while we are in a session. `link_position` and
// `link_rotation`: our Link's, as the PLAYER sample has them. Runs with the
// fairies off too (the others may have them on).
void ShareNavi(Core::System& system, std::uint64_t main_base, coop::host::Session* session,
               const std::optional<Vec3>& link_position, const std::optional<CoopPuppet::Quat>& link_rotation);
// The network thread: NAVI_SAMPLE kept, a player who left forgotten.
void OnEvent(const coop::net::Event& event);

} // namespace CoopFairy
