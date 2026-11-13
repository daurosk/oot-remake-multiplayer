// SPDX-License-Identifier: GPL-3.0-or-later
// Shim for the NX2navi port (nx2navi/dll/objects/emu_port.hpp): the emulator's
// guest memory, as far as the hook's object files use it, over the port's
// GuestPort.
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/common_types.h"
#include "emu_port.hpp"

namespace Core::Memory {

class Memory {
public:
    // Whether every byte of the range can be read now.
    bool IsValidVirtualAddressRange(u64 address, std::size_t size) const {
        auto* port = ::CoopNx2navi::Objects::Port();
        if (port == nullptr || size == 0 || address + size < address || address < 0x1000) {
            return port != nullptr && size == 0;
        }
        // A one-byte read at each end and at every page boundary between.
        unsigned char byte = 0;
        for (u64 at = address; at < address + size; at = (at | 0xfff) + 1) {
            if (!port->Read(at, &byte, 1)) {
                return false;
            }
        }
        return port->Read(address + size - 1, &byte, 1);
    }
    bool ReadBlockUnsafe(u64 address, void* out, std::size_t size) {
        auto* port = ::CoopNx2navi::Objects::Port();
        return port != nullptr && port->Read(address, out, size);
    }
    bool ReadBlock(u64 address, void* out, std::size_t size) { return ReadBlockUnsafe(address, out, size); }
    bool WriteBlockUnsafe(u64 address, const void* in, std::size_t size) {
        auto* port = ::CoopNx2navi::Objects::Port();
        return port != nullptr && port->Write(address, in, size);
    }
    bool WriteBlock(u64 address, const void* in, std::size_t size) {
        return WriteBlockUnsafe(address, in, size);
    }
    u8 Read8(u64 address) { return Read<u8>(address); }
    u16 Read16(u64 address) { return Read<u16>(address); }
    u32 Read32(u64 address) { return Read<u32>(address); }
    u64 Read64(u64 address) { return Read<u64>(address); }
    void Write8(u64 address, u8 v) { WriteBlockUnsafe(address, &v, sizeof(v)); }
    void Write16(u64 address, u16 v) { WriteBlockUnsafe(address, &v, sizeof(v)); }
    void Write32(u64 address, u32 v) { WriteBlockUnsafe(address, &v, sizeof(v)); }
    void Write64(u64 address, u64 v) { WriteBlockUnsafe(address, &v, sizeof(v)); }
    // Stores `value` when the word holds `expected`; true when it stored.
    bool WriteExclusive32(u64 address, u32 value, u32 expected) {
        auto* port = ::CoopNx2navi::Objects::Port();
        if (port == nullptr) {
            return false;
        }
        const auto was = port->CompareExchange32(address, expected, value);
        return was && *was == expected;
    }

private:
    template <typename T>
    T Read(u64 address) {
        T value{};
        ReadBlockUnsafe(address, &value, sizeof(value));
        return value;
    }
};

}  // namespace Core::Memory
