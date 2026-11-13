// SPDX-License-Identifier: GPL-3.0-or-later
// Shared enemies and boss rooms through NX2navi's plugin interface (objects.hpp).
#include "objects.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "coop_boss.h"
#include "coop_drops.h"
#include "coop_time.h"
#include "coop_objects.h"
#include "coop_objects_sync.h"
#include "core/core.h"
#include "emu_port.hpp"
#include "anim_probe.hpp"
#include "brain.hpp"
#include "hold.hpp"
#include "plugin_api_v2.h"

namespace CoopNx2navi::Objects {
namespace {

// The frame step the emulator's cave stands in front of (coop_svc.h kHooks:
// the bl 0x3b4e300 at main+0x6e70f8 and +0x6e6dd8 in moduleCalc_).
constexpr std::uint64_t kFrameStep = 0x3b4e300;
constexpr std::uint64_t kFrameStepReturn = 0x6e70fc;      // the every-frame call (lr)
constexpr std::uint64_t kFrameStepReturnRare = 0x6e6ddc;  // the rare one
constexpr int kGrassOrder = 40;  // coop_objects.cpp kGrassCallerOrder

void (*g_log)(const char*) = nullptr;
Options g_options;
std::atomic<bool> g_configured{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_broken{false};  // a game call was refused: no more calls
std::atomic<const Nx2PluginApi*> g_api{nullptr};
std::mutex g_install;               // the two installs below
int g_step_handle = 0;
bool g_hold_installed = false;
bool g_hold_tried = false;
bool g_brain_installed = false;

std::atomic<std::uint64_t> g_steps{0}, g_calls{0}, g_step_us{0}, g_worst_us{0};

void Log(const std::string& line) {
    if (g_log != nullptr) {
        g_log(line.c_str());
    }
}

Core::System& System() {
    static Core::System system;
    return system;
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Guest memory through the interface: read and write are fast for the
// game's heap and modules in version 2 (NX2navi docs/plugins.md, "Memory")
// and may be called from any thread.
class ApiPort final : public GuestPort {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
        return size == 0 || (api != nullptr && address >= 0x1000 && api->read(address, out, size) == 1);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        if (Hold::TakeTable(Main(), address, in, size)) {
            return true;  // the Mover's hold table: kept here, not in main's code
        }
        Brain::NoteWrite(address, size);  // a hit record: that copy's update runs for it
        const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
        return size == 0 || (api != nullptr && address >= 0x1000 && api->write(address, in, size) == 1);
    }
    std::optional<std::uint32_t> CompareExchange32(std::uint64_t address, std::uint32_t expected,
                                                   std::uint32_t desired) override {
        const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
        if (api == nullptr || (address & 3) != 0) {
            return std::nullopt;
        }
        // The game's own CAS is a host atomic on the same word (README, "How
        // the game's memory is reached"), so a host CAS on the mapped word is
        // atomic against it.
        void* at = api->map_range(address, 4, 1);
        if (at == nullptr) {
            return std::nullopt;
        }
        std::uint32_t was = expected;
        __atomic_compare_exchange_n(static_cast<std::uint32_t*>(at), &was, desired, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return was;
    }
};

ApiPort& ThePort() {
    static ApiPort port;
    return port;
}

// What the callers read and write on the calc thread: the same port.
class StepGuest final : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        return ThePort().Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return ThePort().Write(address, in, size);
    }
};

// One call as the cave makes it: x0 to x7, s0, and the four stack words.
bool Make(const Nx2PluginApi* api, const CoopPuppet::Call& call, std::uint64_t& result) {
    Nx2GuestRegs regs{};
    regs.size = sizeof(regs);
    for (std::size_t i = 0; i < call.x.size(); ++i) {
        regs.x[i] = call.x[i];
    }
    std::uint32_t s0 = 0;
    std::memcpy(&s0, &call.s0, sizeof(s0));
    regs.v[0][0] = s0;
    regs.stack = call.stack.data();
    regs.nstack = call.stack.size();
    if (api->guest_call_regs(call.function, &regs) != 1) {
        return false;
    }
    result = regs.x[0];
    return true;
}

// The game's calc thread, before the frame step runs: the emulator's cave.
void OnStep(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    const std::uint64_t main = Main();
    if (api == nullptr || g_broken.load(std::memory_order_relaxed) || main == 0) {
        return;
    }
    // Only the two calls the emulator patched (coop_svc.h kHooks), as puppet.cpp.
    if (call->lr != main + kFrameStepReturn && call->lr != main + kFrameStepReturnRare) {
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    StepGuest guest;
    const bool world = g_options.world;
    const std::size_t made = RunCallers(
        guest, NowMs(),
        [api](const CoopPuppet::Call& call, std::uint64_t& result) {
            if (Make(api, call, result)) {
                return true;
            }
            g_broken = true;
            char line[200];
            std::snprintf(line, sizeof(line),
                          "objects: the game refused the call of main+%#llx; no more game calls "
                          "for shared enemies in this run",
                          static_cast<unsigned long long>(call.function - Main()));
            Log(line);
            return false;
        },
        g_options.calls_per_frame, [world](int order) { return !world && order == kGrassOrder; });
    if (AnimProbe::On()) {
        AnimProbe::Step(ThePort(), main, NowMs(), [](const std::string& line) { Log(line); });
    }
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin)
            .count());
    g_steps.fetch_add(1, std::memory_order_relaxed);
    g_calls.fetch_add(made, std::memory_order_relaxed);
    g_step_us.fetch_add(us, std::memory_order_relaxed);
    if (us > g_worst_us.load(std::memory_order_relaxed)) {
        g_worst_us.store(us, std::memory_order_relaxed);
    }
}

// Once main is known: the frame step hook, and the hold when asked for.
void Install(std::uint64_t main_base) {
    const Nx2PluginApi* api = g_api.load();
    std::lock_guard lock(g_install);
    if (g_step_handle == 0) {
        g_step_handle = api->hook_function(main_base + kFrameStep, OnStep, nullptr);
        if (g_step_handle <= 0) {
            g_step_handle = -1;  // not again
            Log("objects: the game refused a hook on its frame step main+0x3b4e300: copies are "
                "not moved and nothing is deleted or played");
        } else {
            Log("objects: frame step hooked (game calls run on the game's calc thread, as the "
                "emulator's cave)");
        }
    }
    AnimProbe::InstallHooks(api, main_base, [](const std::string& line) { Log(line); });
    if (g_options.ai_hold == 3 && !g_hold_tried) {
        // The copies' AI update is left out (brain.hpp); their brain skip bits
        // are not touched. The Mover still names the held copies each frame.
        g_hold_tried = true;
        g_brain_installed = Brain::Install(api, main_base);
        Log(g_brain_installed ? "objects: ai hold mode 3: the copies' AI update main+0xcb9c0 is hooked "
                                "and left out for held copies"
                              : "objects: ai hold NOT hooked: main+0xcb9c0 was refused");
    } else if (g_options.ai_hold != 0 && !g_hold_tried) {
        g_hold_tried = true;
        g_hold_installed = Hold::Install(api, main_base);
        Log(g_hold_installed ? "objects: ai hold hooked (main+0x369b00 and its calc-bits functions)"
                             : "objects: ai hold NOT hooked: main+0x369b00 was refused");
    }
}

}  // namespace

