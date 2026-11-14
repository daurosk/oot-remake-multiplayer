// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module's side of coop_look.h: what applying a look does, and
// the worker that keeps your own Link in it: the tunic in the pack's colours
// (coop_look_tunic.h), the gear painted in any colour (coop_look_parts.h).
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_hook.h"
#include "coop_look.h"
#include "coop_look_view.h"
#include "coop_look_parts.h"
#include "coop_look_share.h"
#include "coop_look_tunic.h"
#include "coop_pause_files.h"
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

std::mutex g_gear_mutex;
std::map<std::string, CoopLook::Look> g_gear_looks;    // guarded: a painting's name -> its look

// The gear file of the pack, painted in a look the module asked for.
std::optional<std::vector<std::uint8_t>> GearFile(const std::string& name) {
    auto base = ReadFile(LookDir() / (std::string(CoopLookParts::kBase) + ".bin"));
    if (!base || name == CoopLookParts::kBase) {
        return base;
    }
    CoopLook::Look look;
    {
        std::lock_guard lock(g_gear_mutex);
        const auto it = g_gear_looks.find(name);
        if (it == g_gear_looks.end()) {
            return std::nullopt;
        }
        look = it->second;
    }
    const auto gear = CoopLookTunic::Parse(*base);
    if (!gear) {
        return base;   // the wardrobe says why when it reads the base
    }
    return CoopLookTunic::Pack(CoopLookParts::Painted(*gear, look));
}

CoopLookShare::Parts Share(const CoopLook::Look& look) {
    CoopLookShare::Parts parts;
    for (const auto& [number, colour] : CoopLook::Wire(look)) {
        parts.push_back({number, colour});
    }
    return parts;
}

GameMemory g_memory;
std::unique_ptr<CoopLookTunic::Wardrobe> g_wardrobe;   // the tunic
std::unique_ptr<CoopLookTunic::Wardrobe> g_gear;       // the gear
std::thread g_worker;
std::mutex g_wake_mutex;
std::condition_variable g_wake;
bool g_kick = false;   // guarded by g_wake_mutex: a new colour, no need to wait the round out
bool g_stop = false;   // guarded by g_wake_mutex

void Wear(const std::string& name, const CoopLook::Look& look) {
    g_wardrobe->Wear(name);
    const std::string gear = CoopLookParts::Name(look);
    {
        std::lock_guard lock(g_gear_mutex);
        g_gear_looks[gear] = look;
    }
    g_gear->Wear(gear);
    g_wardrobe->Kick();   // the pause screen closed: the equipment may have changed
    g_gear->Kick();
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
        g_gear->Step(static_cast<std::uint64_t>(ms.count()));
        lock.lock();
    }
}

// The pause screen closed with the look unchanged: the equipment may have
// changed all the same (a new shield's texture to find).
void Unpaused() {
    g_wardrobe->Kick();
    g_gear->Kick();
    {
        std::lock_guard lock(g_wake_mutex);
        g_kick = true;
    }
    g_wake.notify_one();
}

void WearLook(const Look& look) {
    const auto& tunic = look[Part::tunic];
    CoopHook::SetColor(tunic);   // the others' puppet of you, and a session started later
    CoopLookShare::Set(Share(look));   // every part, for the others (PLAYER_LOOK)
    PaintOwnLink(look);   // your own Link
    CoopTitle::SaveLook(look);   // the next start
}

// A look chosen with the menu still open (the Customize page's live picture):
// your own Link only; the others and the settings get it on unpause.
void PreviewLook(const Look& look) {
    PaintOwnLink(look);
}

}  // namespace

void PaintOwnLink(const Look& look) {
    // NOT the game's own path yet: the gear's texels are written over on the heap (coop_look_paint, the wardrobes).
    // customizer-native replaces the inside of this function with the game's material path; nothing else changes.
    if (!g_wardrobe || !g_gear) {
        return;   // (before StartModule or after StopModule)
    }
    Wear(TunicName(look[Part::tunic]), look);
}

