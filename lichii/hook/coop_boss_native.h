// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native boss sync on Hookshot (docs/dungeons-boss.md, "Native boss
// sync"). No emulator code: tested in tests/lichii/boss_native_test.cpp.
//
// A boss is an enemy with the engine's BSA component (brain, sequence,
// action). Its brain AI picks a brain verb (the phase: Gohma's land battle,
// ceiling battle, wander) and asks for action verbs (walk, climb, attack,
// chance time, spawn), each through one game function:
//   set brain verb       main+0x6733b0 (x0 BSA, w1 verb, w2 force, x3 node)
//   request action verb  main+0xc08f0  (x0 BSA, x1 request, w2 changed)
// Both are wrapped (Hookshot's hook table, ids in hos_coop_bosses.h).
//
// The owner's game runs the boss: each change it makes is kept here and sent
// in the boss's samples (OBJECT_SAMPLE component 20, coop::protocol::BossBsa).
// A copy (another player's boss) gets the owner's changes through the same
// two functions, called by our caller on the game's calc thread; its own
// brain's changes are skipped at the wrap (the function does not run), once
// the owner's first state has come. So every game runs the same phases and
// action sequences: the game's own animations, movement and attacks.
//
// Except where the copy's own game must run it (boss-fix, issues of player
// reports #30/#31): while our game runs an event (the intro, the death; the
// set brain verb's callers include the game's events) and for a moment after
// our Link's own hit on a copy (its weak point hit, the stun and chance time
// are its own AI's reaction), the copy's own changes run and the owner's
// wait. Its own changes skipped just before (the event or the hit is seen up
// to a few frames late) are made then, late. A new mid-boss copy also runs
// its own start (its appear, issue 62) for kStartSteps, and so does one of ours
// handed to another game during its start, and any whose start animation still
// plays (#238), up to kStartMaxSteps. Afterwards the copy starts again
// from the owner's state. The owner keeps nothing of its own events (their
// verbs belong to its cutscene, not the fight) and sends its brain verb as a
// change once the event is over.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossNative {

using coop::protocol::BossAction;
using coop::protocol::BossBsa;
using coop::protocol::BossBrain;
using coop::protocol::BossChange;

