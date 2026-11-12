// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 194 (docs/puppet.md "Cutscenes without snaps"): the other player's puppet snapped
// after cutscenes. Two causes, both in coop_puppet.cpp:
// - While their cutscene ran the puppet was held where the cutscene began (Show::hold), so
//   when it ended the puppet jumped to where the event had moved them (through a door, down a
//   ladder). Now a held puppet keeps its frozen frame but follows their place.
// - Any other jump in the place we are given (a hide ending, a late sample after a stall) was
//   drawn in one frame. Now a jump of more than kSnapM in one frame and less than kWarpM is
//   eased out (EaseMs): the drawn place stays where the puppet was and closes the gap while
//   it follows the new place. Longer jumps are real warps and are drawn at once.
// Meter counts what was drawn: a drawn step of more than kSnapM is a snap (logged by the
// Director as "coop: puppet: P<n> snap <m> m (<why>)", why "warp" past kWarpM), so a run's
// log says whether the other player's Link still jumps.
// No emulator code: tested in tests/lichii/snap_test.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace CoopSnap {

using Vec3 = std::array<float, 3>;

// A drawn step longer than this in one frame is a snap. Running is about 0.2 m a frame at
// 30 frames a second, a roll about 0.3 m.
inline constexpr float kSnapM = 0.75f;
// Longer than this is a warp (a scene's spawn point, a teleport): drawn at once.
inline constexpr float kWarpM = 20.0f;
// How long a jump takes to close: at least kEaseMs, longer gaps at kEaseSpeed, so no drawn
// frame steps past kSnapM at 20 frames a second or more.
inline constexpr std::uint64_t kEaseMs = 250;
inline constexpr float kEaseSpeed = 15.0f;  // metres a second

inline std::uint64_t EaseMs(float gap) {
    const float ms = gap / kEaseSpeed * 1000.0f;
    if (!std::isfinite(ms) || ms <= static_cast<float>(kEaseMs)) {
        return kEaseMs;
    }
    return static_cast<std::uint64_t>(ms);
}

inline float Distance(const Vec3& a, const Vec3& b) {
    float d2 = 0.0f;
    for (std::size_t i = 0; i < 3; ++i) {
        const float d = a[i] - b[i];
        d2 += d * d;
    }
    return std::sqrt(d2);
}

// The place a puppet is drawn at, one per puppet.
class Smoother {
public:
    // Where to draw it this frame, for `target` (where its player is now). `jump` is set to
    // the length of a jump that began an ease this frame (0 none), for the log.
    Vec3 Step(const Vec3& target, std::uint64_t now_ms, float* jump = nullptr) {
        if (jump != nullptr) {
            *jump = 0.0f;
        }
        for (const float c : target) {
            if (!std::isfinite(c)) {
                return target;
            }
        }
        if (!drawn_) {
            Set(target);
            return target;
        }
        // Where the current ease draws it now.
        Vec3 now = target;
        if (offset_) {
            const std::uint64_t gone = now_ms - start_ms_;
            if (now_ms < start_ms_ || gone >= ease_ms_) {
                offset_.reset();
            } else {
                const float left = 1.0f - static_cast<float>(gone) / static_cast<float>(ease_ms_);
                for (std::size_t i = 0; i < 3; ++i) {
                    now[i] = target[i] + (*offset_)[i] * left;
                }
            }
        }
        const float step = Distance(now, *drawn_);
        if (step > kSnapM && step < kWarpM) {
            // A jump: drawn where it was this frame; the gap closes over EaseMs.
            Vec3 off{};
            for (std::size_t i = 0; i < 3; ++i) {
                off[i] = (*drawn_)[i] - target[i];
            }
            offset_ = off;
            start_ms_ = now_ms;
            ease_ms_ = EaseMs(Distance(*drawn_, target));
            if (jump != nullptr) {
                *jump = step;
            }
            now = *drawn_;
        } else if (step >= kWarpM) {
            offset_.reset();
            now = target;
        }
        drawn_ = now;
        return now;
    }

    // Drawn here from now on, no ease (a new puppet; a hide: it is out of sight).
    void Set(const Vec3& at) {
        drawn_ = at;
        offset_.reset();
    }
    void Reset() {
        drawn_.reset();
        offset_.reset();
    }
    bool Easing() const { return offset_.has_value(); }
    const std::optional<Vec3>& Drawn() const { return drawn_; }

private:
    std::optional<Vec3> drawn_;
    std::optional<Vec3> offset_;  // drawn minus target when the ease began
    std::uint64_t start_ms_ = 0;
    std::uint64_t ease_ms_ = kEaseMs;
};

// The drawn steps of one puppet: Step returns the snap to log, if any.
class Meter {
public:
    // `drawn` is where it was drawn this frame. A step past kSnapM is returned.
    std::optional<float> Step(const Vec3& drawn) {
        std::optional<float> snap;
        if (last_) {
            const float d = Distance(drawn, *last_);
            if (std::isfinite(d) && d > kSnapM) {
                snap = d;
                ++snaps_;
                if (d < kWarpM) {
                    ++near_snaps_;
                }
            }
        }
        last_ = drawn;
        return snap;
    }
    void Reset() { last_.reset(); }
    int Snaps() const { return snaps_; }
    int NearSnaps() const { return near_snaps_; }  // not warps

private:
    std::optional<Vec3> last_;
    int snaps_ = 0;
    int near_snaps_ = 0;
};

}  // namespace CoopSnap
