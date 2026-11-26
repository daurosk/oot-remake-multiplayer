// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty: enemies with more life, harder hits and more of them, by the
// number of players or as the host sets it (docs/play-online.md "Difficulty";
// docs/protocol.md "Difficulty"). The server decides the three values for
// everyone (coop/protocol/difficulty.hpp has the scaler and the presets);
// this is the game's side: the rules that turn the values into life, damage
// and extra enemies (no emulator code: built straight into the tests), and
// the session's value with the host's settings for the pause menu
// (coop_difficulty_game.cpp).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/difficulty.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopDifficulty {

using Values = coop::protocol::DifficultyValues;
using Preset = coop::protocol::DifficultyPreset;
using Choice = coop::protocol::DifficultyChoice;

// ---------------------------------------------------------------------------
// The host's settings (pause menu: Multiplayer > Host settings > Difficulty)
// ---------------------------------------------------------------------------

// What the menu shows. `choice` is the session's (the server's), `values`
// what is in force for everyone now.
struct State {
    bool connected = false;  // in a session, and the server has said its difficulty
    bool can_edit = false;   // we host: Set() changes it for everyone
    Choice choice;
    Values values;
    std::uint8_t set_by = 0;  // 0: nobody chose yet (automatic), else the host who did
};

// Any thread.
State Get();

// Any thread: the host's new choice, for everyone. Saved for this player's
// next session as host, and sent on the next frame; the menu shows it once
// the server has answered (Get). False when we do not host a session now.
bool Set(const Choice& choice);

// Any thread: the values in force now (the game as it is while not in a
// session or before the server said them).
Values Now();
// The values in one word, for Now() on any thread: all five percents (boss
// life and boss damage too), 12 bits each (kMaxBossHpPct 1600 is the most;
// a bigger one is kept at 4095).
std::uint64_t PackValues(const Values& v) noexcept;
Values UnpackValues(std::uint64_t word) noexcept;

// The menu's rows. A preset or a moved slider turns the automatic scaler
// off: what the host sets stays, whoever joins or leaves.
enum class Slider : std::uint8_t { hp, damage, count };
std::string_view PresetName(Preset preset);  // "Easy", "Normal", "Hard", "Custom"
Choice WithAuto(const Choice& now, bool on);
Choice WithPreset(const Choice& now, Preset preset);
// `steps` slider steps up (down when negative) from `in_force`, the values
// shown now, kept within the slider's ends; the preset becomes custom.
Choice WithSlider(const Choice& now, const Values& in_force, Slider slider, int steps);

// "Automatic (3 players): life 180%, damage 140%, enemies 150%",
// "Hard: life 200%, ...". `players` is only shown with the automatic scaler.
std::string Describe(const Choice& choice, const Values& values, std::size_t players);

// The host's choice as kept between sessions (coop-difficulty.ini beside
// coop.ini, written by Set): `key = value` lines. Nothing when the text is not one.
std::string FormatChoice(const Choice& choice);
std::optional<Choice> ParseChoice(std::string_view text);

// /difficulty in the chat (docs/play-online.md "Difficulty"): alone, the values
// in force; the host's /difficulty auto|easy|normal|hard, or /difficulty custom
// <life> <damage> <enemies> (percent, steps of 25 within the sliders' ends).
// Nothing when the text is not /difficulty ("/difficultyx" is chat).
struct Command {
    enum class Kind : std::uint8_t { show, change, bad };
    Kind kind = Kind::show;
    bool auto_scale = false;            // change: the automatic scaler on
    Preset preset = Preset::normal;     // change, not automatic
    Values custom;                      // change, Preset::custom
};
std::optional<Command> ParseCommand(std::string_view text);
// The host's choice after the command `change` on `now` (a preset keeps the
// custom values; the automatic scaler keeps the preset for when it goes off).
Choice Applied(const Choice& now, const Command& change);

// ---------------------------------------------------------------------------
// The rules the game's side applies
// ---------------------------------------------------------------------------

