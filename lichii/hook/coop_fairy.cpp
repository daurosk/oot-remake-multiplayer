// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_fairy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

#include "coop_pose.h"

namespace CoopFairy {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float Distance(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

} // namespace

Vec3 Follower::Goal(const CoopPuppet::Target& puppet, float t, std::uint8_t number) const {
    // Each player's fairy is a little out of step with the others.
    const float phase = static_cast<float>(number) * 1.7f;
    Vec3 local = settings_.offset;
    const std::string_view actor = puppet.actor;
    if (actor.size() >= 6 && actor.substr(actor.size() - 6) == "_Adult") {
        for (float& v : local) {
            v *= settings_.adult_scale;
        }
    }
    local[0] += settings_.circle * std::cos(kTwoPi * settings_.circle_hz * t + phase);
    local[2] += settings_.circle * std::sin(kTwoPi * settings_.circle_hz * t + phase);
    local[1] += settings_.bob * std::sin(kTwoPi * settings_.bob_hz * t + phase);
    const auto& m = puppet.rotation;  // rows
    Vec3 out = puppet.position;
    for (int i = 0; i < 3; ++i) {
        out[i] += m[i * 3 + 0] * local[0] + m[i * 3 + 1] * local[1] + m[i * 3 + 2] * local[2];
    }
    return out;
}

CoopPuppet::Input Follower::Follow(const CoopPuppet::Input& puppets, std::uint64_t now_ms,
                                   std::uint32_t live, const std::optional<Vec3>& local) {
    CoopPuppet::Input out;
    out.main_base = puppets.main_base;
    out.scene = puppets.scene;
    // The puppets' wait for the drawing after a load: a fairy made during the
    // loading screen is refused or never drawn (issue 93), so she waits too.
    out.camera = puppets.camera;
    out.drawn_since_ms = puppets.drawn_since_ms;
    out.settle_ms = puppets.settle_ms;
    if (puppets.scene != scene_) {
        scene_ = puppets.scene;
        tracks_.clear();
        far_.clear();
    }
    const float t = static_cast<float>(now_ms % 3600000ULL) / 1000.0f;
    std::map<std::uint8_t, Track> kept;
    std::map<std::uint8_t, bool> far_kept;
    for (const auto& p : puppets.targets) {
        const Vec3 goal = Goal(p, t, p.number);
        Track track{goal, now_ms};
        if (const auto it = tracks_.find(p.number); it != tracks_.end()) {
            const Track& was = it->second;
            const float dt =
                now_ms > was.ms ? static_cast<float>(now_ms - was.ms) / 1000.0f : 0.0f;
            if (Distance(was.at, goal) <= settings_.snap) {
                const float k = 1.0f - std::exp(-settings_.follow_rate * std::min(dt, 0.5f));
                for (int i = 0; i < 3; ++i) {
                    track.at[i] = was.at[i] + (goal[i] - was.at[i]) * k;
                }
            }
        }
        kept[p.number] = track;
        CoopPuppet::Target f;
        f.number = p.number;
        f.actor = settings_.actor;
        f.position = track.at;
        f.rotation = p.rotation;
        f.show = p.show;  // hidden, held or shown with her puppet
        f.why = p.why;
        // Issue 145: never without her puppet. Placed from the puppet's
        // current spot either way (parked under it), so she shows there
        // when she may.
        bool far = false;
        if (local && settings_.far > 0.0f) {
            const auto it = far_.find(p.number);
            const bool was_far = it != far_.end() && it->second;
            const float d = Distance(*local, p.position);
            far = was_far ? d > settings_.near : d > settings_.far;
        }
        far_kept[p.number] = far;
        if (p.show != CoopPuppet::Show::hide) {
            const bool puppet_live = p.number < 32 && (live >> p.number & 1u) != 0;
            if (!puppet_live) {
                f.show = CoopPuppet::Show::hide;
                f.why = "her puppet is not shown";
            } else if (far) {
                f.show = CoopPuppet::Show::hide;
                f.why = "her puppet is far away";
            }
        }
        out.targets.push_back(std::move(f));
    }
    tracks_ = std::move(kept);
    far_ = std::move(far_kept);
    return out;
}

namespace {

template <typename T>
bool ReadValue(CoopPuppet::Guest& guest, std::uint64_t at, T& out) {
    return at != 0 && guest.Read(at, &out, sizeof(T));
}

bool NameIs(CoopPuppet::Guest& guest, std::uint64_t actor, std::string_view name) {
    std::uint64_t text = 0;
    std::array<char, 64> buf{};
    if (name.size() >= buf.size() || !ReadValue(guest, actor + addr::kActorName, text) ||
        !guest.Read(text, buf.data(), name.size() + 1)) {
        return false;
    }
    return std::string_view(buf.data(), name.size()) == name && buf[name.size()] == '\0';
}

bool Present(CoopPuppet::Guest& guest, std::uint64_t actor) {
    std::uint32_t state = 0;
    return ReadValue(guest, actor + addr::kActorState, state) && state == addr::kStatePresent;
}

std::string Hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

std::uint64_t FindActor(CoopPuppet::Guest& guest, std::uint64_t main_base, std::string_view name) {
    std::uint64_t manager = 0, lists = 0;
    if (main_base == 0 || !ReadValue(guest, main_base + addr::kManagerPtr, manager) ||
        !ReadValue(guest, manager + addr::kLists, lists) || lists == 0) {
        return 0;
    }
    const std::uint64_t head = lists + addr::kListHead;
    std::uint64_t node = 0;
    if (!ReadValue(guest, head, node)) {
        return 0;
    }
    for (std::size_t walked = 0; node != 0 && node != head && walked < addr::kMaxWalk; ++walked) {
        std::uint64_t owner = 0;
        if (ReadValue(guest, node + addr::kNodeOwner, owner) && owner != 0 &&
            owner + addr::kActorNode == node && Present(guest, owner) &&
            NameIs(guest, owner, name)) {
            return owner;
        }
        if (!ReadValue(guest, node, node)) {
            return 0;
        }
    }
    return 0;
}

bool Wings::StillNavi(CoopPuppet::Guest& guest) {
    return navi_ != 0 && Present(guest, navi_) && NameIs(guest, navi_, source_);
}

void Wings::Frame(CoopPuppet::Guest& guest, std::uint64_t main_base,
                  const std::vector<std::uint64_t>& fairies, std::uint64_t now_ms) {
    if (main_base == 0 || fairies.empty()) {
        return;
    }
    if (navi_ != 0 && !StillNavi(guest)) {
        Say("wings: " + source_ + " " + Hex(navi_) + " gone");
        navi_ = 0;
    }
    if (navi_ == 0) {
        if (searched_ && now_ms - std::min(now_ms, search_ms_) < kSearchMs) {
            return;
        }
        searched_ = true;
        search_ms_ = now_ms;
        navi_ = FindActor(guest, main_base, source_);
        if (navi_ == 0) {
            return;
        }
        Say("wings: copying " + source_ + " " + Hex(navi_) + " onto the fairies");
    }
    if (notes_.size() > 16) {
        notes_.clear();  // fairies come and go; a fresh map logs each again once
    }
    for (const std::uint64_t fairy : fairies) {
        std::string note;
        const bool copied = fairy != 0 && CoopPose::CopySkeleton(guest, navi_, fairy, &note);
        if (copied) {
            ++copies_;
        }
        std::string& last = notes_[fairy];
        const std::string line = (copied ? "copied " : "not copied: ") + note;
        if (line != last) {
            last = line;
            Say("wings: fairy " + Hex(fairy) + " " + line);
        }
    }
}

} // namespace CoopFairy

#ifdef LICHII_COOP

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/presence/presence.hpp"
#include "coop_svc.h"

namespace CoopFairy {

namespace {

std::string Env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

bool EnvOn(const char* name) {
    const auto v = Env(name);
    return !v.empty() && v != "0";
}

struct Glue {
    bool on = false;
    std::unique_ptr<CoopPuppet::Director> director;
    std::unique_ptr<CoopPuppet::Planner> planner;  // the present thread only
    std::unique_ptr<Follower> follower;            // the present thread only
    std::unique_ptr<Wings> wings;                  // the calc thread only
    std::atomic<std::uint64_t> main_base{0};       // as last published

