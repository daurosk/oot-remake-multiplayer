// SPDX-License-Identifier: GPL-3.0-or-later
// The guarded boundary between the game (the emulator's threads, Hookshot's C exports) and co-op's C++: nothing co-op
// throws takes the game down (save-crash-0118). A thrown std::bad_alloc at the PC's commit limit (two lane pairs, 4
// games of ~24-28 GB each, runs 081813 + 082906) ended the game from whichever co-op allocation failed first; now the
// call ends, its fallback is returned, and the site logs once (then each 1024th throw, with the count). Pure: the log
// is the caller's (no LOG_* here), so the tests run without the emulator.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace CoopBoundary {

// The text a C export wrote into buf (Hookshot's patches: snprintf, always ended), read without trusting that: up to its
// first NUL inside cap; none there = nothing (empty), never a read past the buffer.
inline std::string_view EndedText(const char* buf, std::size_t cap) noexcept {
    if (buf == nullptr) return {};
    for (std::size_t i = 0; i < cap; ++i) {
        if (buf[i] == '\0') return std::string_view(buf, i);
    }
    return {};
}

// One guarded call site (a static per site: its name and its throws so far).
struct Site {
    const char* name;
    std::atomic<std::uint64_t> thrown{0};
    explicit constexpr Site(const char* n) : name(n) {}
};

// Whether a site's throw number n (1 = the first) is logged: the first, then each 1024th.
constexpr bool ShouldLog(std::uint64_t n) {
    return n == 1 || n % 1024 == 0;
}

// What a throw was, for the log line ("std::bad_alloc: bad allocation", "unknown").
inline const char* What(const std::exception_ptr& e) noexcept {
    try {
        if (e) std::rethrow_exception(e);
    } catch (const std::bad_alloc&) {
        return "out of memory (std::bad_alloc: the PC's commit limit?)";
    } catch (const std::exception& x) {
        return x.what();
    } catch (...) {
    }
    return "an unknown exception";
}

// Runs f(); a throw counts at site and, when ShouldLog, calls log(site.name, what, count) (itself guarded: a log that
// throws too is dropped). Returns f's value or fallback. Never throws.
template <class R, class F, class Log>
R Call(Site& site, R fallback, F&& f, Log&& log) noexcept {
    std::exception_ptr e;
    try {
        return std::forward<F>(f)();
    } catch (...) {
        e = std::current_exception();
    }
    const std::uint64_t n = site.thrown.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ShouldLog(n)) {
        try {
            log(site.name, What(e), n);
        } catch (...) {
        }
    }
    return fallback;
}

// The same for a call without a value.
template <class F, class Log>
void Run(Site& site, F&& f, Log&& log) noexcept {
    Call(
        site, 0,
        [&] {
            std::forward<F>(f)();
            return 0;
        },
        std::forward<Log>(log));
}

}  // namespace CoopBoundary