// Enemy life and damage go through the game's own Life step (patch 0007:
// each Life component's damage of a frame, summed, reaches the module before
// the game takes it off; coop_hookshot.cpp). Nothing is written into an
// enemy: the game's own hit takes what the step lets through.
//
// Enemy life: an enemy at `hp_pct` takes 100 / hp_pct of each frame's damage
// `sum`, so it lasts hp_pct / 100 as many hits (300: three times as many).
// The parts of a point are carried in `carry` (hundredths, below hp_pct) to
// the enemy's next hit, so small hits add up to what one big one takes. A
// heal (0 or less) is passed on as it is.
std::int32_t EnemyTakes(std::int32_t sum, std::uint16_t hp_pct, std::int32_t& carry);
// The carry an enemy's first scaled hit starts from, per point of that hit:
// half the hit (EnemyTakes keeps it below one life point), so with hits of
// one size its hits to kill are life * hp_pct / 100 / hit rounded to the
// nearest (a half rounds down) instead of always up, and never fewer than at
// 100%. From 0 a one-life enemy at 125% or 140% took two sword hits, as at
// 200% (player report 2026-11-24: 'at 125% mobs took way more hits'): an
// enemy is hit only a few times, so its first hit's round-up was most of
// what it gained. Our Link's carry lasts across all his hits, so LinkTakes
// starts from 0: what he takes adds up to damage_pct.
inline constexpr std::int32_t kEnemyFirstCarry = 50;
// Enemy damage: our Link takes damage_pct / 100 of what an enemy dealt him
// (quarter hearts), the parts of a quarter carried in `carry` (hundredths,
// below 100) to his next hit from an enemy.
std::int32_t LinkTakes(std::int32_t sum, std::uint16_t damage_pct, std::int32_t& carry);
// Enemy count, folded: what an enemy that gets no copies (Treatment::fold)
// has instead, `pct` (its life or damage %) times count_pct / 100 (enemies at
// 200%: twice the life and twice the damage), at most 65535. Never less than
// `pct`: a count of 100% or less leaves it as it is.
std::uint16_t Folded(std::uint16_t pct, std::uint16_t count_pct);
// The count an enemy folds by. `fold_key` 0: a Treatment::fold enemy (it never
// gets copies by design): count_pct. Otherwise the placement hash of an enemy
// whose copies found no ground (no_spot): the copies it would have had,
// 100 * (1 + ExtraCount(fold_key, count_pct)), so each such enemy is exactly
// as strong as it and its extras together (player report 111 review: x1.5
// for every no-spot enemy at 150% left those that would get 1 extra at x1.5
// instead of x2, and those with none at x1.5 instead of x1).
std::uint16_t FoldCount(std::uint64_t fold_key, std::uint16_t count_pct);

// What the object walk found, for the Life step on the game's threads.
enum class Kind : std::uint8_t {
    none = 0,
    enemy_life = 1,     // an enemy's (or a boss's) Life component
    enemy = 2,          // an enemy actor, or an enemy's shot: an attacker whose hits on Link are scaled
    our_link_life = 3,  // our own Link's Life component
    // An enemy that gets no extra copies but their weight instead (Treatment::fold,
    // or no ground for them: no_spot): its life and its hits on Link are scaled
    // by Folded(pct, FoldCount(its slot's fold_key, count_pct)).
    folded_enemy_life = 4,
    folded_enemy = 5,
    // A boss or a mini-boss (ScalesAsBoss): its life at boss_hp_pct, its hits
    // on Link at boss_damage_pct.
    boss_life = 6,
    boss = 7,
};
// An attacker whose hit on Link is the enemies' damage: an enemy or a boss
// (Enemy_*), or an enemy's shot (`enemy_shot`: coop_enemy_shots.h).
bool IsEnemy(std::string_view actor_name, bool enemy_shot);
// The incoming hit's attacker actor: a u64 at this offset in a Life component
// (the hit record the Life step reads; coop_objects.cpp kHitTemplate).
inline constexpr std::uint64_t kHitAttacker = 0x240;

