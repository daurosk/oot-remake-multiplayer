// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every place in Ocarina's main that co-op patches, and the check that they
// are still the game's own (issue 136: co-op allows no game mods). No
// emulator code: coop_svc.cpp runs it on main after mods and the emulator's
// built-in patches went in, tests/lichii/puppet_test.cpp on fake modules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_hold.h"
#include "coop_svc.h"

namespace CoopSites {

// The u32 at `at`, little-endian; 0 past the end.
inline std::uint32_t WordAt(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    if (at + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 |
           static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// The first offset of main (from `header` bytes into `module`) at a place co-op
// patches that is not as the game ships it: a hooked call that is not the
// game's word, or a byte of a cave or the hold table that is not zero.
// Nothing when all are as shipped, or `module` is too short to tell.
inline std::optional<std::uint64_t> ChangedSite(const std::vector<std::uint8_t>& module,
                                                std::size_t header = 0) {
    if (module.size() < header + CoopSvc::kCaveEnd) {
        return std::nullopt;
    }
    for (const auto& h : CoopSvc::kHooks) {
        if (WordAt(module, header + h.offset) != h.original) {
            return h.offset;
        }
    }
    if (WordAt(module, header + CoopHold::kHookOffset) != CoopHold::kHookOriginal) {
        return CoopHold::kHookOffset;
    }
    const std::pair<std::uint64_t, std::size_t> zero[] = {
        {CoopSvc::kCaveOffset, CoopSvc::kCave.size()},
        {CoopHold::kCaveOffset, CoopHold::kCave.size()},
        {CoopHold::kTableOffset, CoopHold::kTableSize},
    };
    for (const auto& [from, size] : zero) {
        for (std::uint64_t i = from; i < from + size; ++i) {
            if (module[header + i] != 0) {
                return i;
            }
        }
    }
    return std::nullopt;
}

// Whether a running co-op must stop now: the patcher refused (a code mod
// sits where co-op patches, `refusal` names where) and co-op has not been
// stopped for it yet. The patcher runs when the game's main loads, which can
// be after co-op started and connected (the emulator started directly, not
// through the kit's launcher: test7). Once only.
inline bool MustStopForMod(bool running, bool stopped_already, std::string_view refusal) {
    return running && !stopped_already && !refusal.empty();
}

} // namespace CoopSites
