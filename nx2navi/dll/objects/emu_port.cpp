// SPDX-License-Identifier: GPL-3.0-or-later
// The emulator's side of the hook's object files, in the NX2navi port
// (emu_port.hpp).
#include "emu_port.hpp"

#include <chrono>

#include <string_view>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "coop_spawn.h"
#include "coop_svc.h"

namespace CoopNx2navi::Objects {
namespace {

std::atomic<GuestPort*> g_port{nullptr};
std::atomic<std::uint64_t> g_main{0};

std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}
std::function<void(const std::string&)>& LogSink() {
    static std::function<void(const std::string&)> sink;
    return sink;
}

// The callers by order, as coop_svc.cpp keeps them. The mutex is held across
// a frame's run; only AddCaller (rare) waits on it.
struct Callers {
    std::mutex mutex;
    std::vector<std::pair<int, CoopSvc::Caller>> list;
};
Callers& TheCallers() {
    static Callers callers;
    return callers;
}

}  // namespace

void SetPort(GuestPort* port) { g_port = port; }
GuestPort* Port() { return g_port.load(std::memory_order_acquire); }

void SetMain(std::uint64_t main_base) { g_main = main_base; }
std::uint64_t Main() { return g_main.load(std::memory_order_acquire); }

void SetLog(std::function<void(const std::string&)> sink) {
    std::lock_guard lock(LogMutex());
    LogSink() = std::move(sink);
}

void LogLine(const std::string& line) {
    std::lock_guard lock(LogMutex());
    // The hook's walk says where the actor list is on every first pass, which
    // in this port is some twenty times a second (3,306 lines in 150 s): said
    // when the list's address changes, and else at most once a minute.
    static constexpr std::string_view kListHead = "coop: objects: list head ";
    if (line.compare(0, kListHead.size(), kListHead) == 0) {
        static std::string last_head;
        static std::chrono::steady_clock::time_point last_time;
        const std::string head = line.substr(0, line.find(','));
        const auto now = std::chrono::steady_clock::now();
        if (head == last_head && now - last_time < std::chrono::seconds(60)) {
            return;
        }
        last_head = head;
        last_time = now;
    }
    if (LogSink()) {
        LogSink()(line);
    }
}

std::size_t RunCallers(CoopPuppet::Guest& guest, std::uint64_t now_ms, const MakeCall& make,
                       unsigned max_calls, const std::function<bool(int order)>& skip) {
    Callers& callers = TheCallers();
    std::lock_guard lock(callers.mutex);
    std::size_t made = 0;
    // Each caller is asked once (after_call false) when its turn comes and
    // gets the result of every call it made; the turn passes on when it has
    // no more. The cave's own loop does the same (coop_svc.cpp, Call).
    for (auto& [order, caller] : callers.list) {
        if (skip && skip(order)) {
            continue;
        }
        auto call = caller(guest, false, 0, now_ms);
        while (call) {
            if (made >= max_calls) {
                return made;
            }
            std::uint64_t result = 0;
            if (!make(*call, result)) {
                return made;
            }
            ++made;
            call = caller(guest, true, result, now_ms);
        }
    }
    return made;
}

std::size_t CallerCount() {
    Callers& callers = TheCallers();
    std::lock_guard lock(callers.mutex);
    return callers.list.size();
}

void ClearCallersForTest() {
    Callers& callers = TheCallers();
    std::lock_guard lock(callers.mutex);
    callers.list.clear();
}

}  // namespace CoopNx2navi::Objects

// coop_callers.h: the callers of the frame step (CoopSvc::AddCaller forwards
// here). The shared stepper (coop_callers.cpp) always asks the puppet director
// first; in this port the director runs in puppet.cpp's own hook, so the list
// is kept here and RunCallers above asks only what was added.
namespace CoopCallers {

void Add(int order, Caller caller) {
    auto& callers = CoopNx2navi::Objects::TheCallers();
    std::lock_guard lock(callers.mutex);
    const auto at = std::upper_bound(
        callers.list.begin(), callers.list.end(), order,
        [](int o, const std::pair<int, Caller>& e) { return o < e.first; });
    callers.list.emplace(at, order, std::move(caller));
}

}  // namespace CoopCallers

// coop_spawn.h: the spawn director makes the rolled enemies another game
// decided (coop_objects.cpp asks for it when sharing starts). In the emulator
// this lives in coop_spawn.cpp's hook part, which also runs the projectile
// sync; here only the director is asked, at the emulator's caller order 10.
namespace CoopSpawn {

void UseDirector() {
    static const bool added = [] {
        CoopSvc::AddCaller(10, [](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                  std::uint64_t now_ms) {
            return TheDirector().Step(guest, after_call, result, now_ms);
        });
        return true;
    }();
    (void)added;
}

}  // namespace CoopSpawn
