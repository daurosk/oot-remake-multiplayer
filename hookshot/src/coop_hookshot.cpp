// SPDX-License-Identifier: GPL-3.0-or-later
//
// Co-op inside Hookshot (docs/hookshot.md): hookshot_coop.dll, loaded by our
// patch's host/coop_bridge.c. It runs lichii/hook's co-op unchanged: the
// emulator's API slice the hook uses is answered by hookshot/compat from the
// bridge's table (CoopRuntime below). What differs from the emulator is only
// how the hook is driven:
// - hos_coop_frame at the every-frame site runs CoopHook::OnFrame, then the
//   callers' game calls (CoopCallers::RunFrame) directly, where the emulator's
//   cave asks them one at a time through `svc #0xf4` (coop_svc.cpp);
// - the AI hold reads the same table the emulator's hold cave reads
//   (coop_hold.h), at the same place in main;
// - settings: coop.ini beside hookshot.exe, then the LICHII_COOP_* variables,
//   as in the emulator (the kit's launcher sets them from coop-options.ini).
#include "coop_hookshot.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_callers.h"
#include "coop_game_build.h"
#include "coop_hold.h"
#include "coop_hook.h"
#include "coop_puppet.h"
#include "coop_runtime.h"
#include "coop_settings.h"
#include "coop_svc.h"
#include "coop_testinput.h"
#include "core/core.h"
#include "hos_coop_api.h"

namespace {

const HosCoopApi* g_api = nullptr;
std::string g_build;
bool g_on = false;
bool g_hold = false;
Core::System g_system;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

bool EnvOn(const char* name) {
    const auto v = Env(name);
    return !v.empty() && v != "0";
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// As CoopSvc::PatchMain's, when the emulator puts its cave in.
void ConfigurePuppets() {
    CoopPuppet::PlannerSettings settings;
    settings.debug = EnvOn("LICHII_COOP_TEST_PUPPET");
    settings.debug_anim = EnvOn("LICHII_COOP_DEBUG_ANIM");
    if (const auto actor = Env("LICHII_COOP_PUPPET_ACTOR"); !actor.empty()) {
        settings.actor = actor;
    }
    LOG_INFO(Core, "coop: puppets on, actor {}{}", settings.actor.empty() ? "by age" : settings.actor,
             settings.debug ? ", test puppet beside you" : "");
    CoopPuppet::Configure(std::move(settings), EnvOn("LICHII_COOP_DEBUG_PUPPET"),
                          [](const std::string& line) { LOG_INFO(Core, "{}", line); });
}

class BridgeGuest final : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return CoopRuntime::Write(address, in, size);
    }
};

// The game calls the hook's callers ask for this frame, made here.
void RunCalls(void* ctx) {
    BridgeGuest guest;
    CoopCallers::RunFrame(guest, SteadyMs(), CoopSvc::kCallsPerFrame, [ctx](const CoopPuppet::Call& c) {
        HosCoopCall k{};
        k.function = c.function;
        std::memcpy(k.x, c.x.data(), sizeof k.x);
        std::memcpy(k.stack, c.stack.data(), sizeof k.stack);
        k.s0 = c.s0;
        std::uint64_t x0 = 0;
        if (!g_api->call(ctx, &k, &x0)) {
            LOG_WARNING(Core, "coop: no game function at {:#x}", c.function);
            return std::optional<std::uint64_t>();
        }
        return std::optional<std::uint64_t>(x0);
    });
}

}  // namespace

