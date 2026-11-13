// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot/compat: the emulator's LOG_* macros, into longshot_log.txt.
#pragma once
#include <fmt/format.h>

#include <string>
#include <utility>

#include "common/common_types.h"
#include "coop_runtime.h"

namespace CoopCompat {
template <typename... Args>
void LogLine(const char* level, fmt::format_string<Args...> format, Args&&... args) {
    CoopRuntime::Log(std::string(level) + fmt::format(format, std::forward<Args>(args)...));
}
}  // namespace CoopCompat

#define LOG_TRACE(category, ...) ((void)0)
#define LOG_DEBUG(category, ...) ((void)0)
#define LOG_INFO(category, ...) ::CoopCompat::LogLine("", __VA_ARGS__)
#define LOG_WARNING(category, ...) ::CoopCompat::LogLine("warning: ", __VA_ARGS__)
#define LOG_ERROR(category, ...) ::CoopCompat::LogLine("error: ", __VA_ARGS__)
#define LOG_CRITICAL(category, ...) ::CoopCompat::LogLine("critical: ", __VA_ARGS__)