namespace addr {
inline constexpr std::uint64_t kActorComponents = 0x270;  // actor -> component table
inline constexpr std::uint64_t kComponentBsa = 0x188;     // table -> BSA component
// Row 178: where the game keeps "this actor takes no hit now" (HitGuard). The Life component (table + 0x2b8)
// holds two lists of invincibility names (ExecuteInvincible / OneShotLifeSetInvincible push and pop them):
// counts at +0x1c70 and +0x1c38; its damage step (main+0x3a9984) refuses a hit while either is > 0 unless
// bit 6 of +0x1d18 is set. QueryIsInvincibleAny (main+0x2a32460) reads the same two counts.
inline constexpr std::uint64_t kComponentLife = 0x2b8;
inline constexpr std::uint64_t kLifeInvincibleNames = 0x1c70;  // s32
inline constexpr std::uint64_t kLifeInvincibleOther = 0x1c38;  // s32
inline constexpr std::uint64_t kLifeFlags = 0x1d18;            // u32, bit 6: hit through invincibility
// OneshotChangeForceNoHitMode (main+0x295f0d0) marks each of the actor's rigid bodies in a map at table +
// 0x240 (keys u64 body at +0x20, values u32 at +0x28, slots s32 at +0x34; a value != 0 = that body takes
// no hit, and the game turns its contact off), so a hit cannot reach it at all.
inline constexpr std::uint64_t kComponentNoHit = 0x240;
inline constexpr std::uint64_t kNoHitKeys = 0x20;
inline constexpr std::uint64_t kNoHitValues = 0x28;
inline constexpr std::uint64_t kNoHitSlots = 0x34;
inline constexpr std::int32_t kNoHitMaxSlots = 1024;  // (a sanity bound for the read)
inline constexpr std::uint64_t kBsaBrainVerb = 0x28;      // s32, the current brain verb
// Boss turns (0.11.8): the attributes of the BSA's current action verbs, as the game's own "has action verb
// attribute" reads them (main+0x507be0, called by QueryBSAIsLastFrameActionVerbAttributeOn and the other action
// queries): five s32 action verbs at +0x54 (one per action slot: main, sub, SP, SP2, SP3), each looked up in the
// action verb table at +0x1a40 (16-byte entries, 282 verbs: the entry of a verb below 282, else the first), the
// u64 at entry +8; the five words OR-ed. Attribute i is bit i in the order of the actor's
// ActionVerbAttributesSetting (Attack, murmur3 4009472201, is bit 0: AttributeFlag 1 on every verb whose
// Attributes list Attack). (The table at +0x1650, 63 entries indexed by the brain verb at +0x28, is the brain
// verb's: its bit 0 is Battle, murmur3 82424555; lane run 20261120-141109 read that one and held "attacking" on
// for the whole fight.)
inline constexpr std::uint64_t kBsaActionVerbs = 0x54;
inline constexpr std::size_t kBsaActionSlots = 5;
inline constexpr std::uint64_t kBsaActionAttributes = 0x1a40;
inline constexpr std::uint64_t kBsaAttributeStride = 0x10;
inline constexpr std::uint64_t kBsaAttributeBits = 0x8;
inline constexpr std::uint32_t kActionVerbCount = 282;
inline constexpr std::uint64_t kAttributeAttack = 1u << 0;
// Where action slot `slot`'s verb is, and where the attribute bits of action verb `verb` are. A verb outside 0..281
// (an empty slot) maps to the first entry, exactly as main+0x507be0 does (its csel picks the table's start), so our
// read ORs the same bits the game's own queries see (review-charsel-0118 m1).
constexpr std::uint64_t ActionVerbAt(std::uint64_t bsa, std::size_t slot) { return bsa + kBsaActionVerbs + 4 * slot; }
constexpr std::uint64_t ActionAttributesAt(std::uint64_t bsa, std::int32_t verb) {
    const auto i = static_cast<std::uint32_t>(verb);
    return bsa + kBsaActionAttributes + (i < kActionVerbCount ? i * kBsaAttributeStride : 0) + kBsaAttributeBits;
}
// Interrupt (murmur3 4112727460) is attribute 1: the reaction moves (a hit, a stun's start) that cut a move.
inline constexpr std::uint64_t kAttributeInterrupt = 1u << 1;
// An attack, or a reaction that cut one (Interrupt): one frame's reading, with no memory.
constexpr bool IsAttack(std::uint64_t bits) { return (bits & (kAttributeAttack | kAttributeInterrupt)) != 0; }
// Boss turns: a boss's attack sequence over the frames, as its samples send it (object_state::attacking). A turn
// passes only where this falls, and it falls only where an attack ENDS BY ITSELF (the Attack attribute falls into
// a plain move: Phantom Ganon's RideJumpAttack 115 into 96, each good turn of run 20261120-184021).
// A reaction (Interrupt) cuts the sequence: the plain move after it is the reaction's own tail, not a choice of
// the boss's AI, and the game hands it nothing to end it with in another game. Phantom Ganon hit into verb 134
// went on into 'Run' (verb 40, attributes 0: his retreat into a painting); handed over there (run
// 20261120-184021, and at the reaction's start in 20261120-180245) the new owner's game started that Run over
// and nothing ever ended it. So after a reaction the sequence stays on until the boss's own next attack, and the
// turn passes at THAT attack's end. `cut` is that memory, kept per actor by the walk.
// Only a boss (`latch`): a plain enemy's turn may also end on the idle rule (server/boss_turns.hpp idle_us),
// which needs its quiet time read as quiet.
struct AttackSequence {
    bool cut = false;  // a reaction ran since its last attack
};
constexpr bool InAttackSequence(AttackSequence& s, std::uint64_t bits, bool latch) {
    if ((bits & kAttributeAttack) != 0) {
        s.cut = false;  // its own next attack: the sequence ends where this attack does
        return true;
    }
    if ((bits & kAttributeInterrupt) != 0) {
        s.cut = latch;
        return true;
    }
    return s.cut;
}
inline constexpr std::uint64_t kSetBrainVerb = 0x6733b0;       // main+
inline constexpr std::uint64_t kRequestActionVerb = 0xc08f0;   // main+
inline constexpr std::uint64_t kMalloc = 0x14c6c90;            // main+, as coop_puppet.h
// What kRequestActionVerb returns for a request it took (its main path,
// main+0xc10f4; 2 and 3 are refusals). A skipped request answers the same.
inline constexpr std::uint64_t kActionTaken = 0;

// The action request (0x60 bytes, built on the asker's stack).
inline constexpr std::size_t kRequestSize = 0x60;
inline constexpr std::size_t kReqPlaces = 0x00;   // 3 x Vec3; the first float NaN: no place
inline constexpr std::size_t kReqKind = 0x24;     // u32 4 (checked by the game)
inline constexpr std::size_t kReqKind2 = 0x28;    // u32 0x11a (checked)
inline constexpr std::size_t kReqExtra = 0x2c;    // 2 x f32, NaN: none
inline constexpr std::size_t kReqKind3 = 0x34;    // u32, 0x10 as the AI nodes make it
inline constexpr std::size_t kReqPlug = 0x38;     // a plug pointer, used when the byte at 0x40 is 1
inline constexpr std::size_t kReqHasPlug = 0x40;
inline constexpr std::size_t kReqSlot = 0x48;     // s32
inline constexpr std::size_t kReqVerb = 0x4c;     // s32
inline constexpr std::size_t kReqFlags = 0x50;    // u16
// Flags bit the request function reads as a new move (main+0xc0f4c: the same verb then counts as another, the
// move starts over from its start, its module's own sub-state with it).
inline constexpr std::uint16_t kReqStartOver = 0x100;
inline constexpr std::size_t kReqNode = 0x58;     // the asking AI node
inline constexpr std::uint32_t kKind = 4;
inline constexpr std::uint32_t kKind2 = 0x11a;
inline constexpr std::uint32_t kKind3 = 0x10;
} // namespace addr

using Request = std::array<std::uint8_t, addr::kRequestSize>;

// The protocol's action from a request as the game made it; nothing when its
// checked words are not the game's. `plug` says the request named its places
// through a plug (those are not sent: the copy then has its own places).
std::optional<BossAction> ActionFromRequest(const Request& r, bool* plug = nullptr);
// A request for the game from the owner's action, asked by `node` (the copy's
// own AI node that asks this move: callers always name one).
Request RequestFromAction(const BossAction& a, std::uint64_t node);

// Row 177 (Phantom Ganon never seen out of his painting on a copy, so never hit there): what the copy's own AI
// names when it asks a move itself, its asking node and its plug. The game's request function (main+0xc08f0)
// keeps the asking node in the move's slot and, for the same verb asked again with its "changed" argument,
// compares that node's name with the request's plug name (main+0xbe070); a mismatch starts the move over. The
// owner's game always names the node that asks that move; a copy that named whichever node of its own asked last
// (another move's) had its move started over by the owner's every ask: PhantomGanon.action.appear waits one step
// (ExecuteGenericDummyAction) before its fade-in (EnemyCommon.OneShotAppearOrDisappear, which also ends his
// forced no-hit), so he stayed unseen and unhittable on every copy while the owner's Disappear (no wait) worked.
struct OwnAsk {
    std::uint64_t node = 0;
    std::uint64_t plug = 0;
    bool has_plug = false;
    bool operator==(const OwnAsk&) const = default;
};
// The request for the owner's action as the copy's own AI would ask it: `ask`'s node, and its plug when the
// owner's action names no place (the owner's own was asked through a plug: its places were not sent).
Request RequestFromAction(const BossAction& a, const OwnAsk& ask);
// The copy's own ask to name for `a` from what its own AI asked: the same slot and verb, else the slot's last,
// else its last ask of all (`last`). Nothing when it never asked (no node yet).
std::optional<OwnAsk> AskFor(const std::map<std::pair<std::int32_t, std::int32_t>, OwnAsk>& by_move,
                             const std::map<std::int32_t, OwnAsk>& by_slot, std::uint64_t last, const BossAction& a);

