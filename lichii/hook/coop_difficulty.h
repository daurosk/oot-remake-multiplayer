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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
// Enemy damage: our Link takes damage_pct / 100 of what an enemy dealt him
// (quarter hearts), the parts of a quarter carried in `carry` (hundredths,
// below 100) to his next hit from an enemy.
std::int32_t LinkTakes(std::int32_t sum, std::uint16_t damage_pct, std::int32_t& carry);

// What the object walk found, for the Life step on the game's threads.
enum class Kind : std::uint8_t {
    none = 0,
    enemy_life = 1,     // an enemy's (or a boss's) Life component
    enemy = 2,          // an enemy actor, or an enemy's shot: an attacker whose hits on Link are scaled
    our_link_life = 3,  // our own Link's Life component
};
// An attacker whose hit on Link is the enemies' damage: an enemy or a boss
// (Enemy_*), or an enemy's shot (`enemy_shot`: coop_enemy_shots.h).
bool IsEnemy(std::string_view actor_name, bool enemy_shot);
// The incoming hit's attacker actor: a u64 at this offset in a Life component
// (the hit record the Life step reads; coop_objects.cpp kHitTemplate).
inline constexpr std::uint64_t kHitAttacker = 0x240;

// Addresses and their Kind, with each Life component's carry. One writer
// (the object walk, present thread: Mark and Clear), any number of readers
// (the game's threads: Step and Pass); no lock. A full table marks nothing
// more (its hits then go through unscaled, counted in Unmarked) until Clear
// (a new scene). An address that holds another actor now is marked again
// (Kind::none when it is no enemy).
class Kinds {
public:
    static constexpr std::size_t kSlots = 1024;
    static constexpr std::size_t kProbes = 32;

    // A replayed hit (another player's, written into an enemy's Life
    // component as a whole hit: coop_objects.cpp Replay) is scaled where it
    // was dealt; this many of the next damage `component` takes within
    // kPassMs go through unscaled.
    static constexpr std::uint64_t kPassMs = 1000;

    // False when the table has no room for it.
    bool Mark(std::uint64_t address, Kind kind) noexcept;
    // `address` holds no enemy (actor or Life component) now: marked none if
    // it was marked an enemy one (a reused address), else nothing.
    void Forget(std::uint64_t address) noexcept;
    void Pass(std::uint64_t component, std::int32_t amount, std::uint64_t now_ms) noexcept;
    Kind Of(std::uint64_t address) const noexcept;
    void Clear() noexcept;

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
        if (kind == Kind::enemy_life) {
            const std::int32_t passed = Passed(*s, sum, now_ms);
            return passed == sum ? sum : passed + Scaled(*s, sum - passed, values.hp_pct, &EnemyTakes, enemy_hits_);
        }
        if (kind == Kind::our_link_life && values.damage_pct != 100 && Of(attacker()) == Kind::enemy) {
            return Scaled(*s, sum, values.damage_pct, &LinkTakes, link_hits_);
        }
        return sum;
    }
    // How many hits Step changed: on enemies, on our Link (for the log).
    std::uint64_t EnemyHits() const noexcept { return enemy_hits_.load(std::memory_order_relaxed); }
    std::uint64_t LinkHits() const noexcept { return link_hits_.load(std::memory_order_relaxed); }
    // Marks the full table refused since the last Clear.
    std::uint64_t Unmarked() const noexcept { return unmarked_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<std::uint64_t> key{0};
        std::atomic<std::uint8_t> kind{0};
        // The carry and the percent it was made at (pct << 16 | carry): a
        // carry from other values is dropped.
        std::atomic<std::uint32_t> carry{0};
        // A replayed hit's amount still to pass unscaled, until pass_until (ms).
        std::atomic<std::int32_t> pass{0};
        std::atomic<std::uint64_t> pass_until{0};
    };
    using Rule = std::int32_t (*)(std::int32_t, std::uint16_t, std::int32_t&);
    static std::int32_t Scaled(Slot& s, std::int32_t sum, std::uint16_t pct, Rule rule,
                               std::atomic<std::uint64_t>& hits) noexcept;
    static std::int32_t Passed(Slot& s, std::int32_t sum, std::uint64_t now_ms) noexcept;
    static std::size_t Hash(std::uint64_t address) noexcept;
    Slot* Find(std::uint64_t address) noexcept;
    const Slot* Find(std::uint64_t address) const noexcept;

    std::array<Slot, kSlots> slots_{};
    std::atomic<std::uint64_t> enemy_hits_{0};
    std::atomic<std::uint64_t> link_hits_{0};
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

// Which placed enemy gets extras: an Enemy_ actor with a placement hash and
// a life, never a boss or a part of one, nor an enemy the sync treats in its
// own way (`special`: rolled, hatching, announced, a puzzle, a shot), nor
// one a copy beside it could spoil (on a wall, a hand of another, a
// manager, a flock).
bool GetsExtras(std::string_view name, std::uint64_t hash, bool has_life, bool special);
// The placement id of extra `index` (0 on) of the enemy placed with `hash`:
// never 0 or ~0 (a made actor's), never `hash` itself.
std::uint64_t ExtraHash(std::uint64_t hash, unsigned index);
// Where extra `index` is made: `at` moved kExtraOffsetM sideways (x, z) in
// a direction picked by its placement id, so it does not stand inside its
// original.
inline constexpr float kExtraOffsetM = 0.6f;
std::array<float, 3> ExtraPlace(const std::array<float, 3>& at, std::uint64_t extra_hash);

// One scene visit's extras (present thread).
class Extras {
public:
    static constexpr std::size_t kMost = 32;          // in one scene visit
    static constexpr std::uint64_t kBindWaitMs = 4000;  // a made one not yet bound to its actor

    struct Original {
        std::string name;
        std::uint64_t hash = 0;
        std::array<float, 3> position{};
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
    std::size_t Plan(std::string_view scene, const std::vector<Original>& walk, std::uint16_t count_pct,
                     bool boss_here, std::uint64_t now_ms);
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
    std::string scene_;
    std::vector<Extra> extras_;
    std::map<std::uint64_t, unsigned> planned_;  // original's hash -> extras asked for it
    struct Bound {
        std::uint64_t hash = 0;
        std::string name;
    };
    std::map<std::uint64_t, Bound> by_address_;  // actor -> its extra
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

} // namespace CoopDifficulty
