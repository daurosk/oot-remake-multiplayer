// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "third_party/plugin_api.h"

namespace CoopNx2navi::Probe {
namespace {

constexpr std::uint64_t kPlayAs = 0x2022a0;     // (request): +0 actor, +8 name
constexpr std::uint64_t kActorName = 0x260;     // char* (coop/host addresses.hpp)
constexpr std::uint64_t kTextSize = 0x3c46000;  // main's code ends before this (the padding the emulator's cave used)
constexpr std::size_t kStackWords = 0x180;      // 3 KiB of stack above the call
constexpr std::size_t kChain = 14;

const Nx2PluginApi* g_api = nullptr;
std::uint64_t g_main = 0;
void (*g_log)(const char*) = nullptr;
bool g_tried = false;
std::vector<int> g_handles;
std::string g_names = "Enemy_";
std::mutex g_mutex;

struct Seen {
    std::uint64_t count = 0;
};
// "actor 'animation': lr chain" -> how often
std::map<std::string, Seen> g_seen;
struct Counted {
    std::uint64_t address = 0;
    std::atomic<std::uint64_t> calls{0};
    std::map<std::uint64_t, std::uint64_t> callers;  // lr -> count, under g_mutex
};
std::array<Counted, 16> g_counted;
std::size_t g_counted_n = 0;
std::uint64_t g_dumped_ms = 0;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void Log(const char* format, ...) {
    if (g_log == nullptr) {
        return;
    }
    char line[1600];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_log(line);
}

std::string Environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

std::string Text(std::uint64_t address, std::size_t most = 63) {
    char buffer[64] = {};
    if (address < 0x10000 || g_api->read(address, buffer, std::min(most, sizeof(buffer) - 1)) != 1) {
        // The text may end before a page that is not there: byte by byte then.
        std::string out;
        char c = 0;
        while (out.size() < most && address >= 0x10000 && g_api->read(address + out.size(), &c, 1) == 1 &&
               c != 0) {
            out.push_back(c);
        }
        return out;
    }
    return std::string(buffer, strnlen(buffer, sizeof(buffer)));
}

// Whether `address` is a place in main's code right after a call instruction.
bool ReturnAddress(std::uint64_t address) {
    if (address < g_main + 4 || address >= g_main + kTextSize || (address & 3) != 0) {
        return false;
    }
    std::uint32_t op = 0;
    if (g_api->read(address - 4, &op, 4) != 1) {
        return false;
    }
    return (op & 0xfc000000u) == 0x94000000u ||   // bl
           (op & 0xfefff000u) == 0xd63f0000u;     // blr, blraa, blrab, ...
}

// The return addresses found on the stack above a call, nearest first. Not a
// true backtrace (the hook does not get the frame pointer): a word that only
// looks like one, left by an older call, can be among them.
std::vector<std::uint64_t> Chain(const Nx2HookCall* call) {
    std::vector<std::uint64_t> out;
    out.push_back(call->lr - g_main);
    std::array<std::uint64_t, kStackWords> words{};
    if (g_api->read(call->sp, words.data(), sizeof(words)) != 1) {
        return out;
    }
    for (const std::uint64_t word : words) {
        if (out.size() >= kChain) {
            break;
        }
        if (ReturnAddress(word) && word - g_main != out.back()) {
            out.push_back(word - g_main);
        }
    }
    return out;
}

void OnPlayAs(Nx2HookCall* call, void*) {
    if (call->lr < g_main || call->lr >= g_main + kTextSize) {
        return;  // not the game's own call (ours, through the interface)
    }
    std::uint64_t actor = 0;
    std::uint64_t name = 0;
    if (g_api->read(call->x[0], &actor, 8) != 1 || g_api->read(call->x[0] + 8, &name, 8) != 1) {
        return;
    }
    std::uint64_t actor_name = 0;
    g_api->read(actor + kActorName, &actor_name, 8);
    const std::string who = Text(actor_name);
    if (g_names != "*" && who.rfind(g_names, 0) != 0) {
        return;
    }
    std::string key = who + " '" + Text(name) + "':";
    for (const std::uint64_t at : Chain(call)) {
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), " %llx", static_cast<unsigned long long>(at));
        key += buffer;
    }
    std::lock_guard lock(g_mutex);
    if (g_seen.size() < 4000 || g_seen.count(key) != 0) {
        ++g_seen[key].count;
    }
}

