// SPDX-License-Identifier: GPL-3.0-or-later
//
// Things a player makes, shown in every game (coop_spawn.h, docs/spawns.md).
// Sync and Director need no emulator code and are tested in
// tests/lichii/spawn_test.cpp; the hook glue at the end is built into the
// emulator only (LICHII_COOP).

#include "coop_spawn.h"

#include "coop_objects_sync.h"  // QuatOf

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <variant>

namespace CoopSpawn {

namespace proto = coop::protocol;

namespace {

struct Kind {
    std::string_view name;
    bool explodes;
    bool flies;
};
constexpr std::array<Kind, 10> kKinds{{
    {"Item_Bomb", true, true},
    {"Item_Bombchu", true, true},
    {"Item_DekuNut", false, true},
    {"Shootable_DekuSeed", false, true},
    {"NormalArrow", false, true},
    {"Weapon_Boomerang", false, true},
    {"Shootable_Hook", false, true},
    {"Shootable_HookLong", false, true},
    // Issue 151: a cast lure (always a stand-in, see StandInOnly).
    {"Weapon_Lure", false, true},
    {"Weapon_SinkingLure", false, true},
}};

const Kind* Find(std::string_view name) {
    for (const Kind& k : kKinds) {
        if (k.name == name) {
            return &k;
        }
    }
    return nullptr;
}

float Distance(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool Finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Main offsets (build e0f3ccfaf50065ae): docs/objects.md and docs/puppet.md.
constexpr std::uint64_t kActorComponents = 0x270;
constexpr std::uint64_t kComponentLife = 0x2b8;
constexpr std::uint64_t kLifeValue = 0x1c10;

template <typename T>
bool ReadValue(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return guest.Read(address, &out, sizeof(T));
}
template <typename T>
bool WriteValue(CoopPuppet::Guest& guest, std::uint64_t address, const T& value) {
    return guest.Write(address, &value, sizeof(T));
}

bool Gone(std::uint32_t state) {
    return state >= 7 && state <= 11;  // as the puppet director
}

std::string Hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

bool IsShared(std::string_view name) {
    return Find(name) != nullptr;
}
bool Explodes(std::string_view name) {
    const Kind* k = Find(name);
    return k != nullptr && k->explodes;
}
// Issue 85: a hookshot tip's animation set draws its chain (ChainExtend in
// Weapon_Range_Hookshot.root.asb), so its copy plays the owner's animation too.
bool Animated(std::string_view name) {
    return Explodes(name) || name == "Shootable_Hook" || name == "Shootable_HookLong";
}
bool Flies(std::string_view name) {
    const Kind* k = Find(name);
    return k != nullptr && k->flies;
}

// ---------------------------------------------------------------- Sync

void Sync::Receive(const proto::Message& message) {
    if (std::holds_alternative<proto::ObjectSpawn>(message) ||
        std::holds_alternative<proto::ObjectSample>(message) ||
        std::holds_alternative<proto::ObjectRemove>(message)) {
        std::lock_guard lock(inbox_mutex_);
        if (inbox_.size() < 1024) {
            inbox_.push_back(message);
        }
    }
}

std::vector<Vec3> Sync::OwnLures() const {
    std::vector<Vec3> out;
    for (const auto& [address, m] : mine_) {
        if (CoopProjectiles::IsLure(m.name) && m.missed == 0) {
            out.push_back(m.position);
        }
    }
    return out;
}

void Sync::Reset() {
    scene_.clear();
    first_walk_ = true;
    old_.clear();
    mine_.clear();
    copies_.clear();
    copy_addresses_.clear();
    impacts_.clear();  // issue 166: the old scene's sparks
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

void Sync::Apply(std::uint64_t now_us) {
    std::vector<proto::Message> inbox;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
    }
    for (const auto& m : inbox) {
        if (const auto* s = std::get_if<proto::ObjectSpawn>(&m)) {
            if (s->id.kind != proto::ObjectKind::made || !IsShared(s->actor) ||
                copies_.count(s->id) != 0 || !s->state.position || !Finite(*s->state.position)) {
                continue;
            }
            Remote r;
            r.key = next_key_++;
            r.actor = s->actor;
            r.track.Add({s->state.time.server_time_us, *s->state.position,
                         s->state.velocity.value_or(Vec3{})});
            r.rotation = s->state.rotation;
            r.blow_at = CoopProjectiles::BlowAt(s->state.time.server_time_us, s->state.fuse);
            r.got_us = now_us;
            // Issue 169: pulled out of the bag, so held from its first sample.
            r.held = Explodes(s->actor) && s->state.state &&
                     (*s->state.state & proto::object_state::held) != 0;
            r.held_path.Add({s->state.time.server_time_us, *s->state.position, {}});
            r.animation = s->state.animation;
            r.animation_at = s->state.time.server_time_us;
            copies_.emplace(s->id, std::move(r));
        } else if (const auto* p = std::get_if<proto::ObjectSample>(&m)) {
            auto it = copies_.find(p->id);
            if (it == copies_.end() || it->second.removed || !p->state.position ||
                !it->second.track.Add({p->state.time.server_time_us, *p->state.position,
                                       p->state.velocity.value_or(Vec3{})})) {
                continue;
            }
            Remote& r = it->second;
            if (p->state.rotation) {
                r.rotation = p->state.rotation;
            }
            if (const auto at = CoopProjectiles::BlowAt(p->state.time.server_time_us, p->state.fuse)) {
                r.blow_at = at;  // the owner's latest word on its fuse
            }
            r.got_us = now_us;
            // Issue 169: held or let go, and the fuse's animation.
            if (p->state.state && Explodes(r.actor)) {
                const bool held = (*p->state.state & proto::object_state::held) != 0;
                if (held != r.held) {
                    Rekey(r, held);
                }
            }
            r.held_path.Add({p->state.time.server_time_us, *p->state.position, {}});
            if (p->state.animation) {
                r.animation = p->state.animation;
                r.animation_at = p->state.time.server_time_us;
            }
        } else if (const auto* g = std::get_if<proto::ObjectRemove>(&m)) {
            auto it = copies_.find(g->id);
            if (it == copies_.end() || it->second.removed) {
                continue;
            }
            Remote& r = it->second;
            r.track.Stop(Finite(g->position) ? g->position : r.Position());
            r.removed = g->reason;
            r.got_us = now_us;
        }
    }
}

void Sync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
                const std::vector<Seen>& seen,
                const std::map<std::uint64_t, std::uint64_t>& copies_live,
                std::vector<proto::Message>& send, std::vector<Copy>& copies) {
    if (scene != scene_) {
        // The server lets go of everything made in the old scene.
        Reset();
        scene_ = scene;
    }
    Apply(now_us);
    flights_.clear();
    made_up_.clear();
    // Issue 169: how fast our Link moves, so a bomb moving with him is held.
    link_velocity_ = {};
    if (link_ && Finite(*link_)) {
        if (link_before_ && now_us > link_before_at_) {
            const float dt = float(now_us - link_before_at_) / 1e6f;
            for (int i = 0; i < 3; ++i) {
                link_velocity_[i] = ((*link_)[i] - (*link_before_)[i]) / dt;
            }
        }
        link_before_ = link_;
        link_before_at_ = now_us;
    } else {
        link_before_.reset();
    }

    for (auto& [id, r] : copies_) {
        if (const auto it = copies_live.find(r.key); it != copies_live.end() && it->second != 0) {
            r.address = it->second;
            copy_addresses_.insert(r.address);
            r.live = true;
        } else if (r.live && !r.blasted && Explodes(r.actor) &&
                   (!r.removed || *r.removed == proto::RemoveReason::exploded)) {
            // Our copy bomb went off by itself (its own fuse, or a hit): its
            // blast may hurt this game's enemies. Not one the owner removed
            // as gone: the director deleted that one, nothing went off.
            CoopProjectiles::TheBlasts().Add(r.track.At(now_us, false), now_us);
            r.blasted = true;
        }
    }

    std::set<std::uint64_t> on_list;
    for (const Seen& s : seen) {
        on_list.insert(s.address);
    }
    if (first_walk_) {
        old_ = on_list;  // already there when we came: not made by anyone now
        first_walk_ = false;
    }

    for (const Seen& s : seen) {
        if (old_.count(s.address) != 0 || copy_addresses_.count(s.address) != 0 ||
            !Finite(s.position)) {
            continue;
        }
        if (auto it = mine_.find(s.address); it != mine_.end() && it->second.name == s.name) {
            Own& m = it->second;
            const double dt = double(now_us - m.seen_at) / 1e6;
            const Vec3 before = m.velocity;
            if (dt > 0.0) {
                for (int i = 0; i < 3; ++i) {
                    m.velocity[i] = float((s.position[i] - m.position[i]) / dt);
                }
            }
            m.position = s.position;
            if (s.rotation) {
                m.rotation = CoopObjects::QuatOf(*s.rotation);
            }
            if (dt > 0.0) {
                const Vec3& v = m.velocity;
                const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                m.top_speed = std::max(m.top_speed, speed);
                // Only steps that start at or after the shot: Shots() runs
                // before this walk with the same now_us, so the step that
                // holds the let-go (seen_at before shot_at) is left out.
                if (m.shot_at && m.seen_at >= *m.shot_at) {
                    m.shot_top_speed = std::max(m.shot_top_speed, speed);
                }
            }
            // Issue 166: a seed off a wall. Its turn is sent at once, so
            // the copies bounce where the owner's did, not a sample later.
            const auto* info = CoopProjectiles::Find(m.name);
            const bool bounced = dt > 0.0 && info != nullptr && info->ballistic &&
                                 CoopProjectiles::Bounced(before, m.velocity);
            if (bounced) {
                ++m.bounces;
            }
            m.far = std::max(m.far, Distance(m.position, m.made_position));
            m.seen_at = now_us;
            m.missed = 0;
            m.animation = s.animation;
            Hands(m, false);
            MakeUpFlight(m, now_us);
            // A made-up flight is sent in place of the place, which stays
            // in the sling.
            const Vec3 at = m.made_up ? ShotPlace(m, now_us) : m.position;
            const Vec3 velocity =
                m.made_up ? (now_us - *m.shot_at < kShotFlightUs ? m.shot_velocity : Vec3{})
                          : m.velocity;
            const bool moved = Distance(at, m.sent_position) > 0.01f;
            // Issue 169: a change of hands goes at once; a held or animated
            // bomb goes at the full rate (its fuse's frames, the hands' moves).
            // Issue 166: a bounce goes at once too.
            const bool hands_changed = m.held != m.sent_held;
            const bool busy = m.held || m.animation.has_value();
            if (hands_changed || (bounced && !m.made_up) ||
                (now_us - m.sent_at >= kSampleEveryUs &&
                 (moved || busy || now_us - m.sent_at >= kStillEveryUs))) {
                proto::ObjectSample out;
                out.id = m.id;
                out.state.time = {now_us, ++m.sequence};
                out.state.position = at;
                out.state.velocity = velocity;
                out.state.rotation = m.rotation;
                if (Explodes(m.name)) {
                    out.state.fuse = fuse_.Left(now_us - m.made_at);
                    out.state.state = m.held ? proto::object_state::held : std::uint8_t{0};
                }
                if (Animated(m.name)) {
                    out.state.animation = m.animation;
                }
                send.emplace_back(std::move(out));
                m.sent_at = now_us;
                m.sent_position = at;
                m.sent_held = m.held;
            }
            continue;
        }
        // A copy the director made but has not reported yet: the nearest
        // unclaimed copy of that name.
        bool claimed = false;
        for (auto& [id, r] : copies_) {
            if (r.address == 0 && r.actor == s.name && Distance(r.Position(), s.position) < kClaimDistance) {
                r.address = s.address;
                copy_addresses_.insert(s.address);
                claimed = true;
                break;
            }
        }
        if (claimed || self == 0) {
            continue;
        }
        // Ours.
        if (counter_ == 0) {
            counter_ = now_us;  // ids stay unique across reconnects
        }
        Own m;
        m.id = {proto::ObjectKind::made, self, ++counter_};
        m.name = s.name;
        m.position = m.sent_position = m.made_position = s.position;
        m.seen_at = m.sent_at = now_us;
        m.made_at = now_us;
        if (s.rotation) {
            m.rotation = CoopObjects::QuatOf(*s.rotation);
        }
        m.animation = s.animation;
        Hands(m, true);
        proto::ObjectSpawn out;
        out.id = m.id;
        out.actor = s.name;
        out.flags = Flies(s.name) ? proto::object_spawn::projectile : 0;
        out.state.time = {now_us, 0};
        out.state.position = s.position;
        out.state.velocity = Vec3{};
        out.state.rotation = m.rotation;
        if (Explodes(s.name)) {
            out.state.fuse = fuse_.Left(0);  // known once one of ours went off
            out.state.state = m.held ? proto::object_state::held : std::uint8_t{0};
        }
        if (Animated(s.name)) {
            out.state.animation = m.animation;
        }
        m.sent_held = m.held;
        send.emplace_back(std::move(out));
        mine_[s.address] = std::move(m);
    }

    for (auto it = mine_.begin(); it != mine_.end();) {
        if (on_list.count(it->first) != 0 || ++it->second.missed < kGoneWalks) {
            ++it;
            continue;
        }
        const Own& m = it->second;
        const std::uint64_t lived = now_us - m.made_at;
        proto::ObjectRemove out;
        out.id = m.id;
        if (const auto* info = CoopProjectiles::Find(m.name)) {
            const Vec3& v = m.velocity;
            out.reason = CoopProjectiles::GoneReason(*info, lived,
                                                     std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
        } else {
            out.reason = proto::RemoveReason::gone;
        }
        if (m.made_up && out.reason == proto::RemoveReason::hit) {
            // A made-up flight's end is a guess: the copies are not held there
            // (issue 166 landing), they go at once.
            out.reason = proto::RemoveReason::gone;
        }
        if (out.reason == proto::RemoveReason::exploded && CoopProjectiles::Find(m.name)->fused) {
            fuse_.Lived(lived);  // the game's fuse, for our next bombs' copies
        }
        out.position = m.made_up ? ShotPlace(m, now_us) : m.position;
        send.emplace_back(out);
        flights_.push_back({m.id, m.name, lived, m.far, m.top_speed, out.reason,
                            m.shot_at.has_value(), m.made_up, m.bounces});
        it = mine_.erase(it);
    }
    // Addresses the game may give to a new actor later.
    std::erase_if(old_, [&](std::uint64_t a) { return on_list.count(a) == 0; });
    std::erase_if(copy_addresses_, [&](std::uint64_t a) { return on_list.count(a) == 0; });

    for (auto it = copies_.begin(); it != copies_.end();) {
        Remote& r = it->second;
        const std::uint64_t age = now_us - r.got_us;
        if ((r.removed && age > 2'000'000) || (!r.removed && age > kCopyLifeUs)) {
            it = copies_.erase(it);
            continue;
        }
        const auto* info = CoopProjectiles::Find(r.actor);
        // The owner's bomb goes off now by its fuse: so does ours, without
        // waiting one network delay for its remove.
        const bool due = !r.removed && r.blow_at && now_us >= *r.blow_at;
        if (due && !r.blasted) {
            ++blown_on_time_;
        }
        const bool explode = due || (r.removed && *r.removed == proto::RemoveReason::exploded);
        if (explode && r.held) {
            // Issue 169: it went off in the owner's hands. The held stand-in
            // cannot explode: the game's own bomb is made there, then blown.
            Rekey(r, false);
            r.explode_once_made = true;
        }
        Copy c;
        c.key = r.key;
        c.actor = r.actor;
        if ((r.held || r.explode_once_made) && !r.held_path.Empty()) {
            // Issue 169: in the puppet's hands, shown as late as the puppet
            // (and blown up there when it went off in them).
            c.position = r.held_path.At(now_us - std::min(now_us, CoopProjectiles::kHeldBehindUs));
        } else if (explode) {
            // Goes off where the owner's did, at the owner's moment.
            c.position = r.track.At(now_us, info != nullptr && info->ballistic);
        } else {
            // Smooth shots: the owner's path as late as the puppet, between
            // its samples, so the copy flies in even steps.
            c.position = r.track.Shown(now_us, info != nullptr && info->ballistic);
        }
        if (r.rotation) {
            c.rotation = CoopPuppet::QuatToMatrix(*r.rotation);
        }
        c.setter = setter_;
        c.held = r.held;
        if (r.animation) {
            proto::Animation a = *r.animation;
            const std::int64_t age = std::int64_t(now_us) - std::int64_t(r.animation_at) -
                                     (r.held ? std::int64_t(CoopProjectiles::kHeldBehindUs) : 0);
            a.frame = CoopObjects::FrameAfter(a.frame, a.rate, age);
            c.animation = std::move(a);
        }
        const auto live_it = copies_live.find(r.key);
        const bool made = live_it != copies_live.end() && live_it->second != 0;
        // Issue 166: a shot that hit something is seen landing there (the
        // track is stopped at the owner's last place) before it goes.
        // Smooth shots: a copy is shown Track::kBehindUs late, so any copy
        // the owner removed (hit or gone) is kept that much longer: it
        // reaches the hit spot (or the hand) first, then gets its full hold.
        const bool hit = r.removed && *r.removed == proto::RemoveReason::hit && info != nullptr;
        const std::uint64_t hold =
            CoopProjectiles::Track::kBehindUs + (hit ? CoopProjectiles::LandHoldUs(info->kind) : 0);
        const bool landing = r.removed && !explode && now_us - r.got_us < hold;
        if (hit && landing && !r.landed) {
            r.landed = true;
            ++landed_;
        }
        if (hit && landing && !r.impacted && impacts_on_ &&
            now_us >= r.got_us + CoopProjectiles::Track::kBehindUs) {
            // Issue 166: the copy reaches the spot now (it is shown kBehindUs
            // late): the spark and the sound the owner's game played there.
            r.impacted = true;
            for (const std::string_view name : CoopProjectiles::ImpactActors(info->kind)) {
                impacts_.push_back({next_key_++, std::string(name), r.Position(),
                                    now_us + CoopProjectiles::kImpactLifeUs});
                ++impacts_made_;
            }
        }
        c.act = explode ? Act::explode : (!r.removed || landing) ? Act::show : Act::remove;
        if (explode && r.explode_once_made && !made) {
            c.act = Act::show;  // made first; blown up once the director has it
        }
        if (explode && !r.blasted) {
            CoopProjectiles::TheBlasts().Add(c.position, now_us);
            r.blasted = true;
        }
        copies.push_back(std::move(c));
        ++it;
    }
    // Issue 166: made where they are and left there (no place written); gone
    // from the list, the director deletes them.
    std::erase_if(impacts_, [now_us](const Impact& i) { return now_us >= i.until_us; });
    for (const Impact& i : impacts_) {
        Copy c;
        c.key = i.key;
        c.actor = i.actor;
        c.position = i.position;
        c.place = false;
        copies.push_back(std::move(c));
    }
}

void Sync::Rekey(Remote& r, bool held) {
    r.held = held;
    r.key = next_key_++;
    r.address = 0;
    r.live = false;  // the old actor's going is no blast
}

void Sync::Hands(Own& m, bool first) {
    const auto* info = CoopProjectiles::Find(m.name);
    if (info == nullptr || info->kind != CoopProjectiles::Kind::bomb || !link_ || !Finite(*link_)) {
        m.held = false;
        m.out_of_hands = 0;
        return;
    }
    Vec3 apart{};
    if (!first) {
        for (int i = 0; i < 3; ++i) {
            apart[i] = m.velocity[i] - link_velocity_[i];
        }
    }
    const float speed_apart =
        std::sqrt(apart[0] * apart[0] + apart[1] * apart[1] + apart[2] * apart[2]);
    const auto said = m.animation ? CoopProjectiles::HeldByAnimation(m.animation->name) : std::nullopt;
    if (said == false) {
        m.held = false;  // its own animation says thrown: at once
        m.out_of_hands = 0;
        return;
    }
    if (said == true || CoopProjectiles::InHands(m.position, *link_, speed_apart)) {
        m.held = true;
        m.out_of_hands = 0;
    } else if (m.held && ++m.out_of_hands >= kLeaveHandsWalks) {
        m.held = false;
        m.out_of_hands = 0;
    }
}

std::size_t Sync::HeldMine() const {
    return std::size_t(std::count_if(mine_.begin(), mine_.end(),
                                     [](const auto& e) { return e.second.held; }));
}

std::size_t Sync::HeldCopies() const {
    return std::size_t(std::count_if(copies_.begin(), copies_.end(), [](const auto& e) {
        return e.second.held && !e.second.removed;
    }));
}

bool Sync::Shot(std::string_view actor, std::uint64_t now_us) {
    Own* newest = nullptr;
    for (auto& [address, m] : mine_) {
        if (m.name == actor && !m.shot_at && (newest == nullptr || m.made_at > newest->made_at)) {
            newest = &m;
        }
    }
    if (newest == nullptr) {
        return false;
    }
    newest->shot_at = now_us;
    newest->shot_from = newest->position;
    return true;
}

Vec3 ShotAxis(const proto::Quat& rotation, int axis) {
    const CoopPuppet::Mat3 m = CoopPuppet::QuatToMatrix(rotation);
    const int column = ((axis % 3) + 3) % 3;
    const float sign = axis >= 3 ? -1.0f : 1.0f;
    return {sign * m[column], sign * m[3 + column], sign * m[6 + column]};
}

void Sync::MakeUpFlight(Own& m, std::uint64_t now_us) {
    if (m.made_up || !m.shot_at || now_us - *m.shot_at < kShotGraceUs ||
        m.shot_top_speed >= kFlySpeed || !m.rotation) {
        return;
    }
    const auto* info = CoopProjectiles::Find(m.name);
    if (info == nullptr) {
        return;
    }
    const float speed = shot_settings_.speed.value_or(CoopProjectiles::ShotSpeed(info->kind));
    const Vec3 dir = ShotAxis(*m.rotation, shot_settings_.axis);
    m.shot_velocity = {dir[0] * speed, dir[1] * speed, dir[2] * speed};
    m.made_up = true;
    made_up_.push_back({m.id, m.name, m.shot_from, m.shot_velocity});
}

Vec3 Sync::ShotPlace(const Own& m, std::uint64_t now_us) const {
    const double t =
        double(std::min<std::uint64_t>(now_us - std::min(now_us, *m.shot_at), kShotFlightUs)) / 1e6;
    return {m.shot_from[0] + float(m.shot_velocity[0] * t),
            m.shot_from[1] + float(m.shot_velocity[1] * t),
            m.shot_from[2] + float(m.shot_velocity[2] * t)};
}

// ---------------------------------------------------------------- Director

void Director::Say(const std::string& text) const {
    if (log_) {
        log_("spawn: " + text);
    }
}

void Director::SetInput(std::uint64_t main_base, std::vector<Copy> copies) {
    std::lock_guard lock(mutex_);
    next_base_ = main_base;
    next_ = std::move(copies);
}

void Director::SetEnemies(std::uint64_t main_base, std::vector<Copy> copies) {
    std::lock_guard lock(mutex_);
    next_enemy_base_ = main_base;
    next_enemies_ = std::move(copies);
}

void Director::SetDrops(std::uint64_t main_base, std::vector<Copy> copies) {
    std::lock_guard lock(mutex_);
    next_drop_base_ = main_base;
    next_drops_ = std::move(copies);
}

std::map<std::uint64_t, std::uint64_t> Director::Live() const {
    std::lock_guard lock(mutex_);
    return live_;
}

bool Director::Due(const Copy& c, std::uint64_t now_ms) const {
    if (c.act != Act::show || started_.count(c.key) != 0) {
        return false;
    }
    const auto r = remade_.find(c.key);
    if (r == remade_.end()) {
        return true;
    }
    // A copy waiting to be made again: the input in force now decides. A
    // copy killed here is seen gone at once, but Sync sends remakes 0 only
    // after it missed two walks, within the delay.
    return c.remakes != 0 && r->second.count <= c.remakes &&
           now_ms - r->second.gone_ms >= kRemakeDelayMs;
}

const Copy* Director::Wanted(std::uint64_t key) const {
    for (const Copy& c : copies_) {
        if (c.key == key) {
            return &c;
        }
    }
    return nullptr;
}

std::optional<CoopPuppet::Call> Director::Step(CoopPuppet::Guest& guest, bool after_call,
                                               std::uint64_t result, std::uint64_t now_ms) {
    namespace addr = CoopPuppet::addr;
    if (!after_call) {
        NewFrame(guest, now_ms);
    } else if (pending_ != Pending::none) {
        Slot& slot = slots_[pending_slot_];
        const std::uint64_t handle = SlotBase(pending_slot_) + 0x110;
        switch (pending_) {
        case Pending::malloc:
            allocating_ = false;
            if (result == 0) {
                retry_ms_ = now_ms + 5000;
                Say("malloc gave nothing; again in 5 s");
            } else {
                const std::vector<std::uint8_t> zero(kBlockSize, 0);
                guest.Write(result, zero.data(), zero.size());
                block_ = result;
                Say("block at " + Hex(result));
            }
            break;
        case Pending::take:
            WriteValue(guest, handle, std::uint64_t{0});
            if (result == 0 || !ReadValue(guest, result, slot.vtable)) {
                Say(slot.actor + ": request gave no actor");
                slot.phase = Phase::done;
            } else {
                slot.taken = result;
                slot.phase = Phase::live;
                if (slot.stand_in) {
                    stand_in_fails_[slot.copy_of] = 0;
                }
                Say(slot.actor + ": copy " + std::to_string(slot.key) + " is actor " + Hex(result));
            }
            slot.request = 0;
            break;
        case Pending::drop:
            WriteValue(guest, handle, std::uint64_t{0});
            slot.phase = Phase::done;
            Say(slot.actor + ": request dropped");
            break;
        default:
            break;
        }
        pending_ = Pending::none;
    }
    if (base_ == 0) {
        return std::nullopt;
    }
    if (calls_ < kMaxCallsPerFrame) {
        if (auto call = Next(guest, now_ms)) {
            ++calls_;
            return call;
        }
    }
    if (auto play = NextPlay(guest)) {
        return play;
    }
    return NextSetter(guest);
}

std::string_view StandIn(std::string_view actor) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kStandIns{{
        {"Shootable_DekuSeed", "CoopShot_Seed"},
        {"NormalArrow", "CoopShot_Arrow"},
        {"Shootable_Hook", "CoopShot_Hook"},
        {"Shootable_HookLong", "CoopShot_HookLong"},
        {"Weapon_Lure", "CoopShot_Lure"},
        {"Weapon_SinkingLure", "CoopShot_SinkingLure"},
        // Issue 85: the game's own boomerang made here homes on our enemies
        // and hits them; the stand-in is its model only.
        {"Weapon_Boomerang", "CoopShot_Boomerang"},
    }};
    for (const auto& [shared, stand_in] : kStandIns) {
        if (shared == actor) {
            return stand_in;
        }
    }
    return {};
}

bool StandInOnly(std::string_view actor) {
    return CoopProjectiles::IsLure(actor);
}

std::string_view HeldStandIn(std::string_view actor) {
    return actor == "Item_Bomb" ? std::string_view("CoopShot_Bomb") : std::string_view{};
}

std::set<std::string> StandInsIn(std::string_view manifest) {
    std::set<std::string> out;
    for (const std::string_view shared :
         {"Shootable_DekuSeed", "NormalArrow", "Shootable_Hook", "Shootable_HookLong", "Weapon_Lure",
          "Weapon_SinkingLure", "Weapon_Boomerang"}) {
        const std::string quoted = "\"" + std::string(StandIn(shared)) + "\"";
        if (manifest.find(quoted) != std::string_view::npos) {
            out.insert(std::string(StandIn(shared)));
        }
    }
    const std::string_view bomb = HeldStandIn("Item_Bomb");
    if (manifest.find("\"" + std::string(bomb) + "\"") != std::string_view::npos) {
        out.insert(std::string(bomb));
    }
    // Issue 166: the impact actors (its "impacts").
    for (const auto kind : {CoopProjectiles::Kind::seed, CoopProjectiles::Kind::arrow}) {
        for (const std::string_view name : CoopProjectiles::ImpactActors(kind)) {
            if (manifest.find("\"" + std::string(name) + "\"") != std::string_view::npos) {
                out.insert(std::string(name));
            }
        }
    }
    return out;
}

void Director::SetStandIns(std::set<std::string> names) {
    std::lock_guard lock(mutex_);
    stand_ins_ = std::move(names);
}

std::optional<CoopPuppet::Call> Director::NextSetter(CoopPuppet::Guest& guest) {
    // The vtable slot ExecuteActorForceSetMtx calls after it writes the
    // place (docs/objects.md): 0x4c6580 for every actor seen so far.
    constexpr std::uint64_t kSetterSlot = 0x218;
    constexpr std::uint64_t kMainSpan = 0x8000000;
    while (!setters_.empty()) {
        const Slot& slot = slots_[setters_.back()];
        setters_.pop_back();
        std::uint64_t vtable = 0, function = 0;
        if (slot.phase != Phase::live || !ReadValue(guest, slot.taken, vtable) ||
            vtable != slot.vtable || !ReadValue(guest, vtable + kSetterSlot, function) ||
            function < base_ || function >= base_ + kMainSpan) {
            continue;
        }
        CoopPuppet::Call c;
        c.function = function;
        c.x[0] = slot.taken;
        c.x[1] = 0;
        return c;
    }
    return std::nullopt;
}

std::optional<CoopPuppet::Call> Director::NextPlay(CoopPuppet::Guest& guest) {
    // The AS controller and slot 0 as the objects' mover reads them
    // (docs/puppet.md "Animation"); the play request for kPlayAs: +0 actor,
    // +8 name, +0x10 slot, +0x14 rate, +0x18 and +0x1c -1.0, +0x20
    // skip-if-same, +0x28 node. The request goes in the slot's create
    // argument (free once the copy is live), the name in its own cell after
    // the slots (kNameCell).
    namespace addr = CoopPuppet::addr;
    constexpr std::size_t kNameMax = kNameCell - 1;
    constexpr float kFrameSlack = 3.0f;
    while (!plays_.empty()) {
        const std::size_t i = plays_.back();
        plays_.pop_back();
        const Slot& slot = slots_[i];
        const Copy* c = Wanted(slot.key);
        if (slot.phase != Phase::live || c == nullptr || !c->animation ||
            c->animation->name.empty() || c->animation->name.size() > kNameMax) {
            continue;
        }
        std::uint64_t at = slot.taken;
        for (const std::uint64_t step : addr::kControllerPath) {
            if (!ReadValue(guest, at + step, at) || at == 0) {
                at = 0;
                break;
            }
        }
        const std::uint64_t controller = at;
        std::int32_t count = 0;
        std::uint64_t array = 0, slot0 = 0, name_at = 0;
        if (controller == 0 || !ReadValue(guest, controller + addr::kSlotCount, count) || count < 1 ||
            !ReadValue(guest, controller + addr::kSlots, array) || array == 0 ||
            !ReadValue(guest, array, slot0) || slot0 == 0) {
            continue;  // no animations (a stand-in without them)
        }
        std::array<char, 64> text{};
        std::string playing;
        if (ReadValue(guest, slot0 + addr::kSlotName, name_at) && name_at != 0 &&
            guest.Read(name_at, text.data(), text.size() - 1)) {
            playing = text.data();
        }
        const auto& want = *c->animation;
        if (playing != want.name) {
            const std::uint64_t b = SlotBase(i);
            auto cell = name_cells_.find(want.name);
            if (cell == name_cells_.end()) {
                if (name_cells_.size() >= kNameCells) {
                    continue;  // every cell taken: not played
                }
                std::array<char, kNameCell> name{};
                std::memcpy(name.data(), want.name.data(), want.name.size());
                const std::size_t index = name_cells_.size();
                if (!guest.Write(block_ + kSlots * kSlotSize + index * kNameCell, name.data(),
                                 name.size())) {
                    continue;
                }
                cell = name_cells_.emplace(want.name, index).first;
            }
            std::array<std::uint8_t, 0x30> request{};
            const float unset = -1.0f;
            const std::uint64_t name_ptr = block_ + kSlots * kSlotSize + cell->second * kNameCell;
            std::memcpy(request.data() + 0x00, &slot.taken, 8);
            std::memcpy(request.data() + 0x08, &name_ptr, 8);
            std::memcpy(request.data() + 0x14, &want.rate, 4);
            std::memcpy(request.data() + 0x18, &unset, 4);
            std::memcpy(request.data() + 0x1c, &unset, 4);
            if (!guest.Write(b, request.data(), request.size())) {
                continue;
            }
            CoopPuppet::Call call;
            call.function = base_ + addr::kPlayAs;
            call.x[0] = b;
            return call;
        }
        float frame = 0.0f;
        if (ReadValue(guest, slot0 + addr::kSlotFrame, frame) && std::isfinite(frame) &&
            std::fabs(frame - want.frame) > kFrameSlack) {
            CoopPuppet::Call call;
            call.function = base_ + addr::kSetAsFrame;
            call.x[0] = controller;
            call.x[1] = 0;  // slot 0
            call.s0 = want.frame;
            return call;
        }
    }
    return std::nullopt;
}

void Director::NewFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    namespace addr = CoopPuppet::addr;
    calls_ = 0;
    setters_.clear();
    plays_.clear();
    if (pending_ == Pending::malloc) {
        allocating_ = false;  // the cave did not make the call
    } else if (pending_ == Pending::create) {
        started_.erase(slots_[pending_slot_].key);
        slots_[pending_slot_].key = 0;
    }
    pending_ = Pending::none;
    std::uint64_t base = 0;
    {
        std::lock_guard lock(mutex_);
        base = next_base_ != 0 ? next_base_ : next_enemy_base_ != 0 ? next_enemy_base_ : next_drop_base_;
        copies_ = next_;
        copies_.insert(copies_.end(), next_enemies_.begin(), next_enemies_.end());
        copies_.insert(copies_.end(), next_drops_.begin(), next_drops_.end());
    }
    if (base != base_) {
        // A new game (or none): nothing we had is ours any more.
        base_ = base;
        block_ = 0;
        allocating_ = false;
        started_.clear();
        remade_.clear();
        slots_.assign(kSlots, Slot{});
        name_cells_.clear();
    }
    std::erase_if(remade_, [this](const auto& e) { return Wanted(e.first) == nullptr; });
    std::map<std::uint64_t, std::uint64_t> live;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.key == 0) {
            continue;
        }
        const std::uint64_t handle = SlotBase(i) + 0x110;
        if (slot.phase == Phase::creating) {
            std::uint64_t request = 0;
            std::uint32_t word = 0;
            if (slot.request == 0 && ReadValue(guest, handle, request) && request != 0 &&
                ReadValue(guest, request, word)) {
                const std::uint32_t state = word & 0xffff;
                if (state == addr::kRequestReady || state == addr::kRequestDrop) {
                    slot.request = request;
                } else if (state == 5 || state == 6) {
                    WriteValue(guest, handle, std::uint64_t{0});
                    Say(slot.actor + ": create failed (state " + std::to_string(state) + ")");
                    slot.phase = Phase::done;
                }
            }
            if (slot.phase == Phase::creating && slot.request == 0 &&
                now_ms - slot.since_ms > kCreateTimeoutMs) {
                Say(slot.actor + ": create timed out");
                slot.phase = Phase::done;
            }
        } else if (slot.phase == Phase::live) {
            std::uint64_t vtable = 0;
            std::uint32_t state = 0;
            if (!ReadValue(guest, slot.taken, vtable) || vtable != slot.vtable ||
                !ReadValue(guest, slot.taken + addr::kActorState, state) || Gone(state)) {
                slot.phase = Phase::done;  // it went by itself (a fuse, a hit)
                if (const Copy* c = Wanted(slot.key);
                    c != nullptr && c->act == Act::show && c->remakes != 0) {
                    Remade& r = remade_[slot.key];
                    if (r.count < c->remakes) {
                        ++r.count;
                        r.gone_ms = now_ms;
                        started_.erase(slot.key);
                        Say(slot.actor + ": copy " + std::to_string(slot.key) +
                            " went by itself; made again (" + std::to_string(r.count) + " of " +
                            std::to_string(c->remakes) + ")");
                    } else {
                        Say(slot.actor + ": copy " + std::to_string(slot.key) +
                            " went by itself; not made again (made " + std::to_string(r.count) +
                            " times already)");
                    }
                }
            } else if (const Copy* c = Wanted(slot.key); c != nullptr && c->act != Act::remove && c->place) {
                if (c->rotation) {
                    // Place and turn are one block at +0x31c, as the Mover writes them.
                    std::array<float, 12> transform{};
                    std::memcpy(transform.data(), c->position.data(), 12);
                    std::memcpy(transform.data() + 3, c->rotation->data(), 36);
                    guest.Write(slot.taken + addr::kActorPosition, transform.data(), 48);
                } else {
                    WriteValue(guest, slot.taken + addr::kActorPosition, c->position);
                }
                if (slot.stand_in) {
                    // No physics to hand the place to: the engine's pending
                    // transform too, as the puppet is placed (CoopPuppet::Director::Pose).
                    WriteValue(guest, slot.taken + addr::kActorPositionRequest, c->position);
                    if (c->rotation) {
                        WriteValue(guest, slot.taken + addr::kActorRotationRequest, *c->rotation);
                    }
                }
                if (c->setter && c->act == Act::show && !slot.stand_in) {
                    setters_.push_back(i);
                }
                if (c->animation && c->act == Act::show && !c->animation->name.empty()) {
                    plays_.push_back(i);  // issue 169: the fuse's blink
                }
                if (c->act == Act::explode && !slot.exploded) {
                    // A bomb's life is 1; at 0 the game runs its own explosion.
                    std::uint64_t table = 0, life = 0, value = 0;
                    if (ReadValue(guest, slot.taken + kActorComponents, table) && table != 0 &&
                        ReadValue(guest, table + kComponentLife, life) && life != 0 &&
                        ReadValue(guest, life + kLifeValue, value) && value != 0) {
                        WriteValue(guest, value + 8, std::int32_t{0});
                        Say(slot.actor + ": copy " + std::to_string(slot.key) + " exploded");
                    }
                    slot.exploded = true;
                    slot.since_ms = now_ms;
                }
                if (slot.exploded && now_ms - slot.since_ms > 3000) {
                    slot.phase = Phase::done;  // it should be gone by now; let go
                }
            }
        }
        if (slot.phase == Phase::done) {
            if (slot.stand_in && slot.taken == 0) {
                // A stand-in that never became an actor (create failed,
                // timed out or dropped, or the take gave none): the copy is
                // made again; after kStandInTries in a row, as the game's
                // own actor for the rest of the session.
                started_.erase(slot.key);
                if (++stand_in_fails_[slot.copy_of] >= kStandInTries &&
                    no_stand_in_.insert(slot.copy_of).second) {
                    Say(slot.copy_of + ": " + slot.actor + " could not be made " +
                        std::to_string(kStandInTries) + " times in a row; the game's own actor is used");
                }
            }
            slot = Slot{};
            continue;
        }
        if (slot.phase == Phase::live) {
            live[slot.key] = slot.taken;
        }
    }
    std::lock_guard lock(mutex_);
    live_ = std::move(live);
}

