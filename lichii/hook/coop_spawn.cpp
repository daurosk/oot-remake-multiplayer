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
constexpr std::array<Kind, 8> kKinds{{
    {"Item_Bomb", true, true},
    {"Item_Bombchu", true, true},
    {"Item_DekuNut", false, true},
    {"Shootable_DekuSeed", false, true},
    {"NormalArrow", false, true},
    {"Weapon_Boomerang", false, true},
    {"Shootable_Hook", false, true},
    {"Shootable_HookLong", false, true},
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

void Sync::Reset() {
    scene_.clear();
    first_walk_ = true;
    old_.clear();
    mine_.clear();
    copies_.clear();
    copy_addresses_.clear();
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
                m.top_speed = std::max(m.top_speed, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            }
            m.far = std::max(m.far, Distance(m.position, m.made_position));
            m.seen_at = now_us;
            m.missed = 0;
            const bool moved = Distance(m.position, m.sent_position) > 0.01f;
            if (now_us - m.sent_at >= kSampleEveryUs && (moved || now_us - m.sent_at >= kStillEveryUs)) {
                proto::ObjectSample out;
                out.id = m.id;
                out.state.time = {now_us, ++m.sequence};
                out.state.position = m.position;
                out.state.velocity = m.velocity;
                out.state.rotation = m.rotation;
                if (Explodes(m.name)) {
                    out.state.fuse = fuse_.Left(now_us - m.made_at);
                }
                send.emplace_back(std::move(out));
                m.sent_at = now_us;
                m.sent_position = m.position;
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
        }
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
        if (out.reason == proto::RemoveReason::exploded && CoopProjectiles::Find(m.name)->fused) {
            fuse_.Lived(lived);  // the game's fuse, for our next bombs' copies
        }
        out.position = m.position;
        send.emplace_back(out);
        flights_.push_back({m.id, m.name, lived, m.far, m.top_speed, out.reason});
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
        Copy c;
        c.key = r.key;
        c.actor = r.actor;
        c.position = r.track.At(now_us, info != nullptr && info->ballistic);
        if (r.rotation) {
            c.rotation = CoopPuppet::QuatToMatrix(*r.rotation);
        }
        c.setter = setter_;
        const bool explode = due || (r.removed && *r.removed == proto::RemoveReason::exploded);
        c.act = explode ? Act::explode : !r.removed ? Act::show : Act::remove;
        if (explode && !r.blasted) {
            CoopProjectiles::TheBlasts().Add(c.position, now_us);
            r.blasted = true;
        }
        copies.push_back(std::move(c));
        ++it;
    }
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
                const std::vector<std::uint8_t> zero(kSlots * kSlotSize, 0);
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
    return NextSetter(guest);
}

std::string_view StandIn(std::string_view actor) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kStandIns{{
        {"Shootable_DekuSeed", "CoopShot_Seed"},
        {"NormalArrow", "CoopShot_Arrow"},
        {"Shootable_Hook", "CoopShot_Hook"},
        {"Shootable_HookLong", "CoopShot_HookLong"},
    }};
    for (const auto& [shared, stand_in] : kStandIns) {
        if (shared == actor) {
            return stand_in;
        }
    }
    return {};
}

std::set<std::string> StandInsIn(std::string_view manifest) {
    std::set<std::string> out;
    for (const std::string_view shared :
         {"Shootable_DekuSeed", "NormalArrow", "Shootable_Hook", "Shootable_HookLong"}) {
        const std::string quoted = "\"" + std::string(StandIn(shared)) + "\"";
        if (manifest.find(quoted) != std::string_view::npos) {
            out.insert(std::string(StandIn(shared)));
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

void Director::NewFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    namespace addr = CoopPuppet::addr;
    calls_ = 0;
    setters_.clear();
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
        c.x[0] = kSlots * kSlotSize;
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
        std::vector<coop::protocol::Message> send;
        std::vector<Copy> copies;
        sync_.Tick(*scene, self, now_us, seen, TheDirector().Live(), send, copies);
        TheDirector().SetInput(main_, std::move(copies));
        if (const auto fuse = sync_.Fuse().Fuse(); fuse && fuse != logged_fuse_) {
            LOG_INFO(Core, "coop: shots: our bombs' fuse is {:.2f} s", double(*fuse) / 1e6);
            logged_fuse_ = fuse;
        }
        if (sync_.BlownOnTime() != logged_blown_) {
            logged_blown_ = sync_.BlownOnTime();
            LOG_INFO(Core, "coop: shots: bomb copy blown on the owner's fuse ({} so far)",
                     logged_blown_);
        }
        for (const auto& f : sync_.Flights()) {
            LOG_INFO(Core,
                     "coop: shots: our {} #{:#x} gone (reason {}) after {} ms: went {:.2f} m from "
                     "where it was made, top speed {:.1f} m/s",
                     f.name, f.id.value, static_cast<int>(f.reason), f.lived_us / 1000, f.far,
                     f.top_speed);
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
    std::optional<std::uint64_t> logged_fuse_;
    std::size_t logged_blown_ = 0;
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