namespace {

std::atomic<bool> g_refused{false};  // Activate said no (version 1): not asked again

// The interface is here: version 2 or nothing.
bool Activate(const Nx2PluginApi* api) {
    if (api == nullptr || !NX2_HAS(api, Nx2PluginApi, hook_function) ||
        !NX2_HAS(api, Nx2PluginApi, map_range) || api->version < 2) {
        Log("objects: off: shared enemies need the plugin interface version 2 (function hooks "
            "and fast memory); this game has an older one");
        g_refused = true;
        return false;
    }
    g_api = api;
    // The hook's files read their switches from the environment on first use.
    SetEnv("LICHII_COOP_OBJECTS", "1");
    SetEnv("LICHII_COOP_BOSS", g_options.boss ? "1" : "0");
    // The shared code asks for the emulator's cave by these names; here the frame step hook is it.
    SetEnv("LICHII_COOP_SPAWN", "1");
    SetEnv("LICHII_COOP_TIME", g_options.time ? "1" : "0");
    SetEnv("LICHII_COOP_DROPS", g_options.drops ? "1" : "0");
    if (g_options.ai_hold != 0) {
        SetEnv("LICHII_COOP_OBJECTS_AI_HOLD", std::to_string(g_options.ai_hold));
    }
    if (g_options.debug) {
        SetEnv("LICHII_COOP_DEBUG_OBJECTS", "1");
    }
    CoopObjects::ShareFriendly(g_options.friendly);
    SetPort(&ThePort());
    SetLog([](const std::string& line) {
        // The hook's lines start with "coop: "; the DLL's log adds that itself.
        Log(line.rfind("coop: ", 0) == 0 ? line.substr(6) : line);
    });
    g_active = true;
    Log(std::string("objects: on (shared enemies") + (g_options.boss ? ", boss rooms" : "") +
        (g_options.friendly ? ", townspeople and animals too" : ", each game its own townspeople and animals") +
        (g_options.ai_hold != 0 ? ", ai hold mode " + std::to_string(g_options.ai_hold) : "") + ")");
    return true;
}

}  // namespace

