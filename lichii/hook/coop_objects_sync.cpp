// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_objects_sync.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <type_traits>
#include <variant>

namespace CoopObjects {

namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kNoHash = ~std::uint64_t{0};

bool Moved(const proto::Vec3& a, const proto::Vec3& b) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::fabs(a[i] - b[i]) > 0.01f) {
            return true;
        }
    }
    return false;
}

bool Turned(const std::optional<proto::Quat>& a, const std::optional<proto::Quat>& b) {
    if (a.has_value() != b.has_value()) {
        return true;
    }
    if (!a) {
        return false;
    }
    float dot = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        dot += (*a)[i] * (*b)[i];
    }
    return std::fabs(dot) < 0.99995f;  // about 1 degree
}

proto::Vec3 Lerp(const proto::Vec3& a, const proto::Vec3& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// Normalised linear blend, the short way round.
proto::Quat Nlerp(const proto::Quat& a, proto::Quat b, float t) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        dot += a[i] * b[i];
    }
    if (dot < 0.0f) {
        for (float& v : b) {
            v = -v;
        }
    }
    proto::Quat q{};
    float len = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = a[i] + (b[i] - a[i]) * t;
        len += q[i] * q[i];
    }
    len = std::sqrt(len);
    if (len < 1e-6f) {
        return a;
    }
    for (float& v : q) {
        v /= len;
    }
    return q;
}

bool Finite(const proto::Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

} // namespace

bool IsShared(std::string_view name) {
    return name.starts_with("Enemy_") || name.starts_with("Animal_");
}

proto::Quat QuatOf(const Mat3& m) {
    // m[3 * row + column], Shepperd's method. Rows that are not unit length
    // (a read of something else) give no turn.
    for (std::size_t row = 0; row < 3; ++row) {
        const float len = m[3 * row] * m[3 * row] + m[3 * row + 1] * m[3 * row + 1] +
                          m[3 * row + 2] * m[3 * row + 2];
        if (!std::isfinite(len) || std::fabs(len - 1.0f) > 0.1f) {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }
    }
    const float trace = m[0] + m[4] + m[8];
    proto::Quat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, 0.25f * s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q = {0.25f * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s};
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q = {(m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[2] - m[6]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q = {(m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[3] - m[1]) / s};
    }
    float len = 0.0f;
    for (const float v : q) {
        len += v * v;
    }
    len = std::sqrt(len);
    if (!std::isfinite(len) || len < 1e-6f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    for (float& v : q) {
        v /= len;
    }
    return q;
}

Mat3 MatOf(const proto::Quat& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
}

std::uint64_t SceneSpawnId(std::string_view scene, std::string_view name, unsigned order) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
        h ^= 0xff;  // a separator no name has
        h *= 0x100000001b3ULL;
    };
    mix(scene);
    mix(name);
    for (int i = 0; i < 4; ++i) {
        h ^= (order >> (8 * i)) & 0xff;
        h *= 0x100000001b3ULL;
    }
    return h | (std::uint64_t{1} << 63);
}

void Sync::Receive(const proto::Message& message) {
    const bool wanted = std::visit(
        [](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            return std::is_same_v<T, proto::ObjectOwner> || std::is_same_v<T, proto::ObjectSample> ||
                   std::is_same_v<T, proto::HandoverAsk> || std::is_same_v<T, proto::HandoverState> ||
                   std::is_same_v<T, proto::Damage>;
        },
        message);
    if (!wanted) {
        return;
    }
    std::lock_guard lock(inbox_mutex_);
    if (inbox_.size() < 4096) {
        inbox_.push_back(message);
    }
}

