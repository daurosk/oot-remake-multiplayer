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
    // Issue 169: a bomb's AS slot 0 (the fuse's Bomb_Blink), when read.
    std::optional<coop::protocol::Animation> animation;
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
    // A copy that goes by itself while still wanted (act show) is made again
    // up to this many times, Director::kRemakeDelayMs after it went. 0: never
    // (a bomb that went off is not made again). Rolled enemies (issue 124):
    // a Stalchild copy can sink into the ground by its own AI while the
    // decider's one still fights.
    std::uint8_t remakes = 0;
    // Issue 169: held in its owner's hands. Made as the held stand-in
    // (HeldStandIn: the model, no physics, placed as the puppet is), so it
    // cannot fall out of the hands between two places. A copy that stops or
    // starts being held comes with a new key: the director deletes the old
    // actor and makes the other kind.
    bool held = false;
    // The owner's animation where the copy is shown (the bomb's fuse): played
    // on the copy when its slot 0 shows another, its frame set when off.
    std::optional<coop::protocol::Animation> animation;
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
    // Issue 169: our Link's place, before each Tick (nothing: not known, so
    // none of our bombs is held).
    void SetLink(std::optional<Vec3> link) { link_ = link; }
    // How many of our bombs are held now, and how many copies are shown held.
    std::size_t HeldMine() const;
    std::size_t HeldCopies() const;
    // A held bomb out of the hands (CoopProjectiles::InHands) for this many
    // walks in a row is let go (thrown, dropped or put down). A held or
    // animated bomb is sampled at kSampleEveryUs whether it moved or not: the
    // fuse's frames and the hands' small moves are what its copy shows.
    static constexpr unsigned kLeaveHandsWalks = 2;

    std::size_t Mine() const { return mine_.size(); }
    // Issue 151: where our own cast lures are (CoopProjectiles::IsLure), as
    // of the last walk.
    std::vector<Vec3> OwnLures() const;
    std::size_t Copies() const { return copies_.size(); }
    // Our bombs' fuse as learnt so far (coop_projectiles.h).
    const CoopProjectiles::FuseClock& Fuse() const { return fuse_; }
    // Bomb copies blown up on the owner's fuse before its remove came.
    std::size_t BlownOnTime() const { return blown_on_time_; }
    // Issue 166: copies held where the owner's shot hit (remove reason hit,
    // CoopProjectiles::LandHoldUs) before they go.
    std::size_t Landed() const { return landed_; }

    // A shot's flight when the game's place does not show it (issue 158).
    // The seed and the arrow are made when they are nocked and sit in the
    // sling or on the string until let go. Whether their place (+0x31c)
    // moves in flight is not known (every shot seed so far went as "gone",
    // slow at its end). So the moment of the shot comes from the ammo count
    // (CoopProjectiles::AmmoWatch), and when the nocked one has not moved
    // fast kShotGraceUs later, its flight is sent for the copies: straight
    // along its turn's axis (`axis`: 0 to 2 +x +y +z, 3 to 5 -x -y -z) at
    // its kind's speed, for at most kShotFlightUs, as ordinary samples.
    static constexpr std::uint64_t kShotGraceUs = 100'000;
    static constexpr std::uint64_t kShotFlightUs = 400'000;
    static constexpr float kFlySpeed = 8.0f;  // faster: its place shows the flight
    struct ShotSettings {
        int axis = 2;
        std::optional<float> speed;  // m/s for every kind; unset: ShotSpeed(kind)
    };
    void SetShots(ShotSettings settings) { shot_settings_ = settings; }
    // Present thread, before Tick: our `actor` (Shootable_DekuSeed,
    // NormalArrow) was let go now. The newest nocked one of that name is it.
    // False when none of ours is on the list.
    bool Shot(std::string_view actor, std::uint64_t now_us);
    // Shots whose flight Tick made up since the last Tick (for the log).
    struct MadeUp {
        coop::protocol::ObjectId id;
        std::string name;
        Vec3 from{};
        Vec3 velocity{};
    };
    const std::vector<MadeUp>& MadeUpFlights() const { return made_up_; }

    // One of ours that left the list, and how it moved (issue 158: whether
    // a slingshot seed's place moves in flight, or the game keeps it at the
    // sling). For the log.
    struct Flight {
        coop::protocol::ObjectId id;
        std::string name;
        std::uint64_t lived_us = 0;
        float far = 0.0f;        // the farthest from where it was first seen, metres
        float top_speed = 0.0f;  // between two walks, metres a second
        coop::protocol::RemoveReason reason = coop::protocol::RemoveReason::gone;
        bool shot = false;       // let go (its ammo count went down)
        bool made_up = false;    // its flight was sent from the shot, not its place
        unsigned bounces = 0;    // turned like a bounce (issue 166)
    };
    // The ones that left in the last Tick.
    const std::vector<Flight>& Flights() const { return flights_; }

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
        Vec3 made_position{};
        float far = 0.0f;
        float top_speed = 0.0f;
        // The shot (issue 158).
        std::optional<std::uint64_t> shot_at;
        float shot_top_speed = 0.0f;  // since the shot
        unsigned bounces = 0;         // issue 166: CoopProjectiles::Bounced steps
        bool made_up = false;
        Vec3 shot_from{};
        Vec3 shot_velocity{};
        // Issue 169.
        bool held = false;
        unsigned out_of_hands = 0;
        bool sent_held = false;
        std::optional<coop::protocol::Animation> animation;
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
        bool landed = false;          // counted in landed_ once (issue 166)
        bool live = false;            // the director had it on the list
        // Issue 169: in the owner's hands (shown as the held stand-in, along
        // `held_path` kHeldBehindUs late), and the owner's animation.
        bool held = false;
        CoopProjectiles::HeldPath held_path;
        std::optional<coop::protocol::Animation> animation;
        std::uint64_t animation_at = 0;  // server clock of its sample
        // Went off while held: the bomb is made first, then blown up.
        bool explode_once_made = false;
        Vec3 Position() const { return track.Empty() ? Vec3{} : track.Last().position; }
    };

    void Apply(std::uint64_t now_us);
    // Issue 169: a copy that starts or stops being held gets a new key.
    void Rekey(Remote& r, bool held);
    // Our bomb `m` this walk: in our Link's hands or not. `first`: just
    // made, so no speed of its own yet.
    void Hands(Own& m, bool first);
    // Issue 158: the made-up flight, once the grace is over (Shot).
    void MakeUpFlight(Own& m, std::uint64_t now_us);
    Vec3 ShotPlace(const Own& m, std::uint64_t now_us) const;

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
    std::size_t landed_ = 0;
    std::vector<Flight> flights_;
    std::vector<MadeUp> made_up_;
    ShotSettings shot_settings_;
    bool setter_ = true;
    std::optional<Vec3> link_;
    std::optional<Vec3> link_before_;
    std::uint64_t link_before_at_ = 0;
    Vec3 link_velocity_{};

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

