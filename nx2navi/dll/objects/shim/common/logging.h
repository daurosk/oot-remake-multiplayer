// SPDX-License-Identifier: GPL-3.0-or-later
// Shim for the NX2navi port (nx2navi/dll/objects/emu_port.hpp): the emulator's
// LOG_* macros with their {} formatting (std::format), into the port's log.
#pragma once

#include <format>
#include <string>

#include "common/common_types.h"
#include "emu_port.hpp"

// The hook formats one hex dump with fmt::format directly.
namespace fmt {
using std::format;
}

#define COOP_SHIM_LOG(level, ...) \
    ::CoopNx2navi::Objects::LogLine(std::string(level) + std::format(__VA_ARGS__))
#define LOG_TRACE(category, ...) ((void)0)
#define LOG_DEBUG(category, ...) ((void)0)
#define LOG_INFO(category, ...) COOP_SHIM_LOG("", __VA_ARGS__)
#define LOG_WARNING(category, ...) COOP_SHIM_LOG("warning: ", __VA_ARGS__)
#define LOG_ERROR(category, ...) COOP_SHIM_LOG("error: ", __VA_ARGS__)
#define LOG_CRITICAL(category, ...) COOP_SHIM_LOG("critical: ", __VA_ARGS__)
