// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game function callers (the puppet director first, then fairies, spawns,
// live events ... by order) and the one stepper both runtimes use to ask them
// for calls. No emulator or Hookshot code here:
// - the emulator's cave asks one call at a time through `svc #0xf4`
//   (coop_svc.cpp: Next);
// - Hookshot's frame hook makes the calls itself (hookshot/src: RunFrame).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "coop_puppet.h"

namespace CoopCallers {

// Once a frame a caller is asked with after_call false; each call it returns
// is made, and it is asked again with after_call true and the call's x0 in
// `result`; nothing means done for this frame. Runs on the game's calc thread;
// keep it short.
using Caller = std::function<std::optional<CoopPuppet::Call>(
    CoopPuppet::Guest& guest, bool after_call, std::uint64_t result, std::uint64_t now_ms)>;

// Adds a caller; lower orders go first, equal orders in the order added. The
// puppet director is always asked before all of them. Any thread.
void Add(int order, Caller caller);

// The next call to make, for a runtime that makes one call per question. Pass
// after_call true and the last call's x0 when the last question returned a
// call; false to start a frame. Nothing: no more calls this frame.
std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, bool after_call,
                                     std::uint64_t result, std::uint64_t now_ms);

// One frame's calls, for a runtime that can call game functions itself:
// asks Next until it says nothing or `max_calls` were made. `invoke` makes a
// call and returns its x0, or nothing when it could not (the frame then ends
// there). Returns how many calls were made.
std::size_t RunFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms, std::size_t max_calls,
                     const std::function<std::optional<std::uint64_t>(const CoopPuppet::Call&)>& invoke);

// Tests only: the callers added so far are dropped, and who is asked first is
// `first` (the puppet director by default; a test sets a stand-in).
void ResetForTest(Caller first = {});

} // namespace CoopCallers
