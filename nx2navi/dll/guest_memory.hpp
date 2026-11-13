// SPDX-License-Identifier: GPL-3.0-or-later
// The game's memory inside the NX2navi port, for coop/host.
//
// NX2navi is a static recompilation: the game's code is host code, and its
// data lives in the same process at the addresses the game itself uses ("guest
// address equals host address", NX2navi docs/architecture.md; the loader
// commits each module at its base and copies the segments there,
// runtime/loader.c load_module). The modules start at a fixed base chosen by
// its recomp/layout.py (FIRST_BASE), with `main` first. So a guest pointer is
// a pointer in this process, and main's offsets (coop/host/src/addresses.hpp)
// are added to that base.
//
// This DLL is a foreign thread in that process: the game runs while we read.
// Every read therefore asks first what is mapped (VirtualQuery) and then goes
// through the kernel (ReadProcessMemory on our own process), which fails
// quietly if the page went away meanwhile and raises no exception, so the
// port's own fault handlers never see us. Guard pages are left alone.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "coop/host/game_flags.hpp"

namespace CoopNx2navi {

// Where NX2navi maps `main` (its recomp/layout.py, FIRST_BASE).
inline constexpr std::uint64_t kMainBase = 0x7000000000ull;

// Copies `size` bytes at `address` of this process. False when any of them
// cannot be read; never faults.
bool SafeRead(std::uint64_t address, void* out, std::size_t size);

// What is mapped at a base that should hold the game's main.
struct MainCheck {
    enum class State {
        not_mapped,   // nothing committed there (not NX2navi, or not loaded yet)
        no_header,    // memory, but no module header (still loading, or not a game)
        no_build_id,  // a module, but no build id note found in it
        other_build,  // a module of another game build: `build_id` says which
        ok,           // the build co-op's addresses are for
    };
    State state = State::not_mapped;
    std::string build_id;      // upper-case hex of the note found (other_build, ok)
    std::uint64_t size = 0;    // bytes committed from the base on (capped)
};

// Looks at `base`: committed memory, the module header ("MOD0" at the offset
// stored at base + 4), and the GNU build id note the linker left in main's
// read-only data, compared with `expected_id` (upper-case hex, the whole id
// or its first bytes).
MainCheck CheckMain(std::uint64_t base, const std::string& expected_id);

// Finds a GNU build id note in a block of bytes: every 20-byte id found, as
// upper-case hex. No game or Windows code: tested on its own.
std::vector<std::string> FindBuildIds(std::span<const std::uint8_t> bytes);

class Memory final : public coop::host::WritableGameMemory {
public:
    explicit Memory(std::uint64_t base = kMainBase) : base_(base) {}

    std::uint64_t main_base() const override { return base_; }
    bool read(std::uint64_t address, std::span<std::uint8_t> out) override;
    // Only into committed read-write memory: never into code or read-only data.
    bool write(std::uint64_t address, std::span<const std::uint8_t> data) override;
    // A locked compare-exchange on the word itself. The port turns the game's
    // own CAS and exclusive stores into host atomics on the same words
    // (NX2navi recomp/lifter.py), so this is atomic against them.
    std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t address, std::uint32_t expected,
                                                      std::uint32_t desired) override;

    // Reads are kept by page until the next Fresh(): one walk through the
    // flag tables is thousands of small reads of the same few pages. Call it
    // at the start of every frame and before a write that depends on what was
    // just read.
    void Fresh() { ++generation_; }

    std::uint64_t reads() const { return reads_; }
    std::uint64_t failed_reads() const { return failed_; }

private:
    static constexpr std::size_t kPage = 4096;
    static constexpr std::size_t kSlots = 128;
    struct Slot {
        std::uint64_t page = 0;
        std::uint64_t generation = 0;  // 0: empty
        bool readable = false;
        std::array<std::uint8_t, kPage> bytes;
    };
    const Slot* Page(std::uint64_t page);

    std::uint64_t base_;
    std::uint64_t generation_ = 1;
    std::uint64_t reads_ = 0;
    std::uint64_t failed_ = 0;
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);
};

}  // namespace CoopNx2navi