// Addresses and their Kind, with each Life component's carry. One writer
// (the object walk, present thread: Mark and Clear), any number of readers
// (the game's threads: Step and Pass); no lock. An address that holds another
// actor now is marked again (Kind::none when it is no enemy).
//
// Row 270: a slot is its address's only while the walk keeps marking it. The
// walk ends with Sweep, and an address no walk marked for kKeepWalks walks
// (its actor deleted: a night's Stalchildren, a boss's shots) gives its slot
// back for the next new address. Before, a slot was an address's until Clear
// (a new scene), so a long stay in one scene with enemies coming and going
// filled the table and every later enemy went unscaled. A table full of live
// addresses still marks nothing more (those hits go through unscaled,
// counted in Unmarked).
class Kinds {
public:
    static constexpr std::size_t kSlots = 1024;
    static constexpr std::size_t kProbes = 32;

    // A replayed hit (another player's, written into an enemy's Life
    // component as a whole hit: coop_objects.cpp Replay) is scaled where it
    // was dealt; this many of the next damage `component` takes within
    // kPassMs go through unscaled.
    static constexpr std::uint64_t kPassMs = 1000;

    // False when the table has no room for it. `fold_key`: for the folded
    // kinds, FoldCount's key (0: the whole count).
    bool Mark(std::uint64_t address, Kind kind, std::uint64_t fold_key = 0) noexcept;
    // `address` holds no enemy (actor or Life component) now: marked none if
    // it was marked an enemy one (a reused address), else nothing.
    void Forget(std::uint64_t address) noexcept;
    void Pass(std::uint64_t component, std::int32_t amount, std::uint64_t now_ms) noexcept;
    Kind Of(std::uint64_t address) const noexcept;
    void Clear() noexcept;
    // The end of one whole walk (the writer's thread): frees the slots of the
    // addresses no walk marked for kKeepWalks walks, and closes the walk's
    // count of refused marks (Unmarked).
    void Sweep() noexcept;
    // An address no walk marked for this many walks is gone: a few seconds
    // of walks, so a walk that misses one read frees nothing.
    static constexpr std::uint64_t kKeepWalks = 120;