void StartModule() {
    if (g_wardrobe) {
        return;
    }
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;   // (said by the module's own start)
    // The parts but the tunic: from coop-options.ini of a kit, else coop.ini.
    const auto kit = CoopFiles::FindKit();
    std::vector<std::string> part_errors;
    Look worn = ReadParts(CoopFiles::ReadText(kit ? *kit / "coop-options.ini" : CoopFiles::CoopIni()), &part_errors);
    worn[Part::tunic] = CoopHook::LoadSettings(ini, errors).color;

    CoopLookTunic::Wardrobe::Host host;
    host.memory = &g_memory;
    host.range = [] {
        return std::pair{kHeapBase, CoopLookTunic::MappedEnd(g_memory, kHeapBase, kHeapBase + kHeapLimit, kHeapStep)};
    };
    host.file = [](const std::string& colour) { return ReadFile(LookDir() / (colour + ".bin")); };
    host.log = [](const std::string& line) { LOG_INFO(Core, "{}", line); };
    CoopLookTunic::Wardrobe::Host gear = host;
    gear.file = GearFile;
    gear.base = CoopLookParts::kBase;
    gear.what = "gear";
    g_wardrobe = std::make_unique<CoopLookTunic::Wardrobe>(std::move(host));
    g_gear = std::make_unique<CoopLookTunic::Wardrobe>(std::move(gear));
    // The game started in the settings' colour (the pack's own tiles): the
    // wardrobe looks for that colour's texels too.
    g_wardrobe->Wear(TunicName(worn[Part::tunic]));
    // The gear starts in the game's own colours: the settings' look is painted.
    {
        std::lock_guard lock(g_gear_mutex);
        g_gear_looks[CoopLookParts::Name(worn)] = worn;
    }
    g_gear->Wear(CoopLookParts::Name(worn));
    if (CoopLookParts::AnyGear(worn)) {
        CoopLookShare::Set(Share(worn));
    }
    for (const std::string& e : part_errors) {
        LOG_WARNING(Core, "coop: look: {}", e);
    }
    Init(worn, WearLook, Unpaused, PreviewLook);
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
    g_wardrobe.reset();   // a later StartModule starts a new worker
    g_gear.reset();
}

}  // namespace CoopLook

// The Customize page's live picture (coop_look_view.h): the module's one orbit,
// with your own Link from the co-op hook.
namespace CoopLookView {

namespace {

std::mutex g_mutex;
Orbit g_orbit;   // guarded

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Link's top above his feet by age (the name tags' heights, coop_hook.cpp).
constexpr float kHeightChild = 1.35f;
constexpr float kHeightAdult = 1.9f;

std::optional<Subject> OwnLink() {
    thread_local CoopHook::MapView view;   // (keeps its room: no allocation once filled)
    if (!CoopHook::CurrentMapView(view) || view.event) {
        return std::nullopt;
    }
    return Subject{view.self, view.adult ? kHeightAdult : kHeightChild};
}

}  // namespace

bool LinkShown() {
    return OwnLink().has_value();
}

void Ask(const Window& window) {
    std::lock_guard lock(g_mutex);
    g_orbit.Ask(window, NowMs());
}

bool Camera(double eye[3], double target[3]) {
    if (eye == nullptr || target == nullptr) {
        return false;
    }
    const auto now = NowMs();
    {
        std::lock_guard lock(g_mutex);
        if (!g_orbit.Asked(now)) {
            g_orbit.Camera({}, {}, std::nullopt, now);   // (ends a turn)
            return false;
        }
    }
    const auto link = OwnLink();   // (outside the lock: the hook's own lock)
    std::lock_guard lock(g_mutex);
    const auto shot = g_orbit.Camera({eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]}, link, now);
    if (!shot) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        eye[i] = shot->eye[i];
        target[i] = shot->target[i];
    }
    return true;
}

}  // namespace CoopLookView
