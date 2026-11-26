// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's actor events on Hookshot (native-plan rows 7-10; patch 0007,
// hookshot/src/hos_coop_actors.h): creates, deletes and damage, as the
// game's C asked for them, from the world hook on whichever game thread ran
// the function. They go into a ring in our own memory (several threads may
// push at once, no lock); the Objects step drains it on the frame hook.
// Plain C++, no emulator or Hookshot code: tests/lichii/actor_events_test.cpp.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace CoopActorEvents {

enum class Kind : std::uint32_t {
    None = 0,
    Create = 1,  // a create request: name, handle slot, caller, create argument
    Delete = 2,  // a delete request: actor, reason
    Damage = 3,  // a Life step's frame damage (not 0): Life component, amount
    Refused = 4, // a Life step took a give tied to our replayed hit, which its damage step never took (LifeTable):
                 // Life component; from: the component's life flags (Life + 0x1d18);
                 // flags kWithdrawn: the replay was still waiting in the component, withdrawn
};

// Event::flags.
inline constexpr std::uint32_t kOwn = 1;      // asked inside one of the module's own game calls (a copy, a remove)
inline constexpr std::uint32_t kStopped = 2;  // Create: the module had the game refuse it (a copy enemy's own shot)
inline constexpr std::uint32_t kWithdrawn = 4;  // Refused: our replay was still waiting, withdrawn (present cleared)
inline constexpr std::uint32_t kIdUnread = 8;   // Refused: the last hit id could not be read (taken as not reached)

// Whether a create or delete the game asks for is the module's own (kOwn: a copy, a remove; never stopped): asked
// inside one of the module's game calls, unless that call runs a copy's own game code (CoopPuppet::Call::copy_game:
// issue 189, the owner's shoot action put into a copy makes the copy's own shot, which the owner's copy doubles).
inline bool Ours(bool in_call, bool copy_game) {
    return in_call && !copy_game;
}

struct Event {
    Kind kind = Kind::None;
    std::uint32_t flags = 0;
    std::uint64_t a = 0;       // Create: the name object; Delete: the actor; Damage: the Life component
    std::uint64_t b = 0;       // Create: the handle slot; Delete: the reason; Damage: the amount (int32)
    std::uint64_t from = 0;    // Create: the caller (guest return address)
    std::uint64_t place[3]{};  // Create: the asked position (bits of 3 floats, 0 when not read)
    std::uint64_t name[4]{};   // Create: the name's first 31 bytes, 0-terminated (read at the request)

    std::string Name() const {
        char text[sizeof name + 1] = {};
        std::memcpy(text, name, sizeof name);
        return std::string(text);
    }
    void SetName(std::string_view n) {
        char text[sizeof name] = {};
        std::memcpy(text, n.data(), n.size() < sizeof text - 1 ? n.size() : sizeof text - 1);
        std::memcpy(name, text, sizeof name);
    }
};

// Many writers, one reader. A slot's sequence says which push it holds:
// the reader takes the slots in push order and stops at one not written
// yet; a slot written over before it was read counts as lost.
class Ring {
public:
    static constexpr std::size_t kSlots = 4096;

    void Push(const Event& e) noexcept {
        const std::uint64_t n = head_.fetch_add(1, std::memory_order_acq_rel);
        Slot& s = slots_[n % kSlots];
        s.seq.store(0, std::memory_order_relaxed);  // (being written)
        std::atomic_thread_fence(std::memory_order_release);
        s.kind.store(static_cast<std::uint32_t>(e.kind), std::memory_order_relaxed);
        s.flags.store(e.flags, std::memory_order_relaxed);
        s.a.store(e.a, std::memory_order_relaxed);
        s.b.store(e.b, std::memory_order_relaxed);
        s.from.store(e.from, std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) {
            s.place[i].store(e.place[i], std::memory_order_relaxed);
        }
        for (int i = 0; i < 4; ++i) {
            s.name[i].store(e.name[i], std::memory_order_relaxed);
        }
        s.seq.store(n + 1, std::memory_order_release);
    }