void OnCounted(Nx2HookCall* call, void* user) {
    auto* c = static_cast<Counted*>(user);
    c->calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    if (c->callers.size() < 64 || c->callers.count(call->lr - g_main) != 0) {
        ++c->callers[call->lr - g_main];
    }
}

void Dump() {
    std::lock_guard lock(g_mutex);
    std::vector<std::pair<std::uint64_t, std::string>> rows;
    for (const auto& [key, seen] : g_seen) {
        rows.emplace_back(seen.count, key);
    }
    std::sort(rows.rbegin(), rows.rend());
    Log("probe: %u kinds of AS play calls so far", static_cast<unsigned>(rows.size()));
    for (std::size_t i = 0; i < rows.size() && i < 60; ++i) {
        Log("probe: x%llu %s", static_cast<unsigned long long>(rows[i].first), rows[i].second.c_str());
    }
    for (std::size_t i = 0; i < g_counted_n; ++i) {
        std::string from;
        for (const auto& [lr, count] : g_counted[i].callers) {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), " %llx x%llu", static_cast<unsigned long long>(lr),
                          static_cast<unsigned long long>(count));
            from += buffer;
        }
        Log("probe: main+%#llx called %llu times, from%s",
            static_cast<unsigned long long>(g_counted[i].address),
            static_cast<unsigned long long>(g_counted[i].calls.load()), from.c_str());
    }
}

}  // namespace

void Frame(const Nx2PluginApi* api, std::uint64_t main_base, void (*log)(const char* line)) {
    if (!g_tried) {
        g_tried = true;
        const std::string what = Environment("OOT_COOP_PROBE");
        const std::string count = Environment("OOT_COOP_PROBE_COUNT");
        if ((what.empty() && count.empty()) || api == nullptr || main_base == 0 ||
            !NX2_HAS(api, Nx2PluginApi, hook_function) || api->hook_function == nullptr) {
            return;
        }
        g_api = api;
        g_main = main_base;
        g_log = log;
        if (const std::string names = Environment("OOT_COOP_PROBE_NAMES"); !names.empty()) {
            g_names = names;
        }
        if (what == "as") {
            g_handles.push_back(api->hook_function(main_base + kPlayAs, OnPlayAs, nullptr));
            Log("probe: the callers of the AS play main+%#llx for actors named %s* (hook %d)",
                static_cast<unsigned long long>(kPlayAs), g_names.c_str(), g_handles.back());
        }
        std::size_t at = 0;
        while (at < count.size() && g_counted_n < g_counted.size()) {
            const std::uint64_t address = std::strtoull(count.c_str() + at, nullptr, 16);
            const auto comma = count.find(',', at);
            at = comma == std::string::npos ? count.size() : comma + 1;
            if (address == 0) {
                continue;
            }
            Counted& c = g_counted[g_counted_n++];
            c.address = address;
            g_handles.push_back(api->hook_function(main_base + address, OnCounted, &c));
            Log("probe: counting the calls of main+%#llx (hook %d)", static_cast<unsigned long long>(address),
                g_handles.back());
        }
        g_dumped_ms = NowMs();
    }
    if (g_api != nullptr && NowMs() - g_dumped_ms >= 10000) {
        g_dumped_ms = NowMs();
        Dump();
    }
}

void Shutdown() {
    if (g_api == nullptr) {
        return;
    }
    for (const int handle : g_handles) {
        if (handle > 0) {
            g_api->remove(handle);
        }
    }
    g_handles.clear();
}

}  // namespace CoopNx2navi::Probe
