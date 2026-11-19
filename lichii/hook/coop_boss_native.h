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
// its own start (its appear, issue 62) for kStartSteps. Afterwards the copy starts again
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
inline constexpr std::uint64_t kBsaBrainVerb = 0x28;      // s32, the current brain verb
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
// own AI node that last asked for an action: callers always name one).
Request RequestFromAction(const BossAction& a, std::uint64_t node);

enum class Role : std::uint8_t { owner, copy };
// A boss whose new copy runs its own start first (Table::kStartSteps): a
// mid-boss (Enemy_MiddleBoss_*: Lizalfos, Stalfos, ...) appears in its room
// by its own AI with no intro event to run it (a dungeon boss's intro is an
// event: SetEvent covers it).
bool RunsOwnStart(std::string_view name);

// Whether a plain enemy's death runs here now: its life is 0 in our game, or its owner's died.
bool PlainDeath(std::optional<std::int32_t> life, bool owner_dead);

// Row 138 (2)(7), #238: whether our game's own hit on a copy took life (its life read before the hit, in the walk
// before, and after it). Such a hit is the owner's to judge: its DAMAGE goes to the owner, the server keeps the boss
// with its owner (only a hit that takes no life, a stun, hands it over), and the owner's game's reaction comes back
// as its verbs. So the copy's own brain stays skipped (Table::NoteOwnDamage). A life not read either time: unknown,
// false (the copy runs its own reaction as before).
bool OwnHitTookLife(std::optional<std::int32_t> before, std::optional<std::int32_t> after);

// One boss the Objects step saw in its walk.
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
    std::string animation;
    std::optional<std::int32_t> life;
};

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
    std::uint64_t copy_hits_to_owner = 0; // our own hit on a copy that took life: the owner reacts (NoteOwnDamage)
    std::uint64_t plain_wants = 0;    // a plain enemy copy's own brain asked another brain verb than its owner's
    std::uint64_t plain_wakes = 0;    // a plain enemy copy's own wake let run (NoteOwnWake)
    std::uint64_t plain_deaths = 0;   // a plain enemy copy whose death began here: its own runs
    std::uint64_t owner_stalls = 0;   // row 149: our boss held one move kStallSteps with no life change: re-chosen
    std::uint64_t owner_rechosen = 0; // its brain verb set again by our caller (a stall, or a copy just made ours)
    std::uint64_t moves_kept = 0;     // a resync's action the copy already runs (put in, none of its own since): not asked
};

class Table {
public:
    // Switches (LICHII_COOP_BOSS_NATIVE, LICHII_COOP_BOSS_ACTIONS): off -> no
    // skip, no apply, nothing kept (the old gohma-sync path alone).
    void SetOn(bool brain, bool actions);
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
    // The plain enemy copies whose own brain asked for another brain verb than their owner's since the last
    // call (skipped at the wrap): their own AI wants to act here. At most kMaxWants kept.
    std::vector<std::uint64_t> TakeOwnWants();
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
    static constexpr std::size_t kMaxWants = 64;
    static constexpr std::size_t kMaxSkippedKept = 8;
    static constexpr std::size_t kMaxHeld = 8;  // copy bosses held at once (more: not held)

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
        // Copy (row 149): the owner's action last put into it, and whether its own AI ran an action of its own
        // since (our event, our hit, its start). A resync whose action is the one put in, with none of its own
        // since, asks nothing: the copy runs that move already (asking it again started King Dodongo's roll
        // over after every sword hit, on every hitter's screen).
        std::optional<BossAction> put_action;
        bool own_action_ran = false;
    };
    void Hold(Entry& e, const Boss& b);   // the owner's stall check (Publish)
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
    void Note(std::string line);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> by_bsa_;
    bool brain_on_ = false;
    bool actions_on_ = false;
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
    enum class Pending : std::uint8_t { none, malloc, brain, action, late, rechoose } pending_ = Pending::none;
    std::uint64_t pending_bsa_ = 0;  // the copy whose change pending_ is (brain, action, late)
    void Made(bool made);            // pending_bsa_'s asked change: out of its queue (made) or asked again
};

Table& TheTable();

} // namespace CoopBossNative
