// SPDX-License-Identifier: GPL-3.0-or-later
// A temp folder of its own for each mod test (ctest -j runs the test cases as processes at the same time).
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define COOP_TEST_PID _getpid()
#else
#include <unistd.h>
#define COOP_TEST_PID getpid()
#endif

inline std::filesystem::path ModTestDir(const std::string& name) {
    static std::atomic<int> next{0};
    return std::filesystem::temp_directory_path() /
           (name + "-" + std::to_string(COOP_TEST_PID) + "-" + std::to_string(next++));
}