    // The events pushed since the last drain (at most kSlots), in order.
    // lost: those written over or torn before they could be read.
    std::vector<Event> Drain(std::size_t* lost = nullptr) noexcept {
        std::vector<Event> out;
        std::size_t gone = 0;
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (head - tail_ > kSlots) {
            gone += static_cast<std::size_t>(head - tail_ - kSlots);
            tail_ = head - kSlots;
        }
        while (tail_ < head) {
            Slot& s = slots_[tail_ % kSlots];
            const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
            if (seq == 0 || seq < tail_ + 1) {
                break;  // (pushed but not written yet: the next drain takes it)
            }
            Event e;
            e.kind = static_cast<Kind>(s.kind.load(std::memory_order_relaxed));
            e.flags = s.flags.load(std::memory_order_relaxed);
            e.a = s.a.load(std::memory_order_relaxed);
            e.b = s.b.load(std::memory_order_relaxed);
            e.from = s.from.load(std::memory_order_relaxed);
            for (int i = 0; i < 3; ++i) {
                e.place[i] = s.place[i].load(std::memory_order_relaxed);
            }
            for (int i = 0; i < 4; ++i) {
                e.name[i] = s.name[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq != tail_ + 1 || s.seq.load(std::memory_order_relaxed) != seq) {
                ++gone;  // (written over by a later push)
            } else {
                out.push_back(e);
            }
            ++tail_;
        }
        if (lost != nullptr) {
            *lost = gone;
        }
        return out;
    }

    std::uint64_t Pushed() const noexcept { return head_.load(std::memory_order_acquire); }
    // A new game boot: the old addresses are another heap's.
    void Forget() noexcept { tail_ = head_.load(std::memory_order_acquire); }

private:
    struct Slot {
        std::atomic<std::uint64_t> seq{0};
        std::atomic<std::uint32_t> kind{0};
        std::atomic<std::uint32_t> flags{0};
        std::atomic<std::uint64_t> a{0}, b{0}, from{0};
        std::atomic<std::uint64_t> place[3]{};
        std::atomic<std::uint64_t> name[4]{};
    };
    std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_ = 0;  // (the reader's)
    std::array<Slot, kSlots> slots_{};
};

inline Ring& TheRing() {
    static Ring ring;
    return ring;
}

// Damage amounts come as the low 32 bits of the event: an int32.
constexpr std::int32_t Amount(std::uint64_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(b));
}

// No hit id (a Life component's last hit id before its first hit, and "no replay of ours").
inline constexpr std::uint32_t kNoHitId = 0xffffffffu;
// In the Life component (coop_hit_replay.h names the same: kLastHitId, kHitRecord + 8 and + 0xc; checked there):
inline constexpr std::uint64_t kLifeLastHitId = 0x1b18;     // u32, the last hit its damage step took
inline constexpr std::uint64_t kLifeRecordId = 0x248;       // u32, the waiting hit record's id
inline constexpr std::uint64_t kLifeRecordPresent = 0x24c;  // u32, != 0 while that record waits

// Has a Life component whose last hit id (Life + 0x1b18) is `last` taken the hit `ours` we replayed into it?
// Hit ids only grow (compared as a wrapping difference). No id read, or no replay of ours: no.
constexpr bool HitIdReached(std::uint32_t last, std::uint32_t ours) {
    return last != kNoHitId && ours != kNoHitId && static_cast<std::int32_t>(last - ours) >= 0;
}

// Damage another player dealt, for the game's own Life step to take (the
// LIFE_STEP hook adds it to that frame's sum, on the game's thread): no
// write into the game's damage field from our thread. One writer (the
// Objects step), the game's job threads taking. A full table refuses (Give
// answers false).
//
// Row 270 (the same cause as CoopDifficulty::Kinds): a slot is its
// component's only while the component lives. The object walk ends with
// Sweep, and a component the walk has not seen for kKeepSweeps walks (its
// actor deleted) gives its slot back; what was still given to it is owed to
// nobody. Before, a component kept its slot until Clear (a new scene), so a
// long stay in one scene filled the 256 slots and the other players' hits on
// every later enemy were lost.
//
// A give may be tied to a hit we replayed into the same component (enemy-late-kill): the owner's fall that the
// replay was to deal, given because its damage step had not taken the replay. At the step it is dropped when the
// component's last hit id has reached the replay's (its damage step took the replay after all, which dealt it), and
// taken otherwise; the step then withdraws the replay (Step's `withdraw`), so the same fall is never dealt twice.
class LifeTable {
public:
    static constexpr std::size_t kSlots = 256;
    static constexpr std::size_t kProbes = 16;

