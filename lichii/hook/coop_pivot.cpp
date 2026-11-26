// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pivot.h"

#include <algorithm>
#include <cmath>

namespace CoopPivot {

namespace {

float Wrap(float deg) {
    float w = std::fmod(deg + 180.0f, 360.0f);
    if (w < 0.0f) {
        w += 360.0f;
    }
    return w - 180.0f;
}

} // namespace

float YawOfDir(int dir) {
    return Wrap(kStepDeg * static_cast<float>(dir % 8));
}

float Short(float a, float b) {
    return Wrap(b - a);
}

float Turn::At(std::uint64_t now_ms) const {
    if (ms == 0 || now_ms >= start_ms + ms) {
        return to;
    }
    const float t = now_ms <= start_ms ? 0.0f : static_cast<float>(now_ms - start_ms) / static_cast<float>(ms);
    return Wrap(from + Short(from, to) * t);
}

std::optional<Turn> Follower::Look(std::uint64_t now_ms, float yaw, std::optional<int> dir) {
    if (!std::isfinite(yaw)) {
        return std::nullopt;
    }
    if (driving_) {
        last_yaw_ = yaw;
        if (!driving_->Done(now_ms)) {
            return std::nullopt;  // ours moving it
        }
        driving_.reset();  // our writes stop here; still from now on
        still_ms_ = now_ms;
        return std::nullopt;
    }
    if (!last_yaw_ || std::fabs(Short(*last_yaw_, yaw)) > kStillDeg) {
        still_ms_ = 0;  // moving (our player's push, however slowly it starts), or not seen before
    } else if (still_ms_ == 0) {
        still_ms_ = now_ms;
    }
    last_yaw_ = yaw;
    if (!dir || still_ms_ == 0 || now_ms - still_ms_ < kOwnQuietMs) {
        return std::nullopt;
    }
    // Only from a resting place (a multiple of 45 degrees): a floor stopped part way is our player's own hold.
    const float rest = std::round(yaw / 45.0f) * 45.0f;
    if (std::fabs(Short(rest, yaw)) >= kSameDeg) {
        return std::nullopt;
    }
    const float to = YawOfDir(*dir);
    const float apart = std::fabs(Short(yaw, to));
    if (apart < kSameDeg) {
        return std::nullopt;
    }
    Turn t;
    t.from = yaw;
    t.to = to;
    t.start_ms = now_ms;
    t.ms = std::max<std::uint64_t>(300, static_cast<std::uint64_t>(static_cast<float>(kStepMs) * apart / 45.0f));
    driving_ = t;
    return t;
}

void Follower::Reset() {
    last_yaw_.reset();
    still_ms_ = 0;
    driving_.reset();
}

} // namespace CoopPivot
