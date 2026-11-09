// SPDX-License-Identifier: GPL-3.0-or-later
//
// Things a player makes, shown in every game (coop_spawn.h, docs/spawns.md).
// Sync and Director need no emulator code and are tested in
// tests/lichii/spawn_test.cpp; the hook glue at the end is built into the
// emulator only (LICHII_COOP).

#include "coop_spawn.h"

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
            r.position = *s->state.position;
            r.velocity = s->state.velocity.value_or(Vec3{});
            r.sample_us = s->state.time.server_time_us;
            r.got_us = now_us;
            copies_.emplace(s->id, std::move(r));
        } else if (const auto* p = std::get_if<proto::ObjectSample>(&m)) {
            auto it = copies_.find(p->id);
            if (it == copies_.end() || it->second.removed || !p->state.position ||
                !Finite(*p->state.position) || p->state.time.server_time_us < it->second.sample_us) {
                continue;
            }
            it->second.position = *p->state.position;
            it->second.velocity = p->state.velocity.value_or(Vec3{});
            it->second.sample_us = p->state.time.server_time_us;
            it->second.got_us = now_us;
        } else if (const auto* g = std::get_if<proto::ObjectRemove>(&m)) {
            auto it = copies_.find(g->id);
            if (it == copies_.end() || it->second.removed) {
                continue;
            }
            it->second.removed = g->reason;
            if (Finite(g->position)) {
                it->second.position = g->position;
            }
            it->second.velocity = {};
            it->second.got_us = now_us;
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

    for (auto& [id, r] : copies_) {
        if (const auto it = copies_live.find(r.key); it != copies_live.end() && it->second != 0) {
            r.address = it->second;
            copy_addresses_.insert(r.address);
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
            m.seen_at = now_us;
            m.missed = 0;
            const bool moved = Distance(m.position, m.sent_position) > 0.01f;
            if (now_us - m.sent_at >= kSampleEveryUs && (moved || now_us - m.sent_at >= kStillEveryUs)) {
                proto::ObjectSample out;
                out.id = m.id;
                out.state.time = {now_us, ++m.sequence};
                out.state.position = m.position;
                out.state.velocity = m.velocity;
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
            if (r.address == 0 && r.actor == s.name && Distance(r.position, s.position) < kClaimDistance) {
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
        m.position = m.sent_position = s.position;
        m.seen_at = m.sent_at = now_us;
        proto::ObjectSpawn out;
        out.id = m.id;
        out.actor = s.name;
        out.flags = Flies(s.name) ? proto::object_spawn::projectile : 0;
        out.state.time = {now_us, 0};
        out.state.position = s.position;
        out.state.velocity = Vec3{};
        send.emplace_back(std::move(out));
        mine_[s.address] = std::move(m);
    }

    for (auto it = mine_.begin(); it != mine_.end();) {
        if (on_list.count(it->first) != 0 || ++it->second.missed < kGoneWalks) {
            ++it;
            continue;
        }
        proto::ObjectRemove out;
        out.id = it->second.id;
        out.reason = Explodes(it->second.name) ? proto::RemoveReason::exploded
                                               : proto::RemoveReason::gone;
        out.position = it->second.position;
        send.emplace_back(out);
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
        Copy c;
        c.key = r.key;
        c.actor = r.actor;
        const float ahead = float(std::min(age, kAheadMaxUs)) / 1e6f;
        for (int i = 0; i < 3; ++i) {
            c.position[i] = r.position[i] + r.velocity[i] * ahead;
        }
        c.act = !r.removed ? Act::show
                : *r.removed == proto::RemoveReason::exploded ? Act::explode
                                                               : Act::remove;
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

std::map<std::uint64_t, std::uint64_t> Director::Live() const {
    std::lock_guard lock(mutex_);
    return live_;
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
    if (base_ == 0 || calls_ >= kMaxCallsPerFrame) {
        return std::nullopt;
    }
    auto call = Next(guest, now_ms);
    if (call) {
        ++calls_;
    }
    return call;
}

void Director::NewFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    namespace addr = CoopPuppet::addr;
    calls_ = 0;
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
        base = next_base_;
        copies_ = next_;
    }
    if (base != base_) {
        // A new game (or none): nothing we had is ours any more.
        base_ = base;
        block_ = 0;
        allocating_ = false;
        started_.clear();
        slots_.assign(kSlots, Slot{});
    }
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
            } else if (const Copy* c = Wanted(slot.key); c != nullptr && c->act != Act::remove) {
                WriteValue(guest, slot.taken + addr::kActorPosition, c->position);
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
        return c.act == Act::show && started_.count(c.key) == 0;
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
            if (want == nullptr || want->act == Act::remove) {
                CoopPuppet::Call c;
                c.function = base_ + addr::kRequestDelete;
                c.x[0] = slot.taken;
                c.x[1] = 0;
                Say(slot.actor + ": copy " + std::to_string(slot.key) + " deleted");
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
        if (copy.act != Act::show || started_.count(copy.key) != 0) {
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
        const std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
        const Vec3 scale{1.0f, 1.0f, 1.0f};
        std::memcpy(arg.data() + 0x00, copy.position.data(), 12);
        std::memcpy(arg.data() + 0x0c, rotation.data(), 36);
        std::memcpy(arg.data() + 0x30, scale.data(), 12);
        std::memcpy(arg.data() + 0x40, &create_default, 8);
        arg[0x98] = 1;
        std::array<char, 0x40> text{};
        std::memcpy(text.data(), copy.actor.data(), std::min<std::size_t>(copy.actor.size(), 0x3f));
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
        free->actor = copy.actor;
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
        Say(copy.actor + ": create copy " + std::to_string(copy.key));
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
#include <unordered_map>

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
        if (send.empty()) {
            return;
        }
        session->with_client([&](coop::net::Client& c) {
            for (const auto& m : send) {
                const bool sent = c.send(m);
                if (const auto* s = std::get_if<coop::protocol::ObjectSpawn>(&m)) {
                    const Vec3 at = s->state.position.value_or(Vec3{});
                    LOG_INFO(Core, "coop: spawn: sent {} #{:#x} at {:.2f} {:.2f} {:.2f}{}",
                             s->actor, s->id.value, at[0], at[1], at[2],
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
            LOG_INFO(Core, "coop: spawn: on, main at {:#x}", main_);
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

    Sync sync_;
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

void Frame(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us) {
    if (On()) {
        static const bool registered = [] {
            CoopSvc::AddCaller(10, [](CoopPuppet::Guest& guest, bool after_call,
                                      std::uint64_t result, std::uint64_t now_ms) {
                return TheDirector().Step(guest, after_call, result, now_ms);
            });
            return true;
        }();
        (void)registered;
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