enum class Role : std::uint8_t { owner, copy };
// A boss whose new copy runs its own start first (Table::kStartSteps): a
// mid-boss (Enemy_MiddleBoss_*: Lizalfos, Stalfos, ...) appears in its room
// by its own AI with no intro event to run it (a dungeon boss's intro is an
// event: SetEvent covers it).
bool RunsOwnStart(std::string_view name);
// #238: whether a mid-boss's AS slot 0 animation is its start (the Lizalfos's 'Appear': it jumps into the fight
// and shows only once its own appear has played).
bool PlaysOwnStart(std::string_view animation);

// Whether a plain enemy's death runs here now: its life is 0 in our game, or its owner's died.
bool PlainDeath(std::optional<std::int32_t> life, bool owner_dead);

// Row 149's stall check (Table::kStallSteps) judges only a fight move: an animation read, a life above 0, and
// neither a death animation (coop::protocol::anim::IsDeath) nor a cutscene's (anim::IsEvent). Every boss and every
// part (Twinrova's sisters, Bongo Bongo's hands, Ganon) is judged by its own animation and life.
bool StallJudged(std::string_view animation, std::optional<std::int32_t> life);

// Row 138 (2)(7), #238: whether our game's own hit on a copy took life (its life read before the hit, in the walk
// before, and after it). Such a hit is the owner's to judge: its DAMAGE goes to the owner, the server keeps the boss
// with its owner (only a hit that takes no life, a stun, hands it over), and the owner's game's reaction comes back
// as its verbs. So the copy's own brain stays skipped (Table::NoteOwnDamage). A life not read either time: unknown,
// false (the copy runs its own reaction as before).
bool OwnHitTookLife(std::optional<std::int32_t> before, std::optional<std::int32_t> after);

// One boss the Objects step saw in its walk.
// Row 178: the game's own "takes no hit now" state of an actor (addr::kLifeInvincible*, kComponentNoHit),
// as its damage step and QueryIsInvincibleAny read it. A hit its own game would refuse (an invincibility,
// or every rigid body in no-hit mode) is TakesNoHit.
struct HitGuard {
    bool read = false;             // the Life component was read (else nothing is known: not refused)
    std::int32_t invincible_names = 0;
    std::int32_t invincible_other = 0;
    std::uint32_t life_flags = 0;
    std::int32_t bodies = 0;       // rigid bodies in the no-hit map
    std::int32_t no_hit_bodies = 0;  // of them, in no-hit mode
    bool TakesNoHit() const {
        const bool invincible = (life_flags & 0x40) == 0 && (invincible_names > 0 || invincible_other > 0);
        return read && (invincible || (bodies > 0 && no_hit_bodies == bodies));
    }
};

struct Boss {
    std::uint64_t actor = 0;
    std::uint64_t bsa = 0;  // [[actor + 0x270] + 0x188]
    Role role = Role::owner;
    std::string name;
    std::uint8_t owner = 0;  // the owning player's number (a new one: a copy starts again)
    // Its Life component (0: not read). A copy's is held at life 1 by its own
    // Life step (HeldLifeStep) until `owner_dead`: the owner's game said it
    // died (its life 0 in the owner's samples, or its ObjectRemove dead).
    std::uint64_t life_component = 0;
    bool owner_dead = false;
    // A plain enemy (every Enemy_ that is not IsBoss: a Deku Baba, a Skulltula, a Lizalfos outside a mid-boss
    // room), on the same path as a boss: one model for every enemy (fable-g4-enemies). What differs:
    // - its Life is never held here: the Sync's own rules share its kills (issue 133), a copy dies of our hit;
    // - `own_death`: its death runs here now (its life is 0 in our game, or its owner's died): the copy runs
    //   every change of its own from then on, as a boss's named by SetDying;
    // - its own brain asking for another brain verb than the owner's is told (TakeOwnWants): it wants to act
    //   on our Link (the wake and chase rules of the Sync hand it over, and NoteOwnWake lets it run).
    bool plain = false;
    bool own_death = false;
    // Row 149 (the stall check, Table::kStallSteps): what our walk read of it now, its AS slot 0 animation and its
    // life (nothing: not read).
    std::string animation = {};
    std::optional<std::int32_t> life = std::nullopt;
    // Row 178 (the fall guard, Table::kFallBelowStart): its place in our walk (nothing: not read).
    std::optional<coop::protocol::Vec3> position = std::nullopt;
    // Boss turns: its current action verb's Attack attribute in our game (addr::IsAttack; nothing: not read).
    std::optional<bool> attacking = std::nullopt;
};

// Boss turns (0.11.8, server/boss_turns.hpp): LICHII_COOP_BOSS_TURNS's text; on unless "0".
constexpr bool TurnsSwitch(std::string_view value) { return value != "0"; }

// What a copy that becomes ours does (Table::Publish), condition (1) of the boss turns: a hand-over between moves
// must let the new owner's own brain go on from where the copy stands, not start the phase or the move over.
enum class MadeOurs : std::uint8_t {
    resume,        // its own AI goes on: nothing asked again (a turn between moves)
    restart_move,  // its move starts over as ours (row 178: made ours mid-move)
    rechoose,      // its brain verb is set again (row 149: its AI never ran its body as a copy)
};
// `turns`: the switch; `attacking`: the move its body runs is an attack in our game (nothing: not read);
// `put_action`: the owner's move was put into it; `own_ask_same`: its own AI asked that same move itself (slot and
// verb: its own brain is in the move its body runs); `own_node`: its own AI named a node for it.
constexpr MadeOurs PlanMadeOurs(bool turns, std::optional<bool> attacking, bool put_action, bool own_ask_same,
                                bool own_node) {
    if (turns && attacking.has_value() && !*attacking && put_action && own_ask_same) {
        return MadeOurs::resume;
    }
    return put_action && own_node ? MadeOurs::restart_move : MadeOurs::rechoose;
}