bool Init(const Nx2PluginApi* api, const Options& options, void (*log)(const char* line)) {
    g_log = log;
    g_options = options;
    g_active = false;
    g_refused = false;
    g_configured = true;
    if (!options.on) {
        return false;
    }
    return api == nullptr || Activate(api);
}

bool Active() { return g_active; }

namespace {
void NearLog(const FrameInput& input);  // below
}

void Frame(const FrameInput& input) {
    if (!g_active && g_options.on && !g_refused && input.api != nullptr) {
        Activate(input.api);
    }
    if (!g_active) {
        return;
    }
    if (input.main_base != 0 && Main() != input.main_base) {
        SetMain(input.main_base);
        Install(input.main_base);
    }
    if (Main() == 0) {
        return;
    }
    Core::System& system = System();
    CoopObjects::SetLocalEvent(input.event_running);
    CoopObjects::OnFrame(system);  // the research walk (debug) only
    CoopObjects::Share(system, input.session, input.scene, input.server_now_us);
    if (g_options.near_log) {
        NearLog(input);
    }
    if (g_options.drops) {
        CoopDrops::Frame(system, input.session, input.scene, input.server_now_us);
    }
    if (g_options.time) {
        if (!input.scene) {
            CoopTime::Loading(input.server_now_us);  // a load: the first read after it is no jump
        }
        CoopTime::Frame(system, Main(), input.session, input.server_now_us, input.event_running);
    }
    if (g_options.boss) {
        std::optional<coop::protocol::Vec3> position;
        if (input.position) {
            position = coop::protocol::Vec3{(*input.position)[0], (*input.position)[1],
                                            (*input.position)[2]};
        }
        CoopBoss::Frame(system, Main(), input.scene, position, input.event_running, std::nullopt,
                        input.session,
                        input.server_now_us);
    }
}

namespace {

float Distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Options::near_log: the enemies this game owns, once a second.
void NearLog(const FrameInput& input) {
    static std::uint64_t next_ms = 0;
    const std::uint64_t now = NowMs();
    if (now < next_ms || !input.position) {
        return;
    }
    next_ms = now + 1000;
    for (const std::uint64_t actor : CoopObjects::OwnedActors()) {
        std::uint64_t name_at = 0;
        char name[48] = {};
        std::array<float, 3> at{};
        if (!ThePort().Read(actor + 0x260, &name_at, 8) || name_at < 0x1000 ||
            !ThePort().Read(name_at, name, sizeof(name) - 1) ||
            std::string_view(name).rfind("Enemy_", 0) != 0 ||
            !ThePort().Read(actor + 0x31c, at.data(), 12) || Distance(at, *input.position) > 150.0f) {
            continue;
        }
        Log(NearLine(name, actor, at, *input.position, input.peers));
    }
}

}  // namespace

