// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets N5 (docs/native-puppets.md, "Puppet physics sync"): another player standing on one of our lifts
// is drawn ON it, not sunk into it (going up) or floating over it (going down).
//
// The cause: a copy is drawn where its owner was CoopShowDelay::Us(owner) ago (presence's show delay, at least
// 100 ms; coop_show_delay.h), while our lift is where every game's lift is NOW (coop_platforms.h moves every
// game's platforms in step by the server clock). On a lift that moves v m/s the copy is v x delay off its floor:
// the Jabu-Jabu lift at about 2 m/s puts it 0.2 m inside the floor, and a copy with a body (--puppet-body) is
// then pushed out or warped by the physics every frame.
//
// The fix carries the drawn place by what OUR lift moved over that delay: Carry(lift then, lift now, place)
// (coop_puppet_body.h), "then" = the lift's place delay ago in this game's own record of it. Which lift the
// copy stands on is asked of that same "then" place, with the footprint CoopLiftRide gives a rider (#65:
// kRideRadiusM / kRideBelowM / kRideAboveM around the lift's actor position), so a copy is carried by the lift
// it stood on when the sample was made, never by one that has since come up under it.
//
// The lifts are the ones the objects walk already finds for #65 (CoopLiftRide::IsRideLift: ElevatorLift_ and
// MoveFloor_ actors); they move without turning, so the record keeps no yaw (Pose::yaw 0: a pure carry).
// LICHII_COOP_PUPPET_CARRY=0 turns it off (the objects walk then feeds nothing).
//
// No emulator code: tested in tests/lichii/puppet_body_test.cpp.
#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "coop_puppet_body.h"

namespace CoopPlatformCarry {

using Vec3 = CoopPuppetBody::Vec3;

// The footprint of a rider (the same numbers as CoopLiftRide's kRide*: one rule for "stands on our lift").
inline constexpr float kRadiusM = 1.8f;
inline constexpr float kBelowM = 0.5f;
inline constexpr float kAboveM = 2.5f;
// A lift not seen by the walk this long is gone (the walk runs every two frames; a scene change clears).
inline constexpr std::uint64_t kGoneMs = 500;
// A carry larger than this is not a lift's move over a show delay (a lift record that jumped: a respawn,
// a reset): nothing is carried. The fastest lift moves ~2 m/s; a show delay over 1 s is a broken link.
inline constexpr float kMaxCarryM = 2.0f;

struct Lift {
    std::uint64_t address = 0;
    Vec3 at{};
};

inline bool Stands(const Vec3& place, const Vec3& lift) {
    const float dx = place[0] - lift[0];
    const float dz = place[2] - lift[2];
    const float dy = place[1] - lift[1];
    return dx * dx + dz * dz <= kRadiusM * kRadiusM && dy >= -kBelowM && dy <= kAboveM;
}

class Store {
public:
    // The objects walk (any thread): our lifts and their places now.
    void Feed(const std::vector<Lift>& lifts, std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        for (const Lift& l : lifts) {
            Kept& k = kept_[l.address];
            k.history.Push(now_ms, CoopPuppetBody::Pose{l.at, 0.0f});
            k.seen_ms = now_ms;
        }
        for (auto it = kept_.begin(); it != kept_.end();) {
            it = now_ms > it->second.seen_ms + kGoneMs ? kept_.erase(it) : std::next(it);
        }
    }
    // The drawn place of a copy shown `delay_ms` late, carried by the lift it stood on then; unset when it
    // stood on none of ours (or the lift's record does not reach that far back).
    std::optional<Vec3> Carried(const Vec3& place, std::uint64_t delay_ms, std::uint64_t now_ms) const {
        if (delay_ms == 0 || delay_ms > now_ms) {
            return std::nullopt;
        }
        std::lock_guard lock(mutex_);
        for (const auto& [address, k] : kept_) {
            const auto then = k.history.At(now_ms - delay_ms);
            const auto at = k.history.At(now_ms);
            if (now_ms > k.seen_ms + kGoneMs || !then || !at || !Stands(place, then->at)) {
                continue;
            }
            const Vec3 out = CoopPuppetBody::Carry(*then, *at, place);
            if (!CoopPuppetBody::Finite(out) || CoopPuppetBody::Length(CoopPuppetBody::Sub(out, place)) > kMaxCarryM) {
                return std::nullopt;
            }
            return out;
        }
        return std::nullopt;
    }
    void Clear() {
        std::lock_guard lock(mutex_);
        kept_.clear();
    }

private:
    struct Kept {
        CoopPuppetBody::PoseHistory<> history;
        std::uint64_t seen_ms = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Kept> kept_;
};

inline Store& TheStore() {
    static Store store;
    return store;
}

}  // namespace CoopPlatformCarry