void Sync::Reset() {
    scene_.clear();
    objects_.clear();
    order_.clear();
    owners_.clear();
    present_sent_.clear();
    present_at_ = 0;
    present_once_ = false;
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

std::optional<proto::ObjectId> Sync::IdOf(std::uint64_t address) const {
    const auto it = objects_.find(address);
    if (it == objects_.end()) {
        return std::nullopt;
    }
    return it->second.id;
}

std::uint8_t Sync::OwnerOf(const proto::ObjectId& id) const {
    const auto it = owners_.find(id);
    return it == owners_.end() ? 0 : it->second.player;
}

proto::ObjectState Sync::StateOf(Object& o, std::uint64_t now_us) {
    proto::ObjectState state;
    state.time = {now_us, ++o.sequence};
    state.position = o.position;
    state.velocity = o.velocity;
    if (o.rotation) {
        state.rotation = QuatOf(*o.rotation);
    }
    if (o.life) {
        state.life = proto::ObjectLife{*o.life, o.max};
    }
    return state;
}

void Sync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
                const std::vector<Seen>& seen, std::vector<proto::Message>& send,
                std::vector<LifeWrite>& writes) {
    if (scene != scene_) {
        const std::string keep = scene;
        Reset();
        scene_ = keep;
    }

    // What this walk shows: new enemies get ids, known ones their life and
    // place; a drop of life on a copy we do not own is our hit.
    std::map<std::uint64_t, const Seen*> now_seen;
    std::vector<const Seen*> fresh;
    for (const Seen& s : seen) {
        if (!IsShared(s.name)) {
            continue;
        }
        now_seen[s.address] = &s;
        const auto it = objects_.find(s.address);
        if (it == objects_.end()) {
            fresh.push_back(&s);
            continue;
        }
        Object& o = it->second;
        if (now_us > o.seen_at && now_us - o.seen_at < 500'000 && Finite(s.position)) {
            const float dt = static_cast<float>(now_us - o.seen_at) / 1e6f;
            for (std::size_t i = 0; i < 3; ++i) {
                o.velocity[i] = (s.position[i] - o.position[i]) / dt;
            }
        } else {
            o.velocity = {};
        }
        o.seen_at = now_us;
        o.position = s.position;
        o.rotation = s.rotation;
        if (s.life) {
            if (o.life && s.life->life < *o.life) {
                o.fight_until = now_us + kFightUs;
                std::int32_t drop = *o.life - s.life->life;
                if (o.hurt_left > 0) {
                    // Our own hurt landing (or the game taking more for it):
                    // not a hit of ours.
                    o.hurt_left = std::max(0, o.hurt_left - drop);
                    if (o.hurt_left == 0) {
                        if (s.life->life == o.hurt_target) {
                            ++hurt_taken_;
                        }
                        o.hurt_until = std::min(o.hurt_until, now_us);  // checked below
                    }
                    drop = 0;
                }
                const auto owner = owners_.find(o.id);
                if (drop > 0 && owner != owners_.end() && owner->second.player != 0 &&
                    owner->second.player != self) {
                    if (o.hit_sum == 0) {
                        o.hit_since = now_us;
                    }
                    o.hit_sum += drop;
                }
            }
            o.life = s.life->life;
            o.max = s.life->max;
        }
    }
    for (auto it = objects_.begin(); it != objects_.end();) {
        it = now_seen.contains(it->first) ? std::next(it) : objects_.erase(it);
    }
    std::sort(fresh.begin(), fresh.end(), [](const Seen* a, const Seen* b) {
        return std::tie(a->name, a->position[0], a->position[2]) <
               std::tie(b->name, b->position[0], b->position[2]);
    });
    for (const Seen* s : fresh) {
        Object o;
        o.address = s->address;
        o.position = s->position;
        o.rotation = s->rotation;
        o.seen_at = now_us;
        if (s->life) {
            o.life = s->life->life;
            o.max = s->life->max;
        }
        if (s->hash != 0 && s->hash != kNoHash) {
            o.id = {proto::ObjectKind::placed, 0, s->hash};
        } else {
            o.id = {proto::ObjectKind::placed, 0, SceneSpawnId(scene_, s->name, order_[s->name]++)};
        }
        objects_.emplace(s->address, o);
    }

    self_ = self;
    Apply(self, now_us, send, writes);

    for (auto& [address, o] : objects_) {
        // A hurt the game did not take (or took differently) in time: the
        // life is written as the owner has it.
        if (o.hurt_until != 0 && now_us >= o.hurt_until) {
            if (o.life && *o.life != o.hurt_target) {
                writes.push_back({o.address, o.hurt_target, 0});
                o.life = o.hurt_target;
                ++hurt_missed_;
            }
            o.hurt_left = 0;
            o.hurt_until = 0;
        }
        const auto owner = owners_.find(o.id);
        const bool known = owner != owners_.end() && owner->second.player != 0;
        // Our hits on a copy, summed over 100 ms, go to the owner.
        if (o.hit_sum > 0 && now_us - o.hit_since >= kDamageSumUs) {
            if (known && owner->second.player != self) {
                proto::Damage d;
                d.id = o.id;
                d.epoch = owner->second.epoch;
                d.amount = o.hit_sum;
                d.cause = static_cast<std::uint8_t>(proto::DamageCause::unknown);
                d.position = o.position;
                send.emplace_back(d);
                o.hit_at = now_us;
            }
            o.hit_sum = 0;
        }
        // The owner's samples.
        if (known && owner->second.player == self &&
            (!o.sent_once || now_us - o.sent_at >= kSampleEveryUs)) {
            const std::optional<proto::ObjectLife> life =
                o.life ? std::optional(proto::ObjectLife{*o.life, o.max}) : std::nullopt;
            const std::optional<proto::Quat> rotation =
                o.rotation ? std::optional(QuatOf(*o.rotation)) : std::nullopt;
            const bool changed = life != o.sent_life || Moved(o.position, o.sent_position) ||
                                 Turned(rotation, o.sent_rotation);
            if (!o.sent_once || changed || now_us - o.sent_at >= kStillEveryUs) {
                proto::ObjectSample sample;
                sample.id = o.id;
                sample.epoch = owner->second.epoch;
                sample.state = StateOf(o, now_us);
                send.emplace_back(sample);
                o.sent_at = now_us;
                o.sent_once = true;
                o.sent_life = life;
                o.sent_position = o.position;
                o.sent_rotation = rotation;
            }
        }
    }

    std::vector<proto::ObjectPresence> present;
    present.reserve(objects_.size());
    for (const auto& [address, o] : objects_) {
        std::uint8_t flags = proto::object_presence::awake;
        if (o.fight_until > now_us) {
            flags |= proto::object_presence::in_fight;
        }
        present.push_back({o.id, flags});
    }
    std::sort(present.begin(), present.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    if (present.size() > proto::kMaxPresentObjects) {
        present.resize(proto::kMaxPresentObjects);
    }
    if (!present_once_ || present != present_sent_ || now_us - present_at_ >= kPresentEveryUs) {
        proto::ObjectPresent p;
        p.scene = scene_;
        p.objects = present;
        send.emplace_back(std::move(p));
        present_sent_ = std::move(present);
        present_at_ = now_us;
        present_once_ = true;
    }
}

void Sync::Apply(std::uint8_t self, std::uint64_t now_us, std::vector<proto::Message>& send,
                 std::vector<LifeWrite>& writes) {
    std::vector<proto::Message> inbox;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
    }
    const auto find = [this](const proto::ObjectId& id) -> Object* {
        for (auto& [address, o] : objects_) {
            if (o.id == id) {
                return &o;
            }
        }
        return nullptr;
    };
    const auto write = [&writes](Object& o, std::int32_t life) {
        writes.push_back({o.address, life, 0});
        o.life = life;
        o.hurt_left = 0;
        o.hurt_until = 0;
    };
    for (const proto::Message& m : inbox) {
        if (const auto* owner = std::get_if<proto::ObjectOwner>(&m)) {
            owners_[owner->id] = {owner->owner, owner->epoch};
            if (owner->owner == self) {
                if (Object* o = find(owner->id)) {
                    o->received.clear();  // ours now: nothing pulls it
                }
            }
        } else if (const auto* sample = std::get_if<proto::ObjectSample>(&m)) {
            Object* o = find(sample->id);
            const auto own = owners_.find(sample->id);
            if (o == nullptr || (own != owners_.end() && own->second.player == self)) {
                continue;
            }
            if (sample->state.position && Finite(*sample->state.position)) {
                const std::uint64_t t = sample->state.time.server_time_us;
                if (o->received.empty() || t > o->received.back().time) {
                    o->received.push_back(
                        {t, *sample->state.position, sample->state.rotation, sample->state.velocity});
                    while (o->received.size() > kKeepSamples) {
                        o->received.pop_front();
                    }
                }
            }
            if (!sample->state.life || !o->life) {
                continue;
            }
            const std::int32_t life = sample->state.life->life;
            const std::int32_t have = o->hurt_until != 0 ? o->hurt_target : *o->life;
            const bool holding = o->hit_sum > 0 || now_us - o->hit_at < kHitHoldUs;
            if (life < have) {
                Lower(*o, life, now_us, writes);
                o->fight_until = now_us + kFightUs;
            } else if (life > have && !holding) {
                write(*o, life);
                o->fight_until = now_us + kFightUs;
            }
        } else if (const auto* damage = std::get_if<proto::Damage>(&m)) {
            // The server sends DAMAGE only to the owner: that is us.
            Object* o = find(damage->id);
            if (o == nullptr || !o->life || damage->amount <= 0) {
                continue;
            }
            const std::int32_t have = o->hurt_until != 0 ? o->hurt_target : *o->life;
            Lower(*o, std::max(0, have - damage->amount), now_us, writes);
            o->fight_until = now_us + kFightUs;
        } else if (const auto* ask = std::get_if<proto::HandoverAsk>(&m)) {
            proto::HandoverState state;
            state.id = ask->id;
            state.epoch = ask->epoch;
            if (Object* o = find(ask->id)) {
                state.state = StateOf(*o, now_us);
            } else {
                state.state.time = {now_us, 0};
            }
            send.emplace_back(std::move(state));
        } else if (const auto* handed = std::get_if<proto::HandoverState>(&m)) {
            Object* o = find(handed->id);
            if (o != nullptr && handed->state.life && o->life &&
                handed->state.life->life != *o->life) {
                write(*o, handed->state.life->life);
            }
        }
    }
}

