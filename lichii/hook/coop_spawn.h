// SPDX-License-Identifier: GPL-3.0-or-later
//
// Things a player makes (bombs, seeds, nuts, arrows, the boomerang, the
// hookshot tip) shown in every game (docs/spawns.md). LICHII_COOP_SPAWN=1.
//
// - Sync (present thread, no emulator code): each frame it gets the made
//   actors of one walk of the actor list. A new one that is not a copy is
//   ours: OBJECT_SPAWN once, OBJECT_SAMPLE up to 30 a second while it moves,
//   OBJECT_REMOVE when it is gone (reason exploded for bombs). From the
//   others' messages it keeps the copies this game should show.
// - Director (the game's calc thread, at the puppet cave's `svc #0xf4`,
//   after the puppets): creates each copy with the game's own create
//   request, writes its place every game frame, sets a bomb's life to 0 at
//   the owner's remove (the game's own explosion), deletes the others.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_projectiles.h"
#include "coop_puppet.h"

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopSpawn {

using coop::protocol::Vec3;

// The made actors that are shared, and whether they explode when gone.
bool IsShared(std::string_view name);
bool Explodes(std::string_view name);
bool Flies(std::string_view name);  // shot or thrown: the projectile flag

// One made actor of one walk.
struct Seen {
    std::uint64_t address = 0;
    std::string name;
    Vec3 position{};
    // The actor's 3x3 at +0x328 (rows), when the walk read one.
    std::optional<CoopPuppet::Mat3> rotation;
};

// What the director should do with one copy.
// release: the copy is made and its maker knows the actor; the director
// forgets it without deleting it (drops, coop_drops.h).
enum class Act : std::uint8_t { show, explode, remove, release };

struct Copy {
    std::uint64_t key = 0;  // unique for this run
    std::string actor;
    Vec3 position{};
    Act act = Act::show;
    // Off for enemy copies (coop_objects): the Mover places them each frame.
    bool place = true;
    // Projectile copies (coop_projectiles.h): the owner's turn, written with
    // the place, and the game's own setter called after the write so the
    // physics takes the new place (as the objects' Mover does).
    std::optional<CoopPuppet::Mat3> rotation;
    bool setter = false;
};

class Sync {
public:
    static constexpr std::uint64_t kSampleEveryUs = 33'000;  // 30 a second
    static constexpr std::uint64_t kStillEveryUs = 500'000;
    static constexpr std::uint64_t kAheadMaxUs = CoopProjectiles::Track::kAheadMaxUs;
    static constexpr std::uint64_t kCopyLifeUs = 20'000'000; // no remove came: dropped
    static constexpr float kClaimDistance = 1.5f;            // a fresh copy found on the list
    static constexpr unsigned kGoneWalks = 2;
    static constexpr std::uint64_t kExplodeMinUs = CoopProjectiles::kExplodeMinUs;

    // Network thread: OBJECT_SPAWN, OBJECT_SAMPLE, OBJECT_REMOVE. Applied on
    // the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, once per walk. `copies_live` are the actors the
    // director made (key to address). Fills `send` and `copies`.
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
              const std::vector<Seen>& seen, const std::map<std::uint64_t, std::uint64_t>& copies_live,
              std::vector<coop::protocol::Message>& send, std::vector<Copy>& copies);

    void Reset();

    // Projectile copies get the game's setter after each place write
    // (Copy::setter). LICHII_COOP_SHOTS_SETTER=0 turns it off.
    void SetSetter(bool on) { setter_ = on; }

    std::size_t Mine() const { return mine_.size(); }
    std::size_t Copies() const { return copies_.size(); }
    // Our bombs' fuse as learnt so far (coop_projectiles.h).
    const CoopProjectiles::FuseClock& Fuse() const { return fuse_; }
    // Bomb copies blown up on the owner's fuse before its remove came.
    std::size_t BlownOnTime() const { return blown_on_time_; }

private:
    struct Own {
        coop::protocol::ObjectId id;
        std::string name;
        Vec3 position{};
        Vec3 velocity{};
        std::optional<coop::protocol::Quat> rotation;
        std::uint64_t seen_at = 0;
        Vec3 sent_position{};
        std::uint64_t sent_at = 0;
        std::uint32_t sequence = 0;
        unsigned missed = 0;
        std::uint64_t made_at = 0;
    };
    struct Remote {
        std::uint64_t key = 0;
        std::string actor;
        CoopProjectiles::Track track;  // the owner's samples on the server clock
        std::optional<coop::protocol::Quat> rotation;
        std::uint64_t got_us = 0;     // our time of the latest message
        std::optional<coop::protocol::RemoveReason> removed;
        std::uint64_t address = 0;    // once the director made it
        // A bomb: when the owner's fuse runs out (server clock). Blown then
        // even if the owner's remove has not come yet.
        std::optional<std::uint64_t> blow_at;
        bool blasted = false;         // added to TheBlasts once
        bool live = false;            // the director had it on the list
        Vec3 Position() const { return track.Empty() ? Vec3{} : track.Last().position; }
    };

    void Apply(std::uint64_t now_us);

    std::string scene_;
    bool first_walk_ = true;
    std::set<std::uint64_t> old_;  // on the list at the first walk of a scene
    std::map<std::uint64_t, Own> mine_;  // by address
    std::map<coop::protocol::ObjectId, Remote> copies_;
    std::set<std::uint64_t> copy_addresses_;
    std::uint64_t counter_ = 0;
    std::uint64_t next_key_ = 1;
    CoopProjectiles::FuseClock fuse_;  // kept across scenes: the game's fuse
    std::size_t blown_on_time_ = 0;
    bool setter_ = true;

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

// The calc thread's side. Same cave contract as CoopPuppet::Director.
class Director {
public:
    using Log = CoopPuppet::Director::Log;
    explicit Director(Log log = {}) : log_(std::move(log)) {}

    static constexpr int kMaxCallsPerFrame = 4;
    static constexpr std::size_t kSlots = 16;
    static constexpr std::uint64_t kSlotSize = 0x180;  // arg 0x100, name 0x10, handle 8, text 0x40
    static constexpr std::uint64_t kCreateTimeoutMs = 3000;

    // Present thread.
    void SetInput(std::uint64_t main_base, std::vector<Copy> copies);
    // Present thread: rolled enemies another game decided (coop_objects,
    // docs/objects.md "Rolled spawns"). Keys have the top bit set, so they
    // never meet SetInput's.
    void SetEnemies(std::uint64_t main_base, std::vector<Copy> copies);
    // Present thread: drops another game rolled (coop_drops). Keys have bit
    // 62 set.
    void SetDrops(std::uint64_t main_base, std::vector<Copy> copies);
    std::map<std::uint64_t, std::uint64_t> Live() const;  // key to actor

    // The calc thread, at each `svc #0xf4` the puppets did not take.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t now_ms);

private:
    enum class Phase { creating, live, done };
    struct Slot {
        std::uint64_t key = 0;  // 0 free
        std::string actor;
        Phase phase = Phase::creating;
        std::uint64_t request = 0;
        std::uint64_t taken = 0;
        std::uint64_t vtable = 0;
        std::uint64_t since_ms = 0;
        bool exploded = false;
    };
    enum class Pending { none, malloc, create, take, drop, remove };

    void NewFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms);
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, std::uint64_t now_ms);
    // The game's setter (the actor's vtable slot 0x218) for a copy whose
    // place was written this frame. Not counted in kMaxCallsPerFrame.
    std::optional<CoopPuppet::Call> NextSetter(CoopPuppet::Guest& guest);
    std::uint64_t SlotBase(std::size_t i) const { return block_ + i * kSlotSize; }
    const Copy* Wanted(std::uint64_t key) const;
    void Say(const std::string& text) const;

    Log log_;
    mutable std::mutex mutex_;
    std::uint64_t next_base_ = 0;  // guarded
    std::vector<Copy> next_;       // guarded
    std::uint64_t next_enemy_base_ = 0;  // guarded
    std::vector<Copy> next_enemies_;     // guarded
    std::uint64_t next_drop_base_ = 0;   // guarded
    std::vector<Copy> next_drops_;       // guarded
    std::map<std::uint64_t, std::uint64_t> live_;  // guarded

    std::uint64_t base_ = 0;
    std::vector<Copy> copies_;
    std::set<std::uint64_t> started_;  // keys ever created: never twice
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    std::uint64_t retry_ms_ = 0;
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);
    Pending pending_ = Pending::none;
    std::size_t pending_slot_ = 0;
    int calls_ = 0;
    std::vector<std::size_t> setters_;  // slots placed this frame
};

Director& TheDirector();
// Registers the director with the svc cave once (the spawn sync and the
// shared enemies' rolled copies both use it).
void UseDirector();

// The hook: present thread, once per frame after the session's frame.
// Does nothing unless LICHII_COOP_SPAWN=1 and puppets (the cave) are on.
void Frame(Core::System& system, coop::host::Session* session, const std::optional<std::string>& scene,
           std::uint64_t server_now_us);
// Network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

} // namespace CoopSpawn
