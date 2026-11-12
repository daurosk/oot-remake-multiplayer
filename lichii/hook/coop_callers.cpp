// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_callers.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace CoopCallers {
namespace {

struct Callers {
    std::mutex mutex;
    std::vector<std::pair<int, Caller>> list;
    Caller first;               // empty: the puppet director
    std::size_t current = 0;    // who made the last call (the calc thread only)
};

Callers& The() {
    static Callers callers;
    return callers;
}

} // namespace

void Add(int order, Caller caller) {
    Callers& c = The();
    std::lock_guard lock(c.mutex);
    const auto at = std::upper_bound(
        c.list.begin(), c.list.end(), order,
        [](int o, const std::pair<int, Caller>& e) { return o < e.first; });
    c.list.emplace(at, order, std::move(caller));
}

std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, bool after_call,
                                     std::uint64_t result, std::uint64_t now_ms) {
    Callers& c = The();
    std::lock_guard lock(c.mutex);
    // Caller 0 is the puppet director, then the others by order. Each is asked
    // once a frame (after_call false) when its turn comes and gets the result
    // of each call it made; the turn passes on when it has no more calls.
    const auto step = [&](std::size_t i, bool after, std::uint64_t r) {
        if (i == 0) {
            return c.first ? c.first(guest, after, r, now_ms)
                           : CoopPuppet::TheDirector().Step(guest, after, r, now_ms);
        }
        return c.list[i - 1].second(guest, after, r, now_ms);
    };
    const std::size_t count = c.list.size() + 1;
    std::optional<CoopPuppet::Call> call;
    std::size_t i = 0;
    if (after_call && c.current < count) {
        call = step(c.current, true, result);
        i = c.current + 1;
    }
    for (; !call && i < count; ++i) {
        call = step(i, false, 0);
        c.current = i;
    }
    return call;
}

std::size_t RunFrame(CoopPuppet::Guest& guest, std::uint64_t now_ms, std::size_t max_calls,
                     const std::function<std::optional<std::uint64_t>(const CoopPuppet::Call&)>& invoke) {
    std::size_t made = 0;
    auto call = Next(guest, false, 0, now_ms);
    while (call && made < max_calls) {
        const auto x0 = invoke(*call);
        if (!x0) {
            break;
        }
        ++made;
        call = Next(guest, true, *x0, now_ms);
    }
    return made;
}

void ResetForTest(Caller first) {
    Callers& c = The();
    std::lock_guard lock(c.mutex);
    c.list.clear();
    c.first = std::move(first);
    c.current = 0;
}

} // namespace CoopCallers
