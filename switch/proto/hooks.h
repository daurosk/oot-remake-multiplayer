// SPDX-License-Identifier: GPL-3.0-or-later
// The module's hook table (section .coop_hooks, exported as `coop_hooks`).
//
// One table, two readers:
//   - on an emulator or console, the module's own _init applies it (rewrites the
//     `bl`, fills the slots with main's addresses);
//   - a recomp that lifts this module as subsdk1 reads it at build time, turns every
//     call through a slot into a direct call to the lifted game function, applies
//     the redirects in its lifted code, and sets `applied_by_host` to 1 in the lifted
//     data, so _init leaves everything alone.
// So the module makes no call the lifter cannot resolve: every game call and every
// hook continue goes through a named, exported slot listed here.
//
// Layout (little-endian, version 1), read from the file's data segment. The linker
// writes module-relative addresses into the pointer fields (--apply-dynamic-relocs),
// so a reader needs no relocation pass:
//   header, 32 bytes: char magic[8] "COOPHOOK"; u32 version; u32 entry_size (32);
//                     u32 count; u32 applied_by_host; u64 reserved
//   entry,  32 bytes: u16 kind; u16 reserved; u32 expected_insn; u64 main_offset;
//                     u64 replacement (module address or 0); u64 slot (module address)
#pragma once

#include <stdint.h>

#define COOP_EXPORT __attribute__((visibility("protected")))

namespace hooks {

enum Kind : uint16_t {
    // The `bl` at main_offset (expected_insn) calls `replacement` instead; the slot
    // gets the bl's original target, where `replacement` continues.
    kCallRedirect = 1,
    // Calls to the function at main_offset go to `replacement`; the slot gets an
    // entry to the original. Defined for the format; the prototype has none and
    // _init does not apply it (it needs a trampoline on emulators).
    kEntry = 2,
    // No hook: the slot gets the game function at main_offset. expected_insn is
    // that function's first instruction, or 0 for no check.
    kGameFn = 3,
};

struct Entry {
    uint16_t kind;
    uint16_t reserved;
    uint32_t expected_insn;
    uint64_t main_offset;
    void (*replacement)();  // data only: never called through, so no indirect call
    uint64_t* slot;
};
static_assert(sizeof(Entry) == 32);

struct Table {
    char magic[8];
    uint32_t version;
    uint32_t entry_size;
    uint32_t count;
    uint32_t applied_by_host;
    uint64_t reserved;
    Entry entries[2];
};
static_assert(sizeof(Table) == 32 + 2 * 32);

}  // namespace hooks
