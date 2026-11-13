// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_platforms.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace CoopPlatforms {
namespace {

// The actor's own place and turn (+0x31c .. +0x34b): the game writes them
// from the clock; they are not the clock.
constexpr std::uint64_t kTransformFrom = 0x318;
constexpr std::uint64_t kTransformTo = 0x350;

bool LooksLikePointer(std::uint64_t w) {
    const std::uint64_t high = w >> 32;
    return high >= 1 && high <= 0xffff && w % 8 == 0;
}

float AsFloat(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

float Distance(const coop::protocol::Vec3& a, const coop::protocol::Vec3& b) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// A rate that reads as seconds (1 a second) or frames (30 or 60 a second):
// the likeliest clocks.
bool ClockLike(float rate) {
    const float r = std::fabs(rate);
    return std::fabs(r - 1.0f) < 0.1f || std::fabs(r - 30.0f) < 3.0f || std::fabs(r - 60.0f) < 6.0f;
}

}  // namespace

bool IsDriver(std::string_view name) {
    if (name.starts_with("GroundControlActor_") || name.starts_with("GrouondControlActor_")) {
        return true;  // the game's movers of floors (GrouondControl: the game's own spelling)
    }
    if (!name.starts_with("Obj_") && !name.starts_with("Land_")) {
        return false;
    }
    static constexpr std::array<std::string_view, 11> kParts{
        "ElevatorLift", "_Elevator_", "MoveFloor", "RotateFloor", "RotateBlade", "MoveSpike",
        "ExtendingPlatform", "_lift_", "ElevatorMirror", "ElevatorPit", "UpDown"};
    for (const std::string_view p : kParts) {
        if (name.find(p) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

std::string Where(std::uint64_t key) {
    const std::uint64_t region = key >> 32;
    const auto offset = static_cast<unsigned long long>(key & 0xffffffffu);
    char text[48];
    if (region == kRegionAsFrame) {
        std::snprintf(text, sizeof text, "as-frame");
    } else if (region == kRegionActor) {
        std::snprintf(text, sizeof text, "actor+0x%llx", offset);
    } else {
        std::snprintf(text, sizeof text, "c+0x%llx+0x%llx",
                      static_cast<unsigned long long>((region - kRegionComponent) * 8), offset);
    }
    return text;
}

Floats FloatsOf(const std::map<std::uint64_t, std::uint64_t>& words) {
    Floats out;
    for (const auto& [key, w] : words) {
        const std::uint64_t region = key >> 32;
        const std::uint64_t offset = key & 0xffffffffu;
        if (region == 1) {
            continue;  // the component table: pointers
        }
        if (region == kRegionAsFrame) {
            out[key] = AsFloat(static_cast<std::uint32_t>(w));  // the frame is in the low half
            continue;
        }
        if (LooksLikePointer(w)) {
            continue;
        }
        for (std::uint64_t half = 0; half < 2; ++half) {
            const std::uint64_t at = offset + 4 * half;
            if (region == kRegionActor && at >= kTransformFrom && at < kTransformTo) {
                continue;
            }
            out[Key(region, at)] = AsFloat(static_cast<std::uint32_t>(w >> (32 * half)));
        }
    }
    return out;
}

void ClockFinder::Add(std::uint64_t now_us, const Floats& floats) {
    jumped_ = false;
    if (!looks_.empty() && now_us <= looks_.back().at_us) {
        return;
    }
    looks_.push_back({now_us, floats});
    while (looks_.size() > kLooks) {
        looks_.pop_front();
    }
    if (looks_.size() < 3) {
        return;
    }
    const Look& a = looks_[looks_.size() - 3];
    const Look& b = looks_[looks_.size() - 2];
    const Look& c = looks_[looks_.size() - 1];
    const float dt1 = static_cast<float>(b.at_us - a.at_us) / 1e6f;
    const float dt2 = static_cast<float>(c.at_us - b.at_us) / 1e6f;
    std::map<std::uint64_t, Candidate> next;
    for (const auto& [key, v2] : c.floats) {
        const auto i0 = a.floats.find(key);
        const auto i1 = b.floats.find(key);
        if (i0 == a.floats.end() || i1 == b.floats.end()) {
            continue;
        }
        const float v0 = i0->second;
        const float v1 = i1->second;
        if (!std::isfinite(v0) || !std::isfinite(v1) || !std::isfinite(v2) ||
            std::fabs(v0) > kMaxValue || std::fabs(v1) > kMaxValue || std::fabs(v2) > kMaxValue) {
            continue;
        }
        const float r1 = (v1 - v0) / dt1;
        const float r2 = (v2 - v1) / dt2;
        const auto in_range = [](float r) {
            return std::fabs(r) >= kMinRate && std::fabs(r) <= kMaxRate;
        };
        const auto it = candidates_.find(key);
        const bool known = it != candidates_.end();
        Candidate cand = known ? it->second : Candidate{};
        const bool agree = in_range(r1) && in_range(r2) && (r1 > 0) == (r2 > 0) &&
                           std::fabs(r1 - r2) <= kRateSlack * std::max(std::fabs(r1), std::fabs(r2));
        if (agree) {
            cand.rate = r2;
            cand.good = std::min(cand.good + 1, 1000);
            cand.bad = 0;
            cand.stalled = 0;
            cand.top = std::max(cand.top, std::max(v1, v2));
            next[key] = cand;
            continue;
        }
        if (!known || cand.good < kKeepAfter) {
            continue;  // a word that does not step steadily: not a clock (yet)
        }
        // A kept clock that did not step at its rate this time. Gone back
        // past where it was: it looped (or a switch reset it), and where it
        // loops is the value it would have had, less the value it has. A
        // stall (a pause, a hitch: about no step) is neither a loop nor a
        // jump to send; the finder lets go of it if it lasts.
        const bool backwards = cand.rate > 0 ? v2 < v1 : v2 > v1;
        const bool stalled = std::fabs(r2) < kMinRate;
        if (stalled) {
            // Held as it is (not a bad step) while the game stands still,
            // but not Found(): nothing frozen is sent.
            if (++cand.stalled <= kMaxStalled) {
                next[key] = cand;
            }
            continue;
        }
        if (backwards && in_range(r1) &&
            std::fabs(r1 - cand.rate) <= kRateSlack * std::fabs(cand.rate)) {
            const float would = v1 + cand.rate * dt2;
            const float cycle = cand.rate > 0 ? would - v2 : v2 - would;
            if (cycle > 0.0f && cycle >= std::fabs(v1) * 0.5f) {
                cand.cycle = cycle;
            }
        }
        if (chosen_ && *chosen_ == key) {
            jumped_ = true;
        }
        if (++cand.bad < kDropAfter) {
            next[key] = cand;  // kept: it may go on stepping from where it jumped to
        }
    }
    candidates_ = std::move(next);
    if (chosen_) {
        const auto it = candidates_.find(*chosen_);
        if (it == candidates_.end()) {
            chosen_.reset();
        }
    }
    if (!chosen_) {
        std::optional<std::uint64_t> best;
        for (const auto& [key, cand] : candidates_) {
            if (cand.good < kKeepAfter || cand.bad != 0) {
                continue;
            }
            if (!best) {
                best = key;
                continue;
            }
            const Candidate& b2 = candidates_.at(*best);
            const bool like = ClockLike(cand.rate);
            const bool best_like = ClockLike(b2.rate);
            if (like != best_like) {
                if (like) {
                    best = key;
                }
            } else if (cand.good > b2.good) {
                best = key;
            }
        }
        chosen_ = best;
    }
}

std::optional<ClockFinder::Clock> ClockFinder::Found() const {
    if (!chosen_ || looks_.empty()) {
        return std::nullopt;
    }
    const auto it = candidates_.find(*chosen_);
    const auto v = looks_.back().floats.find(*chosen_);
    if (it == candidates_.end() || v == looks_.back().floats.end() || it->second.stalled != 0) {
        return std::nullopt;
    }
    Clock c;
    c.key = *chosen_;
    c.rate = it->second.rate;
    c.value = v->second;
    c.cycle = it->second.cycle;
    c.at_us = looks_.back().at_us;
    return c;
}

void ClockFinder::Reset() {
    looks_.clear();
    candidates_.clear();
    chosen_.reset();
    jumped_ = false;
}

float Advance(float value, float rate, float cycle, std::uint64_t sent_us, std::uint64_t now_us) {
    const double elapsed = now_us > sent_us ? static_cast<double>(now_us - sent_us) / 1e6 : 0.0;
    double v = static_cast<double>(value) + static_cast<double>(rate) * elapsed;
    if (cycle > 0.0f) {
        v = std::fmod(v, static_cast<double>(cycle));
        if (v < 0.0) {
            v += cycle;
        }
    }
    return static_cast<float>(v);
}

float DriftSeconds(float ours, float theirs, float rate, float cycle) {
    double d = static_cast<double>(theirs) - static_cast<double>(ours);
    if (cycle > 0.0f) {
        d = std::remainder(d, static_cast<double>(cycle));
    }
    const double r = std::fabs(static_cast<double>(rate));
    return static_cast<float>(std::fabs(d) / (r > 0.0 ? r : 1.0));
}

void PlatformSync::Receive(const coop::protocol::PlatformClock& message) {
    if (message.scene != scene_ || message.player_number == 0) {
        return;
    }
    for (const auto& t : message.ticks) {
        auto& by_player = heard_[t.hash];
        auto [it, fresh] = by_player.try_emplace(message.player_number);
        Heard& h = it->second;
        if (!fresh && message.server_time_us <= h.sent_us) {
            continue;  // late: an older tick than one we have
        }
        if (fresh || message.server_time_us > h.sent_us + kOwnerQuietUs) {
            h.since_us = message.server_time_us;  // a new run of sends
        }
        h.player = message.player_number;
        h.sent_us = message.server_time_us;
        h.got_us = message.server_time_us;  // server time: comparable with ours
        h.tick = t;
    }
}

std::uint8_t PlatformSync::OwnerOf(std::uint64_t hash, std::uint8_t self, std::uint64_t server_us) const {
    // The one that has sent the longest without a break keeps it (a game
    // that comes later follows); two that began together: the lower number.
    std::uint8_t owner = 0;
    std::uint64_t owner_since = 0;
    const auto consider = [&](std::uint8_t player, std::uint64_t since) {
        constexpr std::uint64_t kTogetherUs = 200'000;
        const bool earlier = since + kTogetherUs < owner_since;
        const bool together = !earlier && since <= owner_since + kTogetherUs;
        if (owner == 0 || earlier || (together && player < owner)) {
            owner = player;
            owner_since = since;
        }
    };
    if (const auto it = states_.find(hash); it != states_.end() && it->second.since_us != 0 &&
                                            server_us <= it->second.last_send_us + kOwnerQuietUs) {
        consider(self, it->second.since_us);
    }
    if (const auto it = heard_.find(hash); it != heard_.end()) {
        for (const auto& [player, h] : it->second) {
            if (player != self && server_us <= h.got_us + kOwnerQuietUs) {
                consider(player, h.since_us);
            }
        }
    }
    return owner == 0 ? self : owner;
}

void PlatformSync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t steady_us,
                        std::uint64_t server_us, const std::vector<Driver>& drivers,
                        const std::optional<coop::protocol::Vec3>& player, const ReadDriver& read,
                        bool probe_only, Out& out) {
    if (scene != scene_) {
        Reset();
        scene_ = scene;
    }
    for (auto it = heard_.begin(); it != heard_.end();) {
        std::erase_if(it->second, [&](const auto& p) { return server_us > p.second.got_us + kStaleUs; });
        it = it->second.empty() ? heard_.erase(it) : std::next(it);
    }
    if (!player || self == 0) {
        return;
    }
    std::vector<std::pair<float, const Driver*>> near;
    for (const Driver& d : drivers) {
        if (d.hash == 0 || d.hash == ~std::uint64_t{0} || !IsDriver(d.name)) {
            continue;
        }
        const float m = Distance(d.position, *player);
        if (m <= kNearM) {
            near.emplace_back(m, &d);
        }
    }
    std::sort(near.begin(), near.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (near.size() > kMaxWatched) {
        near.resize(kMaxWatched);
    }
    coop::protocol::PlatformClock message;
    message.scene = scene;
    message.server_time_us = server_us;
    for (const auto& [m, d] : near) {
        State& s = states_[d->hash];
        if (s.last_look_us != 0 && steady_us < s.last_look_us + ClockFinder::kLookEveryUs) {
            continue;
        }
        const auto words = read(*d);
        if (words.empty()) {
            continue;
        }
        s.last_look_us = steady_us;
        const Floats floats = FloatsOf(words);
        s.finder.Add(steady_us, floats);
        const std::uint8_t owner = OwnerOf(d->hash, self, server_us);
        if (owner == self) {
            const auto clock = s.finder.Found();
            if (!clock) {
                continue;
            }
            if (!s.logged_clock) {
                s.logged_clock = true;
                char text[200];
                std::snprintf(text, sizeof text, "%016llx %s: clock %s, %.3f a second, cycle %.3f: ours to send",
                              static_cast<unsigned long long>(d->hash), d->name.c_str(),
                              Where(clock->key).c_str(), static_cast<double>(clock->rate),
                              static_cast<double>(clock->cycle));
                out.notes.emplace_back(text);
            }
            if (s.finder.Jumped() || s.last_send_us == 0 || server_us >= s.last_send_us + kSendEveryUs) {
                if (message.ticks.size() < coop::protocol::kMaxPlatformTicks) {
                    if (s.since_us == 0 || server_us > s.last_send_us + kOwnerQuietUs) {
                        s.since_us = server_us;
                    }
                    s.last_send_us = server_us;
                    message.ticks.push_back({d->hash, clock->key, clock->value, clock->rate, clock->cycle});
                }
            }
            continue;
        }
        s.since_us = 0;  // another game has it
        if (probe_only || s.gave_up) {
            continue;
        }
        const auto hit = heard_.find(d->hash);
        if (hit == heard_.end()) {
            continue;
        }
        const auto hp = hit->second.find(owner);
        if (hp == hit->second.end()) {
            continue;
        }
        const Heard& h = hp->second;
        const auto mine = floats.find(h.tick.key);
        if (mine == floats.end() || !std::isfinite(mine->second)) {
            continue;
        }
        const float want = Advance(h.tick.value, h.tick.rate, h.tick.cycle, h.sent_us, server_us);
        const float drift = DriftSeconds(mine->second, want, h.tick.rate, h.tick.cycle);
        if (drift <= kDriftS) {
            s.no_hold = 0;
            s.written.reset();
            continue;
        }
        if (s.last_write_us != 0 && steady_us < s.last_write_us + kWriteEveryUs) {
            continue;
        }
        if (s.written) {
            // Off again since our last write: the game took it back.
            if (++s.no_hold >= kMaxNoHold) {
                s.gave_up = true;
                char text[200];
                std::snprintf(text, sizeof text,
                              "%016llx %s: our writes of %s do not hold (%d in a row): stopped following",
                              static_cast<unsigned long long>(d->hash), d->name.c_str(),
                              Where(h.tick.key).c_str(), s.no_hold);
                out.notes.emplace_back(text);
                continue;
            }
        }
        if (!s.logged_follow) {
            s.logged_follow = true;
            char text[200];
            std::snprintf(text, sizeof text,
                          "%016llx %s: following player %u's clock %s (ours %.3f, theirs %.3f, %.2f s off)",
                          static_cast<unsigned long long>(d->hash), d->name.c_str(),
                          static_cast<unsigned>(owner), Where(h.tick.key).c_str(),
                          static_cast<double>(mine->second), static_cast<double>(want),
                          static_cast<double>(drift));
            out.notes.emplace_back(text);
        }
        out.writes.push_back({d->address, d->hash, h.tick.key, want, mine->second});
        s.written = want;
        s.last_write_us = steady_us;
        ++writes_;
    }
    if (!message.ticks.empty()) {
        out.send.emplace_back(std::move(message));
    }
}

void PlatformSync::Reset() {
    scene_.clear();
    heard_.clear();
    states_.clear();
}

}  // namespace CoopPlatforms
