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

#include <algorithm>
#include <array>
#include <cmath>
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
#include "coop_npc_anim.h"
#include "coop_guard.h"
#include "coop_show_delay.h"

namespace CoopObjects {

// One actor of one walk.
// A hit place read from a game's hit record: kept only when finite and within `near` of the boss (a stale or
// unrelated record, or a garbage read, sends nothing).
inline std::optional<coop::protocol::Vec3> CheckedHitPlace(const std::array<float, 3>& at,
                                                           const coop::protocol::Vec3& boss, float near) {
    float d2 = 0.0f;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(at[i])) {
            return std::nullopt;
        }
        const float d = at[i] - boss[i];
        d2 += d * d;
    }
    if (!(d2 <= near * near)) {
        return std::nullopt;
    }
    return coop::protocol::Vec3{at[0], at[1], at[2]};
}

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
    // Native boss sync (coop_boss_native.h): a boss's BSA component
    // (0 when not read) and, for one of ours, its brain and action verbs as
    // our game last set them (sent in its samples).
    std::uint64_t bsa_component = 0;
    std::optional<coop::protocol::BossBsa> bsa;
    // Where a boss copy's own new hit landed (its game's hit record, +0x27c; IsBoss only, never one we replayed,
    // checked near the boss): sent with our DAMAGE so the owner's replay lands there (boss-intro).
    std::optional<coop::protocol::Vec3> hit_at{};
    // Since the last walk its Life component's last hit id (+0x1b18) moved: own_hit when it moved to an id we did
    // not replay (our game's own hit: our Link's sword, arrow, bomb), replay_hit when our last replayed id was
    // reached in that move. A fall while a replay waits is the replay landing only without own_hit (g2-bosses
    // probe 20261117-114108: two of A's hits on Gohma's copy were booked as B's refused replay and never sent).
    bool own_hit = false;
    bool replay_hit = false;
    // Protocol 14: with a moved hit id, the hit record its damage step took (CoopObjects' ReadHitRecord).
    std::optional<coop::protocol::HitRecord> hit_record{};
    // Its last hit id was read in this walk and in the one before (so "did not move" is known; PvP's FollowLife).
    bool hit_id_read = false;
    // Issue 133 (whose hit): a new hit in its game's own hit record (not our replay of the owner's), and whether
    // its attacker was our Link or one of our own projectiles (CoopProjectiles::HitIsOurs). Not set: no new hit.
    std::optional<bool> hit_ours{};
    // Its Life component (0: none read); a copy boss's is held at life 1 by its own Life step
    // (CoopBossNative::Table::HeldLifeStep) until its owner's dies.
    std::uint64_t life_component = 0;
    // Row 97 (coop_event_member.h): whether it is an event member in our Link's event
    // (InSameEvent, the game's own test); nullopt when it has no event member component.
    std::optional<bool> event_member;
    // Row 138(8) (coop_sensor_claim.h): whether our Link is in its area by its own logic input (the game's own
    // sensor of our Link: Jabu-Jabu's IsInOpenMouthArea ...); nullopt when it has none of them.
    std::optional<bool> sensor_in_area;
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
    // Not empty: played again only over an animation with this (or, split
    // at '|', one of these) in its name
    // (a chest shown open, issue 195: only over its closed idle "CloseWait",
    // never over its own opening when our Link opens it here).
    std::string only_over;
    // Townspeople and enemies (PlaysByFrame, coop_npc_anim.h). `play_frame`:
    // `frame` is the playing frame (slot +0x10c on both sides) and is
    // compared the short way round a loop of `loop` frames (0: not known),
    // run on at `run` frames a game frame (below 0: at `rate`). `alias`
    // (townspeople only): the copy's slot keeps the name its own AI asked
    // for while it plays this one.
    bool play_frame = false;
    bool alias = false;
    float loop = 0.0f;
    float run = -1.0f;
};

// The frame of an animation `age_us` after it showed `frame` at `rate`
// (30 game frames a second); never below 0.
float FrameAfter(float frame, float rate, std::int64_t age_us);

// deku-visuals-b: a chest's own opening command, the lid opening (read from
// the romfs: AS/Obj_TBox_A_01 has "FrontOpenSmall", AS/Obj_TBox_A_02
// "FrontOpen"; Pack/Actor/Obj_TBox_A_02 and _A_03 bring AS _A_02, the other
// Obj_TBox_* packs (A_01, A_04, A_06, GlassTruth, NoBake) only AS _A_01).
// Empty for the Treasure Chest Shop's chests (their game keeps its own).
std::string ChestOpening(std::string_view actor);

// deku-visuals-b: a cobweb or one of its parts (the Deku Tree's
// Obj_InsideTheDekuTree_SpiderWeb_A_* and _InteractiveSpiderWebParts_A_*,
// and any other Obj_*SpiderWeb*): their AS has a "Burn" command.
bool IsWeb(std::string_view actor);

// PlayWish::only_over: true when `only_over` is empty or one of its
// '|'-separated parts is in `slot_name`.
bool PlaysOver(std::string_view slot_name, std::string_view only_over);

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
    // jitter-b (coop_follow.h): how fast `position` moves, metres a second (unset: still).
    std::optional<coop::protocol::Vec3> velocity;
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
    // With a hurt: where the hitter's hit landed (DAMAGE's hit_at): the replay lands there (Gohma's eye).
    std::optional<coop::protocol::Vec3> hit_at{};
    // With a hurt (protocol 14): the hit that happened (DAMAGE's hit_record, or the owner's sample hit): the
    // replay gives the game that hit (its weapon, element, place) instead of the stand-in sword hit.
    std::optional<coop::protocol::HitRecord> record{};
    // Past the hit replay (a hit record the game's damage rules judge): the amount goes straight into the game's
    // Life step (Hookshot, patch 0007), so the game still runs its own death. For the test kill: a boss takes no
    // plain hit outside its weak point (Gohma on the ceiling). Never written directly: not taken, nothing happens.
    bool direct = false;
    // Raise its life to `life` through the game's own Life step (a negative sum: the game's heal), never a write:
    // a boss copy that becomes ours with less life than its old owner's had (held at 1 by HeldLifeStep, or
    // lowered by its own hits) takes the old owner's life, so one hit never kills a near-full boss (Barinade).
    bool heal = false;
    // The test script's kill (`kill <name>`): a kill, as the game's own killing blow would be. In the game that owns
    // the enemy it goes past its life stops (CoopActorEvents::PassStops; Gohma's stop at 1 outside her chance time
    // held a test kill at 1 in LANE1 run 4). A copy's own Life step still holds a copy boss (HeldLifeStep); its hit
    // goes to the owner as DAMAGE, judged there as any hit.
    bool test_kill = false;
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
// Row 114: a boss off the walk is not dead. Phantom Ganon left the walk at life 15, and "none there" read as his
// death: his host's Game Over was sent as the boss beaten. A boss's death is the game's own life 0, read while its
// death runs (its event deletes it later), so a body off the walk reads by what was last known of it, with no time:
// - last read at life 0: dead (its death ran; deleted after it);
// - last read with life left, no delete request for it since (the game took it off its list, as it does with live
//   actors, churn-b): away, alive;
// - last read with life left, then deleted (the game's delete request, patch 0007; or our own remove of a copy):
//   removed alive. Never a death; it no longer keeps a boss of several bodies alive (Twinrova's sisters), but a
//   boss whose bodies are all removed alive is not dead either.
// A body never read this scene (a save that beat it): none there, dead as before. Without delete requests (the
// emulator) a body off the walk with life left is away. `last` keeps the last reading per body name; it is
// forgotten with the scene and on a new session (CoopObjects' NoteBosses and ForgetBosses).
struct BossLifeSeen {
    std::uint64_t address = 0;
    std::int32_t life = 0;
    bool removed = false;  // a delete request for `address` came after that reading
};
using BossLives = std::map<std::string, BossLifeSeen, std::less<>>;
void NoteBossLives(const std::vector<Seen>& walk, BossLives& last);
void NoteBossRemoved(std::uint64_t address, BossLives& last);
std::optional<bool> BossAliveKept(const std::vector<Seen>& walk, std::span<const std::string_view> bodies,
                                  const BossLives& last);
