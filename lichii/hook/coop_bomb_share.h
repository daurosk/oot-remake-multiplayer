// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets phase 2: a bomb changes hands (docs/native-puppets.md, "Lift
// a bomb, and stash it"; docs/protocol.md, "Bomb hand-over"). The rules the
// spawn sync (coop_spawn.h) follows when LICHII_COOP_BOMB_SHARE=1
// (docs/native-puppets.md "Phase 2 status"); off by default, and with no
// session nothing here runs. No emulator code: tested in
// tests/lichii/bomb_share_test.cpp, the wiring in spawn_test.cpp.
//
// - Our Link lifts the copy of another player's bomb (the game's own lift on
//   the real Item_Bomb copy: its animation says Lifted while its owner's does
//   not): BOMB_TAKE once, and the copy is no longer put at the owner's place
//   (the game's lift holds it). The server asks the owner.
// - HANDOVER_STATE for it: ours. The copy's actor becomes our own bomb (the
//   director lets go of it without deleting it), with the epoch and the fuse
//   left that came with it: it goes off then, through the game's own
//   explosion (its life set to 0), once, in our hands or wherever it is.
// - BOMB_ANSWER (or no answer in kAnswerWaitUs): refused. The copy goes back
//   to the owner's place.
// - HANDOVER_ASK for one of our bombs: answered with its state (fuse left)
//   unless our Link holds it (no answer: the server keeps it ours).
// - OBJECT_OWNER naming another player for one of our bombs: our actor is
//   deleted (no explosion) and the new owner's samples make the copy.
// - An unlit bomb we took that goes while the pouch's "Bomb" count rose by
//   one: stashed through the game's own pick-up; OBJECT_REMOVE picked up, so
//   every other game deletes its copy, no explosion.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopBombShare {

namespace proto = coop::protocol;

// The one shared actor that changes hands.
inline bool IsBomb(std::string_view actor) { return actor == "Item_Bomb"; }

inline constexpr std::uint64_t kAnswerWaitUs = 1'500'000;  // then the lift is dropped
inline constexpr std::uint64_t kAnnounceEveryUs = 5'000'000;  // BOMB_SHARE
// The pouch count rose within this long of the bomb going: a stash.
inline constexpr std::uint64_t kStashWindowUs = 1'000'000;

