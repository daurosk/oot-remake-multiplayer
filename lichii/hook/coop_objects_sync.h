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

// An animation a copy should play (the owner's AS slot 0 command). The
// mover checks the copy's slot 0 every game frame: another name is played
// again at once, and `frame` is set after it (and again when a new sample
// finds the copy more than a few frames off), so the copy's own AI cannot
// pull it back for long and a replay never restarts it from frame 0.
struct PlayWish {
    std::uint64_t address = 0;
    std::string name;
    float rate = 1.0f;
    // The owner's frame where the copy is shown (kBehindUs ago), from the
    // latest sample: its frame plus rate * 30 a second since it was taken.
    float frame = 0.0f;
    std::uint64_t sample = 0;  // the sample's time: a new one checks the frame
};

// The frame of an animation `age_us` after it showed `frame` at `rate`
// (30 game frames a second); never below 0.
float FrameAfter(float frame, float rate, std::int64_t age_us);

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
    // With a hurt: where the player who hit stands, when known (the enemy is
    // knocked away from there).
    std::optional<coop::protocol::Vec3> from;
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

// Enemies the game rolls near a player (docs/objects.md, "Rolled spawns"):
// the night Stalchild the scene's AutoPlacementMgr makes rise out of the
// ground around Link. Only one game in a scene rolls them (the decider, the
// lowest player number there); it shares each with OBJECT_SPAWN of a made id.
// The others delete their own rolls and create the decided ones.
bool IsRolled(std::string_view name);

// A placed actor that may come into being in one game only, after the scene
// loaded: the Gold Skulltula a player shakes out of a tree. Its owner names
// it with OBJECT_SPAWN of its placed id; a game without that placement makes
// a copy, and deletes the copy if its own one turns up.
bool IsAnnounced(std::string_view name);

// Enemies made of several actors (docs/objects.md, "Groups"): a boss and its
// parts, an enemy and what it summons. Each part follows a root of one of the
// names `roots` (the first name found wins, then the nearest), within `reach`
// metres (0: anywhere in the scene). `boss`: the group's fight lock is a
// boss's (60 s).
struct GroupPart {
    std::vector<std::string_view> roots;
    float reach = 0.0f;
    bool boss = false;
};
// The rule for a part, or null when `name` is no part.
const GroupPart* PartOf(std::string_view name);
// A boss or mini-boss on its own (Enemy_DungeonBoss_*, Enemy_MiddleBoss_*,
// and the roots of boss groups); never a picture book _Dictionary actor.
bool IsBoss(std::string_view name);

// One shared actor of a walk, for GroupsOf.
struct GroupInput {
    coop::protocol::ObjectId id;
    std::string_view name;
    coop::protocol::Vec3 position{};
};
// The OBJECT_GROUP entries for one walk, sorted by member: a part with its
// root, a part whose root is not loaded with itself (so it follows nobody), a
// boss with itself and the boss flag. Empty when the walk has none.
std::vector<coop::protocol::ObjectGroupEntry> GroupsOf(const std::vector<GroupInput>& walk);

// A rolled enemy another game decided, for the spawn director to create here
// by name where the decider's one is. The Mover places and animates it after.
struct EnemyCopy {
    std::uint64_t key = 0;  // unique for this run; the director's key
    std::string name;
    coop::protocol::Vec3 position{};
    bool removed = false;   // the maker's one is gone: delete ours
};

class Sync {
public:
    static constexpr std::uint64_t kPresentEveryUs = 2'000'000;
    static constexpr std::uint64_t kSampleEveryUs = 50'000;     // 20 a second
    static constexpr std::uint64_t kStillEveryUs = 1'000'000;
    static constexpr std::uint64_t kDamageSumUs = 100'000;
    // No healing back after our hit: the DAMAGE leaves 100 ms later, the
    // owner's game may take 400 ms over it, then its sample comes back.
    static constexpr std::uint64_t kHitHoldUs = 1'000'000;
    // After a hurt was written as life, a fall the game still makes for it.
    static constexpr std::uint64_t kHurtLateUs = 1'000'000;
    // After our own hit on a copy its game plays the hit reaction itself: the
    // owner's animation from before the hit is not played over it this long.
    static constexpr std::uint64_t kReactHoldUs = 700'000;
    static constexpr std::uint64_t kPlaceStaleUs = 2'000'000;   // another player's place
    static constexpr std::uint64_t kFightUs = 12'000'000;
    static constexpr std::uint64_t kBehindUs = 100'000;       // copies show the owner this late
    static constexpr std::uint64_t kExtrapolateUs = 150'000;
    static constexpr std::uint64_t kPoseStaleUs = 2'500'000;  // no sample this long: let it be
    static constexpr std::uint64_t kHurtWaitUs = 400'000;     // then write life directly
    static constexpr std::size_t kKeepSamples = 16;
    static constexpr std::uint64_t kSceneStaleUs = 5'000'000;  // a player's scene, from its samples
    static constexpr float kClaimDistance = 4.0f;  // a fresh roll this near a copy being made is it
    static constexpr std::uint64_t kCreateWaitUs = 3'000'000;  // then a roll near it is ours to delete
    static constexpr std::uint64_t kLateUs = 3'000'000;  // after the scene's first walk: came later

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
    // animation and its frame (the mover compares it with the copy's own).
    void Plays(std::uint64_t now_us, std::vector<PlayWish>& out) const;