std::string NearLine(std::string_view name, std::uint64_t actor, const std::array<float, 3>& at,
                     const std::array<float, 3>& self, const std::vector<FrameInput::Peer>& peers) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "objects: near: ours %.*s %#llx at %.1f %.1f %.1f: %.1f from us",
                  static_cast<int>(name.size()), name.data(), static_cast<unsigned long long>(actor),
                  static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]),
                  static_cast<double>(Distance(at, self)));
    std::string line = buffer;
    const float mine = Distance(at, self);
    bool nearest = true;
    for (const FrameInput::Peer& peer : peers) {
        const float theirs = Distance(at, peer.position);
        std::snprintf(buffer, sizeof(buffer), ", %.1f from P%u", static_cast<double>(theirs),
                      static_cast<unsigned>(peer.number));
        line += buffer;
        nearest = nearest && mine <= theirs;
    }
    if (!peers.empty()) {
        line += nearest ? " (we are the nearest)" : " (another player is nearer)";
    }
    return line;
}

void OnHostReset() {
    if (g_active) {
        CoopObjects::ForgetKeptBreaks();
    }
}

void OnEvent(const coop::net::Event& event) {
    if (!g_active) {
        return;
    }
    CoopObjects::OnEvent(event);
    if (g_options.drops) {
        CoopDrops::OnEvent(event);
    }
    if (g_options.time) {
        CoopTime::OnEvent(event);
    }
    if (g_options.boss) {
        CoopBoss::OnEvent(event);
    }
}

std::string StatusText() {
    if (!g_configured || !g_options.on) {
        return {};
    }
    if (!g_active) {
        return g_refused ? "; objects: off (no plugin interface version 2)"
                         : "; objects: waiting for the game's plugin interface";
    }
    const std::uint64_t steps = g_steps.load();
    char buffer[400];
    std::snprintf(buffer, sizeof(buffer),
                  "; objects: %llu frame steps (%.3f ms each, the longest %.3f ms), %llu game calls, "
                  "%zu callers%s%s",
                  static_cast<unsigned long long>(steps),
                  steps != 0 ? static_cast<double>(g_step_us.load()) / 1000.0 / static_cast<double>(steps)
                             : 0.0,
                  static_cast<double>(g_worst_us.load()) / 1000.0,
                  static_cast<unsigned long long>(g_calls.load()), CallerCount(),
                  g_step_handle > 0 ? "" : " (no frame step hook)",
                  g_broken ? " (off: a call was refused)" : "");
    std::string text = buffer;
    if (g_brain_installed) {
        const Brain::Counts c = Brain::Take();
        std::snprintf(buffer, sizeof(buffer),
                      ", hold: %zu held, %llu AI updates since the last status, %llu of them left out, "
                      "%llu run for a hit that came in",
                      Hold::TheTable().Count(), static_cast<unsigned long long>(c.calls),
                      static_cast<unsigned long long>(c.skipped),
                      static_cast<unsigned long long>(c.for_hits));
        text += buffer;
    }
    if (g_hold_installed) {
        const Hold::Counts c = Hold::Take();
        std::snprintf(buffer, sizeof(buffer),
                      ", hold: %zu held, %llu loops, %llu answers, %llu altered, %llu after the loop, "
                      "%u answer hooks (%u refused)",
                      Hold::TheTable().Count(), static_cast<unsigned long long>(c.loops),
                      static_cast<unsigned long long>(c.answers),
                      static_cast<unsigned long long>(c.altered),
                      static_cast<unsigned long long>(c.fallback), c.answer_hooks, c.refused);
        text += buffer;
    }
    return text;
}

void Shutdown() {
    const Nx2PluginApi* api = g_api.load();
    int step = 0;
    {
        std::lock_guard lock(g_install);
        step = g_step_handle;
        g_step_handle = 0;
    }
    // Outside the lock: a frame step callback that is running may be in
    // Frame(), which takes it to install.
    if (api != nullptr && step > 0) {
        api->remove(step);
    }
    {
        std::lock_guard lock(g_install);
        if (g_hold_installed) {
            Hold::Remove();
        }
        if (g_brain_installed) {
            Brain::Remove();
            Hold::TheTable().Take(std::array<std::uint8_t, 0x10>{});
        }
        g_brain_installed = false;
        g_hold_installed = false;
        g_hold_tried = false;
    }
    g_active = false;
    SetMain(0);  // a later Init and Frame install again
}

}  // namespace CoopNx2navi::Objects