    // `judged`: the life its owner's game says this copy has (CoopObjects' Lower), not a hit for our game to judge:
    // a step that takes it to a death goes past the component's own life stops (PassStops).
    // `tied_to`: the hit id of our replay this give stands in for (kNoHitId: none).
    bool Give(std::uint64_t component, std::int32_t amount, bool judged = false,
              std::uint32_t tied_to = kNoHitId) noexcept {
        if (component == 0 || amount == 0) {
            return false;
        }
        if (component == kFreed) {
            return false;
        }
        Slot* slot = nullptr;
        std::size_t slot_at = 0;
        bool found = false;
        for (std::size_t i = 0; i < kProbes && !found; ++i) {
            const std::size_t at = (Hash(component) + i) % kSlots;
            const std::uint64_t key = slots_[at].key.load(std::memory_order_acquire);
            if (key == component) {
                slot = &slots_[at];
                slot_at = at;
                found = true;
            } else if (key == kFreed || key == 0) {
                if (slot == nullptr) {  // the first freed or empty slot on the way: a new component goes there
                    slot = &slots_[at];
                    slot_at = at;
                }
                if (key == 0) {
                    break;  // the end of the run: the component is not in the table
                }
            }
        }
        if (slot == nullptr) {
            return false;
        }
        Slot& s = *slot;
        seen_[slot_at] = sweep_;
        if (!found) {
            // (only this writer inserts: no race on the key) A freed slot holds nothing (Sweep).
            s.key.store(component, std::memory_order_release);
        }
        if (tied_to != kNoHitId) {
            if (judged) {
                s.tied_judged.store(true, std::memory_order_release);
            }
            s.tied_to.store(tied_to, std::memory_order_release);  // (the newest replay: ids only grow)
            s.tied.fetch_add(amount, std::memory_order_acq_rel);
        } else {
            if (judged) {
                s.judged.store(true, std::memory_order_release);
            }
            s.add.fetch_add(amount, std::memory_order_acq_rel);
        }
        return true;
    }
    // The end of one whole object walk (the writer's thread). `alive(component)`: the walk saw that Life
    // component, or a give to it still waits for its sleeping actor. A component not alive for kKeepSweeps walks,
    // and given nothing in them, is gone: its slot is free for the next new component, with what it still held.
    static constexpr std::uint64_t kKeepSweeps = 120;  // (a few seconds: a walk that misses one read frees nothing)
    template <class Alive>
    void Sweep(Alive&& alive) noexcept {
        for (std::size_t at = 0; at < kSlots; ++at) {
            Slot& s = slots_[at];
            const std::uint64_t key = s.key.load(std::memory_order_relaxed);
            if (key == 0 || key == kFreed) {
                continue;
            }
            if (alive(key)) {
                seen_[at] = sweep_;
            } else if (sweep_ - seen_[at] >= kKeepSweeps) {
                // The key first: no reader finds the slot while it is emptied. (Its actor is deleted: no
                // Life step of that component runs.)
                s.key.store(kFreed, std::memory_order_release);
                s.add.store(0, std::memory_order_relaxed);
                s.tied.store(0, std::memory_order_relaxed);
                s.tied_to.store(kNoHitId, std::memory_order_relaxed);
                s.tied_judged.store(false, std::memory_order_relaxed);
                s.judged.store(false, std::memory_order_relaxed);
            }
        }
        ++sweep_;
    }
    // What the step adds this frame (0 for a component never given any).
    // judged (when given): whether any of it was a judged give.
    // `last_id`: the component's last hit id now (kNoHitId: not read): a tied give whose replay it reached is dropped.
    // `withdraw` (when given): the replay's hit id when a tied give was taken (its replay to withdraw), else kNoHitId.
    std::int32_t Take(std::uint64_t component, bool* judged = nullptr, std::uint32_t last_id = kNoHitId,
                      std::uint32_t* withdraw = nullptr) noexcept {
        if (judged != nullptr) {
            *judged = false;  // (nothing given: nothing judged)
        }
        if (withdraw != nullptr) {
            *withdraw = kNoHitId;
        }
        Slot* s = Find(component);
        if (s == nullptr ||
            (s->add.load(std::memory_order_relaxed) == 0 && s->tied.load(std::memory_order_relaxed) == 0)) {
            return 0;
        }
        std::int32_t add = s->add.exchange(0, std::memory_order_acq_rel);
        bool was = s->judged.exchange(false, std::memory_order_acq_rel);
        if (s->tied.load(std::memory_order_acquire) != 0) {
            // The id first, then the amount: a Give between the two (it stores its id, then adds) leaves its id
            // with what it added, never an amount with no id.
            const std::uint32_t id = s->tied_to.exchange(kNoHitId, std::memory_order_acq_rel);
            const bool tied_was = s->tied_judged.exchange(false, std::memory_order_acq_rel);
            const std::int32_t tied = s->tied.exchange(0, std::memory_order_acq_rel);
            if (!HitIdReached(last_id, id)) {
                add += tied;
                was = was || tied_was;
                if (withdraw != nullptr) {
                    *withdraw = id;
                }
            }
        }
        if (judged != nullptr) {
            *judged = was && add != 0;
        }
        return add;
    }
    // Given and not yet taken (for the Objects step's fallback and logs).
    std::int32_t Pending(std::uint64_t component) noexcept {
        Slot* s = Find(component);
        return s == nullptr ? 0 : s->add.load(std::memory_order_acquire) + s->tied.load(std::memory_order_acquire);
    }
    void Clear() noexcept {
        for (auto& s : slots_) {
            s.add.store(0, std::memory_order_relaxed);
            s.tied.store(0, std::memory_order_relaxed);
            s.tied_to.store(kNoHitId, std::memory_order_relaxed);
            s.tied_judged.store(false, std::memory_order_relaxed);
            s.judged.store(false, std::memory_order_relaxed);
            s.key.store(0, std::memory_order_release);
        }
        seen_.fill(0);
        sweep_ = 1;
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> key{0};
        std::atomic<std::int32_t> add{0};
        std::atomic<std::int32_t> tied{0};             // given in place of our replay (Give's tied_to)
        std::atomic<std::uint32_t> tied_to{kNoHitId};  // that replay's hit id
        std::atomic<bool> tied_judged{false};          // (judged, for the tied part alone)
        std::atomic<bool> judged{false};
    };
    static std::size_t Hash(std::uint64_t c) noexcept {
        return static_cast<std::size_t>(((c >> 4) * 0x9e3779b97f4a7c15ull) >> 56);
    }
    // A freed slot's key: no component, and not the end of a probe run (Find goes on past it).
    static constexpr std::uint64_t kFreed = 1;
    Slot* Find(std::uint64_t component) noexcept {
        if (component == 0 || component == kFreed) {
            return nullptr;
        }
        for (std::size_t i = 0; i < kProbes; ++i) {
            Slot& s = slots_[(Hash(component) + i) % kSlots];
            const std::uint64_t key = s.key.load(std::memory_order_acquire);
            if (key == 0) {
                return nullptr;
            }
            if (key == component) {
                return &s;
            }
        }
        return nullptr;
    }
    std::array<Slot, kSlots> slots_{};
    // The writer's own: the sweep in which each slot's component was last alive or given to, and the sweep now.
    std::array<std::uint64_t, kSlots> seen_{};
    std::uint64_t sweep_ = 1;
};

