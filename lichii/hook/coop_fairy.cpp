// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_fairy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

#include "coop/host/session.hpp"
#include "coop/presence/presence.hpp"

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

std::optional<CoopPuppet::Call> Chain::Next(const Step& first, const Step& second,
                                            bool after_call, std::uint64_t result) {
    if (!after_call) {
        last_ = Who::none;
        second_started_ = false;
    } else if (last_ == Who::none) {
        return std::nullopt;  // no call was handed out this frame
    }
    if (last_ != Who::second) {
        if (auto call = first(after_call, result)) {
            last_ = Who::first;
            return call;
        }
    }
    const bool resumed = second_started_;
    second_started_ = true;
    if (auto call = second(resumed, result)) {
        last_ = Who::second;
        return call;
    }
    last_ = Who::none;
    return std::nullopt;
}

namespace {

std::once_flag g_once;
std::atomic<bool> g_on = false;
std::unique_ptr<CoopPuppet::Director> g_director;
std::unique_ptr<CoopPuppet::Planner> g_planner;  // the present thread only
std::unique_ptr<Follower> g_follower;            // the present thread only
Chain g_chain;                                   // the calc thread only

bool EnvOff(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool off = std::string(value) == "0";
    std::free(value);
    return off;
#else
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "0";
#endif
}

} // namespace

void Configure(const CoopPuppet::PlannerSettings& puppets, Settings settings, bool verbose,
               CoopPuppet::Director::Log log) {
    if (EnvOff("LICHII_COOP_FAIRY")) {
        return;
    }
    std::call_once(g_once, [&] {
        auto tagged = [log = std::move(log)](const std::string& line) {
            if (log) {
                log("fairy " + line);
            }
        };
        g_director = std::make_unique<CoopPuppet::Director>(std::move(tagged), verbose);
        g_planner = std::make_unique<CoopPuppet::Planner>(puppets);
        g_follower = std::make_unique<Follower>(std::move(settings));
        g_on = true;
    });
}

bool On() {
    return g_on;
}

std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                     std::uint64_t result, std::uint64_t now_ms) {
    auto& puppets = CoopPuppet::TheDirector();
    if (!g_on) {
        return puppets.Step(guest, after_call, result, now_ms);
    }
    return g_chain.Next(
        [&](bool after, std::uint64_t r) { return puppets.Step(guest, after, r, now_ms); },
        [&](bool after, std::uint64_t r) { return g_director->Step(guest, after, r, now_ms); },
        after_call, result);
}

void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, coop::host::Session* session,
             std::uint64_t server_now_us) {
    if (!g_on) {
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
    const auto puppets = g_planner->Plan(main_base, scene, position, remotes, now_ms);
    g_director->SetInput(g_follower->Follow(puppets, now_ms));
}

} // namespace CoopFairy
