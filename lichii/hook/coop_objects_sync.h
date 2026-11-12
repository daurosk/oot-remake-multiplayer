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
#include <functional>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <tuple>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_guard.h"

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
    // Issue 150: a boss's other AS slots (Gohma's eye guard, eyelids),
    // read only for bosses (IsBoss); empty otherwise.
    std::vector<coop::protocol::SlotAnimation> slots;
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
    // Issue 150: the AS slot (0: the whole body). Other slots are a boss's
    // (Gohma's eye guard): played and framed the same way, never counted
    // as the copy's own AI taking over.
    std::int32_t slot = 0;
    // Not empty: played again only over an animation with this in its name
    // (a chest shown open, issue 195: only over its closed idle "CloseWait",
    // never over its own opening when our Link opens it here).
    std::string only_over;
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
// objects (coop_world.h). Not the order-puzzle actors (IsOwnPuzzle).
bool IsShared(std::string_view name);

// A puzzle or minigame each game runs on its own (issues 149, 151): the Scrub Brothers
// (hit 2, 3, 1), the Lost Woods stage scrubs, and the fishing pond's owner.
// Their state is kept in the game's own code from its own player; only the
// solve or the prize is shared (docs/dungeon.md, docs/items.md). The pond's
// fish are shared (IsFish).
bool IsOwnPuzzle(std::string_view name);

// Issue 151 (fishing-2, fishing-3): a fish (Animal_Fish_*). Shared like any animal, with
// one owner game; the lure rules in Sync (SetLures, FISH_LURE) move a fish to the game
// whose lure is nearest it, and a fish its owner catches is gone for everyone.
bool IsFish(std::string_view name);

// Issue 191: guards (docs/objects.md, "Guards"). The stealth guards that throw
// a child out when they spot him (Npc_Hylia_Soldier_SecurityGuard in the
// castle garden, Npc_Gerudo_GEN_SecurityGuard and Npc_Gerudo_FortressGuard),
// and the town soldiers (Npc_Hylia_Soldier_*, Npc_Hylia_ImperialSoldier_*),
// who salute, greet and turn to Link. Shared like any townsperson, with one
// owner; a copy that reacts to our Link (IsGuardReaction) is handed to us.
bool IsGuard(std::string_view name);

// A guard animation that answers this game's Link and no other: noticing,
// discovering, surprise, alert, found (the stealth guards' 'Notice',
// 'NoticeTurn_Quick', 'Discover', 'Surprise'), and the town soldiers'
// 'Salute', greeting and talk. Not walking, waiting or turning on a patrol,
// which differ between games only by timing.
bool IsGuardReaction(std::string_view name);

// True for an actor that is gone, or shows done, once its placement key is
// set: chests, pickups (rupees, hearts, heart pieces and containers, silver
// rupees, Gold Skulltulas and tokens), spider webs, red and blue ice,
// cracked walls and floors, broken rocks and the Deku Tree's falling
// floors (the Shadow Temple's stays as ground once fallen). Only these are
// removed when another player sets their key (Sync::Take). Everything else
// with a key (enemy and silver rupee counters, the Fire Temple's hammer
// pillars, torches, waterfalls, paintings, enemies) changes state instead of
// going away, so its actor stays; the room shows it done when it loads again
// (issue 138).
bool RemovedWhenTaken(std::string_view name);

// Issue 109: whether a boss is alive in one walk, from its body actors
// `bodies` (Twinrova: Koume, Kotake and Twinrova; never its eggs, minions or
// shots). True when one of them is there with life left; false when none is
// there, or every one there is at 0 life; nothing when one is there whose
// life was not read (unknown: the boss logic then keeps its old rule).
std::optional<bool> BossAlive(const std::vector<Seen>& seen, std::span<const std::string_view> bodies);

// The id of an enemy the scene made without a placement hash: the scene, the
// actor name and its place in the order such enemies of that name appeared
// in this scene visit (first seen first; the same walk ordered by x, then z).
// Both games load a scene's own enemies the same way, so they match; enemies
// made near one player (a Stalchild under one game's Link) do not, and stay
// each game's own. The top bit is set, the rest is FNV-1a 64.
std::uint64_t SceneSpawnId(std::string_view scene, std::string_view name, unsigned order);

