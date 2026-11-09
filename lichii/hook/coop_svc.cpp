// The emulator side of puppets (coop_svc.h, docs/puppet.md).
#include "coop_svc.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#include "coop_puppet.h"

namespace CoopSvc {
namespace {

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
           EnvOn("LICHII_COOP_SPAWN");
}

// Settings for the puppets, once, when the patch goes in.
void ConfigurePuppets() {
    CoopPuppet::PlannerSettings settings;
    settings.debug = EnvOn("LICHII_COOP_DEBUG_PUPPET");
    if (const auto actor = Env("LICHII_COOP_PUPPET_ACTOR"); !actor.empty()) {
        settings.actor = actor;
    }
    const bool debug = settings.debug;
    LOG_INFO(Core, "coop: puppets on, actor {}{}", settings.actor,
             debug ? ", debug puppet beside you" : "");
    CoopPuppet::Configure(std::move(settings), debug,
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


// The callers after the puppets, by order. The mutex is held across a step;
// only AddCaller (rare) waits on it.
struct Callers {
    std::mutex mutex;
    std::vector<std::pair<int, Caller>> list;
};
Callers& TheCallers() {
    static Callers callers;
    return callers;
}

} // namespace

void AddCaller(int order, Caller caller) {
    Callers& c = TheCallers();
    std::lock_guard lock(c.mutex);
    const auto at = std::upper_bound(
        c.list.begin(), c.list.end(), order,
        [](int o, const std::pair<int, Caller>& e) { return o < e.first; });
    c.list.emplace(at, order, std::move(caller));
}

void PatchMain(std::uint64_t title_id, std::string_view module, std::string_view build_id,
               const std::vector<std::uint8_t>& original, std::vector<std::uint8_t>& out) {
    if (title_id != kOcarinaProgramId || module != "main" || !Wanted()) {
        return;
    }
    if (build_id != kMainBuild) {
        LOG_WARNING(Loader, "    - Skipping \"coop puppet\": build id {} is not {}", build_id,
                    kMainBuild);
        return;
    }
    constexpr std::size_t header = sizeof(Loader::NSOHeader);
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
    for (std::size_t i = cave; i < header + kCaveEnd; ++i) {
        if (original[i] != 0 || out[i] != 0) {
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
    // Caller 0 is the puppet director, then the others by order. Each is asked
    // once a frame (after_call false) when its turn comes and gets the result
    // of each call it made; the turn passes on when it has no more calls.
    static std::size_t current = 0;  // who made the last call; the calc thread only
    Callers& callers = TheCallers();
    std::lock_guard lock(callers.mutex);
    const auto step = [&](std::size_t i, bool after_call, std::uint64_t result) {
        return i == 0 ? CoopPuppet::TheDirector().Step(guest, after_call, result, now_ms)
                      : callers.list[i - 1].second(guest, after_call, result, now_ms);
    };
    const std::size_t count = callers.list.size() + 1;
    std::optional<CoopPuppet::Call> call;
    std::size_t i = 0;
    if (args[2] != 0 && current < count) {
        call = step(current, true, args[1]);
        i = current + 1;
    }
    for (; !call && i < count; ++i) {
        call = step(i, false, 0);
        current = i;
    }
    if (!call) {
        args[0] = 0;
        return true;
    }
    const bool ok =
        guest.Write(buffer + kBufferStack, call->stack.data(), sizeof(call->stack)) &&
        guest.Write(buffer + kBufferFunction, &call->function, sizeof(call->function)) &&
        guest.Write(buffer + kBufferRegisters, call->x.data(), sizeof(call->x));
    args[0] = ok ? 1 : 0;
    return true;
}

} // namespace CoopSvc