// Issue 85: the copy of another player's slingshot seed, arrow or hookshot
// tip is made from a stand-in pack of ours (tools/puppet_pack, "stand-ins"),
// not from the game's own shootable. A shootable made without a shooter
// starts in its `UsedExternally` state, and the seed's pack hides it while it
// is not shot (`IsPrepareVisibleOff`): the copies were made in the other game
// (player logs) but never seen. The stand-in is the actor's model only
// (no physics, sensor, attack or AI), so it is drawn wherever it is put, and
// it can hit nothing. Empty when the actor has none.
std::string_view StandIn(std::string_view actor);
// Issue 151: shared actors whose copy is only ever the stand-in. A lure made
// as the game's own Weapon_Lure would be a lure the fish here can bite, so
// with no stand-in in the pack (or after it failed) no copy is made at all.
bool StandInOnly(std::string_view actor);
// Issue 169: the stand-in a held copy is made as (Item_Bomb: CoopShot_Bomb,
// its model and animations). Empty when the actor has none.
std::string_view HeldStandIn(std::string_view actor);

// Axis `axis` of a turn (0 to 2: +x +y +z columns of its matrix, 3 to 5 the
// same reversed), as a made-up shot flies along it (Sync::SetShots).
Vec3 ShotAxis(const coop::protocol::Quat& rotation, int axis);