    // Present thread: the enemy copies whose own AI is held (the AI hold,
    // coop_hold.h): an Enemy_ we do not own that its owner's samples place
    // right now, so exactly the enemies Poses moves. One that becomes ours
    // (or whose owner fell silent) is not in the next list.
    std::vector<std::uint64_t> Holds(std::uint64_t now_us) const;

    // The loaded actors to delete: pickups another game took, and rolled
    // enemies this game made while another one decides the scene's rolls.
    std::vector<std::uint64_t> Removes() const;
    // Present thread, before Tick: the enemy copies the director made (key
    // to actor), so a fresh one on the list gets the decided id.
    void Bind(const std::map<std::uint64_t, std::uint64_t>& live);
    // Present thread, after Tick: the decided enemies to create here (and the
    // ones to delete again).
    std::vector<EnemyCopy> Creates() const;
    // The player who rolls enemies in our scene (0 before the first Tick).
    std::uint8_t Decider() const { return decider_; }
    std::size_t Refused() const { return refused_count_; }

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
    std::size_t HurtsLate() const { return hurt_late_; }

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
        bool late = false;       // a placed IsAnnounced actor first seen after kLateUs
        bool announced = false;  // its OBJECT_SPAWN went out
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
        std::uint64_t react_at = 0;  // our last hit on it, as seen in its life
        std::string react_over;      // the owner's animation at that moment
        std::deque<Received> received;  // the owner's samples, oldest first
        // A hurt asked of the game: drops up to this much are not our hits.
        std::int32_t hurt_left = 0;
        std::int32_t hurt_target = 0;
        std::uint64_t hurt_until = 0;
        // A hurt written as life after the wait: a late fall of up to this
        // much is the game taking that same hit.
        std::int32_t late_left = 0;
        std::uint64_t late_until = 0;
        bool breakable = false;                 // coop_world.h
        bool reacts = false;                    // an enemy: hurts go through the game
        coop::protocol::Vec3 first_position{};  // where it was first seen
        int missed = 0;                         // walks in a row without it
    };

    // Takes `o` down to `life`: a hurt when hurt is on, else a write.
    void Lower(Object& o, std::int32_t life, std::uint64_t now_us, std::vector<LifeWrite>& writes,
               std::uint8_t hitter);

    void Flush(std::uint8_t self, std::uint64_t now_us, std::vector<Object>& gone,
               std::vector<coop::protocol::Message>& send);
    void Apply(std::uint8_t self, std::uint64_t now_us, std::vector<coop::protocol::Message>& send,
               std::vector<LifeWrite>& writes);
    coop::protocol::ObjectState StateOf(Object& o, std::uint64_t now_us);

    // A rolled enemy another game decided, from its OBJECT_SPAWN.
    struct Decided {
        coop::protocol::ObjectId id;
        std::string name;
        coop::protocol::Vec3 position{};
        std::uint64_t got_us = 0;
        std::uint64_t address = 0;  // once the director made it and it was seen
        bool removed = false;
    };
    std::uint8_t DeciderOf(std::uint8_t self, std::uint64_t now_us) const;
    std::string scene_;
    std::uint64_t scene_since_ = 0;  // server time of the scene's first walk
    std::optional<coop::protocol::Vec3> player_;
    std::vector<std::string> notes_;
    std::map<std::uint64_t, Object> objects_;  // by actor address
    std::map<std::uint64_t, Decided> decided_;  // by director key
    std::map<std::uint64_t, std::uint64_t> bound_;  // actor to director key
    std::uint64_t next_key_ = 1;
    std::uint64_t made_counter_ = 0;
    std::uint8_t decider_ = 0;
    std::set<std::uint64_t> refused_;  // our own rolls, to delete
    std::size_t refused_count_ = 0;
    // Each other player's scene and the server time of its latest sample
    // (guarded by inbox_mutex_: PLAYER_SAMPLE comes too often for the inbox).
    std::map<std::uint8_t, std::pair<std::string, std::uint64_t>> scenes_;
    // Where the other players stand (same guard), with the sample's server time.
    std::map<std::uint8_t, std::pair<coop::protocol::Vec3, std::uint64_t>> places_;
    std::map<std::string, unsigned> order_;    // next order per name, this scene visit
    std::map<coop::protocol::ObjectId, Owner> owners_;
    std::vector<coop::protocol::ObjectPresence> present_sent_;
    std::uint64_t present_at_ = 0;
    bool present_once_ = false;
    std::vector<coop::protocol::ObjectGroupEntry> groups_sent_;  // as last sent
    bool hurt_ = false;
    std::uint8_t self_ = 0;
    std::set<std::uint64_t> taken_;             // key64 flags seen set to 1
    std::set<std::uint64_t> hidden_;  // loaded actors whose pickup was taken
    std::size_t hurt_taken_ = 0;   // hurts the game took as asked
    std::size_t hurt_late_ = 0;    // late falls put back
    std::size_t hurt_missed_ = 0;  // hurts written directly after the wait

    mutable std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

} // namespace CoopObjects
