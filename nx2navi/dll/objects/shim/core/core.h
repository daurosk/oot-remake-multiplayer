// SPDX-License-Identifier: GPL-3.0-or-later
// Shim for the NX2navi port (nx2navi/dll/objects/emu_port.hpp): the emulator's
// System, as far as the hook's object files ask it. The game "has a process"
// once the port knows main (SetMain), and it is always Ocarina of Time 1.1.1:
// the DLL checks the build id note in main before that (guest_memory.cpp,
// CheckMain).
#pragma once

#include <array>

#include "common/common_types.h"
#include "core/arm/debug.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "emu_port.hpp"

namespace Core {

class System {
public:
    Memory::Memory& ApplicationMemory() { return memory_; }
    Kernel::KProcess* ApplicationProcess() {
        return ::CoopNx2navi::Objects::Main() != 0 ? &process_ : nullptr;
    }
    u64 GetApplicationProcessProgramID() const { return 0x0400486024DEC000ULL; }
    const std::array<u8, 0x20>& GetApplicationProcessBuildID() const {
        static constexpr std::array<u8, 0x20> kBuild{0xe0, 0xf3, 0xcc, 0xfa, 0xf5, 0x00, 0x65, 0xae};
        return kBuild;
    }
    Loader::AppLoader& GetAppLoader() { return loader_; }

private:
    Memory::Memory memory_;
    Kernel::KProcess process_;
    Loader::AppLoader loader_;
};

}  // namespace Core