struct Counts {
    std::uint64_t owner_brain = 0;    // the owner's brain verb changes kept
    std::uint64_t owner_action = 0;   // the owner's action requests kept
    std::uint64_t plug_actions = 0;   // of those, named through a plug (places not sent)
    std::uint64_t copy_skipped = 0;   // a copy's own changes not run
    std::uint64_t brain_applied = 0;  // owner's brain verbs put into a copy
    std::uint64_t brain_came = 0;     // owner's brain verbs a copy took from its queue (a change, or a resync's state);
                                      // one queued and then replaced by a resync is not counted
    std::uint64_t brain_already = 0;  // of those, a resync's brain verb the copy was in already (no call made)
    std::uint64_t action_applied = 0; // owner's actions put into a copy
    std::uint64_t action_waiting = 0; // owner's actions waiting for the copy's own node
    std::uint64_t resyncs = 0;        // a copy started again from the owner's state now
    std::uint64_t drift_fixes = 0;    // a copy's brain verb put back after it differed for long
    std::uint64_t busy = 0;           // a wrap that found the table busy (the game ran its own)
    std::uint64_t minions_stopped = 0; // a copy boss's own minion not made (the owner's comes)
    std::uint64_t copy_own_run = 0;   // a copy's own change run (our event, our hit)
    std::uint64_t copy_own_late = 0;  // a copy's own skipped change made late (event or hit seen after it)
    std::uint64_t copy_life_held = 0; // a copy's own Life step that would have killed it, held at 1
    std::uint64_t copy_killed = 0;    // a copy let die by its Life step after its owner's died
    std::uint64_t copy_finished = 0;  // a dying copy's Life step that its own rules left alive, ended (#105)
    std::uint64_t copy_shot_hits = 0; // our own shot's end at a copy, its own reaction let run (NoteOwnShotEnd)
    std::uint64_t copy_own_stuns = 0; // a copy's own stun begun in our game, its own time (NoteOwnStun; row 169)
    std::uint64_t copy_hits_to_owner = 0; // our own hit on a copy that took life: the owner reacts (NoteOwnDamage)
    std::uint64_t plain_wants = 0;    // a plain enemy copy's own brain asked another brain verb than its owner's
    std::uint64_t plain_wakes = 0;    // a plain enemy copy's own wake let run (NoteOwnWake)
    std::uint64_t plain_deaths = 0;   // a plain enemy copy whose death began here: its own runs
    std::uint64_t owner_stalls = 0;   // row 149: our boss held one move kStallSteps with no life change: re-chosen
    std::uint64_t owner_rechosen = 0; // its brain verb set again by our caller (a stall, or a copy just made ours)
    std::uint64_t owner_restarted = 0; // row 178: a copy made ours mid-move: that move started over as ours
    std::uint64_t owner_fell = 0;      // row 178: our boss fell far below its fight start (the fall guard)
    std::uint64_t moves_kept = 0;     // a resync's action the copy already runs (put in, none of its own since): not asked
};

class Table {
public:
    // Switches (LICHII_COOP_BOSS_NATIVE, LICHII_COOP_BOSS_ACTIONS): off -> no
    // skip, no apply, nothing kept (the old gohma-sync path alone).
    void SetOn(bool brain, bool actions);
    // Boss turns (LICHII_COOP_BOSS_TURNS, TurnsSwitch): a copy made ours between moves resumes (PlanMadeOurs), and
    // every boss fights the Player only (PlayerOnlyBoss names any boss of ours or a copy: every puppet unseen while
    // one is here; replayed hits name our Link): each player gets the boss in their own game by its turns. Off: only
    // FightsPlayerOnly's two (Phantom Ganon, Ganondorf), as before.
    void SetTurns(bool on);
    bool TurnsOn() const;
    bool BrainOn() const;
    bool ActionsOn() const;

    // The Objects step, after each walk (the frame thread): the bosses now.
    // A boss gone from the walk is forgotten; one whose role changed starts
    // again from its current verb.
    void Publish(const std::vector<Boss>& bosses);

    // The wraps, phase 0 (any game thread). own: the call is our caller's.
    // True: skip the game's function (a copy's own change). Never blocks:
    // a busy table lets the game run its own. changed: the game's w2 (the
    // verb changed), sent with the owner's action and given to the copies.
    bool OnBrainVerb(std::uint64_t bsa, std::int32_t verb, bool force, bool own);
    bool OnActionVerb(std::uint64_t bsa, const Request& request, bool changed, bool own);