// Enemies the game rolls near a player (docs/objects.md, "Rolled spawns"):
// the night Stalchild the scene's AutoPlacementMgr makes rise out of the
// ground around Link. Where players are together only one game rolls them (the
// lowest player number whose Link is within Sync::kRollNearM of the roll); it
// shares each with OBJECT_SPAWN of a made id. The others delete their own rolls
// there and create the decided ones. A player with no lower-numbered player
// near keeps and shares its own rolls (issue 124: player 2 far from player 1
// in the night field got none).
bool IsRolled(std::string_view name);

// Multi-stage enemies (gohma-larva): an enemy whose next stage is another
// actor. A Gohma egg (Enemy_Gohma_Egg, Enemy_DungeonBoss_Gohma_Egg) hatches:
// the egg actor goes, alive, and an Enemy_Gohma_Larva is made where it lay,
// only in the game that runs the egg (its owner; a copy is held in the
// owner's animation). That game shares the larva with OBJECT_SPAWN of a made
// id, like a rolled enemy, and ends the egg with OBJECT_REMOVE reason gone,
// so the other games delete their egg copy and make the larva.
bool IsHatchingEgg(std::string_view name);
bool IsHatchling(std::string_view name);

// A placed actor that may come into being in one game only, after the scene
// loaded: the Gold Skulltula a player shakes out of a tree. Its owner names
// it with OBJECT_SPAWN of its placed id; a game without that placement makes
// a copy, and deletes the copy if its own one turns up.
bool IsAnnounced(std::string_view name);

// An enemy animation that plays as it dies ("Die", "Dead_Fall",
// "Damage_Death"): a word Die, Dead or Death at the start or after '_'. An
// owned enemy that leaves the walk while playing one died, even when its life
// could not be read (the big Skulltula has none we can read; issue 133).
bool IsDeathAnimation(std::string_view name);

// Issue 150: a boss animation that plays while it is stunned or knocked down
// ("Stun_Lp", "Down_Start", "Fall_St" when Gohma drops from the ceiling): the
// first word, LOD suffix off, is Stun, Down, Fall or Faint. A seed in Gohma's
// eye stuns it and takes no life, so a stun is the only sign of that hit.
bool IsStunAnimation(std::string_view name);

// An animation name without its level-of-detail suffix ("Wait_ModelOnlyLOD"
// is "Wait"), so a far copy's LOD variant is not another animation.
std::string_view AnimationBase(std::string_view name);

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

// Issue 88: the hearts the game draws come from our Link's Life component,
// not from the shared PlayerStatus.MaxLife flag. The max (quarter hearts) to
// write into the component: the flag when another player raised it above
// ours (at most 20 hearts), else nothing. Never lowers it.
inline constexpr std::int32_t kMostLifeQuarters = 20 * 4;
std::optional<std::int32_t> RaisedMaxLife(std::int32_t link_max, std::int32_t flag);

// One shared actor of a walk, for GroupsOf.
struct GroupInput {
    coop::protocol::ObjectId id;
    std::string_view name;
    coop::protocol::Vec3 position{};
};
// A part keeps the root it was linked to while within this many times the
// rule's reach, so one near the edge does not flip between groups.
inline constexpr float kGroupKeep = 1.25f;
// The OBJECT_GROUP entries for one walk, sorted by member: a part with its
// root, a boss with itself and the boss flag, and a part that `linked` (the
// links of the last call, member to root) had linked but whose root is now
// gone or out of reach with itself, once, so it follows nobody. A part never
// linked here gets no entry. Empty when the walk has none.
std::vector<coop::protocol::ObjectGroupEntry> GroupsOf(
    const std::vector<GroupInput>& walk,
    const std::map<coop::protocol::ObjectId, coop::protocol::ObjectId>& linked = {});

// A rolled enemy another game decided, for the spawn director to create here
// by name where the decider's one is. The Mover places and animates it after.
struct EnemyCopy {
    std::uint64_t key = 0;  // unique for this run; the director's key
    std::string name;
    coop::protocol::Vec3 position{};
    bool removed = false;   // the maker's one is gone: delete ours
    // Our copy went by itself while the maker's one lives (a Stalchild's own
    // AI sinks it, issue 124): how often the director makes it again. 0 once
    // our copy was seen dead.
    std::uint8_t remakes = 0;
};

