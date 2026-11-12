// SPDX-License-Identifier: GPL-3.0-or-later
//
// Projectiles (bombs, bombchus, Deku nuts and seeds, arrows, the boomerang,
// the hookshot and longshot tips) seen in every game (docs/objects.md,
// "Projectiles"). The spawn sync (coop_spawn.h) finds them on the actor list
// and makes the copies; this file holds the rules that make a copy fly the
// owner's path, point the owner's way and go off at the owner's moment:
//
// - Kinds: which actors are projectiles, how they fly, what hit they deal.
// - Track: where a copy is now, from the owner's samples on the server
//   clock, with the gravity the samples show for things that fall.
// - FuseClock: how long the owner's bombs last, learnt from its own bombs,
//   so a bomb's copy can be blown up at the owner's moment and not one
//   network delay late.
// - GoneReason: why an owner's projectile left the list.
// - Blasts: where and when a copy went off here. A copy is the game's own
//   actor, so it can hurt this game's enemies; the objects sync asks Blasts
//   so such a hit is neither sent to the owner nor taken twice.
//
// No emulator code: tested in tests/lichii/projectiles_test.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopProjectiles {

using coop::protocol::Vec3;

enum class Kind : std::uint8_t { bomb, bombchu, nut, seed, arrow, boomerang, hook, hook_long };

// An arrow's element, from its shooter's PLAYER_EQUIP arrow (EquipArrow).
enum class Element : std::uint8_t { none, fire, ice, light };

struct Info {
    std::string_view name;  // the actor's name (actor +0x260)
    Kind kind;
    bool ballistic;  // flies free and falls: the copy is carried on along its arc
    bool fused;      // goes off by a timer: its copy can be blown up on time
    bool blasts;     // its end hurts what is around it
};

const Info* Find(std::string_view actor);
Element ElementOf(std::string_view equip_arrow);  // "Item_Arrow_Fire" and so on
coop::protocol::DamageCause CauseOf(Kind kind, Element element = Element::none);
const char* NameOf(Kind kind);
const char* NameOf(Element element);

// One owner sample on the server clock.
struct Point {
    std::uint64_t time_us = 0;
    Vec3 position{};
    Vec3 velocity{};
};

// The owner's latest samples of one projectile, and where it is now.
class Track {
public:
    // Carried on at most this far past the last sample (then held there):
    // one network delay and a frame or two, never a whole missed flight.
    static constexpr std::uint64_t kAheadMaxUs = 250'000;
    // Gravity as the samples show it (metres a second squared, downwards
    // only), so an arrow's or a seed's copy drops the way the owner's did.
    static constexpr float kGravityMax = 60.0f;
    // Two samples closer than this in time give no gravity (noise).
    static constexpr std::uint64_t kGravityMinGapUs = 20'000;

    // Older or equal times, and points that are not finite, are dropped.
    // True when taken.
    bool Add(const Point& p);
    bool Empty() const { return !last_; }
    const Point& Last() const { return *last_; }
    float Gravity() const { return gravity_; }
    // Where it is at `now_us` (server clock). `ballistic` adds the fall.
    Vec3 At(std::uint64_t now_us, bool ballistic) const;
    // Smooth shots: where a copy is shown at `now_us`. The owner's
    // path kBehindUs late (as late as the puppet that shot it), between the
    // two samples around that time, so the copy moves in even steps every
    // frame. Carried on as At() only when the samples are later than that.
    // Before, a copy was carried on from the latest sample by its velocity
    // (measured between two walks, so noisy) and snapped at each sample.
    static constexpr std::uint64_t kBehindUs = 100'000;
    static constexpr std::size_t kKeep = 16;
    Vec3 Shown(std::uint64_t now_us, bool ballistic) const;
    // Held still at `at` (the owner's remove): no more carrying on. A shown
    // copy goes on to `at` (taken as kStopAfterUs after the last sample, a
    // walk later) and stays there.
    static constexpr std::uint64_t kStopAfterUs = 33'000;
    void Stop(const Vec3& at);
    bool Stopped() const { return stopped_; }

private:
    std::optional<Point> last_;
    std::deque<Point> points_;  // the newest kKeep, oldest first (and the stop)
    float gravity_ = 0.0f;
    bool stopped_ = false;
};

// The life of this game's own bombs, so their copies elsewhere can go off on
// time. Learnt from bombs that exploded (gone after kMinUs to kMaxUs).
class FuseClock {
public:
    static constexpr std::uint64_t kMinUs = 1'000'000;   // shorter: not a fuse
    static constexpr std::uint64_t kMaxUs = 10'000'000;  // longer: odd
    static constexpr std::size_t kKeep = 7;

    void Lived(std::uint64_t lifetime_us);
    // The middle of the kept lives, once one is known.
    std::optional<std::uint64_t> Fuse() const;
    // Seconds left for a bomb made `age_us` ago; nothing when no fuse is
    // known or it is already past.
    std::optional<float> Left(std::uint64_t age_us) const;
    void Reset() { lives_.clear(); }

private:
    std::deque<std::uint64_t> lives_;
};