    // Our game runs an event now (the boss glue, every frame; the player
    // reader's event flag). Copies then run their own changes; the owner keeps
    // none of its event's.
    void SetEvent(bool running);
    // Our game took a hit of its own on this boss that took no life (the Objects walk: its Life's last hit id
    // moved, not to one we replayed, and OwnHitTookLife is false; or our shot ended at it). A copy then runs its
    // own reaction for kReactSteps: the stun, chance time and weak-point reactions are its own AI's, and the
    // stun's DAMAGE of 0 hands the boss to us meanwhile.
    // by_shot: NoteOwnShotEnd's reading (counted in Counts::copy_shot_hits).
    void NoteOwnHit(std::uint64_t actor, bool by_shot = false);
    // Our game's own hit on this copy took life (OwnHitTookLife). Row 138 (2)(7), #238: it ran the copy's own
    // brain for kReactSteps too, every hit again, so with 3 or 4 players hitting a Lizalfos or King Dodongo each
    // hitter's copy ran its own AI against its own Link for much of the fight (one 0.11.4 host log: 11,230 own
    // changes run), its jump slash went another way per game, then it snapped back to the owner's state (King
    // Dodongo twitching, frozen in his roll). Now nothing of its own runs: the owner judges the hit (its DAMAGE)
    // and its reaction comes as verbs. An own time already running (a stun of ours) is left as it is. Counted.
    void NoteOwnDamage(std::uint64_t actor);
    // Row 169 (King Dodongo frozen in 'Down'): the objects walk reads this copy in a stun its owner's boss is not in
    // (CoopObjects::Sync::TakeOwnStuns, every walk while it plays). A weak-point interrupt (a bomb King Dodongo
    // swallowed, a seed in Gohma's eye) is the boss's own interrupt action, not an action request our wrap sees, so
    // it runs in a copy whose brain is skipped and the copy's own game is stunned with no own time running. The
    // stun's DAMAGE of 0 then handed the boss to us, the row-149 rule took it for a copy whose AI never ran its body
    // and set its brain verb again (force), which cut the stun short: the new owner's boss lay in 'Down' at life 1,
    // its chance time gone, no hit taken, the stall check's re-chooses no way out, until another player's stun
    // took it back. Now the stun is the copy's own time (as NoteOwnHit, kReactSteps after the stun's last frame):
    // its own interrupt and AI run, and a hand-over during it keeps the stun (no re-choose). Counted.
    void NoteOwnStun(std::uint64_t actor);
    // Issue 150 (boss-stun-c): our game's own shot ended where it hit or went
    // off (coop_spawn's flights of ours: a seed, arrow, nut, bomb, bombchu, with
    // the remove's place; any thread). A weak-point hit that takes no life (a
    // seed or nut in Gohma's eye, a bomb in King Dodongo's mouth, a shot at
    // Bongo Bongo's hand) never moves the Life's last hit id, so NoteOwnHit's
    // reading never saw it, the copy's own stun was skipped as its own change
    // and only the owner's game could stun the boss. The objects walk asks
    // OwnShotNear for every boss it reads and calls NoteOwnHit on a copy the
    // end lies at. Kept kShotEndKeepSteps.
    void NoteOwnShotEnd(const coop::protocol::Vec3& at);
    // One of our shots ended within kShotNearBoss of `at` (a boss actor's
    // place, or one of its parts') in the last kShotEndKeepSteps.
    bool OwnShotNear(const coop::protocol::Vec3& at) const;
    // A plain enemy copy wakes for our Link (the Sync's own wake, reports 109/111: a Deku Baba rising while its
    // owner's rests): its own brain runs for kWakeSteps, the change it asked a moment ago is made late (its
    // rise), then the owner's state again (by then the wake has handed it to us, or the owner's enemy acts).
    void NoteOwnWake(std::uint64_t actor);
    // Whether NoteOwnWake runs this actor's wake: a plain enemy copy whose own brain is skipped (the owner's
    // verbs came). Its wake is then its own brain's alone; the Mover's pose-era give-back of the wake's
    // animation (Sync::TakeOwnWakes) is for the other copies only, or the rise is played twice (rows 109/111).
    bool WakesOwnBrain(std::uint64_t actor) const;
    // Row 178: whether this actor is a boss (never a plain enemy) on the brain path: a copy of it made ours
    // goes on through its own AI here (its move started over as ours, or its AI chooses again), so the Mover's
    // issue-157 replay of the animation its AI wanted as a copy is not played over it. That replay put
    // SpearAttack_Ready over Phantom Ganon's running RushAttack: its module waits for the dive's AS hold events
    // to return up, they never came, and he fell ~3 km (Geno's log). Plain enemies keep the replay (issue 157).
    bool RunsOwnMoveWhenOurs(std::uint64_t actor) const;
    // Row 178 (perception on): whether this actor is a boss of the table (never a plain enemy), brain path on or
    // off. A hit of another player replayed on it names our Link as the attacker, never their puppet
    // (CoopHitReplay::ReplayAttacker): a boss's attacks wait on the game's attack permission for its battle target
    // (QueryTargetGetTargetInfoActor's isForbidAttackPermission, read by Phantom Ganon's Fly and every Ganondorf
    // move) and the game gives that permission to the Player only, so with a puppet in its target memory
    // (EnemyCommon.BattleCommon's ExecuteMemoryPushLastAttacker) Phantom Ganon flew ~22 s without a throw.
    bool KeepsLinkAsAttacker(std::uint64_t actor) const;
    // Row 178: whether this actor is a boss of ours (its owner's game is this one) in a move its own game takes
    // no hit in (its game says so: HitGuard::TakesNoHit). Phantom Ganon's phase 2 started from another
    // player's hit replayed while he rode inside his painting ('Run'), where no real hit reaches him: in
    // LANE2 runs that start left him in Fly with no light ball in 6 of 10 (the stall check ended each), from
    // 'Appear' (hittable) 0 of 2. Such a hit goes to its Life step as life only (RouteLife), so its damage
    // reaction (his phase change) waits for a hit it can take.
    // `guard` is what its own game says right now (HitGuard, read by the caller from the actor): for every
    // boss of the table, no move list.
    bool OutOfReach(std::uint64_t actor, const HitGuard& guard) const;
    // The move (animation) of a boss of ours (owner, not plain), for the log; nullopt for any other actor.
    std::optional<std::string> OurBossMove(std::uint64_t actor) const;
    // Row 178 (Geno, Forest Temple): a boss whose own native AI (Execute*MetaAI) picks its moves for a battle
    // target that must be the Player. Its perception (PerceptionFilterPlayerOnly) takes a perceived puppet too
    // (Affiliation Player), and then that AI never picks an attack: Phantom Ganon flew his phase 2 with no light
    // ball until our stall check (LANE2: 4 of 4 runs with perceived puppets, 0 of 4 without, and normal with the
    // puppets' perception emitter off at runtime). While such a boss is here every puppet is unseen
    // (coop_dbno_body.h's unseen puppets), so its target is a real Link in every game.
    static bool FightsPlayerOnly(std::string_view name);
    // The first such boss of the table now (alive in our walk, not dying), or none.
    std::optional<std::string> PlayerOnlyBoss() const;
    // The plain enemy copies whose own brain asked for another brain verb than their owner's since the last
    // call (skipped at the wrap): their own AI wants to act here. At most kMaxWants kept.
    std::vector<std::uint64_t> TakeOwnWants();
    // Row 178: our own bosses that fell kFallBelowStart below their fight start since the last call, each with
    // that start (its first seen place): put back there (coop_objects Place, as a copy is placed).
    std::vector<std::pair<std::uint64_t, coop::protocol::Vec3>> TakeReturns();
    // The boss's death has begun (the boss glue, every frame: the area rules'
    // fight is at its death or past it, ours or the owner's; player report
    // #105, a black screen after Gohma's death). Bodies named here are copies
    // no more: their own game runs every change of theirs (the death setting
    // our game was asked, InsideTheDekuTree_BossDie, drives our copy through
    // its own death, the blue warp and the heart container its prefab makes)
    // and their Life step is not held (it dies here of its own). The ones
    // skipped just before are made late. An empty list: none (a new fight).
    void SetDying(std::vector<std::string> bodies);