// Whether that "dead" (BossAliveKept false) is a reading of its death: a body read at life 0, on this walk or as its
// last reading this scene. False for a boss none of whose bodies was read here (a save that beat it, or the walk
// never saw it): dead for the pull, never proof of a death (G2 review: a beaten set must never come from it).
bool BossReadDead(const std::vector<Seen>& walk, std::span<const std::string_view> bodies, const BossLives& last);

// boss-copy-b: whether a boss is still in its pre-battle phase in one walk,
// from its body actors' own animation (AS slot 0): Gohma's `Sleep` and
// `Pre_Battle_*` (on the ceiling, before her drop). True when a body there
// plays one; false when the bodies there play others (the fight, or her
// intro under way); nothing when no body there has its animation read.
bool IsPreBattleAnimation(std::string_view name);
std::optional<bool> BossPreBattle(const std::vector<Seen>& seen, std::span<const std::string_view> bodies);

// Whether our game owns the dungeon's boss (boss-intro): from this walk when the boss is in it (`ours`: the
// addresses of the bosses we own), else the last walk that had it, for kBossOursKeepUs: the owner's Gohma is
// deleted by her own death event, and her death must still be known as the owner's then. `last` keeps one
// reading per boss body name (updated here).
struct BossOwnership {
    bool ours = false;
    std::uint64_t at_us = 0;
};
inline constexpr std::uint64_t kBossOursKeepUs = 10'000'000;
std::optional<bool> BossOurs(const std::vector<Seen>& walk, std::span<const std::uint64_t> ours,
                             std::span<const std::string_view> bodies, std::uint64_t now_us,
                             std::map<std::string, BossOwnership, std::less<>>& last);
// Whether any of the boss's bodies has lost life (life below its maximum):
// false while every body seen is at full life, nothing when none is seen or a
// life is not read.
std::optional<bool> BossHurt(const std::vector<Seen>& seen, std::span<const std::string_view> bodies);

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

// Boss minions (native boss sync, docs/dungeons-boss.md): an enemy a boss makes
// in the fight (King Dodongo's babies, Big Octo's children). With native boss
// sync every game's boss runs the same spawn action, so each would make its
// own. Only the game that owns the boss (its maker, the nearest boss of that
// name within kMinionReachM) keeps and shares them (OBJECT_SPAWN of a made
// id, as a rolled enemy); the others delete their own and make the owner's.
// No owner known for the maker: as before (matched by order, SceneSpawnId).
// Empty when the name is no boss minion; else the maker's actor name.
std::string_view MinionMaker(std::string_view name);
inline constexpr float kMinionReachM = 40.0f;

// A placed actor that may come into being in one game only, after the scene
// loaded: the Gold Skulltula a player shakes out of a tree. Its owner names
// it with OBJECT_SPAWN of its placed id; a game without that placement makes
// a copy, and deletes the copy if its own one turns up.
bool IsAnnounced(std::string_view name);

// Player report 99 (issue 252): an enemy placed in the scene that a game
// makes from its own logic or event, so the games need not all have it:
// the Gerudo Fortress jail guards (Enemy_MiddleBoss_GerudoThief*, made by
// GerudoFortress_Common_Level_CarpenterSet_0N when that player walks into
// the cell), and every other placed enemy a Logic_Create, ambush, room or
// gimmick makes (mini-boss rooms, Gimmick_*_Appearance, ...). Only the game
// whose player set it off has it; the others never did. A game's logic may
// also make it at the scene's load (its own per-game flags say the guard is
// out), so its owner names it whenever it is first seen, at load or later
// (IsAnnounced only later); a game without that placement makes a copy once its
// Link is within kEncounterNearM of it and its own did not come in
// kEncounterWaitUs (one that only streams in by distance comes by then), and
// deletes the copy if its own one turns up. If the copy was killed here
// (OBJECT_REMOVE dead), our own one that turns up later in this visit is that
// same enemy, already dead: it dies at once and is never listed, so the
// server keeps the kill (issue 133, no respawn). A held one whose owner's
// samples stop for kEncounterStaleUs is dropped (its owner left or unloaded
// it). Not the bosses and their parts
// (the boss sync) and not the enemies the sync makes or keeps its own way.
bool IsLateEncounter(std::string_view name);
inline constexpr float kEncounterNearM = 40.0f;
inline constexpr std::uint64_t kEncounterWaitUs = 1'500'000;
inline constexpr std::uint64_t kEncounterStaleUs = 5'000'000;

// An enemy animation that plays as it dies ("Die", "Dead_Fall",
// "Damage_Death"): a word Die, Dead or Death at the start or after '_'. An
// owned enemy that leaves the walk while playing one died, even when its life
// could not be read (the big Skulltula has none we can read; issue 133).
bool IsDeathAnimation(std::string_view name);

// Issue 133 (an enemy whose life cannot be read: Deku Scrubs, Tailpasaran, ...): our copy of it left our walk
// after playing its own death animation here (`own_death_recent`) while its owner's samples show no death and the
// owner has not said it dead: our game killed it. Its life never fell, so no hit was summed; the kill itself goes
// to the owner (DAMAGE cause killed). Never a boss or a boss's part (the boss sync has their deaths).
bool LifelessKillHere(std::string_view name, bool reacts, bool life_read, bool own_death_recent,
                      std::string_view owner_animation, bool owner_said_dead);

// Row 175 (run 20261119-230405): a Poe sister in her painting (Enemy_GhostPicturePerformer_Poe_*, no life, it never
// leaves the walk as it dies) that our arrow hit while ours is a copy: its own 'Pct_Die' was put back to the owner's
// 'Pct_Wait' at once, so the room logic never went on (no scramble, no sister). True when our copy (owned by another
// player) now plays its own death and the owner's newest animation is no death: the kill goes to the owner at once
// (DAMAGE cause killed, as issue 133's lifeless kill), the owner's animation no longer goes on ours, and the owner's
// "dead" leaves ours to its own game (its death runs the room logic here). Phantom Ganon's performers are his fight's
// (the boss sync): never.
bool PictureDeathHere(std::string_view name, bool life_read, bool owned_by_other, std::string_view own_animation,
                      std::string_view owner_animation);

// Issue 35 (4): an animation that hides the model (the withered Deku Baba's
// "Upside_Visible_Off_Spine4", the stem's "DownSide_Visible_Off_Withered_Spine6"):
// "Visible_Off" at the start or after '_'. A plain enemy's copy never
// plays its owner's one: its own game hides it in its own step (its death),
// the same step that takes it off Navi's and the Z-target's list. Replayed
// alone, it hid a copy that stayed alive and targetable.
bool IsHideAnimation(std::string_view name);