std::optional<CoopPuppet::Call> Director::Next(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    namespace addr = CoopPuppet::addr;
    const bool any_new = std::any_of(copies_.begin(), copies_.end(), [&](const Copy& c) {
        return Due(c, now_ms);
    });
    if (block_ == 0) {
        if (!any_new || allocating_ || now_ms < retry_ms_) {
            return std::nullopt;
        }
        CoopPuppet::Call c;
        c.function = base_ + addr::kMalloc;
        c.x[0] = kBlockSize;
        allocating_ = true;
        pending_ = Pending::malloc;
        return c;
    }
    // Takes, drops and deletes first.
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.key == 0) {
            continue;
        }
        if (slot.phase == Phase::creating && slot.request != 0) {
            std::uint32_t word = 0;
            ReadValue(guest, slot.request, word);
            CoopPuppet::Call c;
            c.x[0] = slot.request;
            if ((word & 0xffff) == addr::kRequestReady) {
                c.function = base_ + addr::kTakeCreated;
                c.x[1] = 1;
                pending_ = Pending::take;
            } else {
                c.function = base_ + addr::kDropCreate;
                pending_ = Pending::drop;
            }
            pending_slot_ = i;
            return c;
        }
        if (slot.phase == Phase::live) {
            const Copy* want = Wanted(slot.key);
            if (want != nullptr && want->act == Act::release) {
                // Its maker has the actor now: the slot is free, the actor stays.
                {
                    std::lock_guard lock(mutex_);
                    live_.erase(slot.key);
                }
                slot = Slot{};
                continue;
            }
            if (want == nullptr || want->act == Act::remove) {
                CoopPuppet::Call c;
                c.function = base_ + addr::kRequestDelete;
                c.x[0] = slot.taken;
                c.x[1] = 0;
                Say(slot.actor + ": copy " + std::to_string(slot.key) + " deleted");
                {
                    std::lock_guard lock(mutex_);  // Sync must not take it for live
                    live_.erase(slot.key);
                }
                slot = Slot{};
                pending_ = Pending::remove;
                pending_slot_ = i;
                return c;
            }
        }
    }
    std::uint64_t manager = 0, create_default = 0;
    if (!any_new || !ReadValue(guest, base_ + addr::kActorManagerPtr, manager) || manager == 0 ||
        !ReadValue(guest, base_ + addr::kGotCreateDefault, create_default)) {
        return std::nullopt;
    }
    for (const Copy& copy : copies_) {
        if (!Due(copy, now_ms)) {
            continue;
        }
        if (CoopProjectiles::IsImpact(copy.actor)) {
            // Issue 166: ours only; a pack without them never gets the create.
            std::lock_guard lock(mutex_);
            if (stand_ins_.count(copy.actor) == 0) {
                started_.insert(copy.key);
                continue;
            }
        }
        if (StandInOnly(copy.actor)) {
            // Never the game's own lure: a fish here would go for it.
            std::lock_guard lock(mutex_);
            if (no_stand_in_.count(copy.actor) != 0) {
                started_.insert(copy.key);
                continue;
            }
            if (stand_ins_.count(std::string(StandIn(copy.actor))) == 0) {
                // The pack's stand-in list may not be known yet: wait, and
                // make it once it is (only when the list says none, drop it).
                if (!stand_ins_.empty()) {
                    started_.insert(copy.key);
                }
                continue;
            }
        }
        const auto free = std::find_if(slots_.begin(), slots_.end(),
                                       [](const Slot& s) { return s.key == 0; });
        if (free == slots_.end()) {
            return std::nullopt;  // all busy: next frame
        }
        const std::size_t i = std::size_t(free - slots_.begin());
        const std::uint64_t b = SlotBase(i);
        // The create argument as the puppet director fills it (docs/puppet.md).
        std::array<std::uint8_t, 0x100> arg{};
        const std::array<float, 9> rotation =
            copy.rotation.value_or(std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1});
        const Vec3 scale{1.0f, 1.0f, 1.0f};
        std::memcpy(arg.data() + 0x00, copy.position.data(), 12);
        std::memcpy(arg.data() + 0x0c, rotation.data(), 36);
        std::memcpy(arg.data() + 0x30, scale.data(), 12);
        std::memcpy(arg.data() + 0x40, &create_default, 8);
        arg[0x98] = 1;
        std::string_view stand_in = no_stand_in_.count(copy.actor) != 0 ? std::string_view{}
                                    : copy.held ? HeldStandIn(copy.actor)
                                                : StandIn(copy.actor);
        if (!stand_in.empty()) {
            std::lock_guard lock(mutex_);
            if (stand_ins_.count(std::string(stand_in)) == 0) {
                stand_in = {};  // the pack has none: never ask for an unknown actor
            }
        }
        const std::string name = stand_in.empty() ? copy.actor : std::string(stand_in);
        std::array<char, 0x40> text{};
        std::memcpy(text.data(), name.data(), std::min<std::size_t>(name.size(), 0x3f));
        if (!guest.Write(b, arg.data(), arg.size()) ||
            !guest.Write(b + 0x118, text.data(), text.size()) ||
            !WriteValue(guest, b + 0x100, b + 0x118) ||
            !WriteValue(guest, b + 0x108, std::uint64_t{0x40}) ||
            !WriteValue(guest, b + 0x110, std::uint64_t{0})) {
            return std::nullopt;
        }
        started_.insert(copy.key);
        *free = Slot{};
        free->key = copy.key;
        free->actor = name;
        free->copy_of = copy.actor;
        free->stand_in = !stand_in.empty();
        free->phase = Phase::creating;
        free->since_ms = now_ms;
        CoopPuppet::Call c;
        c.function = base_ + addr::kCreate;
        c.x[0] = manager;
        c.x[1] = b + 0x100;
        c.x[2] = b;
        c.x[3] = b + 0x110;
        c.x[4] = 1;
        pending_ = Pending::create;
        pending_slot_ = i;
        Say(name + ": create copy " + std::to_string(copy.key));
        return c;
    }
    return std::nullopt;
}

