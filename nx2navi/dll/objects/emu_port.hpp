// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the emulator hook's object files (lichii/hook/coop_objects.cpp,
// coop_boss_game.cpp, coop_flags.cpp) get from "the emulator" in the NX2navi
// port. They are compiled unchanged against the small header set in shim/
// (common/logging.h, core/core.h, core/memory.h, ...), whose classes forward
// here:
//
// - guest memory (Core::Memory::Memory) goes to one GuestPort: the plugin
//   interface's read and write in the game (objects.cpp), a fake in the tests;
// - Core::System says the game is up once main is known (SetMain), with
//   Ocarina's program id and build id, and names main's base as the module
//   "main";
// - LOG_INFO and the rest go to one log sink;
// - CoopSvc::AddCaller (coop_svc.h) keeps the callers of the cave, which
//   RunCallers asks once a game frame, as the cave's svc loop does in the
//   emulator (coop_svc.s); CoopSpawn::UseDirector adds the spawn director as
//   the emulator's caller order 10.
//
// No Windows and no plugin interface code here: the tests link it as it is.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "coop_puppet.h"

namespace CoopNx2navi::Objects {

// Guest memory. Read and Write fail quietly (false) on memory that is not
// there; nothing faults.
class GuestPort {
public:
    virtual ~GuestPort() = default;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
    // A compare-exchange on an aligned word: the value that was there (the
    // store happened when it equals `expected`); nothing when unreadable.
    virtual std::optional<std::uint32_t> CompareExchange32(std::uint64_t address,
                                                           std::uint32_t expected,
                                                           std::uint32_t desired) = 0;
};

// The port every Core::Memory::Memory forwards to; null: every access fails.
void SetPort(GuestPort* port);
GuestPort* Port();

// Where main is (0: the game is not up, Core::System has no process).
void SetMain(std::uint64_t main_base);
std::uint64_t Main();

// The log sink for LOG_*; null: dropped. Lines come without a newline.
void SetLog(std::function<void(const std::string&)> sink);
void LogLine(const std::string& line);

// The cave's callers (CoopSvc::AddCaller), asked in order once a game frame.
// Each call one returns goes to `make` (the game call; its x0 in `result`);
// make false stops the frame. At most `max_calls` calls a frame for all,
// as kCallsPerFrame in the emulator. Callers whose order is in `skip` are
// left out (Options::skip_callers). Returns the calls made.
using MakeCall = std::function<bool(const CoopPuppet::Call& call, std::uint64_t& result)>;
std::size_t RunCallers(CoopPuppet::Guest& guest, std::uint64_t now_ms, const MakeCall& make,
                       unsigned max_calls, const std::function<bool(int order)>& skip = {});
// How many callers there are (tests and the status line).
std::size_t CallerCount();
// Takes every caller out (tests only: the emulator files add theirs once,
// from static initialisers or a first call, and never again).
void ClearCallersForTest();

}  // namespace CoopNx2navi::Objects
