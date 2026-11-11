// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a shared change must wait (issue 144). One rule for everything another game does to an
// actor of ours (a kill, a remove, a new place, a played animation): never while our own Link
// is in an event (a talk, a cutscene) with that actor. Which actor the event is with is not
// read, so every actor within kEventNearM of our Link waits while an event runs, and the
// change is made right after the event ends.
//
// Seen in a player test: B beat a Deku Scrub and was in its talk; A beat the same scrub in A's game; the
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
#include <string_view>
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

// Who an actor at an address is, for Held: its placement hash, its name and the object id the
// sync gave it. Actors the game makes share one "no hash" and many have the same name (two
// Keese); the id tells two such copies apart when one takes the other's address.
inline std::uint64_t Who(std::uint64_t hash, std::string_view name, std::optional<std::uint64_t> object) {
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a over the three
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    };
    mix(hash);
    for (const char c : name) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    mix(object ? *object + 1 : 0);
    return h;
}

// Two held life writes for one actor become one (Held's merge for LifeWrite-like changes, with
// `life`, the life it should end at, and `hurt`, above 0 when the game is to take that much
// itself). Two hurts: both hits land (the hurts add up; the newer target). A hurt behind a
// written life: the target is written as life (the game has not taken the first one yet).
// Otherwise the newer one wins.
template <typename Change>
void MergeLife(Change& kept, Change&& newer) {
    if (newer.hurt > 0 && kept.hurt > 0) {
        newer.hurt += kept.hurt;
    } else if (newer.hurt > 0) {
        newer.hurt = 0;
    }
    kept = std::move(newer);
}

// Changes made once (a life write: a kill, a hurt) that must not be lost while they wait.
// Pass() gives back what to apply now and keeps the rest, one per actor (merged with Merge,
// default: the newest wins), with the hash of the actor that was at that address. When the
// event ends, everything kept comes back first, in order, but only where this walk still has
// the same actor at that address: a long event deletes and makes actors, and an address may
// hold another actor by then (that change is dropped). Forget() drops them all (a scene
// change: those actors are gone).
template <typename Change>
class Held {
public:
    using Where = std::function<std::optional<Vec3>(std::uint64_t address)>;
    // The hash of the actor at this address in this walk; nullopt when none is there.
    using HashOf = std::function<std::optional<std::uint64_t>(std::uint64_t address)>;
    using Merge = std::function<void(Change& kept, Change&& newer)>;

    struct Result {
        std::vector<Change> apply;  // the released ones first, then this frame's
        std::size_t released = 0;   // how many at the front of `apply` were held
        std::size_t dropped = 0;    // held, but another actor (or none) is at the address now
    };

    explicit Held(Merge merge = {}) : merge_(std::move(merge)) {}

    Result Pass(std::vector<Change> now, const Local& local, const Where& where, const HashOf& hash_of) {
        Result out;
        if (!local.in_event) {
            for (const std::uint64_t address : order_) {
                Entry& e = held_.at(address);
                const auto hash = hash_of(address);
                if (e.hash && hash && *hash == *e.hash) {
                    out.apply.push_back(std::move(e.change));
                    ++out.released;
                } else {
                    ++out.dropped;
                }
            }
            held_.clear();
            order_.clear();
        }
        for (auto& c : now) {
            if (Waits(local, where(c.address))) {
                const std::uint64_t address = c.address;
                const auto found = held_.find(address);
                if (found == held_.end()) {
                    order_.push_back(address);
                    held_.emplace(address, Entry{std::move(c), hash_of(address)});
                } else if (merge_) {
                    merge_(found->second.change, std::move(c));
                } else {
                    found->second.change = std::move(c);
                }
                ++held_count_;
            } else {
                out.apply.push_back(std::move(c));
            }
        }
        return out;
    }
    // Drops everything held; returns how many were dropped.
    std::size_t Forget() {
        const std::size_t n = held_.size();
        held_.clear();
        order_.clear();
        return n;
    }
    std::size_t Size() const { return held_.size(); }
    std::uint64_t HeldCount() const { return held_count_; }  // every change that had to wait

private:
    struct Entry {
        Change change;
        std::optional<std::uint64_t> hash;  // the actor at the address when it was held
    };
    Merge merge_;
    std::map<std::uint64_t, Entry> held_;
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
