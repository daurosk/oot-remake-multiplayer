// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_fairy.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

CoopPuppet::Input Follower::Follow(const CoopPuppet::Input& puppets, std::uint64_t now_ms) {
    CoopPuppet::Input out;
    out.main_base = puppets.main_base;
    out.scene = puppets.scene;
    if (puppets.scene != scene_) {
        scene_ = puppets.scene;
        tracks_.clear();
    }
    const float t = static_cast<float>(now_ms % 3600000ULL) / 1000.0f;
    std::map<std::uint8_t, Track> kept;
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
        out.targets.push_back(std::move(f));
    }
    tracks_ = std::move(kept);
    return out;
}

} // namespace CoopFairy

#ifdef LICHII_COOP

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
        puppet_settings.debug = EnvOn("LICHII_COOP_DEBUG_PUPPET");
        LOG_INFO(Core, "coop: fairies on, actor {}", settings.actor);
        director = std::make_unique<CoopPuppet::Director>(
            [](const std::string& line) { LOG_INFO(Core, "fairy {}", line); },
            puppet_settings.debug);
        planner = std::make_unique<CoopPuppet::Planner>(puppet_settings);
        follower = std::make_unique<Follower>(std::move(settings));
        CoopSvc::AddCaller(kCallerOrder,
                           [d = director.get()](CoopPuppet::Guest& guest, bool after_call,
                                                std::uint64_t result, std::uint64_t now_ms) {
                               return d->Step(guest, after_call, result, now_ms);
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
             const std::optional<Vec3>& position, coop::host::Session* session,
             std::uint64_t server_now_us) {
    Glue& glue = TheGlue();
    if (!glue.on) {
        return;
    }
    // The same remotes CoopPuppet::Publish reads, so the plans agree.
    std::vector<CoopPuppet::Remote> remotes;
    if (session != nullptr && server_now_us != 0) {
        session->with_presence([&](coop::presence::Presence& p) {
            for (const std::uint8_t n : p.players()) {
                const auto pose = p.shown(n, server_now_us);
                if (!pose) {
                    continue;
                }
                CoopPuppet::Remote r;
                r.number = n;
                r.position = pose->position;
                r.rotation = pose->rotation;
                r.fade = pose->fade;
                if (const auto* player = p.player(n)) {
                    r.scene = player->scene();
                }
                remotes.push_back(std::move(r));
            }
        });
    }
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const auto puppets = glue.planner->Plan(main_base, scene, position, remotes, now_ms);
    glue.director->SetInput(glue.follower->Follow(puppets, now_ms));
}

} // namespace CoopFairy

#endif  // LICHII_COOP