#ifndef LICHII_COOP

Director& TheDirector() {
    static Director director;
    return director;
}

} // namespace CoopSpawn

#else  // the hook, built into the emulator

} // namespace CoopSpawn

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/player_reader.hpp"
#include "coop_flags.h"  // EmulatorMemory
#include "coop_svc.h"

namespace CoopSpawn {
namespace {

constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr std::array<u8, 8> kGameBuildId{0xe0, 0xf3, 0xcc, 0xfa, 0xf5, 0x00, 0x65, 0xae};
// The live actor list, as coop_objects.cpp walks it (docs/objects.md).
constexpr u64 kManagerLists = 0x118;
constexpr u64 kListHead = 0x670;
constexpr u64 kListCount = 0x10;
constexpr u64 kNodeOwner = 0x10;
constexpr u64 kActorNode = 0x538;
constexpr u64 kActorName = 0x260;
constexpr u64 kAsSlotRate = 0xd4;  // AS slot rate f32 (docs/puppet.md "Animation")
constexpr std::size_t kMaxWalk = 8192;

bool On() {
    static const bool on = [] {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&value, &size, "LICHII_COOP_SPAWN") != 0 || value == nullptr) {
            return false;
        }
        const bool set = value[0] != '\0' && std::string_view(value) != "0";
        std::free(value);
        return set;
#else
        const char* v = std::getenv("LICHII_COOP_SPAWN");
        return v != nullptr && v[0] != '\0' && std::string_view(v) != "0";
#endif
    }();
    return on;
}

