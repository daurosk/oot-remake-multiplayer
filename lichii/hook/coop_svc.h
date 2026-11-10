// The emulator side of puppets (docs/puppet.md): the code cave patched into
// Ocarina's main and the `svc #0xf4` it calls. Built into the emulator's core
// library only when LICHII_COOP is on; the patched emulator calls these two.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_puppet.h"

namespace Core {
class System;
}

namespace CoopSvc {

inline constexpr std::uint32_t kPuppetSvc = 0xf4;
// The two places in main (both in moduleCalc_) that call the frame function
// 0x3b4e300; 0x6e70f8 is the one that runs every frame, 0x6e6dd8 the rare one.
// Both become `bl kCaveOffset`; the cave ends in `b 0x3b4e300`.
struct Hook {
    std::uint64_t offset;   // in main
    std::uint32_t original; // bl 0x3b4e300
};
inline constexpr std::array<Hook, 2> kHooks{{
    {0x6e6dd8, 0x94d19d4a},
    {0x6e70f8, 0x94d19c82},
}};
// In main's zero padding after the code (0x3c461d0 to 0x3c47000, where rodata
// starts), clear of what others patch there (kTakenByOthers). Until test6 the
// cave was at 0x3c46280, which the Hyrule Cheat Menu mod also writes (#136).
inline constexpr std::uint64_t kCaveOffset = 0x3c46a00;
inline constexpr std::uint64_t kCaveEnd = 0x3c47000;       // rodata starts here

// Bytes of main that other patches write, which ours must never touch
// (docs/puppet.md, "Where the caves are"). Half-open ranges.
struct Taken {
    std::uint64_t begin;
    std::uint64_t end;
    const char* who;
};
inline constexpr std::array<Taken, 17> kTakenByOthers{{
    {0x3c46200, 0x3c46278, "Fritchii Dynamic FPS routine"},
    {0x6e6cc4, 0x6e6cc8, "Fritchii Dynamic FPS / high refresh call"},
    {0x3c46e00, 0x3c46f10, "Fritchii v5.7 high refresh (Ctrl+U) cave"},
    {0x475f8, 0x475fc, "Fritchii v5.7 water ripple hook"},
    {0x3c46280, 0x3c46398, "Hyrule Cheat Menu v0.1 stubs"},
    {0x49d8c0, 0x49d8c4, "Hyrule Cheat Menu v0.1 jump"},
    {0x49dcf8, 0x49dcfc, "Hyrule Cheat Menu v0.1 jump"},
    {0x2f8e760, 0x2f8e764, "Hyrule Cheat Menu v0.1 jump"},
    {0x32c1db0, 0x32c1db4, "Hyrule Cheat Menu v0.1 jump"},
    {0x32d8500, 0x32d8504, "Hyrule Cheat Menu v0.1 jump"},
    {0x34694f8, 0x34694fc, "Hyrule Cheat Menu v0.1 jump"},
    {0x3469524, 0x3469528, "Hyrule Cheat Menu v0.1 jump"},
    {0x3469550, 0x3469554, "Hyrule Cheat Menu v0.1 jump"},
    {0x346966c, 0x3469670, "Hyrule Cheat Menu v0.1 jump"},
    {0x3469698, 0x346969c, "Hyrule Cheat Menu v0.1 jump"},
    {0x346bff0, 0x346bff4, "Hyrule Cheat Menu v0.1 jump"},
    {0x346c0d4, 0x346c0d8, "Hyrule Cheat Menu v0.1 jump"},
}};

// `bl kCaveOffset` at `offset`.
constexpr std::uint32_t HookWord(std::uint64_t offset) {
    return 0x94000000u | static_cast<std::uint32_t>(((kCaveOffset - offset) >> 2) & 0x3ffffff);
}

// Game calls the cave makes in one frame at most, for all callers together
// (`mov w20, #256` in coop_svc.s). It was 16: a scene with more enemy copies
// than that left some unmoved and no call for their animations.
inline constexpr unsigned kCallsPerFrame = 256;

// coop_svc.s, assembled at kCaveOffset (the commands are in that file).
inline constexpr std::array<std::uint8_t, 128> kCave{
    0xfd, 0x7b, 0xbd, 0xa9, 0xfd, 0x03, 0x00, 0x91, 0xf3, 0x53, 0x01, 0xa9,
    0xe8, 0x13, 0x00, 0xfd, 0xf3, 0x03, 0x00, 0xaa, 0x08, 0x40, 0x20, 0x1e,
    0xff, 0x03, 0x04, 0xd1, 0x14, 0x20, 0x80, 0x52, 0xe1, 0x03, 0x1f, 0xaa,
    0xe2, 0x03, 0x1f, 0xaa, 0xe0, 0x03, 0x00, 0x91, 0x81, 0x1e, 0x00, 0xd4,
    0xa0, 0x01, 0x00, 0xb4, 0x94, 0x06, 0x00, 0x71, 0x64, 0x01, 0x00, 0x54,
    0xf0, 0x13, 0x40, 0xf9, 0xe0, 0x87, 0x42, 0xa9, 0xe2, 0x8f, 0x43, 0xa9,
    0xe4, 0x97, 0x44, 0xa9, 0xe6, 0x9f, 0x45, 0xa9, 0xe0, 0x6b, 0x40, 0xbd,
    0x00, 0x02, 0x3f, 0xd6, 0xe1, 0x03, 0x00, 0xaa, 0x22, 0x00, 0x80, 0xd2,
    0xf2, 0xff, 0xff, 0x17, 0xff, 0x03, 0x04, 0x91, 0xe0, 0x03, 0x13, 0xaa,
    0x00, 0x41, 0x20, 0x1e, 0xe8, 0x13, 0x40, 0xfd, 0xf3, 0x53, 0x41, 0xa9,
    0xfd, 0x7b, 0xc3, 0xa8, 0x21, 0x1e, 0xfc, 0x17,
};

// The buffer the cave hands over (coop_svc.s).
inline constexpr std::uint64_t kBufferStack = 0x00;
inline constexpr std::uint64_t kBufferFunction = 0x20;
inline constexpr std::uint64_t kBufferRegisters = 0x28;
inline constexpr std::uint64_t kBufferS0 = 0x68;  // f32, loaded into s0 before the call

// PatchManager::PatchNSO, after its built-in patches: when puppets are on
// (LICHII_COOP_PUPPET=1 or LICHII_COOP_DEBUG_PUPPET=1) and this is
// Ocarina's main of build e0f3ccfa..., writes the call and the cave into
// `out` (NSO header, then the module), checked against `original` and `out`.
void PatchMain(std::uint64_t title_id, std::string_view module, std::string_view build_id,
               const std::vector<std::uint8_t>& original, std::vector<std::uint8_t>& out);

// Why PatchMain left co-op's code out of this game (another mod writes where
// the caves or hooks go), for the overlay; empty when it did not.
std::string Refusal();

// Something else that calls game functions from the cave, with the same
// contract as CoopPuppet::Director::Step: once a frame it is asked with
// after_call false; each call it returns is made, and it is asked again with
// after_call true and the call's x0 in result; nothing means done for this
// frame. The puppet director always goes first, then the callers by order
// (CoopSpawn 10). Runs on the game's calc thread; keep it short. Any thread,
// best before the game starts. The cave is patched only when Wanted() is
// true (LICHII_COOP_PUPPET, LICHII_COOP_DEBUG_PUPPET or LICHII_COOP_SPAWN).
using Caller = std::function<std::optional<CoopPuppet::Call>(
    CoopPuppet::Guest& guest, bool after_call, std::uint64_t result, std::uint64_t now_ms)>;
void AddCaller(int order, Caller caller);

// Kernel::Svc::Call, before any other SVC: handles svc 0xf4 from Ocarina
// and returns true, with the registers to give back in `args`. False for
// anything else, which then goes on as before.
bool Call(Core::System& system, std::uint32_t imm, std::array<std::uint64_t, 8>& args);

} // namespace CoopSvc
