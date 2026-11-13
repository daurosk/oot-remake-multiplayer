// SPDX-License-Identifier: GPL-3.0-or-later
// The AI hold on the plugin interface's function hooks (hold.hpp).
#include "hold.hpp"

#include <cstring>
#include <mutex>

#include "plugin_api_v2.h"

namespace CoopNx2navi::Objects::Hold {
namespace {

std::atomic<const Nx2PluginApi*> g_api{nullptr};
std::atomic<std::uint64_t> g_main{0};

std::mutex g_mutex;  // the hook list below
int g_loop_handle = 0;
struct AnswerHook {
    std::uint64_t function = 0;
    int handle = 0;  // 0: refused
};
std::array<AnswerHook, 8> g_answers{};
std::size_t g_answer_count = 0;
std::atomic<bool> g_have_answer_hook{false};

std::atomic<std::uint64_t> g_loops{0}, g_answer_calls{0}, g_altered{0}, g_fallback{0};
std::atomic<unsigned> g_hooked{0}, g_refused{0};

// Hooks an answering function the first time it is seen.
void Want(const Nx2PluginApi* api, std::uint64_t function) {
    if (function == 0) {
        return;
    }
    std::lock_guard lock(g_mutex);
    for (std::size_t i = 0; i < g_answer_count; ++i) {
        if (g_answers[i].function == function) {
            return;
        }
    }
    if (g_answer_count >= g_answers.size()) {
        return;
    }
    AnswerHook& h = g_answers[g_answer_count++];
    h.function = function;
    h.handle = api->hook_function(function, OnAnswer, nullptr);
    if (h.handle > 0) {
        g_hooked.fetch_add(1);
        g_have_answer_hook = true;
    } else {
        h.handle = 0;
        g_refused.fetch_add(1);
    }
}

template <typename T>
bool Get(const Nx2PluginApi* api, std::uint64_t address, T& out) {
    return address >= 0x1000 && api->read(address, &out, sizeof(out)) == 1;
}

}  // namespace

void Table::Take(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 0x10) {
        return;
    }
    std::uint32_t count = 0, and_mask = 0, or_mask = 0;
    std::memcpy(&count, bytes.data(), 4);
    std::memcpy(&and_mask, bytes.data() + 4, 4);
    std::memcpy(&or_mask, bytes.data() + 8, 4);
    if (count > CoopHold::kMaxHeld) {
        count = static_cast<std::uint32_t>(CoopHold::kMaxHeld);
    }
    const std::size_t fit = (bytes.size() - 0x10) / 8;
    if (count > fit) {
        count = static_cast<std::uint32_t>(fit);
    }
    count_.store(0, std::memory_order_release);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t c = 0;
        std::memcpy(&c, bytes.data() + 0x10 + 8 * i, 8);
        held_[i].store(c, std::memory_order_relaxed);
    }
    and_.store(and_mask, std::memory_order_relaxed);
    or_.store(or_mask, std::memory_order_relaxed);
    count_.store(count, std::memory_order_release);
}

bool Table::Alter(std::uint64_t component, std::uint32_t& bits) const {
    if (component == 0) {
        return false;
    }
    const std::size_t n = count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (held_[i].load(std::memory_order_relaxed) == component) {
            bits = (bits & and_.load(std::memory_order_relaxed)) | or_.load(std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

Table& TheTable() {
    static Table table;
    return table;
}

bool TakeTable(std::uint64_t main_base, std::uint64_t address, const void* in, std::size_t size) {
    if (main_base == 0) {
        return false;
    }
    const std::uint64_t begin = main_base + CoopHold::kTableOffset;
    if (address < begin || address >= begin + CoopHold::kTableSize) {
        return false;
    }
    if (address == begin) {
        TheTable().Take({static_cast<const std::uint8_t*>(in), size});
    }
    return true;
}

void OnLoop(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr) {
        return;
    }
    g_loops.fetch_add(1, std::memory_order_relaxed);
    // x1 points at the manager (main+0x369b30: mov x19, x1; ldr x27, [x19]).
    std::uint64_t manager = 0, answerer = 0, vtable = 0;
    if (Get(api, call->x[1], manager) && Get(api, manager + kManagerAnswerer, answerer) &&
        Get(api, answerer, vtable)) {
        for (const std::uint64_t slot : kAnswerSlots) {
            std::uint64_t function = 0;
            if (Get(api, vtable + slot, function)) {
                Want(api, function);
            }
        }
    }
    if (g_have_answer_hook.load(std::memory_order_acquire)) {
        return;  // the loop runs; the answer hooks do the work
    }
    // Fallback: no answering function could be hooked. Let the loop run, then
    // put the held components' bits 0 to 3 as the hold wants them.
    const Table& table = TheTable();
    const std::size_t n = table.Count();
    if (n == 0) {
        return;
    }
    api->call_original(call);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t component = table.At(i);
        std::uint16_t flags = 0;
        if (!Get(api, component + kBsaFlags, flags)) {
            continue;
        }
        std::uint32_t bits = flags & 0xfu;
        table.Alter(component, bits);
        const std::uint16_t now = static_cast<std::uint16_t>((flags & ~0xfu) | (bits & 0xfu));
        if (now != flags && api->write(component + kBsaFlags, &now, sizeof(now)) == 1) {
            g_fallback.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OnAnswer(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    const std::uint64_t main = g_main.load(std::memory_order_acquire);
    if (api == nullptr || main == 0 ||
        (call->lr != main + kAnswerReturn && call->lr != main + kInlineReturn)) {
        return;  // another caller of the same function: left alone
    }
    g_answer_calls.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t probe = 0;
    if (!TheTable().Alter(call->x[1], probe)) {
        return;
    }
    api->call_original(call);
    auto bits = static_cast<std::uint32_t>(call->x[0]);
    TheTable().Alter(call->x[1], bits);
    call->x[0] = (call->x[0] & ~std::uint64_t{0xffffffffu}) | bits;
    g_altered.fetch_add(1, std::memory_order_relaxed);
}

bool Install(const Nx2PluginApi* api, std::uint64_t main_base) {
    if (api == nullptr || main_base == 0 || !NX2_HAS(api, Nx2PluginApi, hook_function) ||
        !NX2_HAS(api, Nx2PluginApi, call_original)) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (g_loop_handle > 0) {
        return true;
    }
    g_api = api;
    g_main = main_base;
    g_loop_handle = api->hook_function(main_base + kLoop, OnLoop, nullptr);
    if (g_loop_handle <= 0) {
        g_loop_handle = 0;
        return false;
    }
    return true;
}

void Remove() {
    std::lock_guard lock(g_mutex);
    const Nx2PluginApi* api = g_api.load();
    if (api != nullptr) {
        if (g_loop_handle > 0) {
            api->remove(g_loop_handle);
        }
        for (std::size_t i = 0; i < g_answer_count; ++i) {
            if (g_answers[i].handle > 0) {
                api->remove(g_answers[i].handle);
            }
        }
    }
    g_loop_handle = 0;
    g_answers = {};
    g_answer_count = 0;
    g_have_answer_hook = false;
    g_hooked = 0;
    g_refused = 0;
    TheTable().Take(std::array<std::uint8_t, 0x10>{});
}

Counts Take() {
    Counts c;
    c.loops = g_loops.exchange(0);
    c.answers = g_answer_calls.exchange(0);
    c.altered = g_altered.exchange(0);
    c.fallback = g_fallback.exchange(0);
    c.answer_hooks = g_hooked.load();
    c.refused = g_refused.load();
    return c;
}

}  // namespace CoopNx2navi::Objects::Hold