// The server repeats OBJECT_OWNER for every shared object about every 10 s.
// Changed() is true only when an object's owner or epoch differs from what
// was seen last, so the log shows changes, not repeats (issue 111).
// Owner, epoch and group (enemy-sync-3, issue 105: a part joining its
// boss's group changes only the group, and is logged too).
class OwnerLog {
public:
    bool Changed(const coop::protocol::ObjectOwner& o) {
        if (last_.size() >= kMax && !last_.contains(o.id)) last_.clear();
        const auto now = std::make_tuple(o.owner, o.epoch, o.group);
        const auto [it, added] = last_.try_emplace(o.id, now);
        if (added) return true;
        if (it->second == now) return false;
        it->second = now;
        return true;
    }
    void Clear() { last_.clear(); }

private:
    static constexpr std::size_t kMax = 4096;
    std::map<coop::protocol::ObjectId, std::tuple<std::uint8_t, std::uint16_t, std::uint32_t>> last_;
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
    // Issue 150: a boss copy that went into a stun of its own sends a DAMAGE
    // of 0 (the server hands the boss to the stunner) at most this often.
    static constexpr std::uint64_t kStunEveryUs = 2'000'000;
    static constexpr std::uint64_t kPlaceStaleUs = 2'000'000;   // another player's place
    static constexpr std::uint64_t kFightUs = 12'000'000;
    static constexpr std::uint64_t kBehindUs = 100'000;       // copies show the owner this late
    static constexpr std::uint64_t kExtrapolateUs = 150'000;
    static constexpr std::uint64_t kPoseStaleUs = 2'500'000;  // no sample this long: let it be
    static constexpr std::uint64_t kHurtWaitUs = 400'000;     // then write life directly
    static constexpr std::size_t kKeepSamples = 16;
    static constexpr std::uint64_t kSceneStaleUs = 5'000'000;  // a player's scene, from its samples
    static constexpr float kClaimDistance = 4.0f;  // a fresh roll this near a copy being made is it
    // A roll this near a lower-numbered player's Link is that player's to make
    // (issue 124). Stalchild rise within about 20 m of the Link they rise for.
    static constexpr float kRollNearM = 40.0f;
    static constexpr std::uint64_t kCreateWaitUs = 3'000'000;  // then a roll near it is ours to delete
    static constexpr std::uint8_t kRemakes = 3;  // EnemyCopy::remakes for a live one
    static constexpr std::uint64_t kLateUs = 3'000'000;  // after the scene's first walk: came later
    // A hatchling first seen this near an egg we own (still on the walk, or
    // gone alive at most kHatchWindowUs before) came out of it.
    static constexpr float kHatchReachM = 3.0f;
    static constexpr std::uint64_t kHatchWindowUs = 2'000'000;
    std::uint64_t Hatched() const { return hatched_count_; }

    // Hurt through the game's damage field (LifeWrite::hurt); off: write life.
    void SetHurt(bool on) { hurt_ = on; }

    // Projectiles (coop_projectiles.h): true for a life drop at a place and
    // server time that a projectile copy's blast here may have made. A copy
    // bomb is the game's own bomb, so the owner's real bomb and our copy both
    // hit. Such a drop on a copy is not sent to the owner (its own bomb hits
    // its enemy); on an enemy we own it is kept as credit for kBlastCreditUs,
    // and a DAMAGE that comes for the same bomb takes the credit first. A
    // DAMAGE taken first is kept as debit for the same time; a blast drop
    // after it is written back up by that much.
    using BlastNear = std::function<bool(const coop::protocol::Vec3&, std::uint64_t)>;
    void SetBlastNear(BlastNear near) { blast_near_ = std::move(near); }
    static constexpr std::uint64_t kBlastCreditUs = 2'000'000;
    std::size_t BlastDrops() const { return blast_drops_; }  // drops kept back so far

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

