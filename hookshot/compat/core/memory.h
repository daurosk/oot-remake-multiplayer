// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot/compat: the emulator's guest memory, over the bridge.
#pragma once
#include <cstddef>
#include <cstdint>

#include "common/common_types.h"
#include "coop_runtime.h"

namespace Core::Memory {
class Memory {
public:
    bool IsValidVirtualAddressRange(u64 address, u64 size) const {
        return CoopRuntime::Valid(address, static_cast<std::size_t>(size));
    }
    bool ReadBlockUnsafe(u64 address, void* out, std::size_t size) const {
        return CoopRuntime::Read(address, out, size);
    }
    bool WriteBlockUnsafe(u64 address, const void* in, std::size_t size) {
        return CoopRuntime::Write(address, in, size);
    }
    bool ReadBlock(u64 address, void* out, std::size_t size) const {
        return CoopRuntime::Read(address, out, size);
    }
    bool WriteBlock(u64 address, const void* in, std::size_t size) {
        return CoopRuntime::Write(address, in, size);
    }
    u8 Read8(u64 a) const {
        return ReadT<u8>(a);
    }
    u16 Read16(u64 a) const {
        return ReadT<u16>(a);
    }
    u32 Read32(u64 a) const {
        return ReadT<u32>(a);
    }
    u64 Read64(u64 a) const {
        return ReadT<u64>(a);
    }
    void Write8(u64 a, u8 v) {
        CoopRuntime::Write(a, &v, sizeof v);
    }
    void Write16(u64 a, u16 v) {
        CoopRuntime::Write(a, &v, sizeof v);
    }
    void Write32(u64 a, u32 v) {
        CoopRuntime::Write(a, &v, sizeof v);
    }
    void Write64(u64 a, u64 v) {
        CoopRuntime::Write(a, &v, sizeof v);
    }
    // Stores `value` when the word is `expected`; true when it stored.
    bool WriteExclusive32(u64 address, u32 value, u32 expected) {
        u32 seen = 0;
        return CoopRuntime::Cas32(address, expected, value, seen) && seen == expected;
    }

private:
    template <typename T>
    T ReadT(u64 a) const {
        T v{};
        CoopRuntime::Read(a, &v, sizeof v);
        return v;
    }
};
}  // namespace Core::Memory
