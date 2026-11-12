// The emulator side of puppets (coop_svc.h, docs/puppet.md).
#include "coop_svc.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#include "coop_hold.h"
#include "coop_sites.h"
#include "coop_puppet.h"

namespace CoopSvc {
namespace {

// Any mod folder's coop_puppet.json under load/<title id> with both green
// puppets (CoopPuppet::PackHasGreenPuppets), as coop_spawn's PackStandIns looks.
bool PackGreenPuppets() {
    std::error_code ec;
    const auto title = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400486024DEC000";
    for (const auto& mod : std::filesystem::directory_iterator(title, ec)) {
        std::ifstream in(mod.path() / "coop_puppet.json", std::ios::binary);
        if (!in) {
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (CoopPuppet::PackHasGreenPuppets(text)) {
            return true;
        }
    }
    return false;
}

constexpr std::uint64_t kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr std::string_view kMainBuild = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";

std::string Env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

bool EnvOn(const char* name) {
    const auto v = Env(name);
    return !v.empty() && v != "0";
}

bool Wanted() {
    return EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET") ||
           EnvOn("LICHII_COOP_SPAWN") || EnvOn("LICHII_COOP_LIVE_EVENTS");
}

// Settings for the puppets, once, when the patch goes in.
void ConfigurePuppets() {
    CoopPuppet::PlannerSettings settings;
    // The test puppet has its own switch: the logs alone must not put a
    // second Link in the game (Caleb's run of 2026-11-09 with the logs on).
    settings.debug = EnvOn("LICHII_COOP_TEST_PUPPET");
    settings.debug_anim = EnvOn("LICHII_COOP_DEBUG_ANIM");
    if (const auto actor = Env("LICHII_COOP_PUPPET_ACTOR"); !actor.empty()) {
        settings.actor = actor;
    }
    if (const auto tunics = Env("LICHII_COOP_TUNICS"); tunics == "0" || tunics == "off") {
        settings.tunics = false;
    }
    // The kit's tools/release/kit/scripts/kit-paths.ps1 Set-KitSelfTunic. Only
    // with a pack that has the green puppets (a hand-set switch and an older
    // pack must not leave green players without a puppet).
    if (const auto self = Env("LICHII_COOP_SELF_TUNIC"); !self.empty() && self != "green") {
        settings.green_puppet = PackGreenPuppets();
        LOG_INFO(Core, "coop: your Link wears {}; green players {}", self,
                 settings.green_puppet ? "get CoopPuppet_<Age>_Green"
                                       : "get the plain puppet (the pack has no green puppet; make it again)");
    }
    const bool verbose = EnvOn("LICHII_COOP_DEBUG_PUPPET");
    LOG_INFO(Core, "coop: puppets on, actor {}{}", settings.actor.empty() ? "by age" : settings.actor,
             settings.debug ? ", test puppet beside you" : "");
    CoopPuppet::Configure(std::move(settings), verbose,
                          [](const std::string& line) { LOG_INFO(Core, "{}", line); });
}

std::uint32_t Word(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t w = 0;
    std::memcpy(&w, bytes.data() + at, sizeof(w));
    return w;
}

class ProcessGuest : public CoopPuppet::Guest {
public:
    explicit ProcessGuest(Core::Memory::Memory& memory) : memory_(memory) {}
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        if (size == 0) {
            return true;
        }
        if (address + size < address || !memory_.IsValidVirtualAddressRange(address, size)) {
            return false;
        }
        return memory_.ReadBlockUnsafe(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        if (size == 0) {
            return true;
        }
        if (address + size < address || !memory_.IsValidVirtualAddressRange(address, size)) {
            return false;
        }
        return memory_.WriteBlockUnsafe(address, in, size);
    }

private:
    Core::Memory::Memory& memory_;
};


} // namespace

namespace {
std::mutex g_refusal_mutex;
std::string g_refusal;  // guarded: why co-op is off in this game (Refusal())

void SetRefusal(std::string why) {
    std::lock_guard lock(g_refusal_mutex);
    g_refusal = std::move(why);
}

} // namespace

std::string Refusal() {
    std::lock_guard lock(g_refusal_mutex);
    return g_refusal;
}

void PatchMain(std::uint64_t title_id, std::string_view module, std::string_view build_id,
               const std::vector<std::uint8_t>& original, std::vector<std::uint8_t>& out) {
    if (title_id != kOcarinaProgramId || module != "main") {
        return;
    }
    SetRefusal({});  // a new game start
    constexpr std::size_t header = sizeof(Loader::NSOHeader);
    if (build_id == kMainBuild && original.size() >= header + kCaveEnd &&
        out.size() >= header + kCaveEnd) {
        // No game mods with co-op (issue 136): a mod that patches the game's
        // code where co-op does would break one or the other. Checked always,
        // so co-op stays off with any such mod, puppets on or not.
        if (const auto changed = CoopSites::ChangedSite(out, header)) {
            char where[24];
            std::snprintf(where, sizeof where, "%#llx", static_cast<unsigned long long>(*changed));
            SetRefusal(std::string("main+") + where);
            LOG_WARNING(Loader,
                        "    - Skipping \"coop puppet\": main+{} is not the game's own (a game "
                        "mod patches it); co-op stays off. Remove or turn off the game's mods.",
                        where);
            return;
        }
    }
    if (!Wanted()) {
        return;
    }
    if (build_id != kMainBuild) {
        LOG_WARNING(Loader, "    - Skipping \"coop puppet\": build id {} is not {}", build_id,
                    kMainBuild);
        return;
    }
    const std::size_t cave = header + kCaveOffset;
    if (original.size() < header + kCaveEnd || out.size() < header + kCaveEnd) {
        LOG_WARNING(Loader, "    - Skipping \"coop puppet\": main is too small");
        return;
    }
    for (const auto& h : kHooks) {
        const std::size_t at = header + h.offset;
        if (Word(original, at) != h.original || Word(out, at) != h.original) {
            LOG_WARNING(Loader,
                        "    - Skipping \"coop puppet\": main+{:#x} holds {:#010x} (patched "
                        "{:#010x}), expected {:#010x}",
                        h.offset, Word(original, at), Word(out, at), h.original);
            return;
        }
    }
    for (std::size_t i = cave; i < cave + kCave.size(); ++i) {
        if (original[i] != 0) {  // `out` was checked by ChangedSite
            LOG_WARNING(Loader, "    - Skipping \"coop puppet\": main+{:#x} is not free",
                        i - header);
            return;
        }
    }
    for (const auto& h : kHooks) {
        const std::uint32_t call = HookWord(h.offset);
        std::memcpy(out.data() + header + h.offset, &call, sizeof(call));
    }
    std::memcpy(out.data() + cave, kCave.data(), kCave.size());
    LOG_INFO(Loader,
             "    - Applying \"coop puppet\": bl {:#x} at main+{:#x} and main+{:#x}, {} bytes "
             "of cave",
             kCaveOffset, kHooks[0].offset, kHooks[1].offset, kCave.size());
    // The AI hold's own cave and hook (coop_hold.h), when asked for.
    if (EnvOn("LICHII_COOP_OBJECTS_AI_HOLD")) {
        const std::size_t at = header + CoopHold::kHookOffset;
        if (Word(original, at) != CoopHold::kHookOriginal ||
            Word(out, at) != CoopHold::kHookOriginal) {
            LOG_WARNING(Loader, "    - Skipping \"coop ai hold\": main+{:#x} holds {:#010x}",
                        CoopHold::kHookOffset, Word(out, at));
        } else {
            const std::uint32_t call = CoopHold::HookWord();
            std::memcpy(out.data() + at, &call, sizeof(call));
            std::memcpy(out.data() + header + CoopHold::kCaveOffset, CoopHold::kCave.data(),
                        CoopHold::kCave.size());
            LOG_INFO(Loader, "    - Applying \"coop ai hold\": bl {:#x} at main+{:#x}",
                     CoopHold::kCaveOffset, CoopHold::kHookOffset);
        }
    }
    if (EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET")) {
        ConfigurePuppets();  // the cave may be here for the other callers alone
    }
}

bool Call(Core::System& system, std::uint32_t imm, std::array<std::uint64_t, 8>& args) {
    if (imm != kPuppetSvc) {
        return false;
    }
    auto& process = Kernel::GetCurrentProcess(system.Kernel());
    if (!process.Is64Bit() || process.GetProgramId() != kOcarinaProgramId) {
        return false;
    }
    ProcessGuest guest(process.GetMemory());
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const std::uint64_t buffer = args[0];
    // The puppet director, then the others by order (coop_callers.h).
    const auto call = CoopCallers::Next(guest, args[2] != 0, args[1], now_ms);
    if (!call) {
        args[0] = 0;
        return true;
    }
    const bool ok =
        guest.Write(buffer + kBufferStack, call->stack.data(), sizeof(call->stack)) &&
        guest.Write(buffer + kBufferFunction, &call->function, sizeof(call->function)) &&
        guest.Write(buffer + kBufferRegisters, call->x.data(), sizeof(call->x)) &&
        guest.Write(buffer + kBufferS0, &call->s0, sizeof(call->s0));
    args[0] = ok ? 1 : 0;
    return true;
}

} // namespace CoopSvc
