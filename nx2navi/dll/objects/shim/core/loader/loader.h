// SPDX-License-Identifier: GPL-3.0-or-later
// Shim for the NX2navi port (nx2navi/dll/objects/emu_port.hpp): the loaded
// modules, which here are only main at its fixed base.
#pragma once

#include <map>
#include <string>

#include "common/common_types.h"
#include "emu_port.hpp"

namespace Loader {

enum class ResultStatus { Success, ErrorNotInitialized };

class AppLoader {
public:
    using Modules = std::map<VAddr, std::string>;
    ResultStatus ReadNSOModules(Modules& out) {
        const u64 main = ::CoopNx2navi::Objects::Main();
        if (main == 0) {
            return ResultStatus::ErrorNotInitialized;
        }
        out.clear();
        out.emplace(main, "main");
        return ResultStatus::Success;
    }
};

}  // namespace Loader