    // One Life step of `component` with `sum` (the frame's damage): what the
    // game takes at `values`. `attacker()` reads the hit's attacker actor;
    // called only for our Link's component. Counts what it scaled.
    template <class AttackerOf>
    std::int32_t Step(std::uint64_t component, std::int32_t sum, const Values& values, std::uint64_t now_ms,
                      AttackerOf&& attacker) noexcept {
        if (sum <= 0) {
            return sum;
        }
        Slot* s = Find(component);
        if (s == nullptr) {
            return sum;
        }
        const Kind kind = static_cast<Kind>(s->kind.load(std::memory_order_acquire));
        if (kind == Kind::enemy_life || kind == Kind::folded_enemy_life || kind == Kind::boss_life) {
            const std::uint16_t pct =
                kind == Kind::enemy_life  ? values.hp_pct
                : kind == Kind::boss_life ? values.boss_hp_pct
                                          : Folded(values.hp_pct,
                                                   FoldCount(s->fold_key.load(std::memory_order_acquire), values.count_pct));
            const std::int32_t passed = Passed(*s, sum, now_ms);
            const std::int32_t took =
                passed == sum ? sum : passed + Scaled(*s, sum - passed, pct, &EnemyTakes, kEnemyFirstCarry, enemy_hits_);
            if (kind == Kind::boss_life) {
                // (traced apart: does a boss's damage reach this step, and what of it is taken)
                boss_steps_.fetch_add(1, std::memory_order_relaxed);
                boss_in_.fetch_add(static_cast<std::uint64_t>(sum), std::memory_order_relaxed);
                boss_took_.fetch_add(static_cast<std::uint64_t>(std::max(took, 0)), std::memory_order_relaxed);
            }
            return took;
        }
        if (kind == Kind::our_link_life) {
            const Slot* a = Find(attacker());
            const Kind by = a == nullptr ? Kind::none : static_cast<Kind>(a->kind.load(std::memory_order_acquire));
            if (by == Kind::enemy || by == Kind::folded_enemy || by == Kind::boss) {
                const std::uint16_t pct =
                    by == Kind::enemy  ? values.damage_pct
                    : by == Kind::boss ? values.boss_damage_pct
                                       : Folded(values.damage_pct,
                                                FoldCount(a->fold_key.load(std::memory_order_acquire), values.count_pct));
                return Scaled(*s, sum, pct, &LinkTakes, 0, link_hits_);  // (100: unchanged)
            }
        }
        return sum;
    }
    // How many hits Step changed: on enemies, on our Link (for the log).
    std::uint64_t EnemyHits() const noexcept { return enemy_hits_.load(std::memory_order_relaxed); }
    std::uint64_t LinkHits() const noexcept { return link_hits_.load(std::memory_order_relaxed); }
    // A boss's Life steps with damage (Kind::boss_life), the damage that came in and what the game took.
    std::uint64_t BossSteps() const noexcept { return boss_steps_.load(std::memory_order_relaxed); }
    std::uint64_t BossDamageIn() const noexcept { return boss_in_.load(std::memory_order_relaxed); }
    std::uint64_t BossDamageTaken() const noexcept { return boss_took_.load(std::memory_order_relaxed); }
    // Marks the full table refused in the last whole walk (Sweep): that many
    // actors or Life components are unscaled now.
    std::uint64_t Unmarked() const noexcept { return unmarked_last_.load(std::memory_order_relaxed); }
    // Slots that hold an address now, as of the last Sweep.
    std::uint64_t Held() const noexcept { return held_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<std::uint64_t> key{0};
        std::atomic<std::uint8_t> kind{0};
        std::atomic<std::uint64_t> fold_key{0};  // FoldCount's key (folded kinds)
        // The carry and the percent it was made at (pct << 16 | carry): a
        // carry from other values is dropped.
        std::atomic<std::uint32_t> carry{0};
        // A replayed hit's amount still to pass unscaled, until pass_until (ms).
        std::atomic<std::int32_t> pass{0};
        std::atomic<std::uint64_t> pass_until{0};
    };
    using Rule = std::int32_t (*)(std::int32_t, std::uint16_t, std::int32_t&);
    // `first`: the carry a hit starts from, per point of it, when the slot has none at `pct`.
    static std::int32_t Scaled(Slot& s, std::int32_t sum, std::uint16_t pct, Rule rule, std::int32_t first,
                               std::atomic<std::uint64_t>& hits) noexcept;
    static std::int32_t Passed(Slot& s, std::int32_t sum, std::uint64_t now_ms) noexcept;
    static std::size_t Hash(std::uint64_t address) noexcept;
    Slot* Find(std::uint64_t address) noexcept;
    const Slot* Find(std::uint64_t address) const noexcept;

    // A freed slot's key: no address, and not the end of a probe run (a
    // reader goes on past it; the writer puts the next new address there).
    static constexpr std::uint64_t kFreed = 1;

    std::array<Slot, kSlots> slots_{};
    // The writer's own: the walk that last marked each slot, and the walk now.
    std::array<std::uint64_t, kSlots> marked_{};
    std::uint64_t walk_ = 1;
    std::atomic<std::uint64_t> unmarked_last_{0};
    std::atomic<std::uint64_t> held_{0};
    std::atomic<std::uint64_t> enemy_hits_{0};
    std::atomic<std::uint64_t> link_hits_{0};
    std::atomic<std::uint64_t> boss_steps_{0};
    std::atomic<std::uint64_t> boss_in_{0};
    std::atomic<std::uint64_t> boss_took_{0};
    std::atomic<std::uint64_t> unmarked_{0};
};
Kinds& TheKinds();
// A steady clock in ms, for Kinds::Step and Pass.
std::uint64_t SteadyMs() noexcept;