    // The game's Life step (patch 0007, any game thread), after every other
    // change to the frame's damage `sum`: for a copy boss's Life component
    // whose owner still lives, what its game takes so its life stays at
    // least 1 (`life`: its life before the step). One death for everyone: a
    // copy never dies of its own game's hits (they still go to the owner as
    // DAMAGE); it dies when the owner's does, through this same step (the
    // owner's death comes as its life 0: CoopObjects Lower). Lock-free.
    std::int32_t HeldLifeStep(std::uint64_t component, std::int32_t life, std::int32_t sum);
    // A copy boss's Life component HeldLifeStep watches (the hook reads its life only then). Lock-free.
    bool Watches(std::uint64_t component) const;
    // A copy boss whose death is its owner's: its death has begun here (SetDying) or its owner's died (released).
    // Its Life step then goes past its own life stops (CoopActorEvents::PassStops). Lock-free.
    bool Finishes(std::uint64_t component) const;

    // A create request (any game thread): true when a boss named `maker` is a
    // copy here that takes its owner's verbs, so its own minion is not made
    // (the owner's comes as a copy; CoopObjects::MinionMaker). Never blocks.
    bool CopyMakes(std::string_view maker);

    // The copies that run their own changes now (our event and kAfterEventSteps after it, kReactSteps after our
    // hit, a new mid-boss's kStartSteps, its death: OwnTime), by actor. Row 130: the objects layer puts none of
    // their owner's place, animation or frame on them meanwhile (CoopObjects), so their own game's event or
    // reaction runs to its end. Empty while the native sync is off (the old path: the owner's motion alone).
    std::vector<std::uint64_t> CopiesRunningOwn() const;

    // The owner's state to send for one of our bosses; nothing before its
    // first change was seen.
    std::optional<BossBsa> OwnerState(std::uint64_t actor) const;

    // An owner's state for a copy (from its OBJECT_SAMPLE). Its changes the
    // copy has not had are queued in the owner's order and made by Step, one
    // call each. When the copy missed more than the sample holds, or the
    // owner is new (Publish), it starts from the state now. An older sample
    // than the newest had (seq not above it) is dropped.
    void Receive(std::uint64_t actor, const BossBsa& state);

    // The caller (coop_callers.h contract), order kCallerOrder.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t main_base);

    Counts Totals() const;
    // Lines for the log since the last call (first sightings, first applies).
    std::vector<std::string> TakeNotes();
    // Everything forgotten, the block too (tests; co-op off).
    void Reset();

