// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot/compat: the loader's module list: main at its Hookshot address.
#pragma once
#include <map>
#include <string>

#include "common/common_types.h"
#include "coop_runtime.h"

namespace Loader {
enum class ResultStatus { Success, ErrorNotInitialized };
class AppLoader {
public:
    using Modules = std::map<u64, std::string>;
    ResultStatus ReadNSOModules(Modules& out) {
        if (!CoopRuntime::Running()) {
            return ResultStatus::ErrorNotInitialized;
        }
        out = {{CoopRuntime::MainBase(), "main"}};
        return ResultStatus::Success;
    }
};
}  // namespace Loader
