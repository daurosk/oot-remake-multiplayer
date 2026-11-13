// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module's side of coop_look.h: what applying a look does, and
// the worker that keeps your own Link's tunic in it (coop_look_tunic.h).
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_hook.h"
#include "coop_look.h"
#include "coop_look_tunic.h"
#include "coop_runtime.h"
#include "coop_settings.h"
#include "coop_title.h"

namespace CoopLook {
namespace {

namespace fs = std::filesystem;

// The game's heap (Hookshot host/hos.h HOS_HEAP_BASE), searched up to where
// it is mapped: the textures the game loaded are in it.
constexpr std::uint64_t kHeapBase = 0x1000000000ULL;
constexpr std::uint64_t kHeapLimit = std::uint64_t{16} << 30;
constexpr std::uint64_t kHeapStep = std::uint64_t{1} << 20;
constexpr auto kRound = std::chrono::seconds(1);

class GameMemory final : public CoopLookTunic::Memory {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) const override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return CoopRuntime::Write(address, in, size);   // the bridge marks the pages: the texture is decoded again
    }
};

// The puppet pack where Build-Hookshot-Coop.bat puts it (hookshot-build.ps1).
fs::path LookDir() {
    return CoopRuntime::GameDir() / "mods" / "contents" / "0400486024DEC000" / "coop-puppet" / "coop_look";
}

std::optional<std::vector<std::uint8_t>> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

GameMemory g_memory;
std::unique_ptr<CoopLookTunic::Wardrobe> g_wardrobe;
std::thread g_worker;
std::mutex g_wake_mutex;
std::condition_variable g_wake;
bool g_kick = false;   // guarded by g_wake_mutex: a new colour, no need to wait the round out
bool g_stop = false;   // guarded by g_wake_mutex

void Wear(const std::string& name) {
    g_wardrobe->Wear(name);
    {
        std::lock_guard lock(g_wake_mutex);
        g_kick = true;
    }
    g_wake.notify_one();
}

void Work() {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(g_wake_mutex);
    while (!g_stop) {
        g_wake.wait_for(lock, kRound, [] { return g_stop || g_kick; });
        if (g_stop) {
            break;
        }
        g_kick = false;
        lock.unlock();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        g_wardrobe->Step(static_cast<std::uint64_t>(ms.count()));
        lock.lock();
    }
}

void Apply(const Look& look) {
    const auto& tunic = look[Part::tunic];
    CoopHook::SetColor(tunic);   // the others' puppet of you, and a session started later
    Wear(TunicName(tunic));      // your own Link
    CoopTitle::SaveLook(look);   // the next start
}

}  // namespace

void StartModule() {
    if (g_wardrobe) {
        return;
    }
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;   // (said by the module's own start)
    Look worn;
    worn[Part::tunic] = CoopHook::LoadSettings(ini, errors).color;

    CoopLookTunic::Wardrobe::Host host;
    host.memory = &g_memory;
    host.range = [] {
        return std::pair{kHeapBase, CoopLookTunic::MappedEnd(g_memory, kHeapBase, kHeapBase + kHeapLimit, kHeapStep)};
    };
    host.file = [](const std::string& colour) { return ReadFile(LookDir() / (colour + ".bin")); };
    host.log = [](const std::string& line) { LOG_INFO(Core, "{}", line); };
    g_wardrobe = std::make_unique<CoopLookTunic::Wardrobe>(std::move(host));
    // The game started in the settings' colour (the pack's own tiles): the
    // wardrobe looks for that colour's texels too.
    g_wardrobe->Wear(TunicName(worn[Part::tunic]));
    Init(worn, Apply);
    g_stop = false;
    g_worker = std::thread(Work);
    LOG_INFO(Core, "coop: look: tunic {} (changed in the pause menu, worn on unpause)",
             worn[Part::tunic] ? ColourText(worn[Part::tunic]) : std::string("by player number"));
}

void StopModule() {
    if (!g_worker.joinable()) {
        return;
    }
    {
        std::lock_guard lock(g_wake_mutex);
        g_stop = true;
    }
    g_wake.notify_one();
    g_worker.join();
}

}  // namespace CoopLook