void Sync::Lower(Object& o, std::int32_t life, std::uint64_t now_us,
                 std::vector<LifeWrite>& writes) {
    if (!o.life) {
        return;
    }
    if (!hurt_) {
        writes.push_back({o.address, life, 0});
        o.life = life;
        return;
    }
    const std::int32_t from = o.hurt_until != 0 ? o.hurt_target : *o.life;
    if (life >= from) {
        return;
    }
    // The game takes it off itself; the walk sees the drop and does not
    // count it as our hit. o.life stays as read until then.
    writes.push_back({o.address, life, from - life});
    o.hurt_left += from - life;
    o.hurt_target = life;
    o.hurt_until = now_us + kHurtWaitUs;
}

void Sync::Poses(std::uint64_t now_us, std::vector<PoseWrite>& out) const {
    const std::uint64_t t = now_us > kBehindUs ? now_us - kBehindUs : 0;
    for (const auto& [address, o] : objects_) {
        if (o.received.empty() || now_us < o.received.back().time ||
            now_us - o.received.back().time > kPoseStaleUs) {
            continue;
        }
        const auto owner = owners_.find(o.id);
        if (owner == owners_.end() || owner->second.player == 0 || owner->second.player == self_) {
            continue;
        }
        PoseWrite w;
        w.address = address;
        const Received& last = o.received.back();
        std::optional<proto::Quat> rotation;
        if (t >= last.time) {
            const float ahead =
                static_cast<float>(std::min(t - last.time, kExtrapolateUs)) / 1e6f;
            w.position = last.position;
            if (last.velocity && Finite(*last.velocity)) {
                for (std::size_t i = 0; i < 3; ++i) {
                    w.position[i] += (*last.velocity)[i] * ahead;
                }
            }
            rotation = last.rotation;
        } else if (t <= o.received.front().time) {
            w.position = o.received.front().position;
            rotation = o.received.front().rotation;
        } else {
            std::size_t i = 1;
            while (o.received[i].time <= t) {
                ++i;
            }
            const Received& a = o.received[i - 1];
            const Received& b = o.received[i];
            const float f = static_cast<float>(t - a.time) / static_cast<float>(b.time - a.time);
            w.position = Lerp(a.position, b.position, f);
            if (a.rotation && b.rotation) {
                rotation = Nlerp(*a.rotation, *b.rotation, f);
            } else {
                rotation = b.rotation ? b.rotation : a.rotation;
            }
        }
        if (rotation) {
            w.rotation = MatOf(*rotation);
        }
        out.push_back(w);
    }
}

} // namespace CoopObjects
