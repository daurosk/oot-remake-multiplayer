// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "coop/protocol/codec.hpp"

namespace CoopWorld {

namespace {

// Every Obj_ pack of build e0f3ccfaf50065ae that has LifeRef and
// DestructionRef (read from the romfs, 2026-11-09), by name part. Left out:
// the chest Obj_TBox_A_04 (opening a chest is a flag, flags.md), shop
// targets and moving obstacles (minigames), icicles and hammer pillars
// (they fall or tilt rather than break), Obj_Well_CandleStick_A_03.
constexpr std::array<std::string_view, 6> kPrefixes{
    "Obj_Pot_",          // pots, the respawning and dream ones too
    "Obj_BrokenBox_",    // crates
    "Obj_LiftRock_",     // rocks Link lifts and throws
    "Obj_BrokenRock_",   // rocks a bomb or hammer breaks
    "Obj_Plant_Grass_",  // grass
    "Obj_RedIce_",       // red ice (melted with blue fire: it goes, no life)
};
constexpr std::array<std::string_view, 11> kParts{
    "SignBoard_",  "Signboard_",  "GuidePost_",  // cuttable signs, every region's
    "CrackWall_",  "CrackFloor_",     // bombable walls and floors
    "BrokenRock_",                    // Goron and Death Mountain rocks
    "BrokenIcePillar_", "SkullPot_", "ArcheryPot_", "Beehive_", "SpiderWeb_",
};

// The grass that is an actor without an Obj_Plant_Grass_ name: every pack
// with the life of Obj_Plant_DestructibleGrass_Base (a pot's: 1, then gone).
// It has no DestructionRef, so the list above missed it; the 13 tufts by
// the Kokiri side of Hyrule Field are the third. The rest of the field's
// grass is foliage, no actor at all, and is not shared (world.md).
constexpr std::array<std::string_view, 4> kGrass{
    "Obj_Plant_DestructibleGrass_Base",
    "Land_Kokiri_Plant_G_01",
    "Land_kokiri_Plant_A_02_DropFixed_03",
    "Land_Kokiri_Zenmai_A_02",
};

} // namespace

bool IsBreakable(std::string_view name) {
    for (const auto grass : kGrass) {
        if (name == grass) {
            return true;
        }
    }
    if (!name.starts_with("Obj_")) {
        return false;
    }
    for (const auto prefix : kPrefixes) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    for (const auto part : kParts) {
        if (name.find(part) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool IsMovable(std::string_view name) {
    if (!name.starts_with("Obj_") || name.find("BasePosAnchor") != std::string_view::npos) {
        return false;
    }
    return name.find("MovableBox_") != std::string_view::npos ||
           name.starts_with("Obj_SliderBox_") || name.starts_with("Obj_AmosStatue_");
}

bool IsSign(std::string_view name) {
    return name.starts_with("Obj_") &&
           (name.find("SignBoard_") != std::string_view::npos ||
            name.find("Signboard_") != std::string_view::npos ||
            name.find("GuidePost_") != std::string_view::npos);
}

bool IsGrassActor(std::string_view name) {
    if (name.starts_with("Obj_Plant_Grass_")) {
        return true;
    }
    return std::find(kGrass.begin(), kGrass.end(), name) != kGrass.end();
}

namespace {

namespace proto = coop::protocol;

constexpr std::uint64_t kNoHash = ~std::uint64_t{0};
constexpr std::size_t kMaxInbox = 4096;

float Distance(const proto::Vec3& a, const proto::Vec3& b) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool Finite(const proto::Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// The rotation as a unit quaternion the codec takes; none: no turn.
proto::Quat UnitQuat(const std::optional<CoopObjects::Mat3>& m) {
    proto::Quat q{0.0f, 0.0f, 0.0f, 1.0f};
    if (!m) {
        return q;
    }
    const proto::Quat got = CoopObjects::QuatOf(*m);
    const float n =
        std::sqrt(got[0] * got[0] + got[1] * got[1] + got[2] * got[2] + got[3] * got[3]);
    if (!std::isfinite(n) || n < 0.5f || n > 2.0f) {
        return q;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = got[i] / n;
    }
    return q;
}

const std::string* SceneOfMessage(const proto::Message& m) {
    if (const auto* b = std::get_if<proto::WorldBreak>(&m)) {
        return &b->scene;
    }
    if (const auto* v = std::get_if<proto::WorldMove>(&m)) {
        return &v->scene;
    }
    if (const auto* p = std::get_if<proto::WorldPlace>(&m)) {
        return &p->scene;
    }
    if (const auto* h = std::get_if<proto::WorldHit>(&m)) {
        return &h->scene;
    }
    return nullptr;
}

// A log line: "%016llx" is the hash, the first "%s" the name, the second `more`.
std::string Line(std::string_view format, std::uint64_t hash, const std::string& name,
                 std::string_view more) {
    char hex[24];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    std::string out(format);
    if (const auto at = out.find("%016llx"); at != std::string::npos) {
        out.replace(at, 7, hex);
    }
    if (const auto at = out.find("%s"); at != std::string::npos) {
        out.replace(at, 2, name);
        if (const auto next = out.find("%s", at + name.size()); next != std::string::npos) {
            out.replace(next, 2, more);
        }
    }
    return out;
}

} // namespace

void WorldSync::Receive(const proto::Message& message) {
    if (SceneOfMessage(message) == nullptr) {
        return;
    }
    std::lock_guard lock(inbox_mutex_);
    if (inbox_.size() < kMaxInbox) {
        inbox_.push_back(message);
    }
}

void WorldSync::Reset() {
    scene_.clear();
    scene_since_ = 0;
    objects_.clear();
    broken_.clear();
    remote_.clear();
    gone_.clear();
    went_.clear();
    hits_.clear();
    early_.clear();
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

std::uint8_t WorldSync::MoverOf(std::uint64_t hash) const {
    for (const auto& [address, o] : objects_) {
        if (o.hash == hash && o.ours) {
            return self_;
        }
    }
    const auto r = remote_.find(hash);
    return r != remote_.end() && !r->second.samples.empty() ? r->second.player : 0;
}

std::vector<std::uint64_t> WorldSync::Removes() const {
    std::vector<std::uint64_t> out;
    for (const auto& [address, o] : objects_) {
        if (o.remove) {
            out.push_back(address);
        }
    }
    return out;
}

void WorldSync::Drain(std::uint64_t now_us) {
    std::vector<proto::Message> inbox;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
    }
    for (proto::Message& m : inbox) {
        if (*SceneOfMessage(m) != scene_) {
            // The server's replay for a scene can come before our walk has
            // seen that scene: kept until the scene changes.
            if (!std::holds_alternative<proto::WorldMove>(m) && early_.size() < kMaxInbox) {
                early_.push_back(std::move(m));
            }
            continue;
        }
        if (const auto* b = std::get_if<proto::WorldBreak>(&m)) {
            if ((b->flags & proto::world_break::restored) != 0) {
                broken_.erase(b->hash);
                for (auto& [address, o] : objects_) {
                    if (o.hash == b->hash) {
                        o.remove = false;
                        o.life_written_us = 0;
                    }
                }
            } else if (const auto known = broken_.find(b->hash); known == broken_.end()) {
                Break k;
                k.got_us = now_us;
                broken_.emplace(b->hash, k);
            } else if (known->second.local && now_us - known->second.got_us < kEchoUs) {
                // The life 0 we read a moment ago was this player's break
                // (it reached our copy through the enemy sharing first).
                known->second.local = false;
            }
        } else if (const auto* v = std::get_if<proto::WorldMove>(&m)) {
            Remote& r = remote_[v->hash];
            const bool fresh = !r.samples.empty() && now_us - r.got_us < kRemoteHoldUs;
            if (fresh && r.player != v->player_number) {
                if (v->player_number > r.player) {
                    continue;  // two movers: the lower number keeps it
                }
                r.samples.clear();
            }
            if (!fresh) {
                r.samples.clear();
            }
            r.player = v->player_number;
            r.got_us = now_us;
            if (r.samples.empty() || v->time_us > r.samples.back().time_us) {
                r.samples.push_back({v->time_us, v->position, v->rotation});
                if (r.samples.size() > kKeepSamples) {
                    r.samples.pop_front();
                }
            }
        } else if (const auto* h = std::get_if<proto::WorldHit>(&m)) {
            auto& waiting = hits_[h->hash];
            if (waiting.size() < proto::kMaxWorldHitsPerObject) {
                waiting.emplace_back(h->at, h->from);
            }
        } else if (const auto* p = std::get_if<proto::WorldPlace>(&m)) {
            Remote& r = remote_[p->hash];
            r.place = Sample{now_us, p->position, p->rotation};
            ++r.place_serial;
            if (r.samples.empty()) {
                r.player = p->player_number;
            }
        }
    }
}

void WorldSync::LocalBreak(Object& o, std::uint64_t now_us, const char* how, Out& out) {
    if (o.sent_break || broken_.contains(o.hash)) {
        return;
    }
    o.sent_break = true;
    Break k;
    k.got_us = now_us;
    k.local = true;
    broken_.emplace(o.hash, k);
    proto::WorldBreak b;
    b.scene = scene_;
    b.hash = o.hash;
    b.position = Finite(o.position) ? o.position : proto::Vec3{};
    out.send.emplace_back(std::move(b));
    out.notes.push_back(Line("world: %016llx %s broken here (%s): sent", o.hash, o.name, how));
}

void WorldSync::MakeBreak(Object& o, const Break& b, std::uint64_t now_us, Out& out) {
    if (o.remove || b.local) {
        return;  // our own break: the game is doing it
    }
    if (b.got_us <= o.first_us + kStaleUs) {
        // Broken before we ever saw it (we came later, or it loaded later).
        o.remove = true;
        out.notes.push_back(
            Line("world: %016llx %s was broken before we saw it: deleted%s", o.hash, o.name, ""));
        return;
    }
    const bool alive = !o.life || *o.life > 0;
    if (o.life_written_us == 0) {
        if (o.life && alive) {
            out.lives.push_back({o.address, 0, 0, {}});
        }
        o.life_written_us = now_us;
        out.notes.push_back(Line("world: %016llx %s broken by another player%s", o.hash, o.name,
                                 o.life ? "" : " (no life: deleted after a wait)"));
        return;
    }
    // Still standing. Grass does not go when its life is 0; something with
    // no life has nothing to write; a life that is back above 0 did not take.
    const std::uint64_t wait = o.grass || !o.life ? kDeleteGrassUs : kDeleteUs;
    if (now_us - o.life_written_us >= wait && (o.grass || alive)) {
        o.remove = true;
        out.notes.push_back(
            Line("world: %016llx %s still stands after the break: deleted%s", o.hash, o.name, ""));
    }
}

std::optional<WorldSync::Sample> WorldSync::PoseAt(const Remote& r, std::uint64_t now_us) const {
    const std::uint64_t t = now_us > kBehindUs ? now_us - kBehindUs : 0;
    if (r.samples.empty() || (r.place && t >= r.samples.back().time_us)) {
        return r.place ? r.place : std::nullopt;
    }
    if (t <= r.samples.front().time_us) {
        return r.samples.front();
    }
    if (t >= r.samples.back().time_us) {
        return r.samples.back();
    }
    for (std::size_t i = 1; i < r.samples.size(); ++i) {
        const Sample& a = r.samples[i - 1];
        const Sample& b = r.samples[i];
        if (t > b.time_us) {
            continue;
        }
        const float f = static_cast<float>(t - a.time_us) / static_cast<float>(b.time_us - a.time_us);
        Sample s;
        s.time_us = t;
        float dot = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            dot += a.rotation[k] * b.rotation[k];
        }
        float n = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            s.rotation[k] = a.rotation[k] + f * ((dot < 0.0f ? -b.rotation[k] : b.rotation[k]) - a.rotation[k]);
            n += s.rotation[k] * s.rotation[k];
        }
        n = std::sqrt(n);
        for (std::size_t k = 0; k < 4; ++k) {
            s.rotation[k] = n > 1e-4f ? s.rotation[k] / n : (k == 3 ? 1.0f : 0.0f);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            s.position[k] = a.position[k] + f * (b.position[k] - a.position[k]);
        }
        return s;
    }
    return r.samples.back();
}

void WorldSync::Poses(std::uint64_t now_us, std::vector<CoopObjects::PoseWrite>& out) const {
    for (const auto& [address, o] : objects_) {
        if (!o.following || o.remove) {
            continue;
        }
        const auto r = remote_.find(o.hash);
        if (r == remote_.end()) {
            continue;
        }
        if (const auto s = PoseAt(r->second, now_us)) {
            CoopObjects::PoseWrite w;
            w.address = address;
            w.position = s->position;
            w.rotation = CoopObjects::MatOf(s->rotation);
            out.push_back(w);
        }
    }
}

void WorldSync::Move(Object& o, std::uint8_t self, std::uint64_t now_us,
                     const std::optional<proto::Vec3>& player, Out& out) {
    const auto found = remote_.find(o.hash);
    Remote* r = found != remote_.end() ? &found->second : nullptr;
    bool remote = r != nullptr && !r->samples.empty() && now_us - r->got_us < kRemoteHoldUs;
    if (r != nullptr && !remote && r->place) {
        r->samples.clear();  // the mover stopped: only its rest place counts now
    }
    if (o.ours && remote) {
        if (r->player < self) {
            o.ours = false;  // two movers: the lower number keeps it
            out.notes.push_back(Line("world: %016llx %s: another player moves it too; theirs%s",
                                     o.hash, o.name, ""));
        } else {
            remote = false;
        }
    }
    if (!o.ours && r != nullptr && r->place && o.place_seen != r->place_serial) {
        o.place_seen = r->place_serial;
        o.settle_us = now_us;
        if (!o.following) {
            out.notes.push_back(
                Line("world: %016llx %s put where another player left it%s", o.hash, o.name, ""));
        }
    }
    const bool settling = !o.ours && o.settle_us != 0 && now_us - o.settle_us < kSettleUs;
    if (!o.ours && (remote || settling)) {
        if (!o.following && remote) {
            out.notes.push_back(
                Line("world: %016llx %s moved by another player%s", o.hash, o.name, ""));
        }
        o.following = true;
        o.rest = o.position;
        o.quiet_until = now_us + kQuietUs;
        return;
    }
    if (o.following) {
        o.following = false;
        o.rest = o.position;
        o.quiet_until = now_us + kQuietUs;
    }
    if (now_us < o.quiet_until) {
        o.rest = o.position;  // settling after another game's move: no claim
        return;
    }
    if (!o.ours) {
        const bool near = player && Distance(*player, o.position) <= kMoverReachM;
        if (!near || !Finite(o.position) ||
            Distance(o.position, o.rest) <= (o.breakable ? kLooseStartM : kBlockStartM)) {
            return;
        }
        o.ours = true;
        o.moved_us = now_us;
        o.sent_us = 0;
        out.notes.push_back(Line("world: %016llx %s moved here: sending%s", o.hash, o.name, ""));
    }
    if (o.stirred) {
        o.moved_us = now_us;
    }
    if (!Finite(o.position)) {
        o.ours = false;
        return;
    }
    if (now_us - o.moved_us >= kRestUs) {
        proto::WorldPlace p;
        p.scene = scene_;
        p.hash = o.hash;
        p.position = o.position;
        p.rotation = UnitQuat(o.rotation);
        out.send.emplace_back(std::move(p));
        o.ours = false;
        o.rest = o.position;
        out.notes.push_back(Line("world: %016llx %s at rest: place sent%s", o.hash, o.name, ""));
        return;
    }
    if (o.sent_us == 0 || (now_us - o.sent_us >= kMoveEveryUs &&
                           Distance(o.position, o.sent_position) > 0.003f)) {
        proto::WorldMove v;
        v.scene = scene_;
        v.hash = o.hash;
        v.time_us = now_us;
        v.position = o.position;
        v.rotation = UnitQuat(o.rotation);
        if (player && Distance(*player, o.position) <= 1.5f) {
            v.flags |= proto::world_move::carried;
        }
        out.send.emplace_back(std::move(v));
        o.sent_us = now_us;
        o.sent_position = o.position;
    }
}

void WorldSync::Swing(const proto::GrassCut& cut, const std::optional<proto::Vec3>& player,
                      std::uint64_t now_us, Out& out) {
    if (cut.scene != scene_ || std::isnan(cut.sweep[0][0])) {
        return;  // a sphere cut (an AI's), no sword
    }
    std::vector<proto::Vec3> points{cut.position};
    for (const proto::Vec3& p : cut.sweep) {
        if (Finite(p)) {
            points.push_back(p);
        }
    }
    for (auto& [address, o] : objects_) {
        if (!o.sign || now_us - o.hit_sent_us < kHitEveryUs) {
            continue;
        }
        const proto::Vec3* best = nullptr;
        float best_level = kSignReachM;
        for (const proto::Vec3& p : points) {
            const float dx = p[0] - o.position[0];
            const float dz = p[2] - o.position[2];
            const float level = std::sqrt(dx * dx + dz * dz);
            if (level <= best_level && std::fabs(p[1] - o.position[1]) <= kSignHighM) {
                best_level = level;
                best = &p;
            }
        }
        if (best == nullptr) {
            continue;
        }
        o.hit_sent_us = now_us;
        proto::WorldHit h;
        h.scene = scene_;
        h.hash = o.hash;
        h.at = *best;
        h.from = player && Finite(*player) ? *player : *best;
        out.send.emplace_back(std::move(h));
        out.notes.push_back(Line("world: %016llx %s: our sword passed it: hit sent%s", o.hash, o.name, ""));
    }
}

void WorldSync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
                     const std::vector<CoopObjects::Seen>& seen,
                     const std::optional<proto::Vec3>& player, Out& out) {
    self_ = self;
    if (scene != scene_) {
        scene_ = scene;
        scene_since_ = now_us;
        objects_.clear();
        broken_.clear();
        remote_.clear();
        gone_.clear();
        went_.clear();
        hits_.clear();
        std::lock_guard lock(inbox_mutex_);
        for (proto::Message& m : early_) {
            if (*SceneOfMessage(m) == scene_) {
                inbox_.insert(inbox_.begin(), std::move(m));
            }
        }
        early_.clear();
    }
    Drain(now_us);

    std::set<std::uint64_t> now_seen;
    for (const CoopObjects::Seen& s : seen) {
        if (s.hash == 0 || s.hash == kNoHash) {
            continue;
        }
        const bool breakable = IsBreakable(s.name);
        if (!breakable && !IsMovable(s.name)) {
            continue;
        }
        now_seen.insert(s.address);
        auto it = objects_.find(s.address);
        if (it != objects_.end() && it->second.hash != s.hash) {
            objects_.erase(it);  // the address has another actor now
            it = objects_.end();
        }
        std::optional<std::int32_t> life;
        if (s.life) {
            life = s.life->life;
        }
        if (it == objects_.end()) {
            Object o;
            o.address = s.address;
            o.hash = s.hash;
            o.name = s.name;
            o.breakable = breakable;
            o.grass = IsGrassActor(s.name);
            o.sign = IsSign(s.name);
            o.life = life;
            o.position = s.position;
            o.rotation = s.rotation;
            o.first_position = s.position;
            o.first_us = now_us;
            o.rest = s.position;
            o.sent_position = s.position;
            if (const auto b = broken_.find(s.hash);
                b != broken_.end() && b->second.seen_go && now_us - b->second.gone_us >= kRespawnUs &&
                (!life || *life > 0)) {
                // We saw it go and here it is again: the game put it back.
                broken_.erase(b);
                proto::WorldBreak back;
                back.scene = scene_;
                back.hash = o.hash;
                back.position = Finite(o.position) ? o.position : proto::Vec3{};
                back.flags = proto::world_break::restored;
                out.send.emplace_back(std::move(back));
                out.notes.push_back(
                    Line("world: %016llx %s is back: break forgotten%s", o.hash, o.name, ""));
            }
            objects_.emplace(s.address, std::move(o));
            continue;
        }
        Object& o = it->second;
        o.missed = 0;
        o.stirred = Finite(s.position) && Distance(s.position, o.position) > 0.003f;
        o.position = s.position;
        o.rotation = s.rotation;
        // As the enemy sharing reads it: a breakable whose life can no longer
        // be read has died (a crate's life object goes a frame after 0).
        if (!life && o.breakable && o.life && *o.life > 0) {
            life = 0;
        }
        if (life) {
            const bool died = o.life && *o.life > 0 && *life <= 0;
            o.life = life;
            if (died && o.breakable) {
                LocalBreak(o, now_us, "life 0", out);
            }
        }
    }

    for (auto it = objects_.begin(); it != objects_.end();) {
        Object& o = it->second;
        if (now_seen.contains(it->first) || ++o.missed < kMissedWalks) {
            ++it;
            continue;
        }
        const float from_link = player ? Distance(*player, o.position) : -1.0f;
        const bool far = !player || from_link > kBreakNearM;
        went_.emplace_back(now_us, far);
        if (const auto b = broken_.find(o.hash); b != broken_.end()) {
            b->second.seen_go = true;
            b->second.gone_us = now_us;
        } else if (o.breakable && player && (!o.life || *o.life > 0)) {
            const bool moved = Distance(o.position, o.first_position) >= 0.5f;
            if (from_link <= (moved ? kBreakNearM : kBreakReachM)) {
                gone_.push_back({o, now_us});
            }
        }
        it = objects_.erase(it);
    }
    if (!player) {
        gone_.clear();  // no Link in the walk: the scene is loading or going
    }
    for (auto it = gone_.begin(); it != gone_.end();) {
        if (now_us - it->at_us < kGoneHoldUs) {
            ++it;
            continue;
        }
        // Left the walk next to our Link. Broken, unless a crowd went with
        // it and some of that crowd was far away: then the area unloaded.
        int together = 0;
        bool any_far = false;
        for (const auto& [at, far] : went_) {
            const std::uint64_t apart = at > it->at_us ? at - it->at_us : it->at_us - at;
            if (apart <= kGoneHoldUs) {
                ++together;
                any_far = any_far || far;
            }
        }
        if (together >= kUnloadCount && any_far) {
            out.notes.push_back(Line("world: %016llx %s left with the area: no break%s",
                                     it->object.hash, it->object.name, ""));
        } else {
            LocalBreak(it->object, now_us, "gone next to Link", out);
            if (const auto b = broken_.find(it->object.hash); b != broken_.end()) {
                b->second.seen_go = true;
                b->second.gone_us = it->at_us;
            }
        }
        it = gone_.erase(it);
    }
    std::erase_if(went_, [now_us](const auto& w) { return now_us - w.first > 4 * kGoneHoldUs; });

    for (auto& [address, o] : objects_) {
        // Another player's swings at this sign, once our copy has settled in.
        if (const auto waiting = hits_.find(o.hash);
            waiting != hits_.end() && o.sign && now_us - o.first_us >= kHitWaitUs) {
            for (const auto& [at, from] : waiting->second) {
                out.hits.push_back({address, at, from});
            }
            out.notes.push_back(Line("world: %016llx %s hit by another player's sword%s", o.hash,
                                     o.name, ""));
            hits_.erase(waiting);
        }
        if (const auto b = broken_.find(o.hash); b != broken_.end()) {
            MakeBreak(o, b->second, now_us, out);
            o.ours = false;
            o.following = false;
            continue;
        }
        Move(o, self, now_us, player, out);
    }
    // A mover that stopped without a rest place (it left, or the place was
    // lost): forgotten once nothing follows it.
    std::erase_if(remote_, [now_us](const auto& e) {
        const Remote& r = e.second;
        return !r.place && (r.samples.empty() || now_us - r.got_us > 4 * kRemoteHoldUs);
    });
}

namespace {

constexpr int kBlockLines = 60;                      // per block and scene
constexpr int kNoisy = 6;  // a word that changed this often is a counter: no more lines for it
constexpr int kAllLines = 1500;                      // per session

bool LooksLikePointer(std::uint64_t v) {
    return (v >> 32) != 0 && v < (1ull << 40) && v % 4 == 0;
}

} // namespace

void FoliageProbe::Compare(const ReadWord& read, const std::string& key, std::uint64_t address,
                           std::size_t count, const std::string& where,
                           std::vector<std::string>& out) {
    std::vector<std::uint64_t> now(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto w = read(address + 8 * i);
        if (!w) {
            return;  // not readable: not a buffer
        }
        now[i] = *w;
    }
    Block& b = blocks_[key];
    if (b.lines >= kBlockLines || lines_ >= kAllLines) {
        b.address = address;
        b.words = std::move(now);
        b.changes.assign(count, 0);
        return;
    }
    char text[64];
    std::string line;
    if (b.address != address || b.words.size() != count) {
        std::snprintf(text, sizeof text, " at %#llx:", static_cast<unsigned long long>(address));
        line = "foliage: " + key + text;
        b.changes.assign(count, 0);
        for (const std::uint64_t w : now) {
            std::snprintf(text, sizeof text, " %llx", static_cast<unsigned long long>(w));
            line += text;
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (now[i] == b.words[i] || ++b.changes[i] > kNoisy + 1) {
                continue;
            }
            if (b.changes[i] == kNoisy + 1) {
                std::snprintf(text, sizeof text, " +%#zx:noisy", 8 * i);
            } else {
                std::snprintf(text, sizeof text, " +%#zx:%llx>%llx", 8 * i,
                              static_cast<unsigned long long>(b.words[i]),
                              static_cast<unsigned long long>(now[i]));
            }
            line += text;
        }
        if (line.empty()) {
            return;
        }
        line = "foliage: " + key + " changed" + line;
    }
    b.address = address;
    b.words = std::move(now);
    ++b.lines;
    ++lines_;
    out.push_back(line + where);
}

std::vector<std::string> FoliageProbe::Look(const ReadWord& read, std::uint64_t main,
                                            const std::optional<std::array<float, 3>>& link) {
    std::vector<std::string> out;
    if (++calls_ % 4 != 0 || main == 0) {
        return out;
    }
    const auto module = read(main + kFoliageModule);
    const std::uint64_t flags = module && *module ? read(*module + 0x30).value_or(0) & 0xff : 0;
    // As the game's own 0x2c013c0: with bit 0 of the module's byte +0x30 set there is no scene.
    const bool off = (flags & 1) != 0;
    const auto holder = module && *module ? read(*module + 0xf0) : std::nullopt;
    const auto scene = !off && holder && *holder ? read(*holder + 0x338) : std::nullopt;
    const std::uint64_t s = scene.value_or(0);
    char text[160];
    if (s != scene_) {
        scene_ = s;
        blocks_.clear();
        std::snprintf(text, sizeof text,
                      "foliage: module %#llx flags %#llx holder %#llx scene %#llx%s",
                      static_cast<unsigned long long>(module.value_or(0)),
                      static_cast<unsigned long long>(flags),
                      static_cast<unsigned long long>(holder.value_or(0)),
                      static_cast<unsigned long long>(s), off ? " (off)" : "");
        out.emplace_back(text);
    }
    if (s == 0) {
        return out;
    }
    std::string where;
    if (link) {
        std::snprintf(text, sizeof text, " | Link %.2f %.2f %.2f", (*link)[0], (*link)[1],
                      (*link)[2]);
        where = text;
    }
    // The scene's first 0x400 bytes: SsboGrassCut is +0x58, SsboGather +0x88. A cut that
    // lands in a field this walk does not follow still shows here.
    Compare(read, "scene", s, 128, where, out);
    for (std::uint64_t field = 0x58; field < 0x88; field += 8) {
        const auto p = read(s + field);
        if (!p || !LooksLikePointer(*p)) {
            continue;
        }
        std::snprintf(text, sizeof text, "cut+%#llx", static_cast<unsigned long long>(field));
        const std::string key = text;
        Compare(read, key, *p, 64, where, out);
        for (std::uint64_t inner = 0; inner < 0x40; inner += 8) {
            const auto q = read(*p + inner);
            if (!q || !LooksLikePointer(*q) || *q == *p) {
                continue;
            }
            std::snprintf(text, sizeof text, "->+%#llx", static_cast<unsigned long long>(inner));
            Compare(read, key + text, *q, 64, where, out);
        }
    }
    return out;
}

namespace {

constexpr std::uint64_t kMalloc = 0x14c6c90;  // main + this (coop_puppet.h)
constexpr float kRequestDefault = 0.3f;       // what the game puts at +0x10

float FloatAt(const GrassSync::Slot& s, std::size_t offset) {
    float f = 0.0f;
    std::memcpy(&f, s.data() + offset, 4);
    return f;
}

void PutFloat(GrassSync::Slot& s, std::size_t offset, float f) {
    std::memcpy(s.data() + offset, &f, 4);
}

// The same cut: position and radius, bit for bit.
bool SameCut(const GrassSync::Slot& a, const GrassSync::Slot& b) {
    return std::memcmp(a.data(), b.data(), 16) == 0;
}

std::optional<std::uint64_t> SceneOf(const GrassSync::ReadWord& read, std::uint64_t main) {
    const auto module = read(main + kFoliageModule);
    if (!module || *module == 0) {
        return std::nullopt;
    }
    if ((read(*module + 0x30).value_or(1) & 1) != 0) {
        return std::nullopt;  // off, as the game's 0x2c013c0 reads it
    }
    const auto holder = read(*module + 0xf0);
    if (!holder || *holder == 0) {
        return std::nullopt;
    }
    const auto scene = read(*holder + 0x338);
    if (!scene || *scene == 0) {
        return std::nullopt;
    }
    return scene;
}

} // namespace

coop::protocol::GrassCut GrassSync::ToMessage(const Slot& slot, const std::string& scene) {
    coop::protocol::GrassCut c;
    c.scene = scene;
    for (std::size_t i = 0; i < 3; ++i) {
        c.position[i] = FloatAt(slot, 4 * i);
        for (std::size_t j = 0; j < 3; ++j) {
            c.sweep[i][j] = FloatAt(slot, 0x18 + 0xc * i + 4 * j);
        }
    }
    c.radius = FloatAt(slot, 0xc);
    c.flags = slot[0x14];
    return c;
}

GrassSync::Slot GrassSync::ToSlot(const coop::protocol::GrassCut& cut) {
    Slot s{};
    for (std::size_t i = 0; i < 3; ++i) {
        PutFloat(s, 4 * i, cut.position[i]);
        for (std::size_t j = 0; j < 3; ++j) {
            PutFloat(s, 0x18 + 0xc * i + 4 * j, cut.sweep[i][j]);
        }
    }
    PutFloat(s, 0xc, cut.radius);
    PutFloat(s, 0x10, kRequestDefault);
    s[0x14] = cut.flags;
    return s;
}

std::vector<coop::protocol::GrassCut> GrassSync::Look(const ReadWord& read, std::uint64_t main,
                                                      const std::string& scene,
                                                      std::uint64_t now_ms) {
    std::vector<coop::protocol::GrassCut> out;
    const auto s = main != 0 ? SceneOf(read, main) : std::nullopt;
    {
        std::lock_guard lock(mutex_);
        main_ = main;
        if (scene != scene_) {
            scene_ = scene;
            // Cuts of the scene we left go; ones for this scene that came
            // before we knew we were here (the server's replay) stay.
            std::erase_if(waiting_, [&scene](const Waiting& w) { return w.scene != scene; });
        }
        while (!made_.empty() && now_ms - made_.front().first > kEchoMs) {
            made_.pop_front();
        }
    }
    if (!s) {
        scene_address_ = 0;
        last_index_.reset();
        SetReady(false);
        return out;
    }
    if (*s != scene_address_ || scene != settle_scene_) {
        scene_address_ = *s;
        settle_scene_ = scene;
        settle_ms_ = now_ms;
        last_index_.reset();
    }
    const auto capacity = read(*s + 0x20);
    const auto entries = read(*s + 0x28);
    const auto written = read(*s + 0x38);
    const std::uint64_t cap = capacity.value_or(0) & 0xffffffff;
    if (!entries || !written || cap == 0 || cap > 4096 || (cap & (cap - 1)) != 0) {
        return out;
    }
    // Ready once the scene and its foliage scene stayed the same a while: on
    // a scene change the name can change before the foliage is the new one.
    SetReady(!scene.empty() && now_ms - settle_ms_ >= kSettleMs);
    if (!last_index_ || *written < *last_index_) {
        last_index_ = *written;  // the queue is new to us: only cuts from now on
        return out;
    }
    std::uint64_t from = *last_index_;
    if (*written - from > cap) {
        from = *written - cap;
    }
    last_index_ = *written;
    std::optional<Slot> previous;
    for (std::uint64_t i = from; i < *written; ++i) {
        const std::uint64_t entry = *entries + 16 * (i & (cap - 1));
        const auto pointer = read(entry);
        const auto sequence = read(entry + 8);
        // i + 1 while queued, i + capacity once taken out (and not reused).
        if (!pointer || *pointer == 0 || !sequence ||
            (*sequence != i + 1 && *sequence != i + cap)) {
            continue;
        }
        Slot slot{};
        bool ok = true;
        for (std::size_t k = 0; k < kCutSlotSize && ok; k += 8) {
            const auto w = read(*pointer + k);
            ok = w.has_value();
            if (ok) {
                std::memcpy(slot.data() + k, &*w, std::min<std::size_t>(8, kCutSlotSize - k));
            }
        }
        if (!ok || (previous && SameCut(*previous, slot))) {
            continue;
        }
        previous = slot;
        coop::protocol::GrassCut cut = ToMessage(slot, scene);
        bool echo = false;
        {
            std::lock_guard lock(mutex_);
            for (auto it = made_.begin(); it != made_.end(); ++it) {
                if (SameCut(it->second, slot)) {
                    made_.erase(it);
                    echo = true;
                    break;
                }
            }
        }
        const coop::protocol::Message check = cut;
        std::vector<std::uint8_t> bytes;
        if (echo || scene.empty() ||
            coop::protocol::encode(check, bytes) != coop::protocol::Error::none) {
            continue;  // made here for another game, or not a cut we can send
        }
        out.push_back(std::move(cut));
    }
    return out;
}

void GrassSync::SetReady(bool ready) {
    std::lock_guard lock(mutex_);
    ready_ = ready;
}

void GrassSync::Receive(const coop::protocol::GrassCut& cut) {
    std::lock_guard lock(mutex_);
    // A cut for a scene we are not in is kept only while no cut of ours says
    // where we are: the server replays a scene's cuts when our first sample
    // there reaches it, which can be before our objects walk saw the scene.
    if (waiting_.size() >= kMaxWaiting || (cut.scene != scene_ && ready_)) {
        return;
    }
    waiting_.push_back({cut.scene, ToSlot(cut)});
}

std::optional<CoopPuppet::Call> GrassSync::Next(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    // Only once the walk has seen this scene's foliage queue: a cut made
    // while the scene loads would go to the one we left.
    if (waiting_.empty() || main_ == 0 || !ready_ || this_frame_ >= kCutsPerFrame) {
        return std::nullopt;
    }
    while (!waiting_.empty() && waiting_.front().scene != scene_) {
        waiting_.pop_front();
    }
    if (waiting_.empty()) {
        return std::nullopt;
    }
    if (block_ == 0) {
        if (allocating_ || now_ms < retry_ms_) {
            return std::nullopt;
        }
        allocating_ = true;
        CoopPuppet::Call c;
        c.function = main_ + kMalloc;
        c.x[0] = 0x40;
        return c;
    }
    std::uint64_t module = 0;
    if (!guest.Read(main_ + kFoliageModule, &module, sizeof module) || module == 0) {
        waiting_.clear();
        return std::nullopt;
    }
    const Slot slot = waiting_.front().slot;
    waiting_.pop_front();
    if (!guest.Write(block_, slot.data(), slot.size())) {
        return std::nullopt;
    }
    made_.emplace_back(now_ms, slot);
    ++made_count_;
    ++this_frame_;
    CoopPuppet::Call c;
    c.function = main_ + kCutFoliage;
    c.x[0] = module;
    c.x[1] = block_;
    return c;
}

std::optional<CoopPuppet::Call> GrassSync::Step(CoopPuppet::Guest& guest, bool after_call,
                                                std::uint64_t result, std::uint64_t now_ms) {
    {
        std::lock_guard lock(mutex_);
        if (!after_call) {
            this_frame_ = 0;
        } else if (allocating_) {
            allocating_ = false;
            if (result == 0) {
                retry_ms_ = now_ms + 5000;
                notes_.emplace_back("grass: malloc gave nothing; again in 5 s");
            } else {
                block_ = result;
                char text[64];
                std::snprintf(text, sizeof text, "grass: request at %#llx",
                              static_cast<unsigned long long>(result));
                notes_.emplace_back(text);
            }
        }
    }
    return Next(guest, now_ms);
}

std::uint64_t GrassSync::Made() const {
    std::lock_guard lock(mutex_);
    return made_count_;
}

std::vector<std::string> GrassSync::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

} // namespace CoopWorld