    // Issue 151. Present thread, before Tick: where our own cast lures are
    // (CoopProjectiles::TheLures). Each Tick sends FISH_LURE (fishing-3):
    // every fish in the walk with how near our nearest lure is (within
    // kLureReachM), hooked when we own it and it is within kLureHookM, drawn
    // when we own it and it comes at our lure at kLureSpeed or more, or when
    // it is a copy whose own AI swims it at our lure (kLureStepM or more a
    // frame, or held within kLureHookM) for kChaseHoldUs. The server gives the
    // fish to the nearest lure's game, so that game runs the approach, the
    // bite and the fight; a fish is never flagged targeting or fighting (a
    // fight lock would hold it in the wrong game). A fish we own that leaves
    // the walk within kCatchNearM of our Link was caught here (landed, in
    // Link's hands): OBJECT_REMOVE dead, and every other game deletes its copy
    // at once.
    void SetLures(std::vector<coop::protocol::Vec3> lures) { lures_ = std::move(lures); }
    static constexpr float kLureReachM = 6.0f;
    static constexpr float kLureStepM = 0.005f;
    static constexpr float kLureHookM = 1.0f;
    static constexpr float kLureSpeed = 0.2f;  // metres a second
    // fishing-3: FISH_LURE (the server gives each fish to the game whose lure
    // is nearest). Sent on a change at most this often (a change of hooked or
    // drawn at once), and with every presence. Distances in kFishStepCm steps.
    static constexpr std::uint64_t kFishEveryUs = 250'000;
    static constexpr std::uint32_t kFishStepCm = 25;
    static constexpr std::uint64_t kFishRepeatUs = 1'000'000;
    static constexpr float kCatchNearM = 3.0f;
    static constexpr float kBreakNearM = 8.0f;
    static constexpr float kBreakReachM = 3.0f;  // unmoved: within sword reach
    static constexpr int kMissedWalks = 2;

    // Present thread, every frame, before the Mover puts the copies back
    // (issue 101): `put` is where the Mover put the actor at `address` last
    // frame, `now` where the game's own frame left it. An enemy copy we do
    // not own whose own AI pulls it towards our Link (kChaseStepM or more a
    // frame, within kChaseReachM) with no gap over kChaseGapUs for
    // kChaseHoldUs is chasing us: OBJECT_PRESENT then flags it targeting for
    // kChaseKeepUs, and the server gives it to us. An enemy we own that comes
    // towards our Link at kChaseSpeed or more is flagged the same way, which
    // the server takes as a fight, so it stays ours.
    // A life write this Sync asked for earlier, made in the game only now (it waited
    // while our Link was in an event, coop_guard.h, issue 144). The drop to `life` that the
    // next walks see is that write landing, not a hit of our Link's: not sent as DAMAGE.
    void Written(std::uint64_t address, std::int32_t life, std::uint64_t now_us);
    void NoteDrift(std::uint64_t address, const coop::protocol::Vec3& put,
                   const coop::protocol::Vec3& now, std::uint64_t now_us);
    static constexpr float kChaseReachM = 40.0f;
    static constexpr float kChaseStepM = 0.02f;
    static constexpr float kChaseSpeed = 1.0f;  // metres a second
    static constexpr std::uint64_t kChaseHoldUs = 1'000'000;
    static constexpr std::uint64_t kChaseGapUs = 400'000;
    static constexpr std::uint64_t kChaseKeepUs = 2'000'000;