class Glue {
public:
    void Frame(Core::System& system, coop::host::Session* session,
               const std::optional<std::string>& scene, u64 now_us) {
        if (session == nullptr || !scene || scene->empty() || now_us == 0 || !Ready(system)) {
            if (active_) {
                sync_.Reset();
                CoopProjectiles::TheLures().Set({});
                TheDirector().SetInput(0, {});
                active_ = false;
            }
            return;
        }
        auto& memory = system.ApplicationMemory();
        std::vector<Seen> seen;
        if (!Walk(memory, seen)) {
            return;  // the list changed under us
        }
        const u8 self =
            session->with_client([](coop::net::Client& c) { return c.player_number(); });
        if (self == 0) {
            return;
        }
        active_ = true;
        Shots(memory, now_us);
        std::vector<coop::protocol::Message> send;
        std::vector<Copy> copies;
        {
            // Issue 169: our Link's place tells which of our bombs is held.
            CoopHook::EmulatorMemory game(memory, main_);
            sync_.SetLink(coop::host::PlayerReader(game).read_position());
        }
        sync_.Tick(*scene, self, now_us, seen, TheDirector().Live(), send, copies);
        if (const auto held = std::make_pair(sync_.HeldMine(), sync_.HeldCopies()); held != logged_held_) {
            LOG_INFO(Core, "coop: shots: bombs held: {} of ours, {} copies in other players' hands",
                     held.first, held.second);
            logged_held_ = held;
        }
        for (const auto& f : sync_.MadeUpFlights()) {
            LOG_INFO(Core,
                     "coop: shots: our {} #{:#x} did not move in its place after the shot: its "
                     "flight is sent from the shot, from {:.2f} {:.2f} {:.2f} at {:.1f} {:.1f} "
                     "{:.1f} m/s",
                     f.name, f.id.value, f.from[0], f.from[1], f.from[2], f.velocity[0],
                     f.velocity[1], f.velocity[2]);
        }
        CoopProjectiles::TheLures().Set(sync_.OwnLures());  // issue 151, for the objects sync
        TheDirector().SetInput(main_, std::move(copies));
        if (const auto fuse = sync_.Fuse().Fuse(); fuse && fuse != logged_fuse_) {
            LOG_INFO(Core, "coop: shots: our bombs' fuse is {:.2f} s", double(*fuse) / 1e6);
            logged_fuse_ = fuse;
        }
        if (sync_.Landed() != logged_landed_) {
            logged_landed_ = sync_.Landed();
            LOG_INFO(Core, "coop: shots: copy held where the owner's shot hit ({} so far)",
                     logged_landed_);
        }
        if (sync_.Impacts() != logged_impacts_) {
            logged_impacts_ = sync_.Impacts();
            LOG_INFO(Core, "coop: shots: impact actors asked for where copies landed ({} so far)",
                     logged_impacts_);
        }
        if (sync_.BlownOnTime() != logged_blown_) {
            logged_blown_ = sync_.BlownOnTime();
            LOG_INFO(Core, "coop: shots: bomb copy blown on the owner's fuse ({} so far)",
                     logged_blown_);
        }
        for (const auto& f : sync_.Flights()) {
            LOG_INFO(Core,
                     "coop: shots: our {} #{:#x} gone (reason {}) after {} ms: went {:.2f} m from "
                     "where it was made, top speed {:.1f} m/s{}, bounced {} times",
                     f.name, f.id.value, static_cast<int>(f.reason), f.lived_us / 1000, f.far,
                     f.top_speed,
                     f.made_up ? ", shot (flight made up)" : f.shot ? ", shot" : "", f.bounces);
        }
        if (send.empty()) {
            return;
        }
        session->with_client([&](coop::net::Client& c) {
            for (const auto& m : send) {
                const bool sent = c.send(m);
                if (const auto* s = std::get_if<coop::protocol::ObjectSpawn>(&m)) {
                    const Vec3 at = s->state.position.value_or(Vec3{});
                    LOG_INFO(Core, "coop: spawn: sent {} #{:#x} at {:.2f} {:.2f} {:.2f}{} fuse {:.2f}{}",
                             s->actor, s->id.value, at[0], at[1], at[2],
                             s->state.rotation ? " turned" : "", s->state.fuse.value_or(-1.0f),
                             sent ? "" : " (not sent)");
                } else if (const auto* r = std::get_if<coop::protocol::ObjectRemove>(&m)) {
                    LOG_INFO(Core, "coop: spawn: sent remove #{:#x} reason {}{}", r->id.value,
                             static_cast<int>(r->reason), sent ? "" : " (not sent)");
                }
            }
        });
    }