namespace CoopRuntime {
bool Running() {
    return g_api != nullptr;
}
std::uint64_t MainBase() {
    return g_api != nullptr ? g_api->main_base : 0;
}
const std::string& MainBuildHex() {
    return g_build;
}
std::filesystem::path GameDir() {
    if (g_api == nullptr || g_api->game_dir == nullptr) {
        return std::filesystem::path(".");
    }
    // UTF-8 from the bridge: a folder name outside the ANSI code page stays whole.
    const std::string dir = g_api->game_dir;
    return std::filesystem::path(std::u8string(dir.begin(), dir.end()));
}
bool Valid(std::uint64_t address, std::size_t size) {
    // The bridge checks a whole range before it copies: one byte at each end
    // says whether the range is mapped (Hookshot's guest memory has no holes
    // inside a mapped range the hook reads).
    std::uint8_t b = 0;
    return g_api != nullptr && size != 0 && address + size > address && g_api->read(address, &b, 1) != 0 &&
           g_api->read(address + size - 1, &b, 1) != 0;
}
bool Read(std::uint64_t address, void* out, std::size_t size) {
    return g_api != nullptr && g_api->read(address, out, size) != 0;
}
bool Write(std::uint64_t address, const void* in, std::size_t size) {
    return g_api != nullptr && g_api->write(address, in, size) != 0;
}
bool Cas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t& seen) {
    return g_api != nullptr && g_api->cas32(address, expected, desired, &seen) != 0;
}
void Log(const std::string& line) {
    if (g_api != nullptr && g_api->log != nullptr) {
        g_api->log(line.c_str());
    }
}
}  // namespace CoopRuntime

// The emulator-only parts of the hook, as Hookshot has them.
namespace CoopSvc {
std::string Refusal() {
    return {};  // nothing to refuse: Hookshot applies no exefs patches (its host/mods.c)
}
}  // namespace CoopSvc
namespace CoopTestInput {
void OnFrame(Core::System&) {}  // tests drive Hookshot through its control bridge (docs/hookshot.md)
}  // namespace CoopTestInput

namespace CoopHookshot {

bool Start(const HosCoopApi* api) {
    if (api == nullptr || api->version != HOS_COOP_API_VERSION || api->size < sizeof(HosCoopApi)) {
        return false;
    }
    g_api = api;
    g_build = api->main_build_id != nullptr ? api->main_build_id : "";
    if (!BuildSupported(g_build)) {
        LOG_WARNING(Core, "coop: {}", CoopGameBuild::LogRefusal(g_build));
        g_api = nullptr;
        return false;
    }
    LOG_INFO(Core, "coop: Hookshot module, game build {}, main at {:#x}", g_build.substr(0, 16), api->main_base);
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;
    if (!CoopHook::LoadSettings(ini, errors).enabled() || !errors.empty()) {
        CoopHook::Start();  // logs why co-op is off
        g_api = nullptr;
        return false;
    }
    // The same switches that make the emulator put its cave in.
    if (EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET")) {
        ConfigurePuppets();
    }
    g_hold = EnvOn("LICHII_COOP_OBJECTS_AI_HOLD");
    CoopHook::Start();
    g_on = true;
    return true;
}

void Frame(void* ctx, int site) {
    if (!g_on || site != HOS_COOP_SITE_FRAME) {
        return;
    }
    CoopHook::OnFrame(g_system);
    RunCalls(ctx);
}

// coop_hold.s: a held component's calc bits become (bits & and) | or.
std::uint32_t Hold(std::uint64_t component, std::uint32_t bits) {
    if (!g_hold || component == 0) {
        return bits;
    }
    const std::uint64_t table = CoopRuntime::MainBase() + CoopHold::kTableOffset;
    std::uint32_t head[3] = {};
    if (!CoopRuntime::Read(table, head, sizeof head) || head[0] == 0) {
        return bits;
    }
    const std::uint32_t count =
        head[0] < CoopHold::kMaxHeld ? head[0] : static_cast<std::uint32_t>(CoopHold::kMaxHeld);
    std::uint64_t held[CoopHold::kMaxHeld] = {};
    if (!CoopRuntime::Read(table + 0x10, held, 8 * count)) {
        return bits;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (held[i] == component) {
            return (bits & head[1]) | head[2];
        }
    }
    return bits;
}

void Stop() {
    if (g_on) {
        g_on = false;
        CoopHook::Shutdown();
    }
}

}  // namespace CoopHookshot

extern "C" {
COOP_HOOKSHOT_EXPORT int hos_coop_start(const HosCoopApi* api) {
    return CoopHookshot::Start(api) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT void hos_coop_frame(void* ctx, int site) {
    CoopHookshot::Frame(ctx, site);
}
COOP_HOOKSHOT_EXPORT uint32_t hos_coop_hold(uint64_t component, uint32_t bits) {
    return CoopHookshot::Hold(component, bits);
}
COOP_HOOKSHOT_EXPORT void hos_coop_stop(void) {
    CoopHookshot::Stop();
}
}
