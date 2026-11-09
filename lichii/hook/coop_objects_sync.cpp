// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_objects_sync.h"
#include "coop_world.h"

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

} // namespace

bool IsShared(std::string_view name) {
    return name.starts_with("Enemy_") || CoopWorld::IsBreakable(name);
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
        o.position = s.position;
        if (s.life) {
            if (o.life && s.life->life < *o.life) {
                o.fight_until = now_us + kFightUs;
                const auto owner = owners_.find(o.id);
                if (owner != owners_.end() && owner->second.player != 0 &&
                    owner->second.player != self) {
                    if (o.hit_sum == 0) {
                        o.hit_since = now_us;
                    }
                    o.hit_sum += *o.life - s.life->life;
                }
            }
            o.life = s.life->life;
            o.max = s.life->max;
        }
    }
    std::vector<Object> gone;
    for (auto it = objects_.begin(); it != objects_.end();) {
        if (now_seen.contains(it->first)) {
            ++it;
            continue;
        }
        gone.push_back(std::move(it->second));
        it = objects_.erase(it);
    }
    Flush(self, now_us, gone, send);
    std::sort(fresh.begin(), fresh.end(), [](const Seen* a, const Seen* b) {
        return std::tie(a->name, a->position[0], a->position[2]) <
               std::tie(b->name, b->position[0], b->position[2]);
    });
    for (const Seen* s : fresh) {
        Object o;
        o.address = s->address;
        o.position = s->position;
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

    Apply(self, now_us, send, writes);

    for (auto& [address, o] : objects_) {
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
            const bool changed = life != o.sent_life || Moved(o.position, o.sent_position);
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

// Objects that left our walk. A break read just before it went (a pot's life
// 0, then the pot is removed a few frames later) still reaches the others:
// the owner's last life 0 as a sample, a copy's unsent hit as DAMAGE.
void Sync::Flush(std::uint8_t self, std::uint64_t now_us, std::vector<Object>& gone,
                 std::vector<proto::Message>& send) {
    for (Object& o : gone) {
        const auto owner = owners_.find(o.id);
        if (owner == owners_.end() || owner->second.player == 0) {
            continue;
        }
        if (owner->second.player == self) {
            if (o.life && *o.life <= 0 && (!o.sent_life || o.sent_life->life > 0)) {
                proto::ObjectSample sample;
                sample.id = o.id;
                sample.epoch = owner->second.epoch;
                sample.state = StateOf(o, now_us);
                send.emplace_back(sample);
            }
        } else if (o.hit_sum > 0) {
            proto::Damage d;
            d.id = o.id;
            d.epoch = owner->second.epoch;
            d.amount = o.hit_sum;
            d.cause = static_cast<std::uint8_t>(proto::DamageCause::unknown);
            d.position = o.position;
            send.emplace_back(d);
        }
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
        writes.push_back({o.address, life});
        o.life = life;
    };
    for (const proto::Message& m : inbox) {
        if (const auto* owner = std::get_if<proto::ObjectOwner>(&m)) {
            owners_[owner->id] = {owner->owner, owner->epoch};
        } else if (const auto* sample = std::get_if<proto::ObjectSample>(&m)) {
            Object* o = find(sample->id);
            const auto own = owners_.find(sample->id);
            if (o == nullptr || !sample->state.life || !o->life ||
                (own != owners_.end() && own->second.player == self)) {
                continue;
            }
            const std::int32_t life = sample->state.life->life;
            const bool holding = o->hit_sum > 0 || now_us - o->hit_at < kHitHoldUs;
            if (life < *o->life || (life > *o->life && !holding)) {
                write(*o, life);
                o->fight_until = now_us + kFightUs;
            }
        } else if (const auto* damage = std::get_if<proto::Damage>(&m)) {
            // The server sends DAMAGE only to the owner: that is us.
            Object* o = find(damage->id);
            if (o == nullptr || !o->life || damage->amount <= 0) {
                continue;
            }
            write(*o, std::max(0, *o->life - damage->amount));
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

} // namespace CoopObjects
