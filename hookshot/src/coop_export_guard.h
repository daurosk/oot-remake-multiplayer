// SPDX-License-Identifier: GPL-3.0-or-later
// save-crash-0118: every C export Hookshot calls (hos_coop_*) runs behind the guarded boundary (coop_boundary.h):
// nothing thrown in co-op crosses into Hookshot's C, where it ended the game (std::bad_alloc at the PC's commit limit,
// runs 081813 + 082906). The call returns what co-op off would, and the export logs once (then each 1024th throw).
#pragma once

#include <cstdint>
#include <utility>

#include "common/logging.h"
#include "coop_boundary.h"

namespace CoopExportGuard {
inline void LogThrown(const char* where, const char* what, std::uint64_t n) {
    LOG_ERROR(Core, "coop: {} threw ({}, {} so far): that call did what co-op off does; the game goes on", where, what,
              n);
}

template <class R, class F>
R Guard(CoopBoundary::Site& site, R fallback, F&& f) noexcept {
    return CoopBoundary::Call(site, fallback, std::forward<F>(f), CoopExportGuard::LogThrown);
}
template <class F>
void Guard(CoopBoundary::Site& site, F&& f) noexcept {
    CoopBoundary::Run(site, std::forward<F>(f), CoopExportGuard::LogThrown);
}
}  // namespace CoopExportGuard
