// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared enemies, the hook side (docs/objects.md, "Design" and "Telling one
// actor from another"). No emulator code: coop_objects.cpp walks the game's
// actor list and hands each walk to Sync::Tick, which says what to send and
// which lives to write. Tested in tests/lichii/objects_test.cpp.
//
// - OBJECT_PRESENT for the enemies loaded in our scene, on every change and
//   every 2 s.
// - The owner sends OBJECT_SAMPLE (time, position, life) at most 20 a second
//   while it changes, once a second while it is still.
// - A copy we do not own whose life drops sends DAMAGE to the owner, summed
//   over 100 ms; the owner writes life minus the damage into its enemy.
// - A copy we do not own takes the owner's life from its samples (a lower
//   life at once; a higher one only when no hit of ours is in flight).
// - The owner's samples also carry rotation and velocity; a copy is placed
//   where the owner's enemy was 100 ms ago (Poses, every frame), so it stands
//   and moves where the owner's does, whatever its own AI wants.
// - With hurt on, a life the copy must lose (and DAMAGE on the owner) goes
//   through the game's own per-frame damage field, so the game plays its hit
//   reaction; a life that is not taken within 400 ms is written directly.
#pragma once

#include <array>
#include <deque>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopObjects {

// One actor of one walk.
struct Seen {
    std::uint64_t address = 0;  // the actor, in guest memory
    std::string name;           // the game's actor name
    std::uint64_t hash = 0;     // placement hash; 0 or ~0 when the game made it
    std::optional<coop::protocol::ObjectLife> life;
    coop::protocol::Vec3 position{};
    std::optional<std::array<float, 9>> rotation;  // actor +0x328, rows of 3
    std::optional<coop::protocol::Animation> animation;  // AS slot 0
};

// An animation a copy should play (the owner's AS slot 0 command).
struct PlayWish {
    std::uint64_t address = 0;
    std::string name;
    float rate = 1.0f;
};

using Mat3 = std::array<float, 9>;

// A rotation matrix (rows of 3) as a unit quaternion (x, y, z, w), and back.
coop::protocol::Quat QuatOf(const Mat3& m);
Mat3 MatOf(const coop::protocol::Quat& q);

// One-time pickups (docs/items.md): a 64-bit placement key that another game
// set to 1 removes the actor with that placement hash here (a Gold Skulltula
// and its Obj_RupeeToken), so it cannot be taken twice.

// A pose to write into a copy (+0x31c and +0x328).
struct PoseWrite {
    std::uint64_t address = 0;
    coop::protocol::Vec3 position{};
    std::optional<Mat3> rotation;
};

// A life to write into an actor (its +0x1c10 life value).
struct LifeWrite {
    std::uint64_t address = 0;
    std::int32_t life = 0;  // the life it should have
    // Above 0: give it this much damage through the game's own damage field
    // (Life + 0x228) instead of writing life, so the game reacts to the hit.
    std::int32_t hurt = 0;
};

// Shared: enemies, animals (a cucco is Animal_Cucco), townspeople (Npc_, but
// not Navi, held equipment or invisible helpers) and the breakable world
// objects (coop_world.h).
bool IsShared(std::string_view name);

// The id of an enemy the scene made without a placement hash: the scene, the
// actor name and its place in the order such enemies of that name appeared
// in this scene visit (first seen first; the same walk ordered by x, then z).
// Both games load a scene's own enemies the same way, so they match; enemies
// made near one player (a Stalchild under one game's Link) do not, and stay
// each game's own. The top bit is set, the rest is FNV-1a 64.
std::uint64_t SceneSpawnId(std::string_view scene, std::string_view name, unsigned order);

class Sync {
public:
    static constexpr std::uint64_t kPresentEveryUs = 2'000'000;
    static constexpr std::uint64_t kSampleEveryUs = 50'000;     // 20 a second
    static constexpr std::uint64_t kStillEveryUs = 1'000'000;
    static constexpr std::uint64_t kDamageSumUs = 100'000;
    static constexpr std::uint64_t kHitHoldUs = 500'000;        // no healing back after our hit
    static constexpr std::uint64_t kFightUs = 12'000'000;
    static constexpr std::uint64_t kBehindUs = 100'000;       // copies show the owner this late
    static constexpr std::uint64_t kExtrapolateUs = 150'000;
    static constexpr std::uint64_t kPoseStaleUs = 2'500'000;  // no sample this long: let it be
    static constexpr std::uint64_t kHurtWaitUs = 400'000;     // then write life directly
    static constexpr std::size_t kKeepSamples = 16;

    // Hurt through the game's damage field (LifeWrite::hurt); off: write life.
    void SetHurt(bool on) { hurt_ = on; }

