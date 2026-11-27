// Row 299: the module's side of the PauseMgr probe (coop_pause_world.h). Test only: the play-test bridge's `state`
// asks StateJson, the test step `menu hold release N` asks TestRelease; nothing else calls in.
#include "coop_pause_world.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hos_coop_api.h"

namespace CoopPauseWorld {
namespace {

const HosCoopApi* g_api = nullptr;
std::atomic<bool> g_listed{false};
std::atomic<std::int64_t> g_release{-1};   // the hold index a test asked to release, -1 none

bool Read(std::uint64_t a, void* out, std::size_t n) {
    return g_api != nullptr && g_api->read != nullptr && g_api->read(a, out, n) != 0;
}

void Log(const std::string& line) {
    if (g_api != nullptr && g_api->log != nullptr) g_api->log(line.c_str());
}

std::uint64_t Base() {
    return g_api != nullptr ? g_api->main_base : 0;
}

bool CanCall() {
    return g_api != nullptr && g_api->call != nullptr && g_api->cas32 != nullptr &&
           g_api->size >= offsetof(HosCoopApi, call) + sizeof(g_api->call);
}

// SeqOneShotReleasePause's own steps (main+0xdad2c0), for hold `index`: its count down by one unless 0 (cas), then
// the manager's sub-object at +0x48, vtable slot 0, called with (sub, key, 0).
void Release(void* ctx, std::uint32_t index) {
    const Snapshot s = Take(Read, Base());
    if (!s.ok || index >= s.holds) {
        Log("coop: pause world: test release " + std::to_string(index) + ": no such hold (" +
            (s.ok ? std::to_string(s.holds) + " holds" : s.why) + ")");
        return;
    }
    std::uint64_t array = 0, vtable = 0, fn = 0;
    std::uint32_t key = 0, count = 0;
    Read(s.manager + kHolds, &array, sizeof array);
    const std::uint64_t at = array + kHoldSize * index;
    Read(at + kHoldKey, &key, sizeof key);
    Read(at + kHoldCount, &count, sizeof count);
    if ((count & 0x7fffffffu) != 0) {
        std::uint32_t seen = 0;
        g_api->cas32(at + kHoldCount, count, count - 1, &seen);
        if (seen != count) {
            Log("coop: pause world: test release " + std::to_string(index) + ": count moved meanwhile, not released");
            return;
        }
    }
    const std::uint64_t sub = s.manager + kNotify;
    if (!Read(sub, &vtable, sizeof vtable) || !Read(vtable, &fn, sizeof fn) || fn == 0) {
        Log("coop: pause world: test release " + std::to_string(index) + ": no notify function");
        return;
    }
    HosCoopCall k{};
    k.function = fn;
    k.x[0] = sub, k.x[1] = key, k.x[2] = 0;
    std::uint64_t r = 0;
    const bool called = g_api->call(ctx, &k, &r) != 0;
    std::uint32_t after = 0;
    Read(at + kHoldCount, &after, sizeof after);
    char b[160];
    std::snprintf(b, sizeof b, "coop: pause world: test release %u key 0x%x: count %u -> %u, notify 0x%llx %s", index,
                  key, count, after, static_cast<unsigned long long>(fn), called ? "called" : "NOT called");
    Log(b);
}

}  // namespace

void Init(const void* api) {
    g_api = static_cast<const HosCoopApi*>(api);
}

std::string StateJson() {
    if (g_api == nullptr) return "null";
    if (!g_listed.exchange(true)) {
        for (const std::string& line : AllHoldLines(Read, Base())) Log(line);
    }
    return Json(Take(Read, Base()));
}

bool TestRelease(const std::string& index) {
    char* end = nullptr;
    const long v = std::strtol(index.c_str(), &end, 10);
    if (index.empty() || end == nullptr || *end != '\0' || v < 0 || v >= static_cast<long>(kMostHolds)) return false;
    g_release.store(v);
    Log("coop: pause world: test release " + index + ": waits for the next menu screen update");
    return true;
}

void OnScreen(void* ctx) noexcept {
    if (g_release.load(std::memory_order_relaxed) < 0) return;
    const std::int64_t index = g_release.exchange(-1);
    if (index < 0 || !CanCall()) return;
    Release(ctx, static_cast<std::uint32_t>(index));
}

}  // namespace CoopPauseWorld
