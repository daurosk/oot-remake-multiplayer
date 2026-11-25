// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets phase 1 (docs/native-puppets.md, "Who moves the copy's body"): the one rule for
// a copy that has a body of its own (a pack built with --puppet-body, coop_puppet.json "body": true).
//
// The owner game's samples say where the copy is; this game's physics only ever follows. Each frame
// the director has where the game left the body (read back), where the samples put it now and how
// fast that place moves, and asks Drive::Step what to hand the physics:
//   rest  - the body is where it should be: nothing is sent, so the body is a still thing in the
//           game's world (our Link stands on it and leans on it without a tremble);
//   drive - a speed for the next physics step that takes the body to the place (the samples' own
//           speed plus what closes the gap), so its contacts push and carry as a moving body's do;
//   warp  - the game's own place setter (main+0x4c6580: a queued teleport of the controller set and
//           its character controller), speed zero: a scene's spawn point, a teleport, a long gap
//           after lost samples, or a body that something of this game alone held back.
// Before any of it the place gives way to our own Link (Yield): we never put a body inside the one
// Link this game simulates, so a copy can neither trap nor throw him; the other game does the same
// for its own Link, and the overlap (there only by lag) is shown as the copy standing aside.
// On a moving platform the place is carried by this game's own platform (Carry): every game moves
// its platforms in step (coop_platforms.h), so the offset from the platform is what is the same.
//
// No emulator code: tested in tests/lichii/puppet_body_test.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace CoopPuppetBody {

using Vec3 = std::array<float, 3>;

inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 Mul(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline float Length(const Vec3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
inline bool Finite(const Vec3& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

// The pack's manifest says "body": true (build.py --puppet-body) at the top level. Each actor entry has
// a "body" of its own (null, or its files), written before it, so the pack's is the last one that is a
// bool (as CoopPuppet::PackPerception).
inline bool PackBody(std::string_view manifest) {
    constexpr std::string_view key = "\"body\"";
    bool on = false;
    for (auto at = manifest.find(key); at != std::string_view::npos; at = manifest.find(key, at + key.size())) {
        const auto v = manifest.find_first_not_of(" \t\r\n:", at + key.size());
        if (v == std::string_view::npos) {
            break;
        }
        if (manifest.substr(v, 4) == "true") {
            on = true;
        } else if (manifest.substr(v, 5) == "false") {
            on = false;
        }
    }
    return on;
}

// Nearer than this to where the game has the body, and turned less than kRestTurn: nothing is sent.
inline constexpr float kRestM = 0.002f;
inline constexpr float kRestTurn = 0.004f;  // radians
// A gap longer than this is never closed by speed: it is a warp. A thrown or falling Link moves
// about 0.6 m a frame at 30 frames a second.
inline constexpr float kWarpM = 1.5f;
// The fastest speed handed to the physics, metres a second (a gap that needs more is a warp).
inline constexpr float kMaxSpeed = 40.0f;
// How much of the gap one step closes on top of the samples' own speed. Under 1, so a body the
// solver stopped (a contact) is not pressed into it harder each frame.
inline constexpr float kGain = 0.6f;
// A driven body that stays more than kHeldM behind for kHeldFrames frames is held by something only
// this game has: it is warped past it (their real Link is where the samples say, so the way is free
// in their game).
inline constexpr float kHeldM = 0.5f;
inline constexpr int kHeldFrames = 6;
// A frame time outside this is not used for a speed (a stall, a first frame): warp or rest then.
inline constexpr float kMinDt = 0.004f;
inline constexpr float kMaxDt = 0.1f;

enum class Kind : std::uint8_t { rest, drive, warp };

struct Command {
    Kind kind = Kind::rest;
    Vec3 place{};     // drive: where the body should be after the step; warp: where it is put
    Vec3 velocity{};  // drive: metres a second for the next physics step; else zero
};

struct Input {
    std::optional<Vec3> body;  // where the game has the body now (read back); unset: not read
    Vec3 place{};              // where the samples put it this frame (after Yield and Carry)
    Vec3 velocity{};           // how fast that place moves, metres a second; zero when not known
    float turned = 0.0f;       // radians between the body's rotation and the one wanted
    float dt = 0.0f;           // this frame's time, seconds
    bool fresh = true;         // samples are arriving; false: lost (the body stays, speed zero)
    bool jump = false;         // the place jumped (scene change, warp, first frame, back from parked)
    bool speed_channel = false;  // the game takes a speed for this body (else every move is a warp)
};

class Drive {
public:
    Command Step(const Input& in) {
        Command out;
        out.place = in.place;
        if (!Finite(in.place)) {
            held_ = 0;
            return out;  // nothing sane to hand on: the body stays
        }
        if (!in.body || !Finite(*in.body) || in.jump) {
            held_ = 0;
            out.kind = Kind::warp;
            return out;
        }
        const Vec3 gap = Sub(in.place, *in.body);
        const float far = Length(gap);
        if (!in.fresh) {
            // Lost samples: no speed is made up. The body stays where it is (it still blocks and
            // carries); the director's own hold/hide rules decide what is drawn.
            held_ = 0;
            out.place = *in.body;
            return out;
        }
        const float speed = Finite(in.velocity) ? Length(in.velocity) : 0.0f;
        if (far <= kRestM && std::fabs(in.turned) <= kRestTurn && speed * in.dt <= kRestM) {
            held_ = 0;
            return out;  // rest
        }
        const bool dt_ok = in.dt >= kMinDt && in.dt <= kMaxDt;
        if (!in.speed_channel || !dt_ok || far > kWarpM) {
            held_ = 0;
            out.kind = Kind::warp;
            return out;
        }
        held_ = far > kHeldM ? held_ + 1 : 0;
        if (held_ >= kHeldFrames) {
            held_ = 0;
            out.kind = Kind::warp;
            return out;
        }
        // The samples' speed takes the body where the place will be; the gain closes what is left.
        Vec3 v = Add(Finite(in.velocity) ? in.velocity : Vec3{}, Mul(gap, kGain / in.dt));
        const float fast = Length(v);
        if (fast > kMaxSpeed) {
            v = Mul(v, kMaxSpeed / fast);
        }
        out.kind = Kind::drive;
        out.velocity = v;
        return out;
    }
    void Reset() { held_ = 0; }
    int Held() const { return held_; }

private:
    int held_ = 0;
};

// An upright capsule as the character bodies are: feet at `feet`, `radius` wide, `height` tall.
struct Capsule {
    Vec3 feet{};
    float radius = 0.0f;
    float height = 0.0f;
};

// The copy's place with our own Link given way to: `copy` moved the shortest way (sideways, or down
// under his feet, or up over his head) so the two do not overlap. No overlap: `copy.feet` as it is.
// `moved`: how far it was moved.
inline Vec3 Yield(const Capsule& copy, const Capsule& ours, float* moved = nullptr) {
    if (moved != nullptr) {
        *moved = 0.0f;
    }
    if (!Finite(copy.feet) || !Finite(ours.feet)) {
        return copy.feet;
    }
    const float dx = copy.feet[0] - ours.feet[0];
    const float dz = copy.feet[2] - ours.feet[2];
    const float flat = std::sqrt(dx * dx + dz * dz);
    const float side = copy.radius + ours.radius - flat;       // how deep sideways
    const float under = copy.feet[1] + copy.height - ours.feet[1];  // the copy's top over our feet
    const float over = ours.feet[1] + ours.height - copy.feet[1];   // our top over the copy's feet
    if (side <= 0.0f || under <= 0.0f || over <= 0.0f) {
        return copy.feet;
    }
    Vec3 out = copy.feet;
    const float least = std::min({side, under, over});
    if (least == under) {
        out[1] -= under;  // our Link stands on it: it stays under his feet
    } else if (least == over) {
        out[1] += over;   // it is on our Link: it stays on his head
    } else if (flat > 1e-4f) {
        out[0] += dx / flat * side;
        out[2] += dz / flat * side;
    } else {
        out[0] += side;   // the same spot: any side, always the same one
    }
    if (moved != nullptr) {
        *moved = least;
    }
    return out;
}

// A platform's place and heading (about the up axis) at one moment.
struct Pose {
    Vec3 at{};
    float yaw = 0.0f;  // radians
};

// `place`, known while the platform was at `then`, where the platform at `now` has carried it: the
// same offset from the platform, turned with it.
inline Vec3 Carry(const Pose& then, const Pose& now, const Vec3& place) {
    const Vec3 off = Sub(place, then.at);
    const float a = now.yaw - then.yaw;
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {now.at[0] + off[0] * c + off[2] * s, now.at[1] + off[1], now.at[2] - off[0] * s + off[2] * c};
}

// A rider's speed with the platform's own taken out (what is left is their walk on it), so the
// follower's look-ahead and Carry do not count the platform's move twice. `turn`: the platform's
// turning, radians a second, about the up axis through platform.at.
inline Vec3 RiderSpeed(const Vec3& velocity, const Vec3& platform_velocity, const Pose& platform, float turn,
                       const Vec3& place) {
    const Vec3 off = Sub(place, platform.at);
    // d/dt of Carry's turned offset at a = 0: (off.z * turn, 0, -off.x * turn).
    const Vec3 swing{off[2] * turn, 0.0f, -off[0] * turn};
    return Sub(Sub(velocity, platform_velocity), swing);
}

// A short record of one platform's poses in this game, to ask where it was when a sample was made.
template <std::size_t N = 32>
class PoseHistory {
public:
    void Push(std::uint64_t at_ms, const Pose& pose) {
        if (count_ > 0 && at_ms <= times_[(head_ + N - 1) % N]) {
            return;  // not newer: kept in order
        }
        times_[head_] = at_ms;
        poses_[head_] = pose;
        head_ = (head_ + 1) % N;
        count_ = std::min(count_ + 1, N);
    }
    // The pose at `at_ms`, between the two records around it; unset when `at_ms` is older than the
    // record or nothing is kept. Newer than the last record: the last one.
    std::optional<Pose> At(std::uint64_t at_ms) const {
        if (count_ == 0) {
            return std::nullopt;
        }
        const std::size_t first = (head_ + N - count_) % N;
        if (at_ms < times_[first]) {
            return std::nullopt;
        }
        for (std::size_t i = count_; i-- > 0;) {
            const std::size_t k = (first + i) % N;
            if (times_[k] > at_ms) {
                continue;
            }
            if (i + 1 == count_) {
                return poses_[k];
            }
            const std::size_t n = (k + 1) % N;
            const float f = static_cast<float>(at_ms - times_[k]) / static_cast<float>(times_[n] - times_[k]);
            Pose p;
            p.at = Add(poses_[k].at, Mul(Sub(poses_[n].at, poses_[k].at), f));
            p.yaw = poses_[k].yaw + (poses_[n].yaw - poses_[k].yaw) * f;
            return p;
        }
        return std::nullopt;
    }
    void Clear() { count_ = 0; head_ = 0; }

private:
    std::array<std::uint64_t, N> times_{};
    std::array<Pose, N> poses_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

}  // namespace CoopPuppetBody