    static constexpr int kCallerOrder = CoopCallerOrders::kBossTable;  // after the drops (40), before the boss rooms (50)
    // Steps a copy's brain verb may differ from the owner's (nothing queued)
    // before it is put back (about a second at the game's 60 frames).
    static constexpr int kDriftSteps = 60;
    static constexpr int kMaxDriftFixes = 5;  // put-backs per owner state the game refuses, then left alone
    // A copy's own changes skipped up to this many steps before an event or
    // its own hit was seen are made then (about 0.2 s).
    static constexpr std::uint64_t kLookBackSteps = 12;
    // Steps a copy runs its own changes after our own hit on it (about 2 s:
    // the stun's start; its DAMAGE of 0 hands the boss to us meanwhile).
    static constexpr std::uint64_t kReactSteps = 120;
    // Steps a copy still runs its own changes after our event ended (about
    // 2 s). An intro that began the fight here, while the owner's game has
    // not played its own (its boss still pre-battle), hands the boss to us
    // meanwhile (the server's unready rule); taking the owner's pre-battle
    // state at once would put ours back on the ceiling, and its own AI would
    // ask the intro again (boss-copy-b).
    static constexpr std::uint64_t kAfterEventSteps = 120;
    // Steps a new mid-boss copy runs its own start (RunsOwnStart; about 3 s:
    // a Lizalfos's jump into the fight) before the owner's state.
    static constexpr std::uint64_t kStartSteps = 180;
    // #238 (Revirie's 0.11.0 log, Dodongo's Cavern 2F): our two Lizalfos were made ours during player 1's
    // FocusOnEnemy_02 and handed to player 1 while their appear still played; as copies they took the owner's
    // verbs at once, the appear never finished ('Appear', moved 0.00) and one stayed unseen on our screen while it
    // fought. A mid-boss of ours made a copy within kStartSteps of its first sighting, or while its start
    // animation plays (PlaysOwnStart), runs its own start on for kStartSteps; a copy whose start animation still
    // plays when that time ends keeps it, up to kStartMaxSteps from its first sighting (about 10 s).
    static constexpr std::uint64_t kStartMaxSteps = 600;
    // A shot's end counts for a boss actor this near (metres, the actor's
    // place: Gohma's eye is about 3 m from it on the floor and on the
    // ceiling, King Dodongo's mouth about 4 m). A wall hit this close only
    // runs the copy's own AI for kReactSteps, then the owner's state again.
    static constexpr float kShotNearBoss = 6.0f;
    // Steps a shot's end is kept for the walks (about 0.3 s: the shot's
    // remove and the objects walk are a few frames apart).
    static constexpr std::uint64_t kShotEndKeepSteps = 20;
    static constexpr std::size_t kMaxShotEnds = 16;
    // Steps a plain enemy copy runs its own wake (about 3 s, the Sync's kOwnWakeUs).
    static constexpr std::uint64_t kWakeSteps = 180;
    // Row 149 (King Dodongo rolling forever, unwinnable): our own boss (never a plain enemy) whose AS slot 0
    // animation, brain verb and last action verb stayed the same for this many steps (about 25 s at 60 frames)
    // with its life unchanged, while its AI kept asking actions (the held move asked again: it is in a fight,
    // not waiting), is stuck in that move. Its brain verb is then set again (force) through the game's own
    // function by our caller: its AI leaves the held move and chooses again, every copy does the same (the
    // change is sent as any), and the log says so. No real move holds one animation that long with no hit
    // taken (King Dodongo's roll ends at a wall within a few seconds, Barinade's spin, Bongo Bongo's hands,
    // Morpha's and Volvagia's moves each change animation; Gohma waiting on the ceiling asks no actions).
    // The server frees the fight lock of such a boss on its own (boss_stall_us), so it can be handed over.
    static constexpr std::uint64_t kStallSteps = 1500;
    static constexpr int kMaxStallRechoices = 3;  // per held move; then left (the server's hand-over is next)
    // Row 178's fall guard: our own boss (never a plain enemy, not dying, not in our event) whose place is this
    // far below its first seen place (its fight start: no boss room's floor is 40 m under its boss's start;
    // free fall covers it in under 3 s) fell out of its room: its AI chooses again once per fall (its brain
    // verb set again through the game's own function, as a stall: the change goes to every copy), the log says
    // so, at most kMaxFallRechoices times while it stays below. Row 178 again (the Forest playtest: Phantom Ganon fell
    // ~3 km, and a new AI choice does not bring a falling body back up): each time it is also put back at its fight
    // start (TakeReturns: the game's own set-matrix call the copies are placed with), then its AI chooses again.
    static constexpr float kFallBelowStart = 40.0f;
    static constexpr int kMaxFallRechoices = 3;
    static constexpr std::size_t kMaxWants = 64;
    static constexpr std::size_t kMaxSkippedKept = 8;
    static constexpr std::size_t kMaxHeld = 8;  // copy bosses held at once (more: not held)
    static constexpr std::size_t kMaxOwnAsks = 64;     // moves kept per copy (AskFor)
    static constexpr std::uint64_t kMaxStreamNotes = 240;  // row 177's action stream lines per game

private:
    struct Entry {
        std::uint64_t actor = 0;
        Role role = Role::owner;
        std::string name;
        std::uint8_t owner = 0;
        // Owner: what we send.
        BossBsa mine{};
        bool have_mine = false;
        // Copy: the owner's latest, and its changes still to make here.
        std::optional<BossBsa> wanted;
        struct Due {
            BossChange change;
            bool resync = false;  // the state now, not a call the owner made: skipped when already so
            bool counted = false; // (a brain verb: counted in brain_came once, also when asked again)
        };
        std::deque<Due> due;
        std::uint32_t queued_seq = 0;  // the owner's newest change queued (or made)
        int drift = 0;                 // steps the brain verb has differed with nothing queued
        int drift_fixes = 0;           // put-backs since the owner's last new state
        std::uint64_t own_node = 0;    // the copy's own AI node that last asked for an action
        // Row 177: the copy's own asks by move (slot, verb) and by slot (AskFor), at most kMaxOwnAsks moves.
        std::map<std::pair<std::int32_t, std::int32_t>, OwnAsk> own_asks;
        std::map<std::int32_t, OwnAsk> own_slot_asks;
        // Row 177's log: the action stream's last kind (who, slot, verb, changed, plug or node source) and its run.
        std::string stream_last;
        std::uint64_t stream_run = 0;
        // Copy: its own changes skipped lately (step, change), and those to
        // make late; the step until which its own run (our hit); whether its
        // own ran last step (then the owner's state again).
        struct Skipped {
            std::uint64_t step = 0;
            std::variant<BossBrain, BossAction> what;
        };
        std::deque<Skipped> skipped;
        std::deque<std::variant<BossBrain, BossAction>> own_late;
        // The front change of `due` or `own_late` is in the call Step asked last: it leaves its queue only once
        // that call is made (Step's after_call); a call not made asks it again. A queue cleared meanwhile: none.
        enum class Asking : std::uint8_t { none, due, late } asking = Asking::none;
        std::uint64_t react_until = 0;  // (also a new mid-boss copy's start, kStartSteps)
        std::uint64_t seen_since = 0;   // the step it was first seen in our walk (its start's: #238)
        bool held = false;
        // Owner: our event ended: send our brain verb as a change.
        bool record_after_event = false;
        std::uint64_t life_component = 0;  // (Publish's, for SetDying's release)
        bool dying = false;                // its death has begun (SetDying; a plain one's own_death): its own game runs it
        bool plain = false;                // (Boss::plain)
        // Owner (row 149): the move it holds now (animation, brain verb, last action verb, life), since which
        // step, the action requests its AI made since, and how often it was re-chosen in this move. `rechoose`:
        // Step sets its brain verb again (a stall, or a copy just made ours whose AI did not run its body).
        std::string hold_animation;
        std::int32_t hold_brain = 0;
        std::optional<std::int32_t> hold_action;
        std::optional<std::int32_t> hold_life;
        std::uint64_t hold_since = 0;
        std::uint64_t hold_asks = 0;
        int hold_rechoices = 0;
        bool rechoose = false;
        // The move last re-chosen as stuck, and how often. The re-choice itself makes its AI ask another move
        // for a moment before it falls back into the stuck one (Phantom Ganon: 'Run' verb 40, then 96 for 22
        // frames, then 40 again; run 20261120-184021), which read as a new move and started the count over: "1
        // of 3" four times, 100 s, and the server's hand-over (after kMaxStallRechoices) never came. Back in this
        // same move at the same life, the count goes on. A hit that took life forgets it.
        std::string stall_animation;
        std::int32_t stall_brain = 0;
        std::optional<std::int32_t> stall_action;
        std::optional<std::int32_t> stall_life;
        int stall_rechoices = 0;
        // Row 178: a copy made ours in the middle of a move (put_action) whose AI did not run its body: that move
        // asked again as ours with kReqStartOver, naming the node its own AI names for it (restart_ask), so its
        // module runs it from its start in our game (its own sub-state, physics and animation events) and its AI
        // gets the move's end. Taken instead of `rechoose` (setting the brain verb again leaves the move's
        // module where the copy's body was: Phantom Ganon in RushAttack's dive with his rigid body forced no-hit).
        std::optional<BossAction> restart_action;
        OwnAsk restart_ask;
        // Row 178's fall guard: its first seen height, and its falls rechosen while below.
        std::optional<float> start_y;
        std::optional<coop::protocol::Vec3> start;  // (its first seen place, for the put back)
        int fall_rechoices = 0;
        bool fallen = false;
        // Copy (row 149): the owner's action last put into it, and whether its own AI ran an action of its own
        // since (our event, our hit, its start). A resync whose action is the one put in, with none of its own
        // since, asks nothing: the copy runs that move already (asking it again started King Dodongo's roll
        // over after every sword hit, on every hitter's screen).
        std::optional<BossAction> put_action;
        bool own_action_ran = false;
    };
    void Hold(Entry& e, const Boss& b);   // the owner's stall check (Publish)
    void Fall(Entry& e, const Boss& b);   // row 178's fall guard (Publish; a boss, owner or copy: its start)
    bool OwnTime(const Entry& e) const;   // the copy's own changes run now
    void Keep(Entry& e, std::variant<BossBrain, BossAction> what);
    void MakeLate(Entry& e);              // its skipped ones of the last kLookBackSteps, to make
    std::optional<CoopPuppet::Call> Ask(CoopPuppet::Guest& guest, Entry& e, std::uint64_t bsa,
                                        const std::variant<BossBrain, BossAction>& what, std::uint64_t main_base,
                                        bool* made);
    void Record(Entry& e, std::variant<BossBrain, BossAction> what);
    void Resync(Entry& e, const BossBsa& state);
    Entry* Find(std::uint64_t bsa);
    Entry* FindActor(std::uint64_t actor);
    const Entry* FindActor(std::uint64_t actor) const;
    // Row 177 review: the kept own asks (their AI nodes) go: a brain change (its action sequence's nodes may
    // go with it) or a role change; the copy's next own asks fill them again.
    static void ForgetOwnAsks(Entry& e);
    void Note(std::string line);
    // Row 177's log: one line when the stream's kind changes (the last kind's run length with it), capped.
    void Stream(Entry& e, std::string kind);
    std::uint64_t stream_notes_ = 0;

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> by_bsa_;
    bool brain_on_ = false;
    bool actions_on_ = false;
    bool turns_ = false;
    bool event_ = false;           // our game runs an event (SetEvent)
    std::vector<std::string> dying_;  // the bodies whose death has begun (SetDying)
    bool Dying(std::string_view name) const;
    void Release(std::uint64_t component);  // its held_life_ slot: let die
    std::uint64_t steps_ = 0;      // Step calls (game frames)
    struct ShotEnd {
        coop::protocol::Vec3 at{};
        std::uint64_t step = 0;
    };
    std::deque<ShotEnd> shot_ends_;  // NoteOwnShotEnd's, newest last
    // The copy whose call Step made last: the next Step looks from the one after it, so one copy with a change
    // every frame never keeps the others waiting (dozens of plain enemies share the one call a frame).
    std::uint64_t served_ = 0;
    std::vector<std::uint64_t> wants_;  // (TakeOwnWants)
    std::vector<std::pair<std::uint64_t, coop::protocol::Vec3>> returns_;  // (TakeReturns)
    // One entry's turn in Step. True: Step ends with `call` (a call, or none while the block is asked).
    bool StepOne(CoopPuppet::Guest& guest, std::uint64_t bsa, Entry& e, std::uint64_t main_base,
                 std::optional<CoopPuppet::Call>& call);
    Counts counts_{};
    std::vector<std::string> notes_;
    // The Life components HeldLifeStep reads (Publish writes, any thread
    // reads): a copy's held one, or its owner's died (`kReleased` bit: let die,
    // counted once). 0: free.
    static constexpr std::uint64_t kReleased = 1;
    std::array<std::atomic<std::uint64_t>, kMaxHeld> held_life_{};
    std::atomic<std::uint64_t> life_held_{0};
    std::atomic<std::uint64_t> life_killed_{0};
    std::atomic<std::uint64_t> life_finished_{0};
    // The Life components of copies whose death has begun (SetDying; kept when the owner's dead remove takes
    // the boss off the walk's owned list, until the dying list empties). Their Life step ends them: the owner's
    // death is the boss's, whatever their own game's damage rules let through (player report #105).
    std::array<std::atomic<std::uint64_t>, kMaxHeld> dying_life_{};
    void AddDyingLife(std::uint64_t component);
    // The caller's state (frame thread only).
    std::uint64_t block_ = 0;      // our request in guest memory (kRequestSize)
    std::uint64_t main_base_ = 0;  // the game the block belongs to
    bool allocating_ = false;
    enum class Pending : std::uint8_t { none, malloc, brain, action, late, rechoose, restart } pending_ = Pending::none;
    std::uint64_t pending_bsa_ = 0;  // the copy whose change pending_ is (brain, action, late)
    void Made(bool made);            // pending_bsa_'s asked change: out of its queue (made) or asked again
};

Table& TheTable();

} // namespace CoopBossNative