// The stand-ins a puppet pack has: the CoopShot_* names its coop_puppet.json
// lists (build.py writes them under "shots"). A pack built before the
// stand-ins lists none, and a name the pack lacks is never asked for: a create
// request for an actor ActorInfo does not know has never been run.
std::set<std::string> StandInsIn(std::string_view manifest);

// The calc thread's side. Same cave contract as CoopPuppet::Director.
class Director {
public:
    using Log = CoopPuppet::Director::Log;
    explicit Director(Log log = {}) : log_(std::move(log)) {}

    static constexpr int kMaxCallsPerFrame = 4;
    static constexpr std::size_t kSlots = 16;
    static constexpr std::uint64_t kSlotSize = 0x180;  // arg 0x100, name 0x10, handle 8, text 0x40
    // Issue 169: after the slots, one fixed cell per animation name played
    // on a copy (a played slot may keep the name's pointer, so a cell is
    // never written again with another name).
    static constexpr std::uint64_t kNameCell = 0x40;
    static constexpr std::size_t kNameCells = 32;
    static constexpr std::uint64_t kBlockSize = kSlots * kSlotSize + kNameCells * kNameCell;
    static constexpr std::uint64_t kCreateTimeoutMs = 3000;
    static constexpr std::uint64_t kRemakeDelayMs = 1000;  // see Copy::remakes

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
    // Present thread: the stand-ins the installed puppet pack has
    // (StandInsIn). None (the default): every copy is the game's own actor.
    void SetStandIns(std::set<std::string> names);
    // A stand-in that failed this many times in a row (create failed, timed
    // out or dropped, or no actor taken) is not asked for again this session.
    static constexpr int kStandInTries = 3;

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
        // Made from the stand-in pack (StandIn): placed as the puppet is
        // (place and the pending transform), with no setter call.
        bool stand_in = false;
        std::string copy_of;  // the shared actor's name (Copy::actor)
    };
    enum class Pending { none, malloc, create, take, drop, remove };

    void NewFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms);
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, std::uint64_t now_ms);
    // The game's setter (the actor's vtable slot 0x218) for a copy whose
    // place was written this frame. Not counted in kMaxCallsPerFrame.
    std::optional<CoopPuppet::Call> NextSetter(CoopPuppet::Guest& guest);
    // Issue 169: the copy's animation (Copy::animation). Not counted in
    // kMaxCallsPerFrame either: one play or one frame set per copy a frame.
    std::optional<CoopPuppet::Call> NextPlay(CoopPuppet::Guest& guest);
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
    std::set<std::uint64_t> started_;  // keys created: not twice, unless remade
    // Copies that went by themselves and are made again (Copy::remakes).
    struct Remade {
        std::uint8_t count = 0;
        std::uint64_t gone_ms = 0;
    };
    std::map<std::uint64_t, Remade> remade_;
    bool Due(const Copy& c, std::uint64_t now_ms) const;
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    std::uint64_t retry_ms_ = 0;
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);
    Pending pending_ = Pending::none;
    std::size_t pending_slot_ = 0;
    int calls_ = 0;
    std::vector<std::size_t> setters_;  // slots placed this frame
    std::vector<std::size_t> plays_;    // slots with an animation this frame
    std::map<std::string, std::size_t> name_cells_;  // animation name to its cell
    std::set<std::string> stand_ins_;            // guarded: what the pack has
    // Shared actors whose stand-in failed kStandInTries times in a row:
    // copied as the game's own actor from then on.
    std::set<std::string> no_stand_in_;
    std::map<std::string, int> stand_in_fails_;  // in a row, by shared actor
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