// Our own made bombs while the switch is on: the objects sync leaves a
// HANDOVER_ASK for one of them to the spawn sync (it answers every unknown
// id with the time only, which would hand a held bomb over with no fuse).
class OwnBombs {
public:
    void Note(const proto::ObjectId& id, bool ours) {
        std::lock_guard lock(mutex_);
        if (ours) {
            ids_.insert(id);
        } else {
            ids_.erase(id);
        }
    }
    bool Has(const proto::ObjectId& id) const {
        std::lock_guard lock(mutex_);
        return ids_.count(id) != 0;
    }
    void Clear() {
        std::lock_guard lock(mutex_);
        ids_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::set<proto::ObjectId> ids_;
};

inline OwnBombs& TheOwnBombs() {
    static OwnBombs bombs;
    return bombs;
}

// The server-clock moment a handed bomb goes off: its state's time plus the
// fuse left. Nothing for an unlit one (no fuse) or a fuse that is not a
// number of seconds a bomb can have.
inline std::optional<std::uint64_t> BlowAt(const proto::ObjectState& s) {
    if (!s.fuse || !(*s.fuse >= 0.0f) || *s.fuse > 60.0f) {
        return std::nullopt;
    }
    return s.time.server_time_us + static_cast<std::uint64_t>(*s.fuse * 1'000'000.0f);
}

// Seconds left at `now_us` (server clock) of a bomb that goes off at `blow_at`.
inline float FuseLeft(std::uint64_t blow_at, std::uint64_t now_us) {
    return blow_at > now_us ? static_cast<float>(blow_at - now_us) / 1'000'000.0f : 0.0f;
}

// The copy is within this of our own Link when our Link lifts it (it is in
// our Link's reach, or the director still drew it at the owner's place beside
// us for a frame).
inline constexpr float kTakeReachM = 2.5f;

// Whether a copy's own animation says our Link lifted it: Lifted on our copy
// while the owner's last sample did not say held (the owner lifting its own
// bomb plays Lifted on every copy through the director).
inline bool LiftedHere(std::optional<bool> held_by_animation, bool owner_holds) {
    return held_by_animation.value_or(false) && !owner_holds;
}

// Row 308: a copy our Link may be lifting. The director placed it at the
// owner's place and played the owner's animation (Bomb_Wait) on it every
// frame, on the game's own thread, so the game's Lifted on it was replaced
// before the walk read it: no BOMB_TAKE was ever sent, the bomb stayed on the
// floor (probe carry-304-182cd53b-3: no 'lifted the copy' note, the copy at its
// lying place 20 frames after the press). So for such a copy the director
// leaves the place, the setter and the animation alone while the copy's own
// slot 0 plays Lifted, until the take (`taking`) or its refusal. Not a copy
// its owner holds (the held stand-in, the owner's own Lifted), one being
// taken already, a refused or a removed one.
inline bool YieldsToLift(bool share_on, std::string_view actor, bool owner_holds, bool taking, bool refused,
                         bool removed) {
    return share_on && IsBomb(actor) && !owner_holds && !taking && !refused && !removed;
}

// Our own Link's side of a take: its place is known and the lifted copy is
// within kTakeReachM of it. Our Link's place not read (a load, not yet
// sampled) is no take: a copy something else set Lifted never asks.
inline bool NearOurLink(const std::optional<proto::Vec3>& link, const proto::Vec3& copy) {
    if (!link) {
        return false;
    }
    const float dx = (*link)[0] - copy[0], dy = (*link)[1] - copy[1], dz = (*link)[2] - copy[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 == d2 && d2 <= kTakeReachM * kTakeReachM;  // NaN: no
}

// One lift asked for at a time per bomb.
class Takes {
public:
    struct Pending {
        proto::ObjectId id;
        std::uint64_t asked_us = 0;
    };

    // Our Link lifted the copy `id` (epoch as last heard): BOMB_TAKE, once.
    std::optional<proto::BombTake> Lifted(const proto::ObjectId& id, std::uint16_t epoch,
                                          std::uint64_t now_us) {
        if (asking_.count(id) != 0) {
            return std::nullopt;
        }
        asking_[id] = now_us;
        return proto::BombTake{id, epoch};
    }
    bool Asking(const proto::ObjectId& id) const { return asking_.count(id) != 0; }
    // The answer came (a hand-over or a refusal): no longer asking.
    bool Done(const proto::ObjectId& id) { return asking_.erase(id) != 0; }
    // Asks with no answer for kAnswerWaitUs: dropped (returned once).
    std::vector<proto::ObjectId> Expired(std::uint64_t now_us) {
        std::vector<proto::ObjectId> out;
        for (auto it = asking_.begin(); it != asking_.end();) {
            if (now_us >= it->second && now_us - it->second >= kAnswerWaitUs) {
                out.push_back(it->first);
                it = asking_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }
    void Clear() { asking_.clear(); }
    std::size_t Size() const { return asking_.size(); }

private:
    std::map<proto::ObjectId, std::uint64_t> asking_;
};

// The owner's answer to HANDOVER_ASK for one of its bombs. Nothing while our
// Link holds it: the server then keeps it ours and tells the asker.
inline std::optional<proto::HandoverState> Answer(const proto::HandoverAsk& ask, bool our_link_holds,
                                                  proto::ObjectState state) {
    if (our_link_holds) {
        return std::nullopt;
    }
    proto::HandoverState out;
    out.id = ask.id;
    out.epoch = ask.epoch;
    out.state = std::move(state);
    return out;
}

// The pouch's "Bomb" count, read once a walk: when it last rose by exactly
// one (a bigger jump is a save load, a shop or a chest, not one bomb).
class PouchWatch {
public:
    void Read(std::optional<int> count, std::uint64_t now_us) {
        if (!count) {
            return;
        }
        if (last_ && *count == *last_ + 1) {
            rose_at_ = now_us;
        }
        last_ = count;
    }
    // A bomb that went at `gone_us`: stashed when the count rose within
    // kStashWindowUs of it, either side.
    bool RoseNear(std::uint64_t gone_us) const {
        if (!rose_at_) {
            return false;
        }
        const std::uint64_t d = gone_us > *rose_at_ ? gone_us - *rose_at_ : *rose_at_ - gone_us;
        return d <= kStashWindowUs;
    }
    void Reset() {
        last_.reset();
        rose_at_.reset();
    }

private:
    std::optional<int> last_;
    std::optional<std::uint64_t> rose_at_;
};

// Why our taken bomb went: stashed (picked up) when it was unlit and the
// pouch count rose by one around then; otherwise as the sync judged it.
inline proto::RemoveReason GoneReason(bool taken, bool lit, bool pouch_rose,
                                      proto::RemoveReason otherwise) {
    if (taken && !lit && pouch_rose) {
        return proto::RemoveReason::picked_up;
    }
    return otherwise;
}

// BOMB_SHARE every kAnnounceEveryUs while on (the server forgets nothing, but
// a reconnect is a new connection).
class Announce {
public:
    std::optional<proto::BombShare> Due(std::uint64_t now_us) {
        if (sent_ && now_us >= *sent_ && now_us - *sent_ < kAnnounceEveryUs) {
            return std::nullopt;
        }
        sent_ = now_us;
        return proto::BombShare{true};
    }
    void Reset() { sent_.reset(); }

private:
    std::optional<std::uint64_t> sent_;
};

}  // namespace CoopBombShare