// Issue 35 (3): an enemy waiting on the ceiling to drop on a Link (the
// Skulltula's "CeilingWait", any LOD variant).
bool IsCeilingWait(std::string_view name);
// Our Link at `link` stands under a ceiling enemy at `enemy`: within
// kCeilingUnderM across (x, z) and below it by at most kCeilingDropM (y up).
inline constexpr float kCeilingUnderM = 4.0f;
inline constexpr float kCeilingDropM = 30.0f;
bool UnderCeilingEnemy(const coop::protocol::Vec3& enemy, const coop::protocol::Vec3& link);

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
// A real boss or mini-boss: its root actor (Enemy_DungeonBoss_*, Enemy_MiddleBoss_*, and the roots of boss
// groups); never a picture book _Dictionary actor, and never one of a boss's parts (IsBossPart) unless it is
// itself a group's root (Twinrova: Kotake's and Koume's root when no battle manager is loaded). Row 149: the
// old name test took every Enemy_DungeonBoss_* name for a boss of its own, so King Dodongo's roll centre
// (Enemy_DungeonBoss_Dodongo_Attention_RollingCenter, made anew for each roll) sent its own boss group lock
// when it was not linked to King Dodongo, and the native boss sync took its owner, not King Dodongo's: in the
// boss owner's game the roll centre could be another player's copy.
bool IsBoss(std::string_view name);
// A part of a boss (PartRules' boss parts: the Attention actors, King Dodongo's roll centre, Barinade's arms
// and jellyfish, Morpha's tentacle, Bongo Bongo's hands, Gohma's eggs, Twinrova's witches, ...) that is no
// group's root. It follows its root: the native boss sync gives it its root's owner from the first frame
// (BossPartRoot), the server's group link its root's owner and fight.
bool IsBossPart(std::string_view name);
// A boss or one of its parts named as one (what IsBoss was before row 149): the rules every boss actor shares
// (its life, its hits, its slots and animations, its own native boss sync entry).
bool IsBossOrPart(std::string_view name);
// Row 149: the root a boss part follows in a walk (PartOf's roots in order, the nearest in reach; on a tie the
// the first in the walk), as GroupsOf picks it; nothing for no boss part or no root in the walk. `names` and `places`
// are the walk's, one per actor.
std::optional<std::size_t> BossPartRoot(std::string_view part, const coop::protocol::Vec3& at,
                                        const std::vector<std::string_view>& names,
                                        const std::vector<coop::protocol::Vec3>& places);
// A plain enemy whose copy takes its owner's brain and action verbs as a boss's does (CoopBossNative,
// Boss::plain; fable-g4-enemies): every Enemy_ that is no boss, except the ones each game runs of its own
// (IsOwnPuzzle: the scrub puzzles and shops), an enemy's shot (CoopEnemyShots: made by the shooter's game) and
// the picture book's _Dictionary actors. Never an Npc_ (a guard is handed over on its own reaction, issue 191;
// a townsperson has its own sync), a fish or a world object. Of these, only one with a BSA component is
// published (a part with none follows its root as before).
bool OnBrainPath(std::string_view name);
// LICHII_COOP_ENEMY_BRAIN's text (empty when unset): the brain path is opt-in, on only for a value other than
// "0". Unset or empty leaves every plain enemy on the old path (the first g4-enemies probe ran it on everywhere,
// its switch read as on-unless-0).
bool EnemyBrainSwitch(std::string_view value);

// enemy-anim-b: an actor whose slot 0 is sent and set by its playing frame
// (AS slot +0x10c, what the game's getter 0x37b5e70 reads): townspeople
// (Npc_) and enemies (Enemy_) other than bosses. Slot +0xe8 is not the
// playing frame: enemy copies started each animation 8 to 15 frames in and
// their frame was never corrected (owner's Deku Tree run, 2026-11-14).
// Bosses keep +0xe8 for all their slots (issue 150, tested that way).
bool PlaysByFrame(std::string_view name);
// jitter-b / issue 239: a copy whose place the Mover follows each game frame (coop_follow.h) by the
// velocity it came with: townspeople (Npc_) and every enemy (Enemy_: plain, mid-boss, boss and their parts),
// never a picture book _Dictionary actor. Everything else is written where it is, as before.
bool FollowedEachFrame(std::string_view name);

// An actor whose life is never written (the owner's native rule): an enemy or a boss, and any actor whose name
// we do not know (fail safe).
bool NativeOnlyName(std::string_view name);

// How WriteLife hands one life change to the game (pure; WriteLife reads the game for it, then acts).
enum class LifeRoute {
    replay,       // a whole hit for the game's own damage step (Mover::Replay)
    wait_replay,  // our replayed hit still waits for its damage step: nothing more now
    step,         // `amount` into the game's Life step (patch 0007)
    lost,         // nothing: the actor keeps its game's life (native-only)
    write,        // the old direct write (not native-only: a pot, a crate, an animal)
};
struct LifeAsk {
    bool native = false;       // NativeOnlyName, or a test kill (direct)
    bool direct = false;       // past the replay (LifeWrite::direct)
    bool replay_on = false;    // LICHII_COOP_OBJECTS_HIT_REPLAY
    bool has_player = false;   // our Link is known (the replay's attacker)
    std::int32_t hurt = 0;     // LifeWrite::hurt
    std::int32_t life = 0;     // LifeWrite::life (the life to reach)
    std::int32_t old = -1;     // the life read now (-1: unread)
    bool step_seen = false;    // the game's Life step has called us (Hookshot, patch 0007)
    bool has_component = false;
    bool replay_seen = true;   // its damage step saw our last replayed hit (or we never replayed one)
    std::int32_t stale = 0;    // given to the step earlier and never taken (taken back now)
    bool heal = false;         // LifeWrite::heal: a raise to `life` through the Life step
};
struct LifeRouted {
    LifeRoute route = LifeRoute::lost;
    std::int32_t amount = 0;  // for step
};
LifeRouted RouteLife(const LifeAsk& a);

