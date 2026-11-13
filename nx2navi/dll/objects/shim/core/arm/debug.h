// SPDX-License-Identifier: GPL-3.0-or-later
// Shim for the NX2navi port (nx2navi/dll/objects/emu_port.hpp): the emulator's
// module search, answered from the main base the port knows.
#pragma once

#include "common/common_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "emu_port.hpp"

namespace Common {
struct ProcessAddress {
    u64 value = 0;
};
constexpr u64 GetInteger(ProcessAddress a) { return a.value; }
}  // namespace Common

namespace Core {

inline Loader::AppLoader::Modules FindModules(Kernel::KProcess*) {
    Loader::AppLoader::Modules modules;
    Loader::AppLoader loader;
    loader.ReadNSOModules(modules);
    return modules;
}

inline Common::ProcessAddress FindMainModuleEntrypoint(Kernel::KProcess*) {
    return Common::ProcessAddress{::CoopNx2navi::Objects::Main()};
}

}  // namespace Core
