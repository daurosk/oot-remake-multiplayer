// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_rooms.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace CoopRooms {
namespace {

float Weighted(const Point& p, const Vec3& at) {
    const float dx = at[0] - p.x;
    const float dy = (at[1] - p.y) * kHeightWeight;
    const float dz = at[2] - p.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

std::vector<Point> Parse(std::string_view text, int* bad) {
    std::vector<Point> out;
    int wrong = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string line(text.substr(at, end - at));
        at = end + 1;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream in(line);
        Point p;
        if (!(in >> p.scene)) {
            continue;  // blank
        }
        std::string extra;
        if (!(in >> p.room >> p.x >> p.y >> p.z) || (in >> extra) || p.room < 0 ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            ++wrong;
            continue;
        }
        out.push_back(std::move(p));
    }
    if (bad != nullptr) {
        *bad = wrong;
    }
    return out;
}

Table::Table(std::vector<Point> points) {
    for (Point& p : points) {
        std::string scene = p.scene;
        scenes_[std::move(scene)].push_back(std::move(p));
        ++count_;
    }
}

const std::vector<Point>* Table::Find(std::string_view scene) const {
    const auto it = scenes_.find(scene);
    return it != scenes_.end() ? &it->second : nullptr;
}

std::optional<Guess> Table::Nearest(std::string_view scene, const Vec3& at, int distance_to,
                                    float* distance_out) const {
    constexpr float kNone = std::numeric_limits<float>::max();
    Guess best{-1, kNone};
    float to = kNone;
    if (const auto* points = Find(scene)) {
        for (const Point& p : *points) {
            const float d = Weighted(p, at);
            if (d < best.distance) {
                best = {p.room, d};
            }
            if (p.room == distance_to) {
                to = std::min(to, d);
            }
        }
    }
    if (distance_out != nullptr) {
        *distance_out = to;
    }
    if (best.room < 0 || best.distance > kFar) {
        return std::nullopt;
    }
    return best;
}

std::optional<int> Tracker::Update(std::uint32_t key, std::string_view scene, const Vec3& at) {
    const auto it = rooms_.find(key);
    const int was = it != rooms_.end() && it->second.scene == scene ? it->second.room : -1;
    float was_distance = 0.0f;
    const auto guess = table_.Nearest(scene, at, was, &was_distance);
    if (!guess) {
        rooms_.erase(key);
        return std::nullopt;
    }
    int room = guess->room;
    if (was >= 0 && was != room && was_distance <= kFar &&
        was_distance - guess->distance < kMargin) {
        room = was;  // not clearly out of the old room yet
    }
    rooms_[key] = Kept{std::string(scene), room};
    return room;
}

}  // namespace CoopRooms