    void Receive(const coop::protocol::Message& m) {
        if (const auto* s = std::get_if<coop::protocol::ObjectSpawn>(&m)) {
            LOG_INFO(Core, "coop: spawn: got {} #{:#x} from P{}", s->actor, s->id.value,
                     s->player_number);
        } else if (const auto* r = std::get_if<coop::protocol::ObjectRemove>(&m)) {
            LOG_INFO(Core, "coop: spawn: got remove #{:#x} reason {}", r->id.value,
                     static_cast<int>(r->reason));
        }
        sync_.Receive(m);
    }

private:
    // Issue 158: a seed or an arrow let go now, from its ammo count.
    void Shots(Core::Memory::Memory& memory, u64 now_us) {
        CoopHook::EmulatorMemory flag_memory(memory, main_);
        coop::host::GameFlags flags(flag_memory);
        using CoopProjectiles::Kind;
        for (const auto& [kind, actor] : {std::pair{Kind::seed, "Shootable_DekuSeed"},
                                          std::pair{Kind::arrow, "NormalArrow"}}) {
            const auto count = flags.get_int(CoopProjectiles::AmmoFlag(kind));
            if (ammo_.Dropped(kind, count)) {
                const bool found = sync_.Shot(actor, now_us);
                LOG_INFO(Core, "coop: shots: our {} let go ({} left){}", actor, count.value_or(-1),
                         found ? "" : ", none of ours nocked on the list");
            }
        }
    }

