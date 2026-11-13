// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AI hold for enemy copies (docs/objects.md, "Holding a copy's AI") on
// the plugin interface's function hooks (version 2), in place of the
// emulator's patch at main+0x369df8 and its cave (lichii/hook/coop_hold.s).
//
// What the game does (build e0f3ccfaf50065ae, read from main):
// - Each frame the BSA components (the enemies' "brains", component table
//   +0xb0) are walked in three lists: twice by main+0x369b00 (called from the
//   frame function at main+0x3b57010 and +0x3b57020), once by a copy of the
//   same loop inlined in that frame function.
// - For each component the loop asks an object for its calc bits:
//       x0 = [manager + 0x950], x1 = the component, blr [[x0] + 0x40 or 0x48]
//   at main+0x369df8 (returning to +0x369dfc) and at main+0x3b56d60 (returning
//   to +0x3b56d64), and puts bits 0 to 3 of the answer into the component's
//   u16 at +0xc4. Bit 0 is what QueryBsaIsBrainSkip reads. The emulator's
//   patch covers only the first call site; the inlined one was not patched.
//
// The hook here:
// - main+0x369b00 is hooked to find the answering functions: its x1 points at
//   the manager, so [[manager + 0x950]] is the object's vtable and slots 0x40
//   and 0x48 are the two functions. Each one found is hooked once.
// - The answer hook: when the call returns into one of the two call sites
//   (call->lr) and its x1 is a held component, the function runs
//   (call_original) and its answer becomes (answer & and) | or, exactly what
//   the emulator's cave does.
// - If neither answering function can be hooked, the loop hook falls back to
//   rewriting bits 0 to 3 of +0xc4 of every held component after the loop ran
//   (two of the three lists).
//
// The held components and the masks come from the hook's own Mover, which
// writes CoopHold::Table bytes to main+CoopHold::kTableOffset every game
// frame: the port's guest memory catches that write (TakeTable) instead of
// storing it into the game's code.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "coop_hold.h"

struct Nx2PluginApi;
struct Nx2HookCall;

namespace CoopNx2navi::Objects::Hold {

inline constexpr std::uint64_t kLoop = 0x369b00;            // the BSA calc loop
inline constexpr std::uint64_t kAnswerReturn = 0x369dfc;    // after blr x8 at +0x369df8
inline constexpr std::uint64_t kInlineReturn = 0x3b56d64;   // after blr x8 at +0x3b56d60
inline constexpr std::uint64_t kManagerAnswerer = 0x950;    // [manager + 0x950]
inline constexpr std::array<std::uint64_t, 2> kAnswerSlots{0x40, 0x48};
inline constexpr std::uint64_t kBsaFlags = 0xc4;            // u16, bits 0-3 from the answer

// The held components and masks, as the Mover last wrote them. Lock-free:
// the game's threads read it in the hook while the calc thread writes it.
class Table {
public:
    // Bytes in CoopHold::Table's layout (count, and, or, components).
    void Take(std::span<const std::uint8_t> bytes);
    // The answer for `component`: (bits & and) | or when held, else nothing.
    bool Alter(std::uint64_t component, std::uint32_t& bits) const;
    std::size_t Count() const { return count_.load(std::memory_order_acquire); }
    // The held components (Count() of them), for the fallback.
    std::uint64_t At(std::size_t i) const { return held_[i].load(std::memory_order_relaxed); }
    std::uint32_t And() const { return and_.load(std::memory_order_relaxed); }
    std::uint32_t Or() const { return or_.load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<std::uint64_t>, CoopHold::kMaxHeld> held_{};
    std::atomic<std::size_t> count_{0};
    std::atomic<std::uint32_t> and_{0xffffffffu};
    std::atomic<std::uint32_t> or_{0};
};

Table& TheTable();

// Whether a write into guest memory is the Mover's table write; if so it is
// taken here and must not reach the game (it would be refused anyway: it is
// main's code padding).
bool TakeTable(std::uint64_t main_base, std::uint64_t address, const void* in, std::size_t size);

// Hooks the loop (and, once found, the answering functions). False when the
// interface has no function hooks (version 1) or the loop cannot be hooked:
// then nothing is held. Any thread.
bool Install(const Nx2PluginApi* api, std::uint64_t main_base);
// Takes the hooks out.
void Remove();

// Counters for the status line.
struct Counts {
    std::uint64_t loops = 0;        // loop calls seen
    std::uint64_t answers = 0;      // answer calls from the two sites
    std::uint64_t altered = 0;      // of them, held components whose answer was changed
    std::uint64_t fallback = 0;     // components rewritten after the loop (fallback)
    unsigned answer_hooks = 0;      // answering functions hooked
    unsigned refused = 0;           // answering functions the interface refused
};
Counts Take();

// The hook callbacks, for the tests (a fake interface calls them).
void OnLoop(Nx2HookCall* call, void* user);
void OnAnswer(Nx2HookCall* call, void* user);

}  // namespace CoopNx2navi::Objects::Hold