// Enemy count. How many more of the enemy placed with `key` there are at
// `count_pct`: every enemy gets count_pct / 100 - 1, and count_pct % 100 in
// a hundred get one more. Which ones is decided by the key alone, so every
// game picks the same enemies every time.
unsigned ExtraCount(std::uint64_t key, std::uint16_t count_pct);

// The extra enemies (count above 100%). Every game makes the same ones: for
// each placed enemy it has loaded, ExtraCount of the same actor, with the
// game's own create request (CoopSpawn's director), beside it. Each extra
// takes a placement id of its own, made from its original's (ExtraHash), so
// the enemy sync shares it as it shares a placed enemy: one owner, samples,
// hits, its death and its drop as that enemy's own. The ones of a scene
// visit stay until the scene changes; a lower count later takes none away.

// What "more enemies" does with one placed enemy (docs/play-online.md,
// "Difficulty"; the rules and their reasons are one table in
// coop_difficulty.cpp, with the game's own placement data in
// coop_difficulty_placements.inc):
// - copies: it gets ExtraCount copies beside it (most enemies; also those a
//   room's Obj_EnemyCounter counts: it counts only its own linked placements,
//   so a copy never holds a room shut).
// - fold: no copies, its weight instead: life and damage times the count
//   (Folded). A patrol on a rail, an enemy a shutter, an event or a manager
//   waits for, one linked with another enemy, one with a set drop, and the
//   kinds whose number or spot is the puzzle (Armos, Beamos, Like Like,
//   Wallmaster, Floormaster, Dead Hand, Big Poes, the Poe sisters, Shabom,
//   ReDead, Torch Slug, Anubis, Iron Knuckles, Lens of Truth enemies, ...).
// - as_placed: nothing more: not an enemy, a boss or mini-boss (their life
//   and damage scale already), a part of a bigger enemy, a picture-book or
//   cutscene stand-in, a scrub, a Gold Skulltula, a wall enemy.
enum class Treatment : std::uint8_t { copies, fold, as_placed };
// A boss or a mini-boss of that table (Enemy_DungeonBoss_*, Enemy_MiddleBoss_*,
// a battle manager, and their parts under those names): scaled by the boss
// values (boss_hp_pct, boss_damage_pct), never by the enemies' ones.
bool ScalesAsBoss(std::string_view name);
// The Kinds for an enemy's actor and its Life component: boss, folded or plain.
// `no_spot`: a placed enemy whose copies found no ground to stand on (Extras::NoSpot): folded as well.
Kind AttackerKind(std::string_view name, std::uint64_t hash, bool no_spot = false);
Kind LifeKind(std::string_view name, std::uint64_t hash, bool no_spot = false);
Treatment TreatmentOf(std::string_view name, std::uint64_t hash);
// Why TreatmentOf said what it said, for the log and the tests ("rail",
// "puzzle: Armos statues", "boss", ...); empty for copies.
std::string_view TreatmentWhy(std::string_view name, std::uint64_t hash);

