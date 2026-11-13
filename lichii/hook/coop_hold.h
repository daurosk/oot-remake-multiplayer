// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AI hold (docs/objects.md, "Holding a copy's AI"): the second cave in
// Ocarina's main and the table it reads. Only with
// LICHII_COOP_OBJECTS_AI_HOLD; no emulator code here.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace CoopHold {

// main+0x369df8 is `blr x8`: the call whose answer gives a BSA component
// (x21) its calc bits for this frame. It becomes `bl kCaveOffset`.
inline constexpr std::uint64_t kHookOffset = 0x369df8;
inline constexpr std::uint32_t kHookOriginal = 0xd63f0100;  // blr x8
inline constexpr std::uint64_t kCaveOffset = 0x3c46300;     // after the co-op cave
inline constexpr std::uint64_t kTableOffset = 0x3c46400;
inline constexpr std::size_t kMaxHeld = 64;
inline constexpr std::size_t kTableSize = 0x10 + 8 * kMaxHeld;

constexpr std::uint32_t HookWord() {
    return 0x94000000u |
           static_cast<std::uint32_t>(((kCaveOffset - kHookOffset) >> 2) & 0x3ffffff);
}

// coop_hold.s, assembled at kCaveOffset.
inline constexpr std::array<std::uint8_t, 76> kCave{
    0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91, 0x00, 0x01, 0x3f, 0xd6,
    0x1f, 0x20, 0x03, 0xd5, 0x89, 0x07, 0x00, 0x10, 0x2a, 0x01, 0x40, 0xb9,
    0x6a, 0x01, 0x00, 0x34, 0x2b, 0x41, 0x00, 0x91, 0x6c, 0x85, 0x40, 0xf8,
    0x9f, 0x01, 0x15, 0xeb, 0x80, 0x00, 0x00, 0x54, 0x4a, 0x05, 0x00, 0x71,
    0x81, 0xff, 0xff, 0x54, 0x04, 0x00, 0x00, 0x14, 0x2a, 0xad, 0x40, 0x29,
    0x00, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x0b, 0x2a, 0xfd, 0x7b, 0xc1, 0xa8,
    0xc0, 0x03, 0x5f, 0xd6,
};

// What the held components' calc bits become: (bits & and_mask) | or_mask.
struct Masks {
    std::uint32_t and_mask = 0xffffffffu;
    std::uint32_t or_mask = 0;
};

// The bit QueryBsaIsBrainSkip reads.
inline constexpr std::uint32_t kBrainSkip = 1;

// LICHII_COOP_OBJECTS_AI_HOLD: 1 sets the brain skip bit on held copies, 2
// clears it (to tell which way the bit works in one run), anything else
// changes nothing.
constexpr Masks MasksFor(int mode) {
    if (mode == 1) {
        return {0xffffffffu, kBrainSkip};
    }
    if (mode == 2) {
        return {~kBrainSkip, 0};
    }
    return {};
}

// The table the cave reads, for these BSA components (0 entries and those
// past kMaxHeld are left out).
inline std::array<std::uint8_t, kTableSize> Table(const std::vector<std::uint64_t>& components,
                                                  const Masks& masks) {
    std::array<std::uint8_t, kTableSize> out{};
    std::uint32_t count = 0;
    for (const std::uint64_t c : components) {
        if (c == 0 || count >= kMaxHeld) {
            continue;
        }
        std::memcpy(out.data() + 0x10 + 8 * count, &c, 8);
        ++count;
    }
    std::memcpy(out.data() + 0, &count, 4);
    std::memcpy(out.data() + 4, &masks.and_mask, 4);
    std::memcpy(out.data() + 8, &masks.or_mask, 4);
    return out;
}

// Hookshot (native-plan row 6): no cave and no guest table. The hold hook
// (patch 0001, main+0x369dfc) calls the module, which looks the component
// up here, in our own memory. One writer (the Objects frame step) and any
// number of readers (the game's job threads running the hook): a sequence
// lock; a reader that meets a write in progress leaves the bits as they
// are for that one call (the next frame holds it again).
class HostTable {
public:
    void Publish(const std::vector<std::uint64_t>& components, const Masks& masks) noexcept {
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);  // odd: a write is going on
        std::atomic_thread_fence(std::memory_order_release);
        std::uint32_t count = 0;
        for (const std::uint64_t c : components) {
            if (c == 0 || count >= kMaxHeld) {
                continue;
            }
            held_[count++].store(c, std::memory_order_relaxed);
        }
        count_.store(count, std::memory_order_relaxed);
        and_.store(masks.and_mask, std::memory_order_relaxed);
        or_.store(masks.or_mask, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }
    // The bits a component gets this frame: (bits & and) | or when held.
    std::uint32_t Apply(std::uint64_t component, std::uint32_t bits) const noexcept {
        if (component == 0) {
            return bits;
        }
        const std::uint32_t s = seq_.load(std::memory_order_acquire);
        if (s & 1) {
            return bits;
        }
        const std::uint32_t count = count_.load(std::memory_order_relaxed);
        bool held = false;
        for (std::uint32_t i = 0; i < count && i < kMaxHeld; ++i) {
            if (held_[i].load(std::memory_order_relaxed) == component) {
                held = true;
                break;
            }
        }
        const std::uint32_t a = and_.load(std::memory_order_relaxed);
        const std::uint32_t o = or_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!held || seq_.load(std::memory_order_relaxed) != s) {
            return bits;
        }
        return (bits & a) | o;
    }
    std::uint32_t Count() const noexcept { return count_.load(std::memory_order_acquire); }
    void Clear() noexcept { Publish({}, Masks{}); }

private:
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<std::uint32_t> count_{0};
    std::atomic<std::uint32_t> and_{0xffffffffu};
    std::atomic<std::uint32_t> or_{0};
    std::array<std::atomic<std::uint64_t>, kMaxHeld> held_{};
};
inline HostTable& TheHostTable() {
    static HostTable table;
    return table;
}

} // namespace CoopHold
