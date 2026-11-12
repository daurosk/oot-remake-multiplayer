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
#include <vector>

namespace CoopActorEvents {

enum class Kind : std::uint32_t {
    None = 0,
    Create = 1,  // a create request: name, handle slot, caller, create argument
    Delete = 2,  // a delete request: actor, reason
    Damage = 3,  // a Life step's frame damage: Life component, amount
};

struct Event {
    Kind kind = Kind::None;
    std::uint64_t a = 0;       // Create: the name object; Delete: the actor; Damage: the Life component
    std::uint64_t b = 0;       // Create: the handle slot; Delete: the reason; Damage: the amount (int32)
    std::uint64_t from = 0;    // Create: the caller (guest return address)
    std::uint64_t place[3]{};  // Create: the asked position (bits of 3 floats, 0 when not read)
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
        s.a.store(e.a, std::memory_order_relaxed);
        s.b.store(e.b, std::memory_order_relaxed);
        s.from.store(e.from, std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) {
            s.place[i].store(e.place[i], std::memory_order_relaxed);
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
            e.a = s.a.load(std::memory_order_relaxed);
            e.b = s.b.load(std::memory_order_relaxed);
            e.from = s.from.load(std::memory_order_relaxed);
            for (int i = 0; i < 3; ++i) {
                e.place[i] = s.place[i].load(std::memory_order_relaxed);
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
        std::atomic<std::uint64_t> a{0}, b{0}, from{0};
        std::atomic<std::uint64_t> place[3]{};
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

} // namespace CoopActorEvents