// Which placed enemy gets extras: a placement hash, a life, TreatmentOf
// copies, and not an enemy the sync treats in its own way (`special`:
// rolled, hatching, announced, a puzzle, a shot).
bool GetsExtras(std::string_view name, std::uint64_t hash, bool has_life, bool special);
// The placement id of extra `index` (0 on) of the enemy placed with `hash`:
// never 0 or ~0 (a made actor's), never `hash` itself.
std::uint64_t ExtraHash(std::uint64_t hash, unsigned index);
// Where an extra (or a horde Stalchild) is made (owner, 2026-11-14: never on top of each other).
// The module has no ground or navmesh query of the game (none is mapped yet), so a spot is only a
// point the game itself puts a body on the floor at, never a point worked out between two: for an
// extra, a floor marker the area's placement data has (Area: where the game puts Link down, after a
// fall, at the start, a tuft of grass); for the horde, where the game's own rolls rose this visit.
// FindSpot takes the nearest such point within kSpotReachM (x, z) and kSpotStepM up or down, at
// least ApartFor(name) from `from`, from every point of `apart` (the other enemies, the spots taken
// before); ties by the candidates' order. A door's landing (a marker with a facing) only when `from`
// is in front of it: a door has one on each side, each facing into its own room, so never the one
// in the next room. None: nullopt.
inline constexpr float kSpotReachM = 8.0f;  // tools/puppet_pack/extra_spots.py REACH_M
inline constexpr float kSpotStepM = 1.0f;   // and STEP_M
// The least distance between an extra and any other enemy: 2.5 m, 3.5 m for the big ones
// (Moblin, Lizalfos, Dinolfos, Stalfos, Gibdo, Wolfos, Lynel, ...), 2 m for the small ones.
float ApartFor(std::string_view name);
using Point = std::array<float, 3>;
struct Marker {
    Point at{};
    float fx = 0.0f, fz = 0.0f;  // where it faces (x, z; a door's landing), or 0, 0: any side
    constexpr Marker() = default;
    constexpr Marker(const Point& p, float x = 0.0f, float z = 0.0f) : at(p), fx(x), fz(z) {}
    constexpr Marker(float x, float y, float z, float face_x = 0.0f, float face_z = 0.0f)
        : at{x, y, z}, fx(face_x), fz(face_z) {}
};
std::optional<Point> FindSpot(const Point& from, std::string_view name, std::span<const Marker> candidates,
                              const std::vector<Point>& apart);

// One area of the game's placement data (coop_difficulty_spots.inc, made by
// tools/puppet_pack/extra_spots.py from the Banc files): its placed enemies,
// in placement id order, and the floor markers near them.
struct AreaEnemy {
    std::uint64_t hash = 0;
    std::string_view name;
    Point at{};  // where it is placed
};
struct Area {
    std::string_view name;  // the Banc scene
    std::span<const AreaEnemy> enemies;
    std::span<const Marker> ground;
};
// The area whose placements hold `hash` (the game's table); nullopt for one it does not have.
std::optional<Area> AreaOf(std::uint64_t hash);
// The extras of a whole area at `count_pct`, from its placement data only, so
// every game decides the same whatever it has loaded: each enemy that gets
// copies (TreatmentOf copies, not `special` by name), in placement id order,
// takes ExtraCount spots (FindSpot from where it is placed, apart from every
// enemy placed there and every spot taken before), all of them or none: with
// none, its weight instead (no_spot). At most `most` extras in the area, the
// first enemies in that order first; the ones past it get their weight too.
struct AreaPlan {
    std::map<std::uint64_t, std::vector<Point>> spots;
    std::set<std::uint64_t> no_spot;
};
using NameTest = std::function<bool(std::string_view)>;
AreaPlan PlanArea(const Area& area, std::uint16_t count_pct, const NameTest& special, std::size_t most);

// One scene visit's extras (present thread).
class Extras {
public:
    static constexpr std::size_t kMost = 32;          // in one area (PlanArea's `most`)
    static constexpr std::uint64_t kBindWaitMs = 4000;  // a made one not yet bound to its actor

    using Lookup = std::function<std::optional<Area>(std::uint64_t)>;
    // `areas`: the placement data (AreaOf; tests give their own); `special`:
    // the names the sync treats in its own way (rolled, hatching, ...).
    explicit Extras(Lookup areas = AreaOf, NameTest special = nullptr)
        : areas_(std::move(areas)), special_(std::move(special)) {}

    struct Original {
        std::string name;
        std::uint64_t hash = 0;
    };
    struct Extra {
        std::uint64_t key = 0;   // the director's key (1 on, unique in this run)
        std::uint64_t hash = 0;  // its placement id (ExtraHash)
        std::string name;
        std::array<float, 3> position{};
        std::uint64_t asked_ms = 0;
        std::uint64_t address = 0;  // its actor, once the director made it
        bool gone = false;          // that actor is no more (Keep): never bound again
    };

