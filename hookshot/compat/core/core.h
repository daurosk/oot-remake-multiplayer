// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot/compat: the emulator's System, as far as the hook asks it.
#pragma once
#include <array>
#include <cctype>
#include <string>

#include "common/common_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "coop_runtime.h"

namespace Core {
class System {
public:
    Memory::Memory& ApplicationMemory() {
        return memory_;
    }
    Kernel::KProcess* ApplicationProcess() {
        return CoopRuntime::Running() ? &process_ : nullptr;
    }
    u64 GetApplicationProcessProgramID() const {
        return 0x0400486024DEC000ULL;
    }
    // Main's build id, as the emulator keeps it: 0x20 bytes, the id first.
    const std::array<u8, 0x20>& GetApplicationProcessBuildID() {
        build_.fill(0);
        const std::string& hex = CoopRuntime::MainBuildHex();
        const auto nibble = [](char c) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return c >= 'a' ? c - 'a' + 10 : c - '0';
        };
        for (std::size_t i = 0; i + 1 < hex.size() && i / 2 < build_.size(); i += 2) {
            build_[i / 2] = static_cast<u8>(nibble(hex[i]) * 16 + nibble(hex[i + 1]));
        }
        return build_;
    }
    Loader::AppLoader& GetAppLoader() {
        return loader_;
    }

private:
    Memory::Memory memory_;
    Kernel::KProcess process_;
    Loader::AppLoader loader_;
    std::array<u8, 0x20> build_{};
};
}  // namespace Core
