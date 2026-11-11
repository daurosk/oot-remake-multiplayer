// SPDX-License-Identifier: GPL-3.0-or-later
//
// Projectile rules (coop_projectiles.h, docs/objects.md "Projectiles").

#include "coop_projectiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace CoopProjectiles {

namespace proto = coop::protocol;

namespace {

// Names from the actor packs (docs/objects.md, "The projectile actors").
// Seen live: Shootable_DekuSeed and Item_DekuNut.
constexpr std::array<Info, 8> kInfos{{
    {"Item_Bomb", Kind::bomb, false, true, true},
    {"Item_Bombchu", Kind::bombchu, false, false, true},
    {"Item_DekuNut", Kind::nut, true, false, false},
    {"Shootable_DekuSeed", Kind::seed, true, false, false},
    {"NormalArrow", Kind::arrow, true, false, false},
    {"Weapon_Boomerang", Kind::boomerang, false, false, false},
    {"Shootable_Hook", Kind::hook, false, false, false},
    {"Shootable_HookLong", Kind::hook_long, false, false, false},
}};

bool Finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float Length(const Vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

const Info* Find(std::string_view actor) {
    for (const Info& i : kInfos) {
        if (i.name == actor) {
            return &i;
        }
    }
    return nullptr;
}

Element ElementOf(std::string_view equip_arrow) {
    if (equip_arrow.ends_with("_Fire")) {
        return Element::fire;
    }
    if (equip_arrow.ends_with("_Ice")) {
        return Element::ice;
    }
    if (equip_arrow.ends_with("_Light")) {
        return Element::light;
    }
    return Element::none;
}

proto::DamageCause CauseOf(Kind kind, Element element) {
    switch (kind) {
    case Kind::bomb:
    case Kind::bombchu:
        return proto::DamageCause::bomb;
    case Kind::nut:
        return proto::DamageCause::nut;
    case Kind::seed:
        return proto::DamageCause::seed;
    case Kind::arrow:
        switch (element) {
        case Element::fire:
            return proto::DamageCause::fire_arrow;
        case Element::ice:
            return proto::DamageCause::ice_arrow;
        case Element::light:
            return proto::DamageCause::light_arrow;
        case Element::none:
            break;
        }
        return proto::DamageCause::arrow;
    case Kind::boomerang:
        return proto::DamageCause::boomerang;
    case Kind::hook:
    case Kind::hook_long:
        return proto::DamageCause::hookshot;
    }
    return proto::DamageCause::unknown;
}

const char* NameOf(Kind kind) {
    switch (kind) {
    case Kind::bomb: return "bomb";
    case Kind::bombchu: return "bombchu";
    case Kind::nut: return "nut";
    case Kind::seed: return "seed";
    case Kind::arrow: return "arrow";
    case Kind::boomerang: return "boomerang";
    case Kind::hook: return "hookshot";
    case Kind::hook_long: return "longshot";
    }
    return "?";
}

const char* NameOf(Element element) {
    switch (element) {
    case Element::none: return "plain";
    case Element::fire: return "fire";
    case Element::ice: return "ice";
    case Element::light: return "light";
    }
    return "?";
}

// ---------------------------------------------------------------- Track

bool Track::Add(const Point& p) {
    if (!Finite(p.position) || !Finite(p.velocity) || (last_ && p.time_us <= last_->time_us)) {
        return false;
    }
    // A bounce turns the velocity: that is no fall (issue 166).
    if (last_ && p.time_us - last_->time_us >= kGravityMinGapUs &&
        !Bounced(last_->velocity, p.velocity)) {
        // Only a fall counts: something thrown up slows the same way.
        const float dt = float(p.time_us - last_->time_us) / 1e6f;
        const float g = (last_->velocity[1] - p.velocity[1]) / dt;
        gravity_ = std::clamp(g, 0.0f, kGravityMax);
    }
    last_ = p;
    stopped_ = false;
    return true;
}

Vec3 Track::At(std::uint64_t now_us, bool ballistic) const {
    if (!last_) {
        return {};
    }
    if (stopped_ || now_us <= last_->time_us) {
        return last_->position;
    }
    const float t = float(std::min(now_us - last_->time_us, kAheadMaxUs)) / 1e6f;
    Vec3 at{};
    for (int i = 0; i < 3; ++i) {
        at[i] = last_->position[i] + last_->velocity[i] * t;
    }
    if (ballistic) {
        at[1] -= 0.5f * gravity_ * t * t;
    }
    return at;
}

void Track::Stop(const Vec3& at) {
    if (!Finite(at)) {
        return;
    }
    if (!last_) {
        last_ = Point{};
    }
    last_->position = at;
    last_->velocity = {};
    stopped_ = true;
}

// ---------------------------------------------------------------- FuseClock

void FuseClock::Lived(std::uint64_t lifetime_us) {
    if (lifetime_us < kMinUs || lifetime_us > kMaxUs) {
        return;
    }
    lives_.push_back(lifetime_us);
    while (lives_.size() > kKeep) {
        lives_.pop_front();
    }
}

std::optional<std::uint64_t> FuseClock::Fuse() const {
    if (lives_.empty()) {
        return std::nullopt;
    }
    std::vector<std::uint64_t> sorted(lives_.begin(), lives_.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
}

std::optional<float> FuseClock::Left(std::uint64_t age_us) const {
    const auto fuse = Fuse();
    if (!fuse || age_us >= *fuse) {
        return std::nullopt;
    }
    return float(*fuse - age_us) / 1e6f;
}

// ---------------------------------------------------------------- reasons

bool AmmoWatch::Dropped(Kind kind, std::optional<std::int32_t> count) {
    if (!count || *count < 0) {
        return false;
    }
    std::int32_t& last = last_[static_cast<std::size_t>(kind)];
    // One shot takes one: a bigger drop (the host's save load lowering the
    // count, a load) is no shot, so no nocked seed is marked shot by it.
    const bool dropped = last >= 0 && *count == last - 1;
    last = *count;
    return dropped;
}

std::uint32_t AmmoFlag(Kind kind) {
    switch (kind) {
    case Kind::seed:
        return 0x5fadf7fc;  // Item_DekuSeed
    case Kind::arrow:
        return 0x518ec74b;  // Item_Arrow_Normal
    default:
        return 0;
    }
}

// From the data files: the bow's BowParam has an initial speed of 1.875 a
// frame (about 56 m/s at 30 frames a second) and a range of 20. The
// slingshot has only the default BowParam; its seed is taken a little slower.
float ShotSpeed(Kind kind) {
    switch (kind) {
    case Kind::arrow:
        return 56.0f;
    case Kind::seed:
        return 40.0f;
    default:
        return 30.0f;
    }
}

bool Bounced(const Vec3& before, const Vec3& after) {
    const float a = std::sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2]);
    const float b = std::sqrt(after[0] * after[0] + after[1] * after[1] + after[2] * after[2]);
    if (!(a >= kHitSpeed) || !(b >= kHitSpeed)) {
        return false;
    }
    const float dot = before[0] * after[0] + before[1] * after[1] + before[2] * after[2];
    return dot < kBounceCos * a * b;
}

std::uint64_t LandHoldUs(Kind kind) {
    switch (kind) {
    case Kind::arrow:
        return 1'500'000;  // sticks where it struck
    case Kind::seed:
        return 200'000;    // seen landing, then gone as on the owner's screen
    default:
        return 0;          // a nut flashes by its own AI; the rest go at once
    }
}

proto::RemoveReason GoneReason(const Info& info, std::uint64_t lifetime_us, float speed) {
    if (info.blasts) {
        // A bomb or bombchu that lived at all went off; one gone at once was
        // the game's help-box bomb (docs/spawns.md).
        return lifetime_us >= kExplodeMinUs ? proto::RemoveReason::exploded
                                            : proto::RemoveReason::gone;
    }
    if (info.ballistic && speed >= kHitSpeed) {
        return proto::RemoveReason::hit;
    }
    return proto::RemoveReason::gone;
}

std::optional<std::uint64_t> BlowAt(std::uint64_t sample_us, std::optional<float> fuse_s) {
    if (!fuse_s || !std::isfinite(*fuse_s) || *fuse_s <= 0.0f ||
        *fuse_s * 1e6f > float(FuseClock::kMaxUs)) {
        return std::nullopt;
    }
    return sample_us + std::uint64_t(double(*fuse_s) * 1e6);
}

// ---------------------------------------------------------------- Blasts

void Blasts::Add(const Vec3& at, std::uint64_t time_us) {
    if (!Finite(at)) {
        return;
    }
    std::lock_guard lock(mutex_);
    blasts_.push_back({at, time_us});
    while (blasts_.size() > kKeep) {
        blasts_.pop_front();
    }
}

bool Blasts::Near(const Vec3& at, std::uint64_t now_us) const {
    std::lock_guard lock(mutex_);
    for (const Blast& b : blasts_) {
        const bool in_time = now_us + kBeforeUs >= b.time_us && now_us <= b.time_us + kAfterUs;
        const Vec3 d{at[0] - b.at[0], at[1] - b.at[1], at[2] - b.at[2]};
        if (in_time && Length(d) <= kRadius) {
            return true;
        }
    }
    return false;
}

std::size_t Blasts::Size() const {
    std::lock_guard lock(mutex_);
    return blasts_.size();
}

void Blasts::Reset() {
    std::lock_guard lock(mutex_);
    blasts_.clear();
}

Blasts& TheBlasts() {
    static Blasts blasts;
    return blasts;
}

bool IsLure(std::string_view actor) {
    return actor == "Weapon_Lure" || actor == "Weapon_SinkingLure";
}

void Lures::Set(std::vector<Vec3> at) {
    std::lock_guard lock(mutex_);
    at_ = std::move(at);
}

std::vector<Vec3> Lures::Get() const {
    std::lock_guard lock(mutex_);
    return at_;
}

Lures& TheLures() {
    static Lures lures;
    return lures;
}

} // namespace CoopProjectiles