    // A walk's placed enemies that get extras (GetsExtras), in `scene`, at
    // `count_pct`. Nothing new while a boss is here. A new scene forgets the
    // old one's extras first. Taken in placement id order, so every game picks
    // the same ones whatever order its walk saw them in. Answers how many were
    // added.
    // Where they stand and which get none comes from the original's area's
    // placement data (PlanArea), decided once per area and visit, at the
    // count then: a higher count later in the visit adds none (its next visit
    // has them). An original the data does not have (a mod's added placement) gets none and no fold.
    std::size_t Plan(std::string_view scene, const std::vector<Original>& walk, std::uint16_t count_pct,
                     bool boss_here, std::uint64_t now_ms);
    // The placed enemy with `hash` gets no copies for want of ground: its life and hits are folded instead.
    bool NoSpot(std::uint64_t hash) const { return no_spot_.contains(hash); }
    std::size_t NoSpots() const { return no_spot_.size(); }
    // The director's actors (key to actor) after it made some.
    void Bind(const std::map<std::uint64_t, std::uint64_t>& live);
    // A whole walk's actors: an extra whose actor is not among them is gone,
    // and its address (which the heap may give to another actor) is no
    // extra's any more.
    void Keep(const std::vector<std::uint64_t>& actors);
    // The placement id of the extra whose actor is at `address`, when the
    // actor there is still named `name` (else another actor has that
    // address now).
    std::optional<std::uint64_t> HashOf(std::uint64_t address, std::string_view name) const;
    // Whether a made actor named `name` with no actor bound to it yet may be
    // one of ours: it is not shared until bound (or kBindWaitMs after the ask).
    bool Holds(std::string_view name, std::uint64_t now_ms) const;
    // Every extra of this visit, for the director (made once each; one that
    // died is not made again).
    const std::vector<Extra>& All() const { return extras_; }
    void Reset();

private:
    Lookup areas_;
    NameTest special_;
    std::string scene_;
    std::vector<Extra> extras_;
    std::map<std::string_view, AreaPlan> plans_;  // this visit's areas (names from the table: static)
    std::map<std::uint64_t, unsigned> planned_;  // original's hash -> extras asked for it
    std::set<std::uint64_t> no_spot_;            // originals folded for want of ground
    struct Bound {
        std::uint64_t hash = 0;
        std::string name;
    };
    std::map<std::uint64_t, Bound> by_address_;  // actor -> its extra
    std::uint64_t next_key_ = 1;
};

// The night horde (Hyrule Field's Stalchild; docs/play-online.md
// "Difficulty"). The game's own night spawner rolls Stalchild around a Link;
// where players stand together one game keeps the rolls and shares them
// (coop_objects_sync.h RollerFor). That game makes PerRoll more beside each
// roll of its own, with the game's own create request (the director), the
// more players stand within the roll range of it; they are hashless
// Stalchild of its walk like the game's own, so the roll sync shares them
// the same way (one game runs each, its hits and death everyone's).
// Each one follows the roll it was made beside: when that roll leaves the
// walk alive (the spawner took it away: dawn, the area turned it off, it
// sank), ours are deleted with the game's own delete request (Removes). A
// roll that died leaves ours to fight on: they join another living roll of
// ours, and with none for kOrphanMs (day: the spawner rolls no more) they go
// as well.
class Horde {
public:
    static constexpr unsigned kPerRollMost = 3;        // beside one roll
    static constexpr std::size_t kMostAlive = 18;      // Stalchild in our walk at once, the game's and ours
    static constexpr std::size_t kMost = 48;           // made in one scene visit
    // A roll seen this long is the game's own: the director binds ours sooner,
    // so one of ours is never taken for a roll (no horde of a horde).
    static constexpr std::uint64_t kSettleMs = 1000;
    // One the director has not made this long after the ask is given up (the
    // game refused it): it no longer counts as alive.
    static constexpr std::uint64_t kBindWaitMs = 4000;
    // Ours whose roll died, with no living roll of ours to join this long, go.
    static constexpr std::uint64_t kOrphanMs = 30'000;