    // Issue 132: an enemy that does not move to fight (a Deku Baba rising
    // out of the ground, a big Skulltula dropping) shows no drift. Its copy
    // still wants to act: within kWakeM of our Link, its own AI plays another
    // animation than the owner's latest. Seen so in walks with no gap over
    // kChaseGapUs for kChaseHoldUs, it is flagged targeting like a chase, and
    // the server gives it to us. An enemy we own within kWakeM of our Link
    // that plays another animation than the one it rested in when first seen
    // is flagged too (a fight to the server), so it stays with the game whose
    // Link woke it. Off with SetWake(false) (LICHII_COOP_OBJECTS_WAKE=0).
    void SetWake(bool on) { wake_ = on; }
    static constexpr float kWakeM = 10.0f;
    // Issue 157: the Mover puts the owner's animation back on the very next
    // game frame, so a copy whose AI asks for its wake animation once (a
    // Peahat rising out of the ground) is rarely seen in it by the walk.
    // The Mover tells each such put-back (NoteTakeover, present thread); the
    // copy then counts as playing its own animation for kWakeLatchUs.
    // `name` is what the copy's own AI played (a guard is taken only for an
    // IsGuardReaction one).
    void NoteTakeover(std::uint64_t address, std::uint64_t now_us, std::string_view name = {});
    static constexpr std::uint64_t kWakeLatchUs = 1'200'000;
    // The owner side flags a woken enemy only this long after it left its
    // rest animation, so an enemy that patrols or flies near its owner's
    // Link is not held in a fight for good (that would stop the chase rule).
    static constexpr std::uint64_t kWakeOwnUs = 4'000'000;
    // Issue 191: a guard sees further than an enemy wakes; its reaction to
    // our Link (IsGuardReaction) counts within this, the patrol never does.
    static constexpr float kGuardWakeM = 25.0f;
    // A copy's kill counts for a resend this long after our hit took it to 0.
    static constexpr std::uint64_t kKilledKeepUs = 5'000'000;

    // Issue 133. Hits on an enemy whose owner is not known yet are kept this
    // long and sent once it is (another game's), dropped if it is ours.
    static constexpr std::uint64_t kHitKeepUs = 2'000'000;
    // A copy we killed here that the owner's samples still show alive after
    // kHitHoldUs: the DAMAGE is sent again for the life the owner shows, at
    // most this many times, and the copy is not healed back.
    static constexpr int kResendMax = 2;
    // A copy whose owner said it died (OBJECT_REMOVE, reason dead) and that
    // is still on our walk this long after is deleted with the game's delete
    // request (its own death, if the life we wrote started one, is done).
    static constexpr std::uint64_t kDeadGraceUs = 3'000'000;
    // Present thread, every frame: where each copy we do not own should be.
    void Poses(std::uint64_t now_us, std::vector<PoseWrite>& out) const;

    // Present thread: for each copy we do not own, the owner's latest
    // animation and its frame (the mover compares it with the copy's own).
    void Plays(std::uint64_t now_us, std::vector<PlayWish>& out) const;

    // Present thread: a chest's open idle from Plays was not sent this frame
    // (CoopGuard holds plays near our Link in an event). Its show-open clock
    // starts again, so a long event never turns it into a remove (review of
    // chest-sync-2). Other addresses are ignored.
    void ChestPlayHeld(std::uint64_t address, std::uint64_t now_us);
    // How many chests shown open here our Link then opened again (their
    // content may count twice), this session.
    unsigned ChestsOpenedTwice() const { return chests_opened_twice_; }

    // Present thread: the enemy copies whose own AI is held (the AI hold,
    // coop_hold.h): an Enemy_ we do not own that its owner's samples place
    // right now, so exactly the enemies Poses moves. One that becomes ours
    // (or whose owner fell silent) is not in the next list.
    std::vector<std::uint64_t> Holds(std::uint64_t now_us) const;

    // Present thread: the enemy copies that became ours since the last ask
    // (OBJECT_OWNER moved them to us from another player), by address.
    std::vector<std::uint64_t> TakeOursNow() { return std::exchange(ours_now_, {}); }

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

    // Any thread: the flag watcher wrote this placement key 0 to 1 for another
    // player (a chest opened, a pickup taken elsewhere; FlagWatcher::
    // take_written_for_others). The actors with that hash loaded in the next
    // two Ticks are removed here, unless our Link is within kTakeNearM; ones loaded after it are left alone, since the
    // game made them knowing the key (a chest shows open). Forgotten at a
    // scene change (docs/items.md, "Chests").
    void Take(std::uint64_t hash);
    // Present thread: a placement key taken in this scene. Collect asks this
    // for actors that are not shared, to report them too.
    bool Taken(std::uint64_t hash) const { return taken_.contains(hash); }

    // Present thread: not connected or the game went away. Forgets everything.
    void Reset();