inline LifeTable& TheLifeTable() {
    static LifeTable table;
    return table;
}

// Set once the game's Life step has called the module (the hook is in this
// Hookshot build): only then does a hit go in through LifeTable.
inline std::atomic<bool>& LifeStepSeen() {
    static std::atomic<bool> seen{false};
    return seen;
}

// Set once the step's life stop test has called the module (LIFE_STOPS, the
// hook is in this Hookshot build): a copy's own stops no longer hold its
// owner's death back.
inline std::atomic<bool>& LifeStopsSeen() {
    static std::atomic<bool> seen{false};
    return seen;
}
// Steps that went past their component's own life stops (for the log).
inline std::atomic<std::uint64_t>& StopsPassed() {
    static std::atomic<std::uint64_t> passed{0};
    return passed;
}
// Set once the step's revive check has called the module (LIFE_REVIVE, the
// hook is in this Hookshot build).
inline std::atomic<bool>& LifeReviveSeen() {
    static std::atomic<bool> seen{false};
    return seen;
}
// Deaths whose step skipped the actor's own revive (for the log).
inline std::atomic<std::uint64_t>& RevivesSkipped() {
    static std::atomic<std::uint64_t> skipped{0};
    return skipped;
}
// Later steps of such a death whose revive check was skipped again (for the log; KeptDeaths).
inline std::atomic<std::uint64_t>& RevivesKeptSkipped() {
    static std::atomic<std::uint64_t> skipped{0};
    return skipped;
}