    // At `count_pct` (enemies %; 100 or less: none), with `players_near`
    // Links (ours among them) within the roll range: one more per player
    // beyond the first, at most kPerRollMost.
    static unsigned PerRoll(std::size_t players_near, std::uint16_t count_pct);

    struct Roll {
        std::uint64_t address = 0;  // its actor
        std::string name;           // Enemy_Stalchild or Enemy_Stalchild_Big
        std::array<float, 3> position{};
        std::size_t players_near = 1;
        bool dead = false;  // life 0 (killed): if it leaves now, ours stay
    };
    struct Made {
        std::uint64_t key = 0;  // the director's key (1 on)
        std::string name;
        std::array<float, 3> position{};
        std::uint64_t roll = 0;      // the roll's actor it follows (0: its roll died)
        std::uint64_t asked_ms = 0;  // when it was planned
        std::uint64_t address = 0;   // its actor, once the director made it
        bool gone = false;           // no more (left the walk, given up): never made again
        bool retired = false;        // its roll was taken away alive: to delete
    };

    // Our own rolls on this walk (the ones this game shares) in `scene`;
    // `alive`: the rolled enemies in our walk now. A new scene forgets the
    // old one's first. A roll gets more as more players come near it, up to
    // PerRoll. Answers how many were added.
    // Ours rise only where the game's own rolls rose this visit (FindSpot),
    // apart from `apart` (every enemy of the walk, our Link) and from each
    // other; a roll with no such spot for one more gets none now (tried again
    // on a later walk).
    std::size_t Plan(std::string_view scene, const std::vector<Roll>& rolls, std::size_t alive,
                     const std::vector<Point>& apart, std::uint16_t count_pct, std::uint64_t now_ms);
    // The director's actors (key to actor) after it made some.
    void Bind(const std::map<std::uint64_t, std::uint64_t>& live);
    // A whole walk's actors: a roll or one of ours that is not among them is
    // gone; ours beside a roll that left alive are retired.
    void Keep(std::vector<std::uint64_t> actors);
    // Ours to delete now (retired, made, still here), for the game's delete request.
    std::vector<std::uint64_t> Removes() const;
    // Whether the actor at `address` is one we made.
    bool Ours(std::uint64_t address) const { return by_address_.contains(address); }
    const std::vector<Made>& All() const { return made_; }
    void Reset();

private:
    struct Seen {
        std::uint64_t first_ms = 0;
        bool dead = false;  // as last seen
    };
    std::string scene_;
    std::vector<Made> made_;
    std::map<std::uint64_t, Seen> rolls_;       // a roll's actor -> when first seen, how last seen
    std::vector<Marker> rises_;                 // where the game's rolls were first seen: its own floor
    std::map<std::uint64_t, unsigned> planned_;  // a roll's actor -> made beside it
    std::map<std::uint64_t, std::uint64_t> by_address_;  // our actor -> its key
    std::uint64_t orphan_since_ = 0;  // ms: orphans with no living roll to join since (0: none)
    std::uint64_t next_key_ = 1;
};

// ---------------------------------------------------------------------------
// The session (coop_difficulty_game.cpp)
// ---------------------------------------------------------------------------

// Whether a game that hosts sends its kept choice: once per connection, when
// it was a choice of its own (not the automatic scaler) and nobody has
// chosen for this session yet. A host who takes over a session keeps what
// the host before set.
bool OffersKept(bool hosts, std::uint8_t set_by, const std::optional<Choice>& kept, bool offered);

// Network thread, from the session's on_event (the network lock is held):
// DIFFICULTY; a new connection starts with the game as it is.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Present thread, once per frame: sends the host's choice (Set, or the kept
// one when we start to host a session nobody has chosen for). `session`
// null when not connected.
void Frame(coop::host::Session* session);

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();
// The chat's /difficulty (ParseCommand): true when the text was one, answered
// in the chat (TakeLines); the host's change goes out as Set does.
bool OnChat(std::string_view text, coop::host::Session& session);

} // namespace CoopDifficulty
