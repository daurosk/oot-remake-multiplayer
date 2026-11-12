// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 194 (docs/puppet.md "Cutscenes without snaps"): the other player's puppet snapped
// after cutscenes. Two causes, both in coop_puppet.cpp:
// - While their cutscene ran the puppet was held where the cutscene began (Show::hold), so
//   when it ended the puppet jumped to where the event had moved them (through a door, down a
//   ladder). Now a held puppet keeps its frozen frame but follows their place.
// - Any other jump in the place we are given (a late sample after a stall) was drawn in one
//   frame. Now a move of the target of more than kSnapM in one frame and less than kWarpM is
//   eased out (EaseMs): the drawn place stays where the puppet was and closes the gap while
//   it follows the new place. Longer jumps are real warps and are drawn at once; so is any
//   jump while their cutscene runs (a real teleport then).
// Logs (coop_puppet.cpp, Director::DrawAt): every target jump ("P<n> target jump <m> m
// (<show|hold>, eased|at once)"), and the Meter on the puppet's own place read back from the
// game each frame ("P<n> snap <m> m (<show|hold|warp>)"), so a run's log says both what we
// were handed and what the game drew.
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
    // Where to draw it this frame, for `target` (where its player is now). A jump is the
    // target's own move since the last frame (not the drawn step, so a long ease plus their
    // running never starts a new ease). With `ease` a jump of kSnapM to kWarpM is eased;
    // without (their cutscene: samples move smoothly then, so a jump is a real teleport, a
    // void-out or the event's start mark) and past kWarpM it is drawn at once.
    // `jump` gets the target's move when it was a jump (0 none).
    Vec3 Step(const Vec3& target, std::uint64_t now_ms, bool ease = true, float* jump = nullptr) {
        if (jump != nullptr) {
            *jump = 0.0f;
        }
        for (const float c : target) {
            if (!std::isfinite(c)) {
                return target;
            }
        }
        if (!drawn_ || !last_target_) {
            Set(target);
            return target;
        }
        const float moved = Distance(target, *last_target_);
        last_target_ = target;
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
        if (moved > kSnapM && std::isfinite(moved)) {
            if (jump != nullptr) {
                *jump = moved;
            }
            if (ease && moved < kWarpM) {
                // Drawn where it was this frame; the gap closes over EaseMs.
                Vec3 off{};
                for (std::size_t i = 0; i < 3; ++i) {
                    off[i] = (*drawn_)[i] - target[i];
                }
                offset_ = off;
                start_ms_ = now_ms;
                ease_ms_ = EaseMs(Distance(*drawn_, target));
                now = *drawn_;
            } else {
                offset_.reset();
                now = target;
            }
        }
        drawn_ = now;
        return now;
    }

    // Drawn here from now on, no ease (a new puppet; a hide: it is out of sight).
    void Set(const Vec3& at) {
        drawn_ = at;
        last_target_ = at;
        offset_.reset();
    }
    void Reset() {
        drawn_.reset();
        last_target_.reset();
        offset_.reset();
    }
    bool Easing() const { return offset_.has_value(); }
    const std::optional<Vec3>& Drawn() const { return drawn_; }

private:
    std::optional<Vec3> drawn_;
    std::optional<Vec3> last_target_;
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