    // Network thread, on a reconnect: the server's epochs may start again
    // at 0, so the owners we know of placed objects are forgotten at the
    // next Tick (issue 133). The server sends each owner again.
    // OBJECT_OWNERs of the old connection not yet applied go too, so none
    // lands after the forget with its old, higher epoch.
    void ForgetOwners() {
        std::lock_guard lock(inbox_mutex_);
        forget_owners_ = true;
        std::erase_if(inbox_, [](const coop::protocol::Message& m) {
            return std::holds_alternative<coop::protocol::ObjectOwner>(m);
        });
    }

    // For tests and logs.
    std::size_t Count() const { return objects_.size(); }
    // What Flush decided for each shared object that left the walk, as log
    // lines; taken (and cleared) by the caller.
    std::vector<std::string> TakeNotes() { return std::exchange(notes_, {}); }
    std::optional<coop::protocol::ObjectId> IdOf(std::uint64_t address) const;
    // The actors of the enemies we own, with their names (for the test drive).
    std::vector<std::pair<std::uint64_t, std::string>> Owned() const;
    // The player who owns the object at `address` (0: the server, none: not
    // known). For coop_enemy_shots.
    std::optional<std::uint8_t> OwnerOf(std::uint64_t address) const;
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
        std::vector<coop::protocol::SlotAnimation> slots;  // issue 150
    };
    struct Object {
        coop::protocol::ObjectId id;
        std::uint64_t address = 0;
        std::optional<std::int32_t> life;  // as last read or written
        std::int32_t max = 0;
        coop::protocol::Vec3 position{};
        std::optional<Mat3> rotation;
        std::optional<coop::protocol::Animation> animation;
        std::vector<coop::protocol::SlotAnimation> slots;  // issue 150: a boss's, as last read
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
        std::string sent_slots;  // issue 150: the slot names last sent
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
        // Owner side: life a copy bomb's blast took here (BlastNear).
        std::int32_t blast_credit = 0;
        std::uint64_t blast_until = 0;
        // Owner side: life a DAMAGE from another game took; a copy blast
        // that comes after it (the copy went off at the remove, one network
        // delay late) gives that much back, so the bomb hits once.
        std::int32_t damage_debit = 0;
        std::uint64_t debit_until = 0;
        bool breakable = false;                 // coop_world.h
        bool reacts = false;                    // an enemy: hurts go through the game
        bool fish = false;                      // IsFish: the lure rules
        bool guard = false;                     // IsGuard: handed over on a reaction
        coop::protocol::Vec3 first_position{};  // where it was first seen
        int missed = 0;                         // walks in a row without it
        // Chasing our Link (NoteDrift): the run of steps towards it.
        std::uint64_t chase_first = 0;
        std::uint64_t chase_last = 0;
        std::uint64_t chase_until = 0;
        // Issue 132: the animation it was first seen in (an owned enemy at rest).
        std::string rest_animation;
        std::uint64_t woke_at = 0;  // left its rest animation (0 = resting)
        std::uint64_t took_over_at = 0;  // issue 157: its own AI's play put back (NoteTakeover)
        std::string took_over_name;      // ... and what it was (a guard's, issue 191)
        // Our own hit took its life to 0 here (the DAMAGE resend's mark).
        std::uint64_t killed_at = 0;
        // Issue 133: resends of a DAMAGE for a copy we killed here, and when
        // its owner said it died (OBJECT_REMOVE dead; 0 = not said).
        int resent = 0;
        std::uint64_t dead_at = 0;
        bool dead_now = false;  // a kill from before we came: deleted at once
        // enemy-sync-3 (issue 133): our enemy, killed by another game's hit
        // that our game did not take, already said dead (OBJECT_REMOVE).
        bool dead_said = false;
        // An egg we own that a hatchling came out of: when it leaves the
        // walk, OBJECT_REMOVE reason gone goes out for it.
        bool hatched = false;
        // Issue 150: a boss copy whose own game stunned it (a DAMAGE of 0 is
        // due), and when that last went out.
        bool stun_due = false;
        std::uint64_t stun_at = 0;
    };

    // One step of `o` towards our Link at `now_us` (NoteDrift).
    static void ChaseStep(Object& o, std::uint64_t now_us);

    // Takes `o` down to `life`: a hurt when hurt is on, else a write.
    void Lower(Object& o, std::int32_t life, std::uint64_t now_us, std::vector<LifeWrite>& writes,
               std::uint8_t hitter);

    // enemy-sync-3 (issue 133): an enemy we own whose life was written to 0
    // (another game's hit our game did not take, or hurt off) may stand on
    // at life 0. Said dead to the others now, deleted here kDeadGraceUs
    // later if its own death has not taken it by then.
    void DeadByWrite(Object& o, std::uint16_t epoch, std::uint64_t now_us,
                     std::vector<coop::protocol::Message>& send, const char* why);

    void Flush(std::uint8_t self, std::uint64_t now_us, std::vector<Object>& gone,
               std::vector<coop::protocol::Message>& send);
    void Apply(std::uint8_t self, std::uint64_t now_us, std::vector<coop::protocol::Message>& send,
               std::vector<LifeWrite>& writes);
    coop::protocol::ObjectState StateOf(Object& o, std::uint64_t now_us);
    // gohma-larva: a fresh hatchling next to an egg we own (eggs_, or one
    // still on the walk, then marked hatched). Sends the egg's OBJECT_REMOVE
    // when it already left. True: the hatchling is ours to share.
    bool Hatch(std::uint8_t self, const Seen& s,
               std::vector<coop::protocol::Message>& send);
    // gohma-sync: an egg of ours still on the walk, marked hatched, near `at`.
    Object* HatchedSibling(std::uint8_t self, const coop::protocol::Vec3& at);

    // A rolled enemy another game decided, from its OBJECT_SPAWN.
    struct Decided {
        coop::protocol::ObjectId id;
        std::string name;
        coop::protocol::Vec3 position{};
        std::uint64_t got_us = 0;
        std::uint64_t address = 0;  // once the director made it and it was seen
        bool removed = false;
        // Our copy left the walk dead (life 0 or a death animation): the
        // director must not make it again (issue 124).
        bool dead = false;
    };
    std::uint8_t DeciderOf(std::uint8_t self, std::uint64_t now_us) const;
    // The game that keeps a roll that rose at `at`: the lowest player number in
    // our scene whose Link is within kRollNearM of it (or whose place we do not
    // know), else us.
    std::uint8_t RollerFor(std::uint8_t self, const coop::protocol::Vec3& at, std::uint64_t now_us) const;
    // Eggs we owned that left the walk alive, waiting kHatchWindowUs for the
    // hatchling that came out of them.
    struct Egg {
        coop::protocol::ObjectId id;
        std::uint16_t epoch = 0;
        coop::protocol::Vec3 position{};
        std::uint64_t at = 0;
    };
    std::vector<Egg> eggs_;
    std::uint64_t hatched_count_ = 0;
    std::string scene_;
    std::uint64_t scene_since_ = 0;  // server time of the scene's first walk
    std::optional<coop::protocol::Vec3> player_;
    std::vector<coop::protocol::Vec3> lures_;  // our own cast lures (SetLures)
    std::vector<coop::protocol::FishLureEntry> fish_sent_;  // the last FISH_LURE
    std::uint64_t fish_at_ = 0;
    // One fish's FISH_LURE entry: how near our nearest lure is, hooked (ours,
    // within kLureHookM) or drawn (ours coming at it at kLureSpeed, or a copy
    // whose own AI swims at it: NoteDrift's chase steps).
    coop::protocol::FishLureEntry FishClaim(const Object& o, std::uint8_t self,
                                            std::uint64_t now_us) const;
    // The nearest of our lures to `at` (metres), or nothing with none.
    std::optional<float> LureDistance(const coop::protocol::Vec3& at) const;
    std::vector<std::string> notes_;
    std::map<std::uint64_t, Object> objects_;  // by actor address
    std::vector<std::uint64_t> ours_now_;     // copies that became ours (TakeOursNow)
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
    std::map<coop::protocol::ObjectId, coop::protocol::ObjectId> group_links_;  // member to root
    bool hurt_ = false;
    std::uint8_t self_ = 0;
    // Take(): each key taken in this scene, and the actors that had it in the
    // two Ticks after it came (the first walk was collected before Taken()
    // knew the key); only those are removed.
    static constexpr unsigned kTakeBindTicks = 2;
    // A take is kept only when the Tick before came at most this long ago: one
    // that came while no walk ran (title screen, save load, scene load: the
    // join snapshot) is for actors the game makes knowing the key.
    static constexpr std::uint64_t kTakeGapUs = 1'500'000;
    // An actor this close to our Link is not removed (a pickup we may be
    // taking ourselves). Not for chests (issue 87): a chest still closed
    // this close to our Link goes at once, so our Link cannot open it a
    // second time; one ours was already opening when the flag came (both at
    // once) stays, logged, as its content may count twice.
    static constexpr float kTakeNearM = CoopGuard::kChestNearM;
    // A taken breakable that still has life gets life 0 first, so the game
    // plays its own break (a web, a cracked wall); deleted only if it still
    // stands this long after (a break with no life to write is deleted at once).
    static constexpr std::uint64_t kTakeBreakUs = 1'500'000;
    // A taken falling floor or web part (CoopWorld::IsFallTaken) stays this
    // long, so the fall (as the others' moves) or the break shows first.
    static constexpr std::uint64_t kTakeFallUs = 4'000'000;
    // A chest whose animation changed to one with "Open" in it this recently
    // when the take came is opening now: beside our Link, ours opened it too
    // (both at once); away from it, its AI reacted to the flag. Only a change
    // counts: a closed chest's own idle animation may have "Open" in its name
    // (integrate-p8: an untouched closed chest was taken for "both at once").
    static constexpr std::uint64_t kChestOpeningUs = 2'000'000;
    struct Taking {
        unsigned ticks = 0;
        std::set<std::uint64_t> addresses;
        bool near_said = false;
        std::map<std::uint64_t, std::uint64_t> broke_at;  // address: when life 0 was written
        std::uint64_t since_us = 0;          // when the take came
        std::set<std::uint64_t> shown_open;  // chests that showed themselves open
        std::map<std::uint64_t, std::string> bound_animation;  // each chest's animation when bound
        std::set<std::uint64_t> show_failed;  // chests the open idle never took on: removed
    };
    // Every loaded chest's animation and when it last changed (0: the same
    // since it was first seen), for the opening tests above.
    struct ChestAnimation {
        std::string name;
        std::uint64_t changed_us = 0;
    };
    std::map<std::uint64_t, ChestAnimation> chest_animations_;
    // Issue 195: a loaded closed chest that another player opened is shown
    // open here with its own open idle (kChestOpenIdle, through Plays), not
    // removed (it vanished in Caleb's play-test). Address: when first played.
    // Removed only if it still shows its closed idle kChestShowFailUs after.
    static constexpr const char* kChestOpenIdle = "OpenWait";
    static constexpr const char* kChestClosedIdle = "Close";  // "CloseWait"
    static constexpr std::uint64_t kChestShowFailUs = 3'000'000;
    struct ChestShow {
        std::uint64_t since_us = 0;  // when first played
        bool took = false;           // seen showing kChestOpenIdle since
        unsigned flicks = 0;         // walks that saw its closed idle again after
    };
    unsigned chests_opened_twice_ = 0;
    std::map<std::uint64_t, ChestShow> chest_shown_;
    std::uint64_t last_tick_us_ = 0;
    bool ticked_ = false;  // a Tick has run (last_tick_us_ is set)
    std::map<std::uint64_t, Taking> taken_;
    std::vector<std::uint64_t> takes_in_;  // guarded by inbox_mutex_
    std::set<std::uint64_t> hidden_;  // loaded actors whose pickup was taken
    std::set<std::uint64_t> dead_;    // copies whose owner said dead kDeadGraceUs ago
    bool wake_ = true;
    std::size_t hurt_taken_ = 0;   // hurts the game took as asked
    std::size_t hurt_late_ = 0;    // late falls put back
    std::size_t hurt_missed_ = 0;  // hurts written directly after the wait
    BlastNear blast_near_;
    std::size_t blast_drops_ = 0;

    mutable std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
    bool forget_owners_ = false;  // guarded by inbox_mutex_
};

} // namespace CoopObjects
