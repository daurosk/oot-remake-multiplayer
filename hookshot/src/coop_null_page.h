// SPDX-License-Identifier: GPL-3.0-or-later
// The null page, armed by the module (issue 150). A co-op puppet is a stripped
// copy of Link; the game's code for it reads through pointers of components
// it does not have and, on the emulator, reads 0 there. Hookshot leaves the
// guest's first MB unmapped, so such a read faults. Patch 0002's guard
// (nx2/coop_puppet_guard.py, host/coop_null_page.c) arms that MB read-only
// when it meets a Link with no component in slot 10; a puppet built with
// perception has one (its Physics), so the guard never fired and the game's
// read of the puppet's missing Player component (f_7102dcee60 after a
// held-item change: guest 0x7d8, lr 0x82deeef8) crashed it. So the module
// arms the same MB itself before its first puppet, with no game rebuild:
// committed read-only with PAGE_GUARD, a read there gives 0 and the first
// read of each page each minute is counted in the log; a write still faults.
// HOS_COOP_NULL_READS=0 leaves it alone (as the exe's switch). Linux: the
// whole guest window is mapped readable already.
// That held-item read is the bow's update (BowRef: the bow and the
// slingshot) and patch 0038 (nx2/coop_puppet_bow_guard.py) now sends a
// puppet's bow through the update's own exit before it: the page stays for
// the other reads of the puppet's missing components (and a game without 0038).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace CoopNullPage {

// Hookshot's guest window (a64-kit rt/a64.h A64_HOST_BASE: guest address a is
// host address base + a) and the part a null pointer's reads land in.
inline constexpr std::uint64_t kGuestBase = 0x100000000000ULL;
inline constexpr std::uint64_t kSize = 0x100000;

// The guest address of a host address in that MB, else nothing.
inline std::optional<std::uint64_t> GuestOffset(std::uint64_t host) {
    if (host < kGuestBase || host >= kGuestBase + kSize) {
        return std::nullopt;
    }
    return host - kGuestBase;
}

// The MB as VirtualQuery finds it.
enum class Mb { Reserved, Committed, Other };

// Whether null reads give 0 once Arm has run: the switch is not "0", and the
// MB is reserved (Arm commits it) or committed already (the exe's guard, or
// large pages).
inline bool Armable(const std::string& null_reads_env, Mb mb) {
    return null_reads_env != "0" && mb != Mb::Other;
}

// This game's MB (Windows; Committed on Linux, readable whole).
Mb Query();
// Arms the MB (once, from any thread). True when null reads give 0 now.
bool Arm(const std::string& null_reads_env);

}  // namespace CoopNullPage