    // Network thread: OBJECT_OWNER, OBJECT_SAMPLE, HANDOVER_ASK,
    // HANDOVER_STATE, DAMAGE. Others are ignored. Applied on the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, once per walk. `self` is our player number (1 to 4),
    // `now_us` the server time. A new scene forgets every object.
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
              const std::vector<Seen>& seen, std::vector<coop::protocol::Message>& send,
              std::vector<LifeWrite>& writes);

    // Present thread, before Tick: our own Link's position, when the walk saw
    // it. A breakable that leaves the list within kBreakNearM of it after it
    // was moved (lifted, carried, thrown), or within kBreakReachM unmoved,
    // was broken: a thrown rock, and a crate cut by the sword, are removed
    // with their life never read at 0. One that leaves further away was
    // unloaded.
    void SetPlayer(std::optional<coop::protocol::Vec3> player) { player_ = player; }
    static constexpr float kBreakNearM = 8.0f;
    static constexpr float kBreakReachM = 3.0f;  // unmoved: within sword reach
    static constexpr int kMissedWalks = 2;
    // Present thread, every frame: where each copy we do not own should be.
    void Poses(std::uint64_t now_us, std::vector<PoseWrite>& out) const;

    // Present thread: for each copy we do not own, the owner's latest
    // animation, when the copy shows another one.
    void Plays(std::uint64_t now_us, std::vector<PlayWish>& out) const;

    // The loaded actors whose pickup another game took: to delete.
    std::vector<std::uint64_t> Removes() const;

    // Present thread: a placement key set to 1 by a flag message. Collect
    // asks this for actors that are not shared, to report them too.
    bool Taken(std::uint64_t hash) const { return taken_.contains(hash); }

    // Present thread: not connected or the game went away. Forgets everything.
    void Reset();

    // For tests and logs.
    std::size_t Count() const { return objects_.size(); }
    // What Flush decided for each shared object that left the walk, as log
    // lines; taken (and cleared) by the caller.
    std::vector<std::string> TakeNotes() { return std::exchange(notes_, {}); }
    std::optional<coop::protocol::ObjectId> IdOf(std::uint64_t address) const;
    // The actors of the enemies we own, with their names (for the test drive).
    std::vector<std::pair<std::uint64_t, std::string>> Owned() const;
    std::uint8_t OwnerOf(const coop::protocol::ObjectId& id) const;
    std::size_t HurtsTaken() const { return hurt_taken_; }
    std::size_t HurtsMissed() const { return hurt_missed_; }

private:
    struct Owner {
        std::uint8_t player = 0;
        std::uint16_t epoch = 0;
    };
    struct Received {
        std::uint64_t time = 0;
        coop::protocol::Vec3 position{};
        std::optional<coop::protocol::Quat> rotation;
        std::optional<coop::protocol::Vec3> velocity;
        std::optional<coop::protocol::Animation> animation;
    };
    struct Object {
        coop::protocol::ObjectId id;
        std::uint64_t address = 0;
        std::optional<std::int32_t> life;  // as last read or written
        std::int32_t max = 0;
        coop::protocol::Vec3 position{};
        std::optional<Mat3> rotation;
        std::optional<coop::protocol::Animation> animation;
        coop::protocol::Vec3 velocity{};
        std::uint64_t seen_at = 0;
        std::string name;
        std::uint64_t fight_until = 0;
        // Owner side.
        std::uint64_t sent_at = 0;
        bool sent_once = false;
        std::optional<coop::protocol::ObjectLife> sent_life;
        coop::protocol::Vec3 sent_position{};
        std::optional<coop::protocol::Quat> sent_rotation;
        std::string sent_animation;
        std::uint32_t sequence = 0;
        // Copy side.
        std::int32_t hit_sum = 0;
        std::uint64_t hit_since = 0;
        std::uint64_t hit_at = 0;  // last DAMAGE sent
        std::deque<Received> received;  // the owner's samples, oldest first
        // A hurt asked of the game: drops up to this much are not our hits.
        std::int32_t hurt_left = 0;
        std::int32_t hurt_target = 0;
        std::uint64_t hurt_until = 0;
        bool breakable = false;                 // coop_world.h
        coop::protocol::Vec3 first_position{};  // where it was first seen
        int missed = 0;                         // walks in a row without it
    };

    // Takes `o` down to `life`: a hurt when hurt is on, else a write.
    void Lower(Object& o, std::int32_t life, std::uint64_t now_us, std::vector<LifeWrite>& writes);

    void Flush(std::uint8_t self, std::uint64_t now_us, std::vector<Object>& gone,
               std::vector<coop::protocol::Message>& send);
    void Apply(std::uint8_t self, std::uint64_t now_us, std::vector<coop::protocol::Message>& send,
               std::vector<LifeWrite>& writes);
    coop::protocol::ObjectState StateOf(Object& o, std::uint64_t now_us);

    std::string scene_;
    std::optional<coop::protocol::Vec3> player_;
    std::vector<std::string> notes_;
    std::map<std::uint64_t, Object> objects_;  // by actor address
    std::map<std::string, unsigned> order_;    // next order per name, this scene visit
    std::map<coop::protocol::ObjectId, Owner> owners_;
    std::vector<coop::protocol::ObjectPresence> present_sent_;
    std::uint64_t present_at_ = 0;
    bool present_once_ = false;
    bool hurt_ = false;
    std::uint8_t self_ = 0;
    std::set<std::uint64_t> taken_;             // key64 flags seen set to 1
    std::set<std::uint64_t> hidden_;  // loaded actors whose pickup was taken
    std::size_t hurt_taken_ = 0;   // hurts the game took as asked
    std::size_t hurt_missed_ = 0;  // hurts written directly after the wait

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

} // namespace CoopObjects
