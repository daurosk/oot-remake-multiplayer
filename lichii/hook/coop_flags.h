// Shared world flags in the emulator: guest memory that can be written (for
// coop_host's GameFlags and GameFlagStore), the shared list, and debug logging
// of the game's flag tables (docs/flags.md, "Live check").
//
// Present thread only, except LoadShared (called once at start).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "coop/host/flags.hpp"
#include "coop/host/game_flags.hpp"
#include "coop_every.h"

namespace Core::Memory {
class Memory;
}

namespace CoopHook {

// Guest memory through the emulator. Unmapped reads and writes fail quietly.
// The compare-and-swap uses the emulator's exclusive write, a host atomic on
// the same bytes the guest's LDXR/STXR use.
class EmulatorMemory final : public coop::host::WritableGameMemory {
public:
    EmulatorMemory(Core::Memory::Memory& memory, u64 base) : memory_(memory), base_(base) {}
    std::uint64_t main_base() const override {
        return base_;
    }
    bool read(std::uint64_t address, std::span<std::uint8_t> out) override;
    bool write(std::uint64_t address, std::span<const std::uint8_t> data) override;
    std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t address,
                                                      std::uint32_t expected,
                                                      std::uint32_t desired) override;

private:
    bool Valid(std::uint64_t address, std::size_t size) const;

    Core::Memory::Memory& memory_;
    u64 base_;
};

// The shared flag list: the player's items (coop::host::item_shared_flags,
// docs/items.md; off with LICHII_COOP_ITEMS=0) and the flags in the file
// LICHII_COOP_FLAGS (coop::host::SharedFlags format). Nothing when both are
// off; `error` is set when the file is bad (then flags are not shared).
std::optional<coop::host::SharedFlags> LoadShared(std::string& error);

// The debug side, from environment variables (docs/lichii.md):
// - LICHII_COOP_DEBUG_FLAGS=1: logs every change in the Bool, Int and 64-bit
//   key tables, a few times a second;
// - LICHII_COOP_DEBUG_KEY64=<hex key>: also logs get_key64 of that key;
// - LICHII_COOP_SET_KEY64=<hex key>: sets that key once the game is up;
// - LICHII_COOP_DEBUG_BOOL=<hex hash>: also logs get_bool of that flag;
// - LICHII_COOP_SET_BOOL=<hex hash>: sets that Bool flag once the game is up;
// - LICHII_COOP_SET_AFTER=<seconds>: waits that long after the game is up
//   before setting.
class FlagDebug {
public:
    FlagDebug();
    bool on() const {
        return on_;
    }
    // Once per frame while the right game runs.
    void Frame(EmulatorMemory& memory);
    // A new game process: forget the tables and set again.
    void Reset();

private:
    struct Table {
        bool known = false;
        std::vector<std::uint8_t> bytes;
    };
    void Scan(EmulatorMemory& memory);
    void ScanNamed(EmulatorMemory& memory, u64 manager, const char* name, u64 table,
                   std::size_t entry_size, std::size_t value_offset, std::size_t value_size,
                   Table& last);
    void ScanKeys(EmulatorMemory& memory, u64 manager);
    void Sets(EmulatorMemory& memory);

    bool on_ = false;
    std::optional<std::uint64_t> watch_key_;
    std::optional<std::uint64_t> set_key_;
    std::optional<std::uint32_t> set_bool_;
    bool key_set_ = false;
    bool bool_set_ = false;
    std::optional<std::uint32_t> watch_bool_;
    std::chrono::seconds set_after_{0};
    std::optional<std::chrono::steady_clock::time_point> up_at_;
    std::optional<std::optional<bool>> watch_last_;
    std::optional<std::optional<bool>> watch_bool_last_;
    CoopEvery::Every scan_{CoopEvery::kQuarterUs};
    CoopEvery::Every relog_{5 * CoopEvery::kSecondUs};
    Table bools_, ints_, keys_;
};

} // namespace CoopHook