    Glue() {
        // The cave is there only with the puppets; the fairy follows them.
        const bool puppets = EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET");
        if (!puppets || Env("LICHII_COOP_FAIRY") == "0") {
            LOG_INFO(Core, "coop: fairies off");
            return;
        }
        Settings settings;
        if (const auto actor = Env("LICHII_COOP_FAIRY_ACTOR"); !actor.empty()) {
            settings.actor = actor;
        }
        // The same plan as the puppets' (CoopSvc's ConfigurePuppets); the
        // actor name is not used, the fairy has its own.
        CoopPuppet::PlannerSettings puppet_settings;
        puppet_settings.debug = EnvOn("LICHII_COOP_TEST_PUPPET");
        LOG_INFO(Core, "coop: fairies on, actor {}", settings.actor);
        director = std::make_unique<CoopPuppet::Director>(
            [](const std::string& line) { LOG_INFO(Core, "fairy {}", line); },
            EnvOn("LICHII_COOP_DEBUG_PUPPET"));
        // Navi has a physics body: a plain write of her place is undone.
        director->SetMatrixCall(true);
        planner = std::make_unique<CoopPuppet::Planner>(puppet_settings);
        follower = std::make_unique<Follower>(std::move(settings));
        // LICHII_COOP_FAIRY_WINGS=0: no wing beat (the fairies hang still).
        if (Env("LICHII_COOP_FAIRY_WINGS") != "0") {
            wings = std::make_unique<Wings>(
                "Npc_Navi", [](const std::string& line) { LOG_INFO(Core, "fairy {}", line); });
        }
        CoopSvc::AddCaller(kCallerOrder, [this](CoopPuppet::Guest& guest, bool after_call,
                                                std::uint64_t result, std::uint64_t now_ms) {
            auto call = director->Step(guest, after_call, result, now_ms);
            if (!after_call && wings) {
                // The start of a frame: the director has just found its live
                // fairies, and the copy is made before any call of ours.
                std::vector<std::uint64_t> fairies;
                for (const auto& slot : director->Slots()) {
                    if (slot.phase == CoopPuppet::Phase::live && slot.actor != 0) {
                        fairies.push_back(slot.actor);
                    }
                }
                wings->Frame(guest, main_base.load(), fairies, now_ms);
            }
            return call;
        });
        on = true;
    }
};

Glue& TheGlue() {
    static Glue glue;  // kept for the whole run: the cave holds its director
    return glue;
}

} // namespace

void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us) {
    Glue& glue = TheGlue();
    if (!glue.on) {
        return;
    }
    // The same remotes CoopPuppet::Publish plans from, so the plans agree.
    const std::vector<CoopPuppet::Remote> remotes = CoopPuppet::Collect(session, server_now_us);
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    glue.main_base.store(main_base);
    auto puppets = glue.planner->Plan(main_base, scene, position, local_event, remotes, now_ms);
    CoopPuppet::StampDrawing(puppets, now_ms);
    glue.director->SetInput(
        glue.follower->Follow(puppets, now_ms, CoopPuppet::LivePuppets(), position));
}

} // namespace CoopFairy

#endif  // LICHII_COOP
