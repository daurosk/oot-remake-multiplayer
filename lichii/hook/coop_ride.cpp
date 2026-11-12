// SPDX-License-Identifier: GPL-3.0-or-later
//
// Another player's horse under their puppet (coop_ride.h, issue 199).

#include "coop_ride.h"

#include "coop/host/scene.hpp"
#include "coop_objects_sync.h"  // QuatOf, FrameAfter

#include <cmath>

namespace CoopRide {

namespace proto = coop::protocol;

bool IsHorse(std::string_view actor_name) {
    return actor_name == kHorse;
}

namespace {

// a * b, rows.
Mat3 Mul(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) {
                s += a[r * 3 + k] * b[k * 3 + c];
            }
            out[r * 3 + c] = s;
        }
    }
    return out;
}

Mat3 Transpose(const Mat3& m) {
    return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

Vec3 Apply(const Mat3& m, const Vec3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

bool Finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float Distance(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

constexpr Mat3 kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1};

} // namespace

Mat3 Relative(const Mat3& frame, const Mat3& what) {
    return Mul(Transpose(frame), what);
}
Mat3 Absolute(const Mat3& frame, const Mat3& relative) {
    return Mul(frame, relative);
}
Vec3 ToFrame(const Mat3& frame, const Vec3& world_offset) {
    return Apply(Transpose(frame), world_offset);
}
Vec3 FromFrame(const Mat3& frame, const Vec3& local_offset) {
    return Apply(frame, local_offset);
}

// ---------------------------------------------------------------- Rider

void Rider::Reset() {
    riding_ = false;
    off_left_ = 0;
    last_us_ = 0;
    tried_us_ = 0;
    horse_ = 0;
}

bool Rider::Wants(bool riding, std::uint64_t now_us) const {
    if (!riding && off_left_ == 0 && !riding_) {
        return false;
    }
    return tried_us_ == 0 || now_us >= tried_us_ + kEveryUs || (!riding && riding_);
}

std::optional<proto::PlayerRide> Rider::Tick(bool riding, const std::optional<Vec3>& position,
                                             const std::optional<Quat>& rotation,
                                             const std::vector<HorseSeen>& horses,
                                             std::uint64_t now_us) {
    tried_us_ = now_us;
    if (riding && position && Finite(*position)) {
        if (last_us_ != 0 && now_us < last_us_ + kEveryUs && riding_) {
            return std::nullopt;
        }
        const HorseSeen* best = nullptr;
        float best_d = kNear;
        for (const HorseSeen& h : horses) {
            const float d = Distance(h.position, *position);
            if (Finite(h.position) && d <= best_d) {
                best = &h;
                best_d = d;
            }
        }
        if (best == nullptr) {
            // Getting on (the horse not seen yet) or a horse we do not know:
            // nothing to draw under the puppet; it still shows riding.
            return std::nullopt;
        }
        const Mat3 frame = rotation ? CoopPuppet::QuatToMatrix(*rotation) : kIdentity;
        proto::PlayerRide m;
        m.flags = proto::kRideRiding;
        m.horse = std::string(kHorse);
        m.offset = ToFrame(frame, {best->position[0] - (*position)[0],
                                   best->position[1] - (*position)[1],
                                   best->position[2] - (*position)[2]});
        for (float& f : m.offset) {
            f = std::fmax(-proto::kMaxRideOffset, std::fmin(proto::kMaxRideOffset, f));
        }
        m.rotation = CoopObjects::QuatOf(Relative(frame, best->rotation));
        for (const float f : m.rotation) {
            if (!std::isfinite(f)) {
                return std::nullopt;
            }
        }
        if (best->animation && std::isfinite(best->animation->frame) &&
            std::isfinite(best->animation->rate) && !best->animation->name.empty() &&
            best->animation->name.size() <= proto::kMaxAnimationChars) {
            m.animation = best->animation;
        }
        riding_ = true;
        off_left_ = kOffSends;
        horse_ = best->address;
        last_us_ = now_us;
        return m;
    }
    if (riding_ || off_left_ > 0) {
        if (riding_) {
            riding_ = false;
            horse_ = 0;
        } else if (now_us < last_us_ + kEveryUs) {
            return std::nullopt;
        }
        if (off_left_ == 0) {
            return std::nullopt;
        }
        --off_left_;
        last_us_ = now_us;
        proto::PlayerRide off;
        return off;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- Rides

void Rides::Receive(const proto::PlayerRide& ride, std::uint64_t now_us) {
    if (ride.player_number < 1 || ride.player_number > 4) {
        return;
    }
    std::lock_guard lock(mutex_);
    Ride& r = rides_[ride.player_number];
    const bool was = (r.message.flags & proto::kRideRiding) != 0 && r.at_us != 0 &&
                     now_us < r.at_us + kStaleUs;
    const bool is = (ride.flags & proto::kRideRiding) != 0;
    if (is && (!was || r.message.horse != ride.horse)) {
        ++r.mount;
    }
    r.message = ride;
    r.at_us = now_us;
}

void Rides::Forget(std::uint8_t number) {
    std::lock_guard lock(mutex_);
    rides_.erase(number);
}

std::vector<CoopSpawn::Copy> Rides::Plan(const std::optional<std::string>& scene, bool local_event,
                                         const std::vector<CoopPuppet::Remote>& remotes,
                                         std::uint32_t live_puppets, std::uint64_t now_us) {
    std::vector<CoopSpawn::Copy> out;
    std::map<std::uint8_t, Ride> rides;
    {
        std::lock_guard lock(mutex_);
        rides = rides_;
    }
    std::map<std::uint8_t, bool> shown;
    for (const auto& r : remotes) {
        if (r.number < 1 || r.number > 4) {
            continue;
        }
        const auto it = rides.find(r.number);
        if (it == rides.end()) {
            continue;
        }
        const Ride& ride = it->second;
        const bool fresh = ride.at_us != 0 && now_us < ride.at_us + kStaleUs &&
                           (ride.message.flags & proto::kRideRiding) != 0;
        if (!fresh || (live_puppets & (1u << r.number)) == 0 || !(r.fade > 0.0f) || !scene) {
            continue;
        }
        const std::optional<std::string_view> theirs =
            r.scene ? std::optional<std::string_view>(*r.scene) : std::nullopt;
        const std::optional<std::string_view> anim =
            r.anim ? std::optional<std::string_view>(r.anim->name) : std::nullopt;
        if (coop::host::visibility(std::string_view(*scene), local_event, theirs, r.status, anim) !=
            coop::host::Visibility::ride) {
            continue;  // got off (its command says so first), elsewhere, or our cutscene
        }
        const Mat3 frame = r.rotation ? CoopPuppet::QuatToMatrix(*r.rotation) : kIdentity;
        CoopSpawn::Copy c;
        c.key = kKeyBit | (std::uint64_t(r.number) << 32) | ride.mount;
        c.actor = ride.message.horse;
        const Vec3 d = FromFrame(frame, ride.message.offset);
        c.position = {r.position[0] + d[0], r.position[1] + d[1], r.position[2] + d[2]};
        c.rotation = Absolute(frame, CoopPuppet::QuatToMatrix(ride.message.rotation));
        if (ride.message.animation) {
            proto::Animation a = *ride.message.animation;
            a.frame = CoopObjects::FrameAfter(a.frame, a.rate,
                                              std::int64_t(now_us) - std::int64_t(ride.at_us));
            c.animation = std::move(a);
        }
        out.push_back(std::move(c));
        shown[r.number] = true;
    }
    for (const auto& [number, on] : shown) {
        if (!shown_[number]) {
            changes_.push_back("P" + std::to_string(number) + " rides " +
                               rides[number].message.horse + " (horse copy " +
                               std::string(kHorseStandIn) + ")");
        }
    }
    for (const auto& [number, on] : shown_) {
        if (on && shown.count(number) == 0) {
            changes_.push_back("P" + std::to_string(number) + " off the horse (copy deleted)");
        }
    }
    shown_ = std::move(shown);
    return out;
}

std::vector<std::string> Rides::Changes() {
    std::vector<std::string> out;
    out.swap(changes_);
    return out;
}

} // namespace CoopRide

#ifdef LICHII_COOP

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

namespace CoopRide {
namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

// The live actor list, as coop_spawn.cpp walks it (docs/objects.md).
constexpr u64 kManagerLists = 0x118;
constexpr u64 kListHead = 0x670;
constexpr u64 kListCount = 0x10;
constexpr u64 kNodeOwner = 0x10;
constexpr u64 kActorNode = 0x538;
constexpr u64 kActorName = 0x260;
constexpr u64 kAsSlotRate = 0xd4;
constexpr std::size_t kMaxWalk = 8192;

bool Off() {
    const char* v = std::getenv("LICHII_COOP_RIDE");
    return v != nullptr && std::string_view(v) == "0";
}

template <typename T>
std::optional<T> Get(Core::Memory::Memory& memory, u64 address) {
    if (address < 0x1000 || !memory.IsValidVirtualAddressRange(address, sizeof(T))) {
        return std::nullopt;
    }
    T value{};
    memory.ReadBlockUnsafe(address, &value, sizeof(T));
    return value;
}

std::string Name(Core::Memory::Memory& memory, u64 actor) {
    const auto p = Get<u64>(memory, actor + kActorName);
    std::array<char, 64> buf{};
    if (!p || !memory.IsValidVirtualAddressRange(*p, buf.size())) {
        return "?";
    }
    memory.ReadBlockUnsafe(*p, buf.data(), buf.size() - 1);
    return std::string(buf.data(), strnlen(buf.data(), buf.size()));
}

// The horse's AS slot 0, as coop_spawn reads a bomb's (docs/puppet.md "Animation").
std::optional<proto::Animation> Slot0(Core::Memory::Memory& memory, u64 actor) {
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
    proto::Animation a;
    a.name = std::string(buf.data(), strnlen(buf.data(), buf.size()));
    if (a.name.empty() || a.name.size() > proto::kMaxAnimationChars) {
        return std::nullopt;
    }
    a.frame = *frame;
    a.rate = *rate;
    return a;
}

class Glue {
public:
    void Frame(Core::System& system, u64 main_base, coop::host::Session* session,
               const std::optional<std::string>& scene, bool local_event,
               const std::optional<Vec3>& own_position, const std::optional<Quat>& own_rotation,
               const std::optional<proto::Animation>& own_animation, u64 now_us, u64 server_now_us) {
        if (session == nullptr || main_base == 0 || now_us == 0 || server_now_us == 0) {
            if (drawing_) {
                CoopSpawn::TheDirector().SetRides(0, {});
                drawing_ = false;
            }
            rider_.Reset();
            return;
        }
        main_ = main_base;
        // Ours: send while we ride.
        const bool riding =
            own_animation &&
            coop::host::activity_of(std::string_view(own_animation->name)) == coop::host::Activity::riding;
        if (rider_.Wants(riding, now_us)) {
            std::vector<HorseSeen> horses;
            if (riding) {
                Walk(system.ApplicationMemory(), horses);
            }
            const bool was = rider_.Riding();
            if (auto m = rider_.Tick(riding, own_position, own_rotation, horses, now_us)) {
                const bool sent = session->with_client([&](coop::net::Client& c) { return c.send(*m); });
                if (rider_.Riding() != was || (!was && !rider_.Riding() && !logged_off_)) {
                    LOG_INFO(Core, "coop: ride: we {} ({} horse{} seen){}",
                             rider_.Riding() ? "ride Animal_Epona_Adult, sending PLAYER_RIDE"
                                             : "got off, riding off sent",
                             horses.size(), horses.size() == 1 ? "" : "s", sent ? "" : " (not sent)");
                    logged_off_ = !rider_.Riding();
                }
            } else if (riding && !rider_.Riding() && !logged_none_) {
                LOG_INFO(Core, "coop: ride: we ride ('{}') but no {} within {:.0f} m on the list ({} seen)",
                         own_animation->name, kHorse, Rider::kNear, horses.size());
                logged_none_ = true;
            }
            if (rider_.Riding()) {
                logged_none_ = false;
            }
        }
        // Theirs: a horse under each riding puppet.
        if (!CoopPuppet::LivePuppets() && !drawing_) {
            return;
        }
        CoopSpawn::UseDirector();
        auto copies = rides_.Plan(scene, local_event, CoopPuppet::Collect(session, server_now_us),
                                  CoopPuppet::LivePuppets(), now_us);
        for (const auto& line : rides_.Changes()) {
            LOG_INFO(Core, "coop: ride: {}", line);
        }
        drawing_ = !copies.empty();
        CoopSpawn::TheDirector().SetRides(main_base, std::move(copies));
    }

    void Receive(const coop::net::Event& event, u64 now_us) {
        if (event.kind == coop::net::EventKind::player_left) {
            rides_.Forget(event.player);
            return;
        }
        if (event.kind != coop::net::EventKind::message) {
            return;
        }
        if (const auto* r = std::get_if<proto::PlayerRide>(&event.message)) {
            rides_.Receive(*r, now_us);
        }
    }

private:
    // The horses on our actor list (name cached by address).
    void Walk(Core::Memory::Memory& memory, std::vector<HorseSeen>& out) {
        const auto manager = Get<u64>(memory, main_ + CoopPuppet::addr::kActorManagerPtr);
        const auto lists =
            manager && *manager != 0 ? Get<u64>(memory, *manager + kManagerLists) : std::nullopt;
        if (!lists || *lists == 0) {
            return;
        }
        const u64 head = *lists + kListHead;
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
                if (IsHorse(name)) {
                    HorseSeen h;
                    h.address = a;
                    for (std::size_t i = 0; i < 3; ++i) {
                        h.position[i] =
                            Get<float>(memory, a + CoopPuppet::addr::kActorPosition + 4 * i).value_or(0.0f);
                    }
                    if (const u64 r = a + CoopPuppet::addr::kActorRotation;
                        memory.IsValidVirtualAddressRange(r, sizeof(Mat3))) {
                        memory.ReadBlockUnsafe(r, h.rotation.data(), sizeof(Mat3));
                    }
                    h.animation = Slot0(memory, a);
                    out.push_back(std::move(h));
                }
                names.emplace(a, std::move(name));
            }
            node = Get<u64>(memory, *node);
        }
        names_ = std::move(names);
    }

    u64 main_ = 0;
    Rider rider_;
    Rides rides_;
    bool drawing_ = false;
    bool logged_none_ = false;
    bool logged_off_ = true;
    std::unordered_map<u64, std::string> names_;
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

std::uint64_t SteadyUs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

} // namespace

void Frame(Core::System& system, std::uint64_t main_base, coop::host::Session* session,
           const std::optional<std::string>& scene, bool local_event,
           const std::optional<Vec3>& own_position, const std::optional<Quat>& own_rotation,
           const std::optional<proto::Animation>& own_animation, std::uint64_t server_now_us) {
    static const bool off = Off();
    if (off) {
        return;
    }
    // Steady time on both sides: Receive stamps with it too (the network thread).
    TheGlue().Frame(system, main_base, session, scene, local_event, own_position, own_rotation,
                    own_animation, SteadyUs(), server_now_us);
}

void OnEvent(const coop::net::Event& event) {
    static const bool off = Off();
    if (!off) {
        TheGlue().Receive(event, SteadyUs());
    }
}

} // namespace CoopRide

#endif  // LICHII_COOP