    static std::optional<std::string> Env(const char* name) {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
            return std::nullopt;
        }
        std::string out(value);
        std::free(value);
        return out;
#else
        const char* v = std::getenv(name);
        return v ? std::optional<std::string>(v) : std::nullopt;
#endif
    }

    // LICHII_COOP_SHOT_AXIS (0 to 5, default 2) and LICHII_COOP_SHOT_SPEED
    // (m/s): how a made-up flight goes (Sync::SetShots).
    static Sync::ShotSettings ShotSettingsFromEnv() {
        Sync::ShotSettings s;
        if (const auto axis = Env("LICHII_COOP_SHOT_AXIS");
            axis && axis->size() == 1 && (*axis)[0] >= '0' && (*axis)[0] <= '5') {
            s.axis = (*axis)[0] - '0';
        }
        if (const auto speed = Env("LICHII_COOP_SHOT_SPEED")) {
            char* end = nullptr;
            const float v = std::strtof(speed->c_str(), &end);
            if (end != speed->c_str() && v > 0.0f && v < 500.0f) {
                s.speed = v;
            }
        }
        return s;
    }

    bool Ready(Core::System& system) {
        auto* process = system.ApplicationProcess();
        if (process == nullptr || system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            return false;
        }
        const auto& build = system.GetApplicationProcessBuildID();
        if (std::memcmp(build.data(), kGameBuildId.data(), kGameBuildId.size()) != 0) {
            return false;
        }
        if (process != process_) {
            process_ = process;
            main_ = FindMain(system, process);
            names_.clear();
            sync_.Reset();
            sync_.SetSetter(SetterOn());
            sync_.SetShots(ShotSettingsFromEnv());
            sync_.SetImpacts(Env("LICHII_COOP_SHOT_IMPACTS").value_or("1") != "0");
            ammo_.Reset();
            LOG_INFO(Core, "coop: spawn: on, main at {:#x}, projectile setter {}", main_,
                     SetterOn() ? "on" : "off");
        }
        return main_ != 0;
    }

    static u64 FindMain(Core::System& system, Kernel::KProcess* process) {
        Loader::AppLoader::Modules modules;
        if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
            modules.empty()) {
            modules = Core::FindModules(process);
        }
        for (const auto& [address, name] : modules) {
            if (name == "main" || name.starts_with("main.")) {
                return address;
            }
        }
        return GetInteger(Core::FindMainModuleEntrypoint(process));
    }

    template <typename T>
    static std::optional<T> Get(Core::Memory::Memory& memory, u64 address) {
        if (address < 0x1000 || !memory.IsValidVirtualAddressRange(address, sizeof(T))) {
            return std::nullopt;
        }
        T value{};
        memory.ReadBlockUnsafe(address, &value, sizeof(T));
        return value;
    }

    static std::string Name(Core::Memory::Memory& memory, u64 actor) {
        const auto p = Get<u64>(memory, actor + kActorName);
        std::array<char, 64> buf{};
        if (!p || !memory.IsValidVirtualAddressRange(*p, buf.size())) {
            return "?";
        }
        memory.ReadBlockUnsafe(*p, buf.data(), buf.size() - 1);
        return std::string(buf.data(), strnlen(buf.data(), buf.size()));
    }

    // Issue 169: an actor's AS slot 0 (command, frame, rate), as the objects
    // sync reads an enemy's (docs/puppet.md "Animation").
    static std::optional<coop::protocol::Animation> Slot0(Core::Memory::Memory& memory, u64 actor) {
        namespace addr = CoopPuppet::addr;
        u64 at = actor;
        for (const u64 step : addr::kControllerPath) {
            const auto next = Get<u64>(memory, at + step);
            if (!next || *next == 0) {
                return std::nullopt;
            }
            at = *next;
        }
        const auto count = Get<s32>(memory, at + addr::kSlotCount);
        const auto array = Get<u64>(memory, at + addr::kSlots);
        const auto slot = array && *array != 0 ? Get<u64>(memory, *array) : std::nullopt;
        if (!count || *count < 1 || !slot || *slot == 0) {
            return std::nullopt;
        }
        const auto text = Get<u64>(memory, *slot + addr::kSlotName);
        const auto frame = Get<float>(memory, *slot + addr::kSlotFrame);
        const auto rate = Get<float>(memory, *slot + kAsSlotRate);
        std::array<char, 64> buf{};
        if (!text || *text == 0 || !frame || !rate || !std::isfinite(*frame) || !std::isfinite(*rate) ||
            !memory.IsValidVirtualAddressRange(*text, buf.size())) {
            return std::nullopt;
        }
        memory.ReadBlockUnsafe(*text, buf.data(), buf.size() - 1);
        coop::protocol::Animation a;
        a.name = std::string(buf.data(), strnlen(buf.data(), buf.size()));
        if (a.name.empty() || a.name.size() > coop::protocol::kMaxAnimationChars) {
            return std::nullopt;
        }
        a.frame = *frame;
        a.rate = *rate;
        return a;
    }

    // The shared made actors on the list. False when the list changed under us.
    bool Walk(Core::Memory::Memory& memory, std::vector<Seen>& out) {
        const auto manager = Get<u64>(memory, main_ + CoopPuppet::addr::kActorManagerPtr);
        const auto lists =
            manager && *manager != 0 ? Get<u64>(memory, *manager + kManagerLists) : std::nullopt;
        if (!lists || *lists == 0) {
            return false;
        }
        const u64 head = *lists + kListHead;
        const auto count = Get<u32>(memory, head + kListCount);
        if (!count) {
            return false;
        }
        std::unordered_map<u64, std::string> names;
        std::size_t walked = 0;
        auto node = Get<u64>(memory, head);
        while (node && *node != 0 && *node != head && walked < kMaxWalk) {
            ++walked;
            const auto owner = Get<u64>(memory, *node + kNodeOwner);
            if (owner && *owner + kActorNode == *node) {
                const u64 a = *owner;
                auto it = names_.find(a);
                std::string name = it != names_.end() ? it->second : Name(memory, a);
                if (IsShared(name)) {
                    Seen s;
                    s.address = a;
                    s.name = name;
                    for (std::size_t i = 0; i < 3; ++i) {
                        s.position[i] =
                            Get<float>(memory, a + CoopPuppet::addr::kActorPosition + 4 * i)
                                .value_or(0.0f);
                    }
                    if (const u64 r = a + CoopPuppet::addr::kActorRotation;
                        memory.IsValidVirtualAddressRange(r, sizeof(CoopPuppet::Mat3))) {
                        CoopPuppet::Mat3 m{};
                        memory.ReadBlockUnsafe(r, m.data(), sizeof(m));
                        s.rotation = m;
                    }
                    if (Animated(s.name)) {
                        // Issue 169: the fuse's blink; issue 85: the hook's chain.
                        s.animation = Slot0(memory, a);
                    }
                    out.push_back(std::move(s));
                }
                names.emplace(a, std::move(name));
            }
            node = Get<u64>(memory, *node);
        }
        if (!node || walked != *count) {
            return false;
        }
        names_ = std::move(names);
        return true;
    }

    static bool SetterOn() {
        static const bool on = [] {
#ifdef _WIN32
            char* value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, "LICHII_COOP_SHOTS_SETTER") != 0 || value == nullptr) {
                return true;
            }
            const bool off = std::string_view(value) == "0";
            std::free(value);
            return !off;
