// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module's side of coop_look.h: what applying a look does, and
// the worker that keeps your own Link in it: the tunic in the pack's colours
// (coop_look_tunic.h), the gear painted in any colour (coop_look_parts.h),
// and the other players' puppets' own copies of the gear painted in their
// looks (coop_look_share.h slots, docs/puppet.md "Gear colours on the puppets").
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

std::mutex g_slots_mutex;
std::map<std::string, CoopLookParts::SlotLooks> g_slots_looks;   // guarded: a painting's name -> the slots' looks

// The puppets' slot textures (the gear file under every slot's names),
// painted in the looks of the players in the slots.
std::optional<std::vector<std::uint8_t>> SlotsFile(const std::string& name) {
    const auto base = ReadFile(LookDir() / (std::string(CoopLookParts::kBase) + ".bin"));
    if (!base) {
        return std::nullopt;
    }
    const auto gear = CoopLookTunic::Parse(*base);
    if (!gear) {
        return std::nullopt;   // the gear's wardrobe says why
    }
    CoopLookParts::SlotLooks looks(CoopLookShare::kSlots);
    if (name != CoopLookParts::kSlotsBase) {
        std::lock_guard lock(g_slots_mutex);
        const auto it = g_slots_looks.find(name);
        if (it == g_slots_looks.end()) {
            return std::nullopt;
        }
        looks = it->second;
    }
    return CoopLookTunic::Pack(CoopLookParts::SlotTexels(*gear, looks));
}

// The slots' looks now, from the network's share.
CoopLookParts::SlotLooks SlotLooksNow() {
    CoopLookParts::SlotLooks looks;
    for (const auto& parts : CoopLookShare::SlotLooks()) {
        if (!parts) {
            looks.emplace_back();
            continue;
        }
        std::vector<std::pair<std::uint8_t, std::uint32_t>> wire;
        for (const auto& p : *parts) {
            wire.emplace_back(p.part, p.colour);
        }
        looks.emplace_back(CoopLook::FromWire(wire));
    }
    return looks;
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
std::unique_ptr<CoopLookTunic::Wardrobe> g_puppets;    // the puppets' slot copies of the gear
std::uint64_t g_slots_serial = ~std::uint64_t{0};      // the worker's: CoopLookShare::Serial last seen
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
        if (const auto serial = CoopLookShare::Serial(); serial != g_slots_serial) {
            // A player's look or slot changed, or a puppet was given a slot's
            // copy to wear (its texture loads now): paint and search again.
            g_slots_serial = serial;
            const auto looks = SlotLooksNow();
            const std::string name = CoopLookParts::SlotsName(looks);
            {
                std::lock_guard slots_lock(g_slots_mutex);
                g_slots_looks[name] = looks;
            }
            g_puppets->Wear(name);
            g_puppets->Kick();
        }
        g_wardrobe->Step(static_cast<std::uint64_t>(ms.count()));
        g_gear->Step(static_cast<std::uint64_t>(ms.count()));
        g_puppets->Step(static_cast<std::uint64_t>(ms.count()));
        lock.lock();
    }
}

// The pause screen closed with the look unchanged: the equipment may have
// changed all the same (a new shield's texture to find).
void Unpaused() {
    g_wardrobe->Kick();
    g_gear->Kick();
    g_puppets->Kick();
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
    Wear(TunicName(tunic), look);   // your own Link
    CoopTitle::SaveLook(look);   // the next start
}

}  // namespace

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
    CoopLookTunic::Wardrobe::Host puppets = host;
    puppets.file = SlotsFile;
    puppets.base = CoopLookParts::kSlotsBase;
    puppets.what = "puppets' gear";
    g_wardrobe = std::make_unique<CoopLookTunic::Wardrobe>(std::move(host));
    g_gear = std::make_unique<CoopLookTunic::Wardrobe>(std::move(gear));
    g_puppets = std::make_unique<CoopLookTunic::Wardrobe>(std::move(puppets));
    g_slots_serial = ~std::uint64_t{0};
    // The others' puppets wear their own gear colours only where the pack has
    // the gear's texels to paint their copies with (an older pack: the game's
    // items on every puppet, as before).
    const bool slots = ReadFile(LookDir() / (std::string(CoopLookParts::kBase) + ".bin")).has_value();
    CoopLookShare::EnableSlots(slots);
    LOG_INFO(Core, "coop: look: other players' gear colours on their puppets {}",
             slots ? "on (pack slots)" : "off (the puppet pack has no coop_look/gear.bin; make it again)");
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
    Init(worn, WearLook, Unpaused);
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
    g_puppets.reset();
    CoopLookShare::EnableSlots(false);
}

}  // namespace CoopLook
