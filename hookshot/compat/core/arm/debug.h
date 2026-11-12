// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot/compat: the module search, answered by the loader's list.
#pragma once
#include "common/common_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"

namespace Common {
struct ProcessAddress {
    u64 value = 0;
};
}  // namespace Common
inline u64 GetInteger(Common::ProcessAddress a) {
    return a.value;
}
namespace Core {
inline Loader::AppLoader::Modules FindModules(Kernel::KProcess*) {
    Loader::AppLoader::Modules m;
    Loader::AppLoader{}.ReadNSOModules(m);
    return m;
}
inline Common::ProcessAddress FindMainModuleEntrypoint(Kernel::KProcess*) {
    return {CoopRuntime::MainBase()};
}
}  // namespace Core