#else
            const char* v = std::getenv("LICHII_COOP_SHOTS_SETTER");
            return v == nullptr || std::string_view(v) != "0";
#endif
        }();
        return on;
    }

    Sync sync_;
    CoopProjectiles::AmmoWatch ammo_;
    std::optional<std::uint64_t> logged_fuse_;
    std::size_t logged_blown_ = 0;
    std::size_t logged_landed_ = 0;
    std::size_t logged_impacts_ = 0;
    std::pair<std::size_t, std::size_t> logged_held_{0, 0};
    bool active_ = false;
    Kernel::KProcess* process_ = nullptr;
    u64 main_ = 0;
    std::unordered_map<u64, std::string> names_;
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

} // namespace

Director& TheDirector() {
    static Director director(
        [](const std::string& text) { LOG_INFO(Core, "coop: {}", text); });
    return director;
}

// The stand-ins the installed puppet pack has (issue 85): every mod folder's
// coop_puppet.json under load/<title id>. None found: no stand-in is asked for.
std::set<std::string> PackStandIns() {
    std::set<std::string> names;
    std::error_code ec;
    const auto title = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400486024DEC000";
    for (const auto& mod : std::filesystem::directory_iterator(title, ec)) {
        std::ifstream in(mod.path() / "coop_puppet.json", std::ios::binary);
        if (!in) {
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        names.merge(StandInsIn(text));
    }
    return names;
}

void UseDirector() {
    static const bool registered = [] {
        auto names = PackStandIns();
        std::string list;
        for (const auto& n : names) {
            list += (list.empty() ? "" : ", ") + n;
        }
        LOG_INFO(Core, "coop: spawn: stand-ins in the puppet pack: {}",
                 list.empty() ? "none (copies are the game's own actors; rebuild the puppet pack)" : list);
        TheDirector().SetStandIns(std::move(names));
        CoopSvc::AddCaller(10, [](CoopPuppet::Guest& guest, bool after_call,
                                  std::uint64_t result, std::uint64_t now_ms) {
            return TheDirector().Step(guest, after_call, result, now_ms);
        });
        return true;
    }();
    (void)registered;
}

void Frame(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us) {
    if (On()) {
        UseDirector();
        TheGlue().Frame(system, session, scene, server_now_us);
    }
}

void OnEvent(const coop::net::Event& event) {
    if (On() && event.kind == coop::net::EventKind::message) {
        TheGlue().Receive(event.message);
    }
}

} // namespace CoopSpawn

#endif  // LICHII_COOP
