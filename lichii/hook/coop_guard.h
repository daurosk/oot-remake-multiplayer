// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a shared change must wait (issue 144). One rule for everything another game does to an
// actor of ours (a kill, a remove, a new place, a played animation): never while our own Link
// is in an event (a talk, a cutscene) with that actor. Which actor the event is with is not
// read, so every actor within kEventNearM of our Link waits while an event runs, and the
// change is made right after the event ends.
//
// Seen in test6: B beat a Deku Scrub and was in its talk; A beat the same scrub in A's game; the
// kill reached B's scrub during B's talk and B's event waited on an actor that was gone, until
// A finished A's own talk. The chest rule (chest-sync: a chest beside our Link is not removed)
// uses the same distance test, Near.
//
// No emulator code: coop_objects.cpp applies it, tests/lichii/guard_test.cpp tests it.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace CoopGuard {

using Vec3 = std::array<float, 3>;

// Who an event is with stands this close (a talking Deku Scrub pops up a few metres away; a
// chest is opened from right in front of it).
inline constexpr float kEventNearM = 8.0f;
// A chest beside our Link is never removed: we may be opening it too (chest-sync).
inline constexpr float kChestNearM = 3.0f;

// True when both places are known and closer than `metres`.
inline bool Near(const std::optional<Vec3>& link, const std::optional<Vec3>& at, float metres) {
    if (!link || !at) {
        return false;
    }
    float d2 = 0.0f;
    for (std::size_t i = 0; i < 3; ++i) {
        const float d = (*link)[i] - (*at)[i];
        if (!std::isfinite(d)) {
            return false;
        }
        d2 += d * d;
    }
    return d2 < metres * metres;
}

// Our Link's side, once a frame: whether an event runs and where Link is.
struct Local {
    bool in_event = false;
    std::optional<Vec3> link;
};

// True when a shared change to an actor at `at` must wait now. An actor whose place is not
// known waits too while an event runs (it may be the one the event is with).
inline bool Waits(const Local& local, const std::optional<Vec3>& at) {
    if (!local.in_event) {
        return false;
    }
    if (!at) {
        return true;
    }
    return !local.link || Near(local.link, at, kEventNearM);
}

// Changes made once (a life write: a kill, a hurt) that must not be lost while they wait.
// Pass() gives back what to apply now and keeps the rest, one per actor (the newest wins);
// when the event ends, everything kept comes back first, in order. Forget() drops them (a
// scene change: those actors are gone).
template <typename Change>
class Held {
public:
    using Where = std::function<std::optional<Vec3>(std::uint64_t address)>;

    std::vector<Change> Pass(std::vector<Change> now, const Local& local, const Where& where) {
        std::vector<Change> out;
        if (!local.in_event) {
            for (const std::uint64_t address : order_) {
                out.push_back(std::move(held_.at(address)));
            }
            held_.clear();
            order_.clear();
        }
        for (auto& c : now) {
            if (Waits(local, where(c.address))) {
                if (!held_.contains(c.address)) {
                    order_.push_back(c.address);
                }
                held_[c.address] = std::move(c);
                ++held_count_;
            } else {
                out.push_back(std::move(c));
            }
        }
        return out;
    }
    void Forget() {
        held_.clear();
        order_.clear();
    }
    std::size_t Size() const { return held_.size(); }
    std::uint64_t HeldCount() const { return held_count_; }  // every change that had to wait

private:
    std::map<std::uint64_t, Change> held_;
    std::vector<std::uint64_t> order_;
    std::uint64_t held_count_ = 0;
};

// Changes the sync makes again every frame (a remove, a place, a played animation): the ones
// that wait are only left out; they come again once the event is over. Returns how many waited.
template <typename Change, typename AddressOf>
std::size_t Skip(std::vector<Change>& changes, const Local& local,
                 const std::function<std::optional<Vec3>(std::uint64_t)>& where, AddressOf address_of) {
    if (!local.in_event) {
        return 0;
    }
    const auto before = changes.size();
    std::erase_if(changes, [&](const Change& c) { return Waits(local, where(address_of(c))); });
    return before - changes.size();
}

} // namespace CoopGuard