// boss-die-0116 (the test kill left Gohma in 'Die' at life 1, LANE2 2026-11-19 06:47 UTC: 'past its revive 1'):
// the game's Life step asks the revive check (main+0x49d6b0) on EVERY step that ends at life 0 (the `b.le` at
// main+0x49dd14), not only on the one that took the life. So a death whose revive LIFE_REVIVE skipped was revived to
// 1 by its next frame's step (Gohma's callback answers 1 outside her chance time): the skip held one frame. The
// components whose death we kept past their revive are kept here; their later steps at 0 skip it too, until a step
// reads that component alive again (a heal, or a new actor's Life component at the same address: every actor starts
// with a life above 0). Any thread (the steps run on the game's workers). The newest kSlots deaths are kept (a ring: a
// new one takes the oldest's slot; a dead copy is deleted and never read alive, so its slot is never freed that way),
// and the table is cleared at a scene load and a new game process (no Life component of the scene before is left).
class KeptDeaths {
public:
    static constexpr std::size_t kSlots = 16;
    void Keep(std::uint64_t component) noexcept {
        if (component == 0 || Kept(component)) {
            return;
        }
        slots_[next_.fetch_add(1, std::memory_order_relaxed) % kSlots].store(component, std::memory_order_release);
    }
    bool Kept(std::uint64_t component) const noexcept {
        if (component == 0) {
            return false;
        }
        for (const auto& slot : slots_) {
            if (slot.load(std::memory_order_acquire) == component) {
                return true;
            }
        }
        return false;
    }
    // A step read this component with a life above 0.
    void Alive(std::uint64_t component) noexcept {
        if (component == 0) {
            return;
        }
        for (auto& slot : slots_) {
            std::uint64_t was = component;
            slot.compare_exchange_strong(was, 0, std::memory_order_acq_rel);
        }
    }

    // A scene load or a new game process: none of the components kept is there any more.
    void Clear() noexcept {
        for (auto& slot : slots_) {
            slot.store(0, std::memory_order_release);
        }
    }

private:
    std::array<std::atomic<std::uint64_t>, kSlots> slots_{};
    std::atomic<std::size_t> next_{0};  // the slot the next kept death takes (the oldest)
};
inline KeptDeaths& TheKeptDeaths() {
    static KeptDeaths kept;
    return kept;
}

// LIFE_REVIVE's answer: skip the revive for the step whose stops it passed (`passed`: an owner's death on its copy, a
// test kill on our own; that death is kept from then on) and for every later step of a death kept so.
inline bool SkipRevive(KeptDeaths& kept, std::uint64_t component, bool passed) noexcept {
    if (component == 0) {
        return false;
    }
    if (passed) {
        kept.Keep(component);
        return true;
    }
    return kept.Kept(component);
}

// One Life step through the module: the damage the game dealt this frame
// (pushed as a Damage event when not 0) and the sum the step then takes.
// judged: whether the given part was a judged give (LifeTable::Give).
// last_id: the component's last hit id (Life + 0x1b18) as the step runs, after this frame's damage step; withdraw:
// the hit id of our replay to withdraw (a tied give was taken in its place), else kNoHitId (LifeTable::Take).
inline std::int32_t Step(Ring& ring, LifeTable& table, std::uint64_t component, std::int32_t sum,
                         bool* judged = nullptr, std::uint32_t last_id = kNoHitId,
                         std::uint32_t* withdraw = nullptr) noexcept {
    if (sum != 0) {
        Event e;
        e.kind = Kind::Damage;
        e.a = component;
        e.b = static_cast<std::uint32_t>(sum);
        ring.Push(e);
    }
    return sum + table.Take(component, judged, last_id, withdraw);
}

// enemy-late-kill: a give tied to our replay was taken in the replay's place (Step's `withdraw`). The replay still
// waiting in the component (its record's hit id is the replay's and its present word is set) is withdrawn, so a
// damage step that takes it later cannot deal the same fall again. `record_id`/`present`: the record's hit id
// (Life + 0x248) and present word (Life + 0x24c) as read now. True: clear the present word.
constexpr bool WithdrawReplay(std::uint32_t withdraw, std::uint32_t record_id, std::uint32_t present) {
    return withdraw != kNoHitId && record_id == withdraw && present != 0;
}

// Review-boss-0114 risk 2 (Gohma's copy held at life 1): the game's Life step
// (main+0x49d8c0) cuts a frame's sum at each of the component's life stops
// (the list at Life+0x60, count +0x70, that its actor's own code keeps: an
// absolute life, or a share of the maximum), so life stays at the stop and
// the crossing is recorded (Life+0x1be8). A copy's stops are its own game's
// state of a fight its owner runs: Gohma's copy kept her stop at 1 while the
// owner's had none, and the owner's death (21) left the copy at 1 for good.
// So a step that takes a component to its death (sum >= life) goes past its
// own stops when that death is its owner's: a judged give (the owner's life
// for a copy), or a copy boss whose death has begun or whose owner's died
// (CoopBossNative::Table::Finishes). A hit our game judges (our own enemy,
// another player's hit on it) keeps every stop.
constexpr bool PassStops(std::int32_t sum, std::int32_t life, bool judged, bool finishing) noexcept {
    return life > 0 && sum >= life && (judged || finishing);
}

} // namespace CoopActorEvents
