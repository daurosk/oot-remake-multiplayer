// Row 299: the module's side of the PauseMgr (coop_pause_world.h). Phase 1: while the host's "World runs in menus" is
// in force (CoopPauseWorldMode::InForce: a session only), OnScreen lets the menu's own holds go. The probe's parts
// (the play-test bridge's `state` asks StateJson, the test step `menu hold release N` asks TestRelease) stay.
#include "coop_pause_world.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "coop_pause_world_mode.h"
#include "hos_coop_api.h"

namespace CoopPauseWorld {
namespace {

const HosCoopApi* g_api = nullptr;
std::atomic<bool> g_listed{false};
std::atomic<std::int64_t> g_release{-1};   // the hold index a test asked to release, -1 none
std::atomic<bool> g_world_runs{false};     // WorldRuns
std::atomic<bool> g_menu_up{false};        // OnScreen's menu_up last time (state `menu_up`, for the test)
bool g_let_go = false;                     // a menu hold let go since the menu last came up (the game's thread)
bool g_was_up = false;

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

// SeqOneShotReleasePause's own steps (main+0xdad2c0; the menu screen's own release main+0xafba70 is the same), for
// hold `index`: its count down by one unless 0 (cas), then the manager's sub-object at +0x48, vtable slot 0, called
// with (sub, key, 0). `who` names the caller in the log ("test release", "menu hold"). true: released.
bool Release(void* ctx, const Snapshot& s, std::uint32_t index, const char* who) {
    const std::string tag = std::string("coop: pause world: ") + who + " " + std::to_string(index);
    if (!s.ok || index >= s.holds) {
        Log(tag + ": no such hold (" + (s.ok ? std::to_string(s.holds) + " holds" : s.why) + ")");
        return false;
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
            Log(tag + ": count moved meanwhile, not released");
            return false;
        }
    }
    const std::uint64_t sub = s.manager + kNotify;
    if (!Read(sub, &vtable, sizeof vtable) || !Read(vtable, &fn, sizeof fn) || fn == 0) {
        Log(tag + ": no notify function");
        return false;
    }
    HosCoopCall k{};
    k.function = fn;
    k.x[0] = sub, k.x[1] = key, k.x[2] = 0;
    std::uint64_t r = 0;
    const bool called = g_api->call(ctx, &k, &r) != 0;
    std::uint32_t after = 0;
    Read(at + kHoldCount, &after, sizeof after);
    char b[200];
    std::snprintf(b, sizeof b, "%s key 0x%x: count %u -> %u, notify 0x%llx %s", tag.c_str(), key, count, after,
                  static_cast<unsigned long long>(fn), called ? "called" : "NOT called");
    Log(b);
    return called;
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
    // (phase 1: the host's setting in force here and WorldRuns, added to the snapshot's object)
    std::string json = Json(Take(Read, Base()));
    if (!json.empty() && json.back() == '}') json.pop_back();
    return json + ",\"in_force\":" + (CoopPauseWorldMode::InForce() ? "true" : "false") +
           ",\"world_runs\":" + (WorldRuns() ? "true" : "false") +
           ",\"menu_up\":" + (g_menu_up.load(std::memory_order_relaxed) ? "true" : "false") + "}";
}

bool TestRelease(const std::string& index) {
    char* end = nullptr;
    const long v = std::strtol(index.c_str(), &end, 10);
    if (index.empty() || end == nullptr || *end != '\0' || v < 0 || v >= static_cast<long>(kMostHolds)) return false;
    g_release.store(v);
    Log("coop: pause world: test release " + index + ": waits for the next menu screen update");
    return true;
}

void OnScreen(void* ctx, bool menu_up) noexcept {
    try {
        if (g_release.load(std::memory_order_relaxed) >= 0) {
            const std::int64_t index = g_release.exchange(-1);
            if (index >= 0 && CanCall()) {
                Release(ctx, Take(Read, Base()), static_cast<std::uint32_t>(index), "test release");
            }
        }
        g_menu_up.store(menu_up, std::memory_order_relaxed);
        if (menu_up != g_was_up) {
            g_was_up = menu_up;
            if (menu_up) g_let_go = false;   // (a release just before the menu read as up counts: it is this menu's)
        }
        if (!CoopPauseWorldMode::InForce() || !CanCall()) {
            g_let_go = false;
            g_world_runs.store(false);
            return;
        }
        // Only the menu's own keys (kMenuHolds), one release per screen update as the menu's own: an event's or Game
        // Over's hold is never touched, and the menu's release at its next state finds 0 and stays there.
        Snapshot s = Take(Read, Base());
        if (const Hold* h = MenuHeld(s)) {
            if (Release(ctx, s, h->index, "menu hold")) g_let_go = true;
            s = Take(Read, Base());
        }
        g_world_runs.store(menu_up && g_let_go && WorldFree(s));
    } catch (...) {
        g_world_runs.store(false);
    }
}

bool WorldRuns() noexcept {
    return g_world_runs.load(std::memory_order_relaxed);
}

}  // namespace CoopPauseWorld