// Issue 88: the hearts the game draws come from our Link's Life component,
// not from the shared PlayerStatus.MaxLife flag. The max (quarter hearts) to
// write into the component: the flag when another player raised it above
// ours (at most 20 hearts), else nothing. Never lowers it.
inline constexpr std::int32_t kMostLifeQuarters = 20 * 4;
std::optional<std::int32_t> RaisedMaxLife(std::int32_t link_max, std::int32_t flag);
// Rows 136/137: the host loaded an older save and its FLAG_RESET lowered MaxLife while our Link stood in a scene
// (FlagWatcher::reset_lowered). The component keeps the replaced save's max and writes it back, which the max rule
// would send to the whole group. The max (and life, held to it) to write into the component: `lower_to` when it is
// below ours and a real maximum (at least one heart), else nothing. Only while that hold lasts; never otherwise.
struct LoweredLife {
    std::int32_t max = 0;
    std::optional<std::int32_t> life;  // only when ours is above the new max
};
std::optional<LoweredLife> LoweredMaxLife(std::int32_t link_max, std::int32_t link_life,
                                          std::optional<std::int32_t> lower_to);

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
    static constexpr std::uint64_t kBehindUs = 100'000;       // copies show the owner at least this late
    // Report 37: as late as that owner's puppet (coop_show_delay.h: from its samples' ages), never under kBehindUs.
    static std::uint64_t BehindUs(std::uint8_t owner) { return std::max(kBehindUs, CoopShowDelay::Us(owner)); }
    // A hold that waits for the owner's answer to our hit (kHitHoldUs, kReactHoldUs) is set for a LAN; over a
    // slower line the answer is later by the extra delay both ways (report 37: at 300 ms ping it came after the
    // hold, the copy played its old animation again and the hit was sent once more).
    static std::uint64_t AnswerHoldUs(std::uint64_t lan_us, std::uint8_t owner) {
        return lan_us + 2 * (BehindUs(owner) - kBehindUs);
    }
    static constexpr std::uint64_t kExtrapolateUs = 150'000;
    static constexpr std::uint64_t kPoseStaleUs = 2'500'000;  // no sample this long: let it be
    static constexpr std::uint64_t kHurtWaitUs = 400'000;     // then write life directly
    // Protocol 14: how long the owner's samples carry its newest hit (a few samples: one may be lost).
    static constexpr std::uint64_t kHitShowUs = 500'000;
    static constexpr std::uint64_t kHandedUs = 2'000'000;     // a DAMAGE of ours that crossed a hand-over is back by then
    static constexpr std::uint64_t kHitPlaceKeepUs = 200'000;  // a hit's place waits this long for its life drop
    // Issue 133: our newest hit (its game's hit record) waits this long for its life drop (a walk or two), so a
    // copy's blast beside it never takes it for the blast's.
    static constexpr std::uint64_t kOurHitKeepUs = 500'000;
    // Issue 133: a copy's own death animation counts for its leaving our walk this long (LifelessKillHere).
    static constexpr std::uint64_t kOwnDeathKeepUs = 5'000'000;
    // Row 175 (run 20261119-232719): the owner's play handed over before our painting's death was read cut it off
    // ('Pct_Wait' at once, the room logic stalled). Its own death is played again over it this long after.
    static constexpr std::uint64_t kPictureReplayUs = 3'000'000;
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
    // How long our own hatchling of a copy egg waits for the owner's.
    static constexpr std::uint64_t kOwnHatchWaitUs = 10'000'000;
    std::uint64_t Hatched() const { return hatched_count_; }

    // Hurt through the game's damage field (LifeWrite::hurt); off: write life.
    void SetHurt(bool on) { hurt_ = on; }
    // Test only (coop_testinput's "stun NAME"): our copy of the boss of that name nearest `near` (another
    // player owns it) tells its owner at the next Tick it is stunned here, as our game's own stun does (a
    // DAMAGE of 0, issue 150's hand-over). False: no such copy (none, or ours).
    bool TestStun(std::string_view name, const std::optional<coop::protocol::Vec3>& near, std::uint8_t self);
    // Test only (coop_testinput's "hit NAME N", given straight to the Life step, so no hit id moves): the next fall
    // of the object at `address` within kTestOwnHitUs is our game's own hit (Seen::own_hit), as a sword's would be.
    void TestOwnHit(std::uint64_t address, std::uint64_t now_us);
    static constexpr std::uint64_t kTestOwnHitUs = 1'000'000;

    // Hookshot with patch 0007 (coop_actor_events.h): the game's own Life
    // step says every hit it deals, so a hit of this game's on a copy is
    // taken from that (GameHit, before the Tick) and no longer guessed from
    // the life read falling. The fall is still read, only to pair it: a hit
    // and its fall may come in either order (the walk and the game's threads
    // do not wait for each other), a fall with a hurt of ours in flight is
    // that hurt, and a fall no step explains within kStepWaitUs counts as a
    // hit as before (LooseDrops says how much: life taken outside the step).
    void SetLifeEvents(bool on) { life_events_ = on; }
    bool LifeEvents() const { return life_events_; }
    void GameHit(std::uint64_t address, std::int32_t amount);
    // Life another game's change takes through the Life step itself (not
    // the game's damage, so no GameHit comes for it): its fall is no hit.
    void Stepped(std::uint64_t address, std::int32_t amount, std::uint64_t now_us);
    static constexpr std::uint64_t kStepWaitUs = 300'000;
    static constexpr std::uint64_t kStepGivenUs = 1'000'000;
    std::size_t LooseDrops() const { return loose_drops_; }
    // Hookshot with patch 0007: the game accepted a delete request for this
    // actor (its own code asked: killed, broken, taken, or our own remove).
    // It is gone at the first walk that misses it, not the second: off the
    // list is no "lifted or changing hands" then. Kept kDeletedKeepUs (a
    // dying actor stays listed a while; the address may be used again later).
    // `by_coop`: asked inside one of the module's own calls (a copy, a remove; CoopActorEvents::kOwn).
    void GameDeleted(std::uint64_t address, std::uint64_t now_us, bool by_coop = false);
    static constexpr std::uint64_t kDeletedKeepUs = 10'000'000;
    // enemy-left-0116 (row 138(6)): an enemy of ours (placed, not a boss or
    // its part, not an egg) that the game itself deleted while no death was
    // read (life 0 or a death animation: a fall into lava or a pit, a burn,
    // its own time running out) is gone from our game for good. Before, only
    // a read death was told: the others' copies lived on and the server handed
    // the enemy to one of them. Now it stays listed (as an egg does) and after
    // kLeftWaitUs an OBJECT_REMOVE `gone` goes out, so the server keeps it
    // nobody's and the copies end too (deleted, no kill counted: a Dead flag
    // the game set comes through the flag sync). Not when the game deleted
    // kDeleteBurst or more actors within kDeleteBurstUs of it: that is the
    // scene going (a load, a warp, the title), not the enemy. Not when the
    // delete may come from our own Link leaving it (review CHANGES r1): its
    // kind's delete input reads our Link (coop_area_deleted.inc, generated
    // from the romfs: Water Temple's Shell Blades and blue Tektites), or our
    // Link was over kLeftNearM from it (an AI's own "far from Link" despawn
    // the logic graphs do not show). Then the copy another player fights lives
    // on and the server hands it over, as before.
    static constexpr float kLeftNearM = 20.0f;
    static constexpr std::uint64_t kLeftWaitUs = 1'000'000;  // (the whole kDeleteBurstUs after it seen)
    static constexpr std::size_t kDeleteBurst = 12;
    static constexpr std::uint64_t kDeleteBurstUs = 1'000'000;
    std::size_t LeftCount() const { return left_.size(); }
    // churn-b: the game's delete requests reach us (Hookshot, patch 0007).
    // Then a placed object that leaves the walk alive with no delete request
    // is not gone: the game takes live actors off its list for a moment and
    // puts them back (townspeople, cuccos, pots, fairies, every ~0.5 s in a
    // big run and in players' 0.11.0 logs, 11 m from Link). Its id stays
    // listed in OBJECT_PRESENT and its owner kept for kAwayKeepUs, so the
    // server does not hand it to another game each time (the owner
    // ping-pong of hundreds of OBJECT_OWNER a minute). Back at the same
    // place it goes on as before; a delete request or the time running out
    // ends it. Without delete events (the emulator) two missed walks still end it.
    void SetDeleteEvents(bool on) { delete_events_ = on; }
    static constexpr std::uint64_t kAwayKeepUs = 2'000'000;
    static constexpr std::uint64_t kStillOursUs = 120'000'000;  // still_ours_
    std::size_t StillOursCount() const { return still_ours_.size(); }
    // enemy-left-0116 (row 138(3), Revirie's log at DC's second Lizalfos
    // pair): an event playing here takes its performers off the list for
    // longer (the Lizalfos' jump-in during FocusOnEnemy_02 went > 2 s, so
    // ours were let go, handed to player 2, and our game then made copies of
    // "player 2's encounter" beside its own). While an event plays here the
    // time does not run out (kAwayKeepUs again from its end), up to
    // kAwayEventMaxUs off the walk.
    static constexpr std::uint64_t kAwayEventMaxUs = 30'000'000;
    std::size_t AwayCount() const { return away_.size(); }

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

    // Present thread, once per walk. `self` is our player number (1 to kMaxPlayers),
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

    // Issue 200 (npc-anim). Present thread, before Tick: whether our Link is
    // in an event (a cutscene or a talk, PlayerReading::event_running). While
    // it is, every townsperson (Npc_, shared) within kEventNpcM of our Link
    // is flagged targeting in OBJECT_PRESENT, so the server gives it to us
    // (or, ours already, keeps it in a fight lock): every other game's copy
    // then plays our NPC's cutscene motion (place, rotation, AS command and
    // frame, the usual copy) instead of its own routine. kEventKeepUs
    // after the event the flag drops and the normal owner rules come back;
    // the copies stay where our NPC was left. No new message.
    //
    // Row 97 (event-copy): not only townspeople. Every shared actor our event
    // is with is claimed the same way, whatever its kind and however far from
    // our Link: one the event binds (Seen::event_member, coop_event_member.h:
    // a scrub whose entrance or talk it is, a shopkeeper, a story event's
    // actor, a mini-game's), and one whose own AI asked for another animation
    // than its owner's while our event runs (NoteTakeover; within kEventNpcM:
    // the event moves it). Its own game runs it for the event: the owner's
    // place, animation and remove are not put on it (EventPartner) and the
    // server hands it to us. Before, only Npc_ within kEventNpcM were, and the
    // 8 m rule (coop_guard.h) covered the rest: a Deku Scrub copy further away
    // was made to sleep by its absent owner every frame and our Link's event
    // that waited on it never ended (Mr. Shorty's 15 min softlock).
    //
    // G1 (coop/host/event_owner.hpp): `ours` false is another player's event that our game
    // plays (a live dungeon event, a shared scene joined, an eye switch's event): its bound
    // actors (member) still act for it here (EventPartner: no owner's motion, changes wait),
    // but none is claimed (no targeting flag: it stays with the player whose event it is, so
    // two games never pull one actor to and fro), and no actor is guessed in (own_ai,
    // townsperson: our Link is not in it).
    void SetEvent(bool in_event, std::uint64_t now_us = 0, bool ours = true) {
        if (in_event && !in_event_) {
            event_since_ = now_us;
        }
        if (!in_event && in_event_) {
            event_end_ = now_us;  // (Away: kAwayKeepUs from here)
        }
        in_event_ = in_event;
        event_ours_ = in_event && ours;
    }
    // A copy our event claimed (within kEventKeepUs of the last Tick that
    // flagged it): the owner's routine is not put on it while the hand-over
    // runs, as the 8 m event rule (coop_guard.h) does nearer. Row 138(8):
    // also a copy our Link is in the area of (AreaClaim).
    bool EventPartner(std::uint64_t address, std::uint64_t now_us) const;
    // S2 (event-owner-0116, issue 144): a copy whose owner's own event binds it
    // (object_state::in_event in the owner's samples, within kEventKeepUs) and that has a game
    // logic of its own (an Enemy_: room B's and room F's Deku Scrubs; the Scrub Brothers and
    // the Business Scrubs are each game's own already, IsOwnPuzzle). Our copy
    // keeps that logic (EventPartner: the owner's motion, animation and remove are not put on
    // it) and is not claimed away from the owner (no wake on it); the event's result reaches us
    // by its flags (coop_scrub_talk.h, coop_scrub_sale.h), as in the owner's game. Before, the
    // owner's talk motion was put on our copy every frame: our own beaten scrub never hopped
    // or talked, and a talk of ours on it waited forever. Townspeople still play the owner's
    // cutscene motion (issue 200); bosses and fish keep their own rules.
    bool OwnerEventHolds(std::uint64_t address, std::uint64_t now_us) const;
    // Row 138(8) (coop_sensor_claim.h): a shared actor whose own sensor input
    // says our Link is in its area (Seen::sensor_in_area; Jabu-Jabu's mouth)
    // is claimed like an event's partner, without an event: flagged targeting
    // (the server hands a copy to us after its chase hold; ours it holds as a
    // fight, so with two players in the area the owner keeps it), and the
    // owner's place, animation and remove are not put on our copy. The claim
    // drops kSensorKeepUs after the input reads 0 (our Link left).
    bool AreaClaim(std::uint64_t address, std::uint64_t now_us) const;
    // How many actors our Link's area claims now (for the log).
    std::size_t AreaClaims(std::uint64_t now_us) const;
    static constexpr std::uint64_t kSensorKeepUs = 1'000'000;
    // Who owns the object at `address` (0: not known, or not on our list).
    std::uint8_t OwnerAt(std::uint64_t address) const;
    // How many actors our event flags now (for the log).
    std::size_t EventPartners(std::uint64_t now_us) const;
    // How many of them the server has handed to us already (for the log:
    // the claim is a wish, this is the hand-over).
    std::size_t EventPartnersOurs(std::uint64_t now_us) const;
    // Why an actor is our event's partner (for the log).
    enum class PartnerWhy : std::uint8_t { none, member, own_ai, townsperson };
    struct Partner {
        std::uint64_t address = 0;
        std::string name;
        std::uint8_t owner = 0;
        PartnerWhy why = PartnerWhy::none;
        float metres = -1.0f;  // from our Link; -1 when not known
    };
    // Every actor our event claims now, with why (the log names them).
    std::vector<Partner> EventPartnerList(std::uint64_t now_us) const;
    static constexpr float kEventNpcM = 30.0f;
    static constexpr std::uint64_t kEventKeepUs = 2'000'000;

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
    // The boss actors our game is not ready to own (its intro not played
    // here): listed with object_presence::unready, so the server keeps them
    // with a game whose boss is fighting.
    void SetUnready(std::vector<std::string> names) { unready_ = std::move(names); }
    // The boss actors whose death has begun (the boss glue; player report #105). A hit such a boss's game did
    // not take (its owner's killing hit, refused outside Gohma's weak point) is given to its Life step as its
    // life after the wait, as for any other enemy: its death plays out here (CoopBossNative lets it die).
    void SetDying(std::vector<std::string> names) { dying_ = std::move(names); }
    // Dying copy bosses whose owner said dead, given their remaining life once (review-boss-0114).
    unsigned DyingFinished() const { return dying_finished_; }
    static constexpr float kWakeM = 10.0f;
    // Issue 157: the Mover puts the owner's animation back on the very next
    // game frame, so a copy whose AI asks for its wake animation once (a
    // Peahat rising out of the ground) is rarely seen in it by the walk.
    // The Mover tells each such put-back (NoteTakeover, present thread); the
    // copy then counts as playing its own animation for kWakeLatchUs.
    // `name` is what the copy's own AI played (a guard is taken only for an
    // IsGuardReaction one).
    void NoteTakeover(std::uint64_t address, std::uint64_t now_us, std::string_view name = {});
    // Issue 101 on the enemies' brain path (CoopBossNative::Table::TakeOwnWants): a plain enemy copy whose own
    // brain asked for another phase than its owner's (skipped: the copy plays the owner's, so neither its
    // animation nor a pull towards our Link shows it). Near our Link (kWakeM) it chases us as a put-back one
    // does (kWakeLatchUs): OBJECT_PRESENT flags it targeting and the server hands it over, never while its
    // owner's game fights it (in_fight, the owner's Link nearer). Present thread.
    void NoteOwnBrainWant(std::uint64_t address, std::uint64_t now_us);
    static constexpr std::uint64_t kWakeLatchUs = 1'200'000;
    // Player report 109 (Deku Babas slow to come out, then stuck upright): a
    // copy's own AI that wakes for our Link (it rises out of the ground) had
    // the owner's resting animation put back over its rise on the next frame.
    // Its AI then waited for the end of a play it no longer had (frozen in
    // the rise's pose) and only the hand-over (kChaseHoldUs and the server)
    // let it act. Now, as after our own hit (kReactHoldUs), the copy's own
    // wake runs: the owner's animation of that moment is not put back for
    // kOwnWakeUs, and a rise the Mover already took away is given back once
    // (TakeOwnWakes). Only while the owner's enemy rests or idles (its rest
    // animation, or no new animation for kOwnWakeIdleUs), never over the
    // owner's own fight; at most once each kOwnWakeAgainUs. Its next
    // animation is played as usual, and the wake hands it to us meanwhile.
    static constexpr std::uint64_t kOwnWakeUs = 3'000'000;
    static constexpr std::uint64_t kOwnWakeIdleUs = 3'000'000;
    static constexpr std::uint64_t kOwnWakeAgainUs = 10'000'000;
    // Present thread: copies whose own wake play the Mover put back, with
    // that play, to give back once at the next game frame.
    std::vector<std::pair<std::uint64_t, std::string>> TakeOwnWakes() { return std::exchange(own_wakes_, {}); }
    // Present thread: the copies whose own wake began since the last call (every one, also those with no play
    // to give back). With the enemies' brain path (CoopBossNative, Boss::plain) the copy's own brain is let
    // run for the wake (NoteOwnWake): the rise is its brain's, not a play put back.
    std::vector<std::uint64_t> TakeOwnWakeStarts() { return std::exchange(own_wake_starts_, {}); }
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
    // Present thread (Tick): a Gold Skulltula placement whose token is taken
    // (from Take, also for its key + 2). Its copies go and none is made again;
    // our own one leaving is its death for everyone (skulltula-0118).
    void GoldTaken(std::uint64_t placement);
    bool IsGoldTaken(std::uint64_t placement) const { return gold_taken_.contains(placement); }
    // Present thread: a placement key taken in this scene. Collect asks this
    // for actors that are not shared, to report them too.
    bool Taken(std::uint64_t hash) const { return taken_.contains(hash); }

    // Present thread: not connected or the game went away. Forgets everything.
    void Reset();
    // Present thread, just before the Reset of a session's end (we left, the server dropped, the host's game
    // crashed): no owner judges our boss copies any more, and their own Life step lets them die again
    // (CoopBossNative Publish({})). A copy held at life 1 by HeldLifeStep (or lower by its own hits the owner
    // refused) would die of the next hit: each live boss copy takes its owner's last life from the owner's samples,
    // through its own Life step (the hand-over's rule, TakeOwnerLife), so the fight goes on in our game where it
    // was. No new network data. A copy whose owner said it died, or with no owner's life heard, is left alone.
    void KeepBossLives(std::uint8_t self, std::vector<LifeWrite>& writes);

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
    // Difficulty's night horde (coop_difficulty.h Horde): how many players'
    // Links stand within kRollNearM of `at`, we among them: the others in our
    // scene whose last place (under kSceneStaleUs old) is that near.
    std::size_t PlayersNear(const coop::protocol::Vec3& at, std::uint64_t now_us) const;
    // jabu-lifts (#65): where the other players of our scene are, from their fresh samples.
    std::vector<coop::protocol::Vec3> OthersHere(std::uint64_t now_us) const;
    // Native boss sync: each copy's latest owner's boss verbs, by the
    // copy's address (coop_boss_native.h puts them into the game).
    std::vector<std::pair<std::uint64_t, coop::protocol::BossBsa>> BossVerbsIn() const;
    // The actors of the enemies we own, with their names (for the test drive).
    std::vector<std::pair<std::uint64_t, std::string>> Owned() const;
    // The player who owns the object at `address` (0: the server, none: not
    // known). For coop_enemy_shots.
    std::optional<std::uint8_t> OwnerOf(std::uint64_t address) const;
    // The owner of the copy at `address` said it died: its life 0 in the owner's samples, or its
    // ObjectRemove dead (one boss death for everyone: the copy's Life step lets it die then).
    bool OwnerSaysDead(std::uint64_t address) const;
    std::uint8_t OwnerOf(const coop::protocol::ObjectId& id) const;
    std::size_t HurtsTaken() const { return hurt_taken_; }
    // Falls counted as our own hit while a replay waited (Seen::own_hit), not as the replay landing.
    std::size_t OwnHitsKept() const { return own_hits_kept_; }
    std::size_t HurtsMissed() const { return hurt_missed_; }
    // Another player's hits our boss refused as a replay, given to its own Life step (boss-hits).
    std::size_t BossHitsStepped() const { return boss_hits_stepped_; }
    // Copies dead here whose owner still showed life after every resend (a lost kill: the games stay split).
    std::size_t SplitKills() const { return split_kills_; }
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
        std::optional<coop::protocol::BossBsa> bsa;         // native boss sync: ours, as last read
        std::optional<coop::protocol::BossBsa> bsa_in;      // native boss sync: a copy's, the owner's latest
        bool boss_native = false;  // a bsa came for it once: its AI runs (never held), across owners
        coop::protocol::Vec3 velocity{};
        std::uint64_t seen_at = 0;
        std::string name;
        std::uint64_t fight_until = 0;
        bool late = false;       // to announce: a placed IsAnnounced actor first seen after kLateUs, or any IsLateEncounter
        bool announced = false;  // its OBJECT_SPAWN went out
        // Owner side.
        std::uint64_t sent_at = 0;
        bool sent_once = false;
        std::optional<coop::protocol::ObjectLife> sent_life;
        coop::protocol::Vec3 sent_position{};
        std::optional<coop::protocol::Quat> sent_rotation;
        std::string sent_animation;
        std::string sent_slots;  // issue 150: the slot names last sent
        std::optional<std::uint32_t> sent_bsa_seq;  // native boss sync: the boss verbs' seq last sent
        std::uint32_t sequence = 0;
        // Protocol 14: the newest hit our game took on our own enemy (Seen::hit_record), in its samples until
        // hit_show_until (kHitShowUs; samples may be lost), seq counting from 1 (ObjectHit).
        std::optional<coop::protocol::ObjectHit> hit_out;
        std::uint64_t hit_show_until = 0;
        std::uint32_t hit_seq = 0;
        // Copy side.
        std::int32_t hit_sum = 0;
        std::uint64_t test_own_hit_until = 0;  // TestOwnHit
        std::optional<coop::protocol::Vec3> place_seen;  // a new hit's place, waiting for its life drop
        std::uint64_t place_seen_at = 0;
        std::optional<coop::protocol::HitRecord> record_seen;  // protocol 14: a new own hit's record, waiting for its drop
        std::uint64_t record_seen_at = 0;
        std::uint64_t ours_hit_at = 0;   // issue 133: our Link's or projectile's newest hit on it (Seen::hit_ours)
        std::uint64_t own_death_at = 0;  // issue 133: its own death animation last read here (LifelessKillHere)
        std::uint64_t picture_die_at = 0;  // row 175: our copy's own death here went to its owner (PictureDeathHere)
        bool picture_kill_due = false;     // row 175: that kill is still to send
        std::string picture_die;           // row 175: its own death animation then (played again if cut off)
        std::optional<coop::protocol::Vec3> hit_place;  // where our own hit summed now landed (a boss; cleared with the sum)
        std::optional<coop::protocol::HitRecord> hit_record;  // protocol 14: our newest own hit summed now (cleared with it)
        std::uint32_t hit_seq_in = 0;  // protocol 14: the owner's newest sample hit seq replayed here
        std::uint64_t hit_since = 0;
        std::uint64_t hit_at = 0;  // last DAMAGE sent
        // A HANDOVER_STATE made it ours: until its owner news (any owner; cleared there) a fall here is our own
        // enemy's, no DAMAGE. `handed_took`: its life became the old owner's there, so a DAMAGE of ours that
        // crossed the hand-over (the server gives it back, player_number our own) is not in it yet; taken off
        // until `returned_until` (kHandedUs), whoever owns it by then.
        std::uint64_t handed_until = 0;
        std::uint64_t returned_until = 0;
        bool handed_took = false;
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
        // Our enemy's killing hit, given to its game after the wait (never written): said dead only once the walk
        // reads its life at 0 before this time (its game took it); a lost kill leaves it alive.
        std::uint64_t kill_given_until = 0;
        bool split_said = false;  // "dead here, owner alive after the resends" noted once
        // With life events (SetLifeEvents). step_new: the game's hits
        // since the last Tick; step_wait: hits whose fall is not seen yet;
        // loose: a fall no hit explained yet; step_given: what we put
        // through the step ourselves; step_rest: this Tick's unexplained fall.
        std::int32_t step_new = 0;
        std::int32_t step_wait = 0;
        std::uint64_t step_wait_until = 0;
        std::int32_t loose = 0;
        std::uint64_t loose_until = 0;
        std::int32_t step_given = 0;
        std::uint64_t step_given_until = 0;
        std::int32_t step_rest = 0;
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
        bool npc = false;                       // Npc_: claimed by our event (issue 200)
        bool play_frame = false;                // PlaysByFrame
        CoopNpcAnim::Loop frame_loop;           // its owner's animation loop, from the samples (play_frame)
        std::uint64_t event_until = 0;          // flagged by our event until then
        std::uint64_t own_event_until = 0;      // ... by our own Link's event (S2: sent as object_state::in_event)
        std::uint64_t owner_event_until = 0;    // a copy: its owner's samples say its owner's event binds it (S2)
        PartnerWhy event_why = PartnerWhy::none; // ... and why (row 97)
        std::optional<bool> event_member;       // Seen::event_member, the last walk (row 97)
        std::optional<bool> sensor_in_area;     // Seen::sensor_in_area, the last walk (row 138(8))
        std::uint64_t sensor_until = 0;         // claimed by our Link in its area until then (row 138(8))
        std::uint64_t own_ai_at = 0;            // its own AI's play put back last (NoteTakeover, any kind)
        coop::protocol::Vec3 first_position{};  // where it was first seen
        int missed = 0;                         // walks in a row without it
        std::uint64_t deleted_until = 0;        // GameDeleted: gone at the first miss until then
        bool game_deleted = false;              // ... and the game's own request, not one of ours (Left)
        // Chasing our Link (NoteDrift): the run of steps towards it.
        std::uint64_t chase_first = 0;
        std::uint64_t chase_last = 0;
        std::uint64_t chase_until = 0;
        // Issue 132: the animation it was first seen in (an owned enemy at rest).
        std::string rest_animation;
        std::uint64_t woke_at = 0;  // left its rest animation (0 = resting)
        std::uint64_t took_over_at = 0;  // issue 157: its own AI's play put back (NoteTakeover)
        std::string took_over_name;      // ... and what it was (a guard's, issue 191)
        std::uint64_t brain_want_at = 0; // issue 101: its own brain asked for another phase (NoteOwnBrainWant)
        // Report 109: its own wake (kOwnWakeUs) began then, over the owner's animation `own_wake_over`.
        std::uint64_t own_wake_at = 0;
        std::string own_wake_over;
        bool own_wake_given = false;  // its rise was given back once in this own wake (TakeOwnWakes)
        std::uint64_t owner_anim_since = 0;  // the owner's animation (base name) is this one since
        // Our own hit took its life to 0 here (the DAMAGE resend's mark).
        std::uint64_t killed_at = 0;
        // Issue 133: resends of a DAMAGE for a copy we killed here, and when
        // its owner said it died (OBJECT_REMOVE dead; 0 = not said).
        int resent = 0;
        std::uint64_t dead_at = 0;
        bool owner_life_zero = false;  // its owner's samples said life 0 (OwnerSaysDead)
        std::optional<std::int32_t> owner_life;  // its owner's last life, from the owner's samples (KeepBossLives)
        bool death_given = false;      // a dying copy boss's remaining life went to its own Life step (once)
        bool dead_now = false;  // a kill from before we came: deleted at once
        // enemy-sync-3 (issue 133): our enemy, killed by another game's hit
        // that our game did not take, already said dead (OBJECT_REMOVE).
        bool dead_said = false;
        // An egg we own that a hatchling came out of: when it leaves the
        // walk, OBJECT_REMOVE reason gone goes out for it.
        bool hatched = false;
        // boss-sync-2 (issue 178): a hatchling our own game hatched out of a
        // copy of another game's egg (when our game saw it hatch, 0: none).
        // It stays: the owner's hatchling, when its OBJECT_SPAWN comes, takes
        // this one as its copy instead of a second one being made; none within
        // kOwnHatchWaitUs: it is ended (one larva per game).
        std::uint64_t own_hatch_us = 0;
        // Issue 150: a boss copy whose own game stunned it (a DAMAGE of 0 is
        // due), and when that last went out.
        bool stun_due = false;
        std::uint64_t stun_at = 0;
    };

    // One step of `o` towards our Link at `now_us` (NoteDrift).
    static void ChaseStep(Object& o, std::uint64_t now_us);
    // Report 109: a copy's own AI woke for our Link (`taken_away`: the Mover put its play back already).
    void OwnWake(Object& o, bool taken_away, std::uint64_t now_us);

    // Takes `o` down to `life`: a hurt when hurt is on, else a write.
    // The hits the game's Life step dealt o that count now (SetLifeEvents);
    // o.step_rest gets the part of `fell` that no step explains.
    std::int32_t StepHits(Object& o, std::int32_t fell, std::int32_t life, std::uint64_t now_us);
    void Lower(Object& o, std::int32_t life, std::uint64_t now_us, std::vector<LifeWrite>& writes,
               std::uint8_t hitter,
               const std::optional<coop::protocol::Vec3>& hit_at = std::nullopt,
               const std::optional<coop::protocol::HitRecord>& record = std::nullopt);

    // A boss copy takes `owner_life` (its old owner's last life): raised (heal) or lowered (direct) through its
    // own Life step, never a write. False when it is no boss, has no life read, or the lives agree.
    bool TakeOwnerLife(Object& o, std::int32_t owner_life, std::vector<LifeWrite>& writes, std::string_view why);

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
    // boss-larva-0116: an egg id whose hatch this game shares when it owns it
    // (placed, or made: a boss laid it); its owner is checked by the caller.
    static bool HatchIsOurs(const coop::protocol::ObjectId& id);
    // boss-larva-0116: a made egg we own left the walk alive: its remove
    // (unless already sent), then eggs_ or its matched sibling freed.
    void EndMadeEgg(std::uint8_t self, const Object& o, std::uint16_t epoch, std::uint64_t now_us,
                    std::vector<coop::protocol::Message>& send, bool remove);
    // boss-sync-2 (issue 178): a fresh hatchling next to a copy of another
    // game's egg (still on the walk, or gone alive at most kHatchWindowUs
    // before, copy_eggs_): our game hatched it through its own AI.
    bool CopyHatch(std::uint8_t self, const Seen& s, std::uint64_t now_us) const;
    // The hatchling our game hatched out of a copy egg near `at` that no
    // owner's hatchling took yet (at most kOwnHatchWaitUs old), or null.
    Object* OwnHatchling(const coop::protocol::Vec3& at, std::uint64_t now_us);

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
    // The owner of the nearest `maker` within kMinionReachM of `at` (0: none known).
    std::uint8_t MinionOwner(std::string_view maker, const coop::protocol::Vec3& at) const;
    // Eggs we owned that left the walk alive, waiting kHatchWindowUs for the
    // hatchling that came out of them.
    struct Egg {
        coop::protocol::ObjectId id;
        std::uint16_t epoch = 0;
        coop::protocol::Vec3 position{};
        std::uint64_t at = 0;
        // boss-larva-0116: an egg our boss laid (made): its OBJECT_REMOVE went
        // as it left, so no second one and no listing.
        bool removed = false;
    };
    std::vector<Egg> eggs_;
    // churn-b: placed objects off the walk without a delete request (SetDeleteEvents).
    struct Away {
        coop::protocol::ObjectId id;
        std::uint64_t address = 0;
        std::uint64_t until = 0;
        std::string name;
        coop::protocol::Vec3 position{};  // where it was last seen (Left)
        std::uint64_t since = 0;          // when it left (kAwayEventMaxUs)
    };
    std::vector<Away> away_;
    // Row 175/176 (run 20261119-233327): an away object still off the walk after kAwayKeepUs with no delete request
    // is told gone (left), but our game still has it (a Poe sister vanishes for longer in her fight). Its id is kept
    // here until it comes back, our game deletes it (GameDeleted), the scene changes or kStillOursUs: meanwhile
    // its new owner's OBJECT_SPAWN makes no encounter copy here (HoldEncounters), which made a second Amy.
    std::vector<Away> still_ours_;
    bool delete_events_ = false;
    // enemy-left-0116: enemies of ours the game deleted with no death read,
    // listed until their OBJECT_REMOVE `gone` goes out (kLeftWaitUs).
    struct Left {
        coop::protocol::ObjectId id;
        std::uint64_t address = 0;
        std::uint64_t deleted_at = 0;
        coop::protocol::Vec3 position{};
        std::string name;
    };
    std::vector<Left> left_;
    std::vector<std::uint64_t> game_deletes_;  // when the game's own delete requests came (kDeleteBurstUs)
    // An enemy of ours that the game's own delete request ends for good (Left).
    bool LeavesForGood(std::uint8_t self, const coop::protocol::ObjectId& id, std::string_view name,
                       std::uint64_t address, const coop::protocol::Vec3& position) const;
    // boss-sync-2: copies of other games' eggs that left our walk alive (our
    // game hatched them), for CopyHatch.
    std::vector<Egg> copy_eggs_;
    std::uint64_t hatched_count_ = 0;
    std::string scene_;
    std::uint64_t scene_since_ = 0;  // server time of the scene's first walk
    std::optional<coop::protocol::Vec3> player_;
    bool in_event_ = false;  // issue 200: SetEvent
    std::uint64_t event_end_ = 0;  // when the last event here ended (SetEvent)
    bool event_ours_ = false;  // G1: our Link is in it (SetEvent's ours)
    // Row 97: why `o` is our running event's partner now (none: it is not):
    // it is an event member in our Link's event (member); it has no event member component and its
    // own AI asked for another animation since the event began, within
    // kEventNpcM (own_ai); a townsperson within kEventNpcM (issue 200).
    PartnerWhy EventPartnerWhy(const Object& o) const;
    static bool OwnerEventKeeps(const Object& o, std::uint64_t now_us);  // S2: OwnerEventHolds
    std::uint64_t event_since_ = 0;  // row 97: when our event began (SetEvent)
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
    // chest-late-b: why a chest read the open idle when another's flag came (log only).
    std::string ChestAlreadyOpenWhy(const Seen& s, std::uint64_t now_us) const;
    std::vector<std::string> notes_;
    std::map<std::uint64_t, Object> objects_;  // by actor address
    std::vector<std::uint64_t> ours_now_;     // copies that became ours (TakeOursNow)
    std::vector<std::pair<std::uint64_t, std::string>> own_wakes_;  // report 109 (TakeOwnWakes)
    std::vector<std::uint64_t> own_wake_starts_;                    // (TakeOwnWakeStarts)
    std::map<std::uint64_t, Decided> decided_;  // by director key
    // Report 99: late encounters other games announced that we lack, held
    // until our Link is near (IsLateEncounter). By id; first heard at got_us,
    // near since near_us (0: not near yet).
    struct Held {
        std::string name;
        coop::protocol::Vec3 position{};
        std::uint8_t owner = 0;
        std::uint64_t near_us = 0;
        std::uint64_t heard_us = 0;  // its OBJECT_SPAWN or last OBJECT_SAMPLE
    };
    std::map<coop::protocol::ObjectId, Held> held_;
    // Report 99 review: placed ids whose copy (decided_) was killed here this
    // visit. Our own one with that id is the same enemy, dead. Forgotten once
    // used (our own one died), on a new OBJECT_SPAWN for it (a respawn) and
    // on a scene change.
    std::set<coop::protocol::ObjectId> copy_killed_;
    // Gold Skulltula placements taken (GoldTaken), this scene.
    std::set<std::uint64_t> gold_taken_;
    void HoldEncounters(std::uint64_t now_us);
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
    bool life_events_ = false;
    std::size_t loose_drops_ = 0;
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
        std::map<std::uint64_t, std::uint64_t> show_failed;  // chests the open idle never took on (left closed): when
    };
    // Every loaded chest's animation and when it last changed (0: the same
    // since it was first seen), for the opening tests above.
    struct ChestAnimation {
        std::string name;
        std::uint64_t changed_us = 0;
        // chest-late-b (row #35 (2)): seen with its closed idle since it was
        // first seen (an "already open" chest that was closed here before).
        bool seen_closed = false;
    };
    std::map<std::uint64_t, ChestAnimation> chest_animations_;
    // Issue 195: a loaded closed chest that another player opened is shown
    // open here with its own open idle (kChestOpenIdle, through Plays), not
    // removed (it vanished in Alex's play-test). Address: when first played.
    // Still closed kChestShowFailUs after: left closed, never removed (chest-lid-b).
    static constexpr const char* kChestOpenIdle = "OpenWait";
    static constexpr const char* kChestClosedIdle = "Close";  // "CloseWait"
    static constexpr std::uint64_t kChestShowFailUs = 3'000'000;
    // deku-visuals-b (0.10.1 playtest: the lid did not move): before the
    // open idle, the chest's own opening command is played (ChestOpening:
    // "FrontOpenSmall" for Obj_TBox_A_01, "FrontOpen" for the others), so
    // the lid opens here as in the opener's game. Once it showed for
    // kChestLidUs (or its AS went on to OpenWait itself) the open idle is
    // played over it. An opening that does not take in kChestLidTryUs (a
    // chest whose AS has no such command) goes straight to the open idle.
    static constexpr std::uint64_t kChestLidUs = 1'500'000;
    static constexpr std::uint64_t kChestLidTryUs = 1'000'000;
    struct ChestShow {
        std::uint64_t since_us = 0;  // when first played (its current phase)
        bool took = false;           // seen showing kChestOpenIdle since
        unsigned flicks = 0;         // walks that saw its closed idle again after
        std::string opening;         // its opening command ("": none, the open idle at once)
        bool lid_done = true;        // the opening phase is over (the open idle is played)
        std::uint64_t lid_seen_us = 0;  // when the opening first showed (0: not yet)
        std::string idle_over = "Close";  // the open idle's only_over until it took
    };
    // deku-visuals-b: a web another player burnt plays its own kWebBurn
    // here (Plays) before its break or remove: kWebBurnUs once it showed,
    // or kWebBurnTryUs if it never takes (then as before).
    static constexpr const char* kWebBurn = "Burn";
    static constexpr std::uint64_t kWebBurnUs = 2'500'000;
    static constexpr std::uint64_t kWebBurnTryUs = 1'000'000;
    struct WebBurn {
        std::uint64_t since_us = 0;  // when first asked
        std::uint64_t seen_us = 0;   // when it first showed kWebBurn (0: not yet)
        bool failed = false;         // never took: broken as before
    };
    std::map<std::uint64_t, WebBurn> web_burns_;
    unsigned chests_opened_twice_ = 0;
    std::map<std::uint64_t, ChestShow> chest_shown_;
    std::uint64_t last_tick_us_ = 0;
    bool ticked_ = false;  // a Tick has run (last_tick_us_ is set)
    std::map<std::uint64_t, Taking> taken_;
    std::vector<std::uint64_t> takes_in_;  // guarded by inbox_mutex_
    std::set<std::uint64_t> hidden_;  // loaded actors whose pickup was taken
    std::set<std::uint64_t> dead_;    // copies whose owner said dead kDeadGraceUs ago
    bool wake_ = true;
    std::vector<std::string> unready_;
    std::vector<std::string> dying_;  // SetDying
    unsigned dying_finished_ = 0;     // DyingFinished
    std::size_t hurt_taken_ = 0;   // hurts the game took as asked
    std::size_t own_hits_kept_ = 0;  // OwnHitsKept
    std::size_t hurt_late_ = 0;    // late falls put back
    std::size_t hurt_missed_ = 0;  // hurts not taken after the wait (given to the Life step, or lost: never written)
    std::size_t boss_hits_stepped_ = 0;  // of those, another player's hits on our boss given to its Life step
    std::size_t split_kills_ = 0;  // copies dead here, owner alive after the resends
    BlastNear blast_near_;
    std::size_t blast_drops_ = 0;

    mutable std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
    bool forget_owners_ = false;  // guarded by inbox_mutex_
};

} // namespace CoopObjects