// The moment of a shot, from the shooter's ammo (issue 158): the seed and
// the arrow sit nocked until let go, and their count goes down then. One
// AmmoWatch per game; Dropped is asked once a walk per kind with the count
// read now (nothing when the flags cannot be read: the last count is kept).
class AmmoWatch {
public:
    // True once for each read exactly one lower than the one before (a
    // bigger drop is a save load or the like, not a shot).
    bool Dropped(Kind kind, std::optional<std::int32_t> count);
    void Reset() { last_.fill(-1); }

private:
    std::array<std::int32_t, 8> last_{-1, -1, -1, -1, -1, -1, -1, -1};
};
// The ammo count's flag (PouchData.<item>.StockNum, docs/items.md) for a
// kind that sits nocked: seeds and arrows. 0 for the others.
std::uint32_t AmmoFlag(Kind kind);
// The speed a made-up flight of `kind` goes at, metres a second.
float ShotSpeed(Kind kind);

// Issue 169: a bomb in its owner's hands. The owner's game holds it over
// Link (the game binds it to Link's hands); every other game shows it as a
// model-only stand-in with no physics, kept in the puppet's hands, until it
// is thrown. Held means: above Link's feet, close to him sideways, and moving
// with him (a thrown bomb that passes the box moves much faster than Link).
inline constexpr float kHeldReach = 0.9f;   // metres from Link, sideways
inline constexpr float kHeldLow = 0.3f;     // metres above Link's feet
inline constexpr float kHeldHigh = 2.8f;    // an adult's raised arms and some
inline constexpr float kHeldSlip = 2.0f;    // m/s apart from Link at most
bool InHands(const Vec3& item, const Vec3& link, float speed_apart);
// What the bomb's own animation says (AS LiftableExplodeObject_BallBomb:
// Lifted, Thrown, Wait, Bomb_Wait, Bomb_Blink, Explode): true for Lifted,
// false for Thrown, nothing for the others (the box decides).
std::optional<bool> HeldByAnimation(std::string_view name);
// The copy of a held bomb is shown this late, as the puppet holding it is
// (Presence::Settings::show_delay_us), so it stays in the puppet's hands.
inline constexpr std::uint64_t kHeldBehindUs = 100'000;

// The owner's latest samples of a held bomb (server clock), so its copy can
// be shown kHeldBehindUs late, between two of them.
class HeldPath {
public:
    static constexpr std::size_t kKeep = 16;
    // Older or equal times, and points that are not finite, are dropped.
    void Add(const Point& p);
    void Clear() { points_.clear(); }
    bool Empty() const { return points_.empty(); }
    // Where it was at `time_us`: between the two samples around it; the
    // first before them, the last after them (never carried on).
    Vec3 At(std::uint64_t time_us) const;

private:
    std::deque<Point> points_;
};

// Why an owner's projectile left the list after `lifetime_us`, last seen
// moving at `speed` metres a second.
coop::protocol::RemoveReason GoneReason(const Info& info, std::uint64_t lifetime_us, float speed);
// A shot that leaves the list faster than this was stopped by what it hit.
inline constexpr float kHitSpeed = 3.0f;
// Issue 166: a flight that turned by more than 45 degrees between two steps,
// both at kHitSpeed or more, bounced (a seed off a wall). Its sample is sent
// at once, and a copy's track keeps its fall across it.
inline constexpr float kBounceCos = 0.7071f;
bool Bounced(const Vec3& before, const Vec3& after);
// Issue 166: how long a copy that hit something (remove reason hit) stays
// where the owner's shot struck before it goes: a seed lands, an arrow
// sticks. 0: it goes at once.
std::uint64_t LandHoldUs(Kind kind);
// A bomb gone sooner than this did not explode: taking one out the first
// time makes a bomb that lasts about 0.1 s while the game shows its help.
inline constexpr std::uint64_t kExplodeMinUs = 1'000'000;

// When the owner's bomb goes off on the server clock: the sample's time plus
// the fuse it said was left. Nothing for a fuse that is not finite, not above
// 0, or over FuseClock::kMaxUs.
std::optional<std::uint64_t> BlowAt(std::uint64_t sample_us, std::optional<float> fuse_s);

// Copies that went off here (server clock). The spawn sync adds, the objects
// sync asks; any thread may call.
class Blasts {
public:
    static constexpr float kRadius = 6.0f;                // a bomb's reach and some
    static constexpr std::uint64_t kBeforeUs = 200'000;   // a read just before
    static constexpr std::uint64_t kAfterUs = 1'200'000;  // the game's hit, then a walk
    static constexpr std::size_t kKeep = 32;

    void Add(const Vec3& at, std::uint64_t time_us);
    // A life drop at `at`, read at `now_us`, that a copy's blast may have made.
    bool Near(const Vec3& at, std::uint64_t now_us) const;
    std::size_t Size() const;
    void Reset();

private:
    struct Blast {
        Vec3 at{};
        std::uint64_t time_us = 0;
    };
    mutable std::mutex mutex_;
    std::deque<Blast> blasts_;
};

Blasts& TheBlasts();

// Issue 151 (fishing-2). A cast lure: Weapon_Lure or Weapon_SinkingLure. The
// spawn sync shares ours (copies are model-only stand-ins, so another
// player's lure is seen and no fish here can bite it) and puts where they
// are here; the objects sync asks, so a fish that goes for our lure (or that
// our lure has hooked) is handed to our game. Any thread may call.
bool IsLure(std::string_view actor);
class Lures {
public:
    void Set(std::vector<Vec3> at);
    std::vector<Vec3> Get() const;

private:
    mutable std::mutex mutex_;
    std::vector<Vec3> at_;
};
Lures& TheLures();

} // namespace CoopProjectiles
