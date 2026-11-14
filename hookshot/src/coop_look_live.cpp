// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module's side of coop_look.h: what applying a look does, and
// the worker that keeps your own Link in it: the tunic in the pack's colours
// (coop_look_tunic.h), the gear painted in any colour (coop_look_parts.h),
// both drawn through Longshot's texture overrides (patch 0020), and the
// other players' puppets' own copies of the gear (coop_look_share.h) the same way.
#include <algorithm>
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
#include "hos_coop_texture.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace CoopLook {
namespace {

namespace fs = std::filesystem;

constexpr auto kRound = std::chrono::seconds(1);

// The game's textures and Longshot's overrides of them: patch 0020's exports
// of the game's executable (hos_coop_texture.h). Nothing of the game is
// written: the renderer draws the override, the game's memory keeps its own.
class GameTextures final : public CoopLookTunic::Textures {
public:
    GameTextures() {
#ifdef _WIN32
        HMODULE exe = GetModuleHandleW(nullptr);
        const auto look = [exe](const char* name) {
            return exe != nullptr ? reinterpret_cast<void*>(GetProcAddress(exe, name)) : nullptr;
        };
#else
        const auto look = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
#endif
        list_ = reinterpret_cast<hos_coop_textures_fn>(look("hos_coop_textures"));
        override_ = reinterpret_cast<hos_coop_texture_override_fn>(look("hos_coop_texture_override"));
        if (list_ == nullptr || override_ == nullptr) {
            list_ = nullptr, override_ = nullptr;
        }
    }
    bool Known() const { return list_ != nullptr; }

    std::vector<CoopLookTunic::Live> List(std::uint64_t size_min, std::uint64_t size_max,
                                          std::uint64_t* generation) const override {
        std::vector<CoopLookTunic::Live> out;
        if (list_ == nullptr) {
            return out;
        }
        std::vector<HosCoopTexture> got;
        for (int tries = 0; tries < 4; ++tries) {   // (more may be made between two calls)
            const int n = list_(size_min, size_max, got.data(), static_cast<int>(got.size()), generation);
            if (n <= static_cast<int>(got.size())) {
                got.resize(static_cast<std::size_t>(std::max(n, 0)));
                break;
            }
            got.resize(static_cast<std::size_t>(n) + 64);
        }
        for (const HosCoopTexture& t : got) {
            out.push_back({t.address, t.size, t.serial});
        }
        return out;
    }
    bool Read(std::uint64_t address, void* out, std::size_t size) const override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Override(std::uint64_t address, std::uint64_t serial, const void* bytes, std::size_t size) override {
        return override_ != nullptr && override_(address, serial, bytes, size) == 1;
    }

private:
    hos_coop_textures_fn list_ = nullptr;
    hos_coop_texture_override_fn override_ = nullptr;
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

// The puppets' gear (coop_look_share.h, docs/puppet.md "Gear colours on the
// puppets"): each copy of the items painted in the look of the player it is
// for, all copies in one wardrobe, drawn by Longshot's texture overrides as
// your own gear is. Nothing of the game is written.
std::unique_ptr<CoopLookTunic::Wardrobe> g_puppets;    // the puppets' copies of the gear
std::mutex g_copies_mutex;
// guarded: each copy's textures as painted last (index 0: copy 1), and the
// paintings the wardrobe may ask for by name
std::vector<std::shared_ptr<const CoopLookTunic::Texels>> g_copies;
std::map<std::string, std::vector<std::shared_ptr<const CoopLookTunic::Texels>>> g_copies_named;
CoopLookParts::CopyLooks g_copy_looks;   // the worker's: what each copy is painted in

std::optional<CoopLookTunic::Texels> GearTexels() {
    const auto base = ReadFile(LookDir() / (std::string(CoopLookParts::kBase) + ".bin"));
    return base ? CoopLookTunic::Parse(*base) : std::nullopt;
}

constexpr std::size_t kCopies = CoopLookShare::kSlots * CoopLookShare::kCopiesPerSlot;

// The copies' wardrobe's file: every copy's textures, in the game's own
// colours (the base) or as painted for a name the worker gave it.
std::optional<std::vector<std::uint8_t>> CopiesFile(const std::string& name) {
    CoopLookTunic::Texels all;
    if (name == CoopLookParts::kCopiesBase) {
        const auto gear = GearTexels();
        if (!gear) {
            return std::nullopt;
        }
        for (std::size_t k = 1; k <= kCopies; ++k) {
            auto t = CoopLookParts::CopyTexels(*gear, k, std::nullopt);
            all.insert(all.end(), std::make_move_iterator(t.begin()), std::make_move_iterator(t.end()));
        }
        return CoopLookTunic::Pack(all);
    }
    std::vector<std::shared_ptr<const CoopLookTunic::Texels>> copies;
    {
        std::lock_guard lock(g_copies_mutex);
        const auto it = g_copies_named.find(name);
        if (it == g_copies_named.end()) {
            return std::nullopt;
        }
        copies = it->second;
    }
    for (const auto& c : copies) {
        all.insert(all.end(), c->begin(), c->end());
    }
    return CoopLookTunic::Pack(all);
}

// Paints copy `copy` in `look` and has the copies' wardrobe draw it. The one
// place the puppets' colours are made.
bool PaintPuppetCopy(std::size_t copy, const CoopLook::Look& look) {
    static std::optional<CoopLookTunic::Texels> gear;   // the worker's: the game's own gear texels, read once
    if (!gear && !(gear = GearTexels())) {
        return false;
    }
    if (copy < 1 || copy > kCopies) {
        return false;
    }
    std::string name;
    {
        std::lock_guard lock(g_copies_mutex);
        if (g_copies.size() != kCopies) {
            g_copies.clear();
            g_copy_looks.assign(kCopies, std::nullopt);
            for (std::size_t k = 1; k <= kCopies; ++k) {
                g_copies.push_back(std::make_shared<const CoopLookTunic::Texels>(
                    CoopLookParts::CopyTexels(*gear, k, std::nullopt)));
            }
        }
    }
    // (painted outside the lock: about 0.1 s a texture of this worker's time)
    auto painted = std::make_shared<const CoopLookTunic::Texels>(CoopLookParts::CopyTexels(*gear, copy, look));
    {
        std::lock_guard lock(g_copies_mutex);
        g_copies[copy - 1] = std::move(painted);
        g_copy_looks[copy - 1] = look;
        name = CoopLookParts::CopiesName(g_copy_looks);
        g_copies_named.clear();   // only the newest painting is asked for
        g_copies_named[name] = g_copies;
    }
    g_puppets->Wear(name);
    g_puppets->Kick();
    return true;
}

// Paints every copy a player's puppet is waiting for (CoopLookShare::Jobs);
// the wardrobe draws it once the game has made the copy's textures (the
// puppet wears the copy from now on). Painting takes this worker's time, so
// many new looks at once delay your own Link's next repaint by as much.
void PaintPuppets(std::uint64_t now_ms) {
    static std::vector<CoopLookShare::Share::Job> broken;   // the worker's: said once, not tried again
    for (const CoopLookShare::Share::Job& job : CoopLookShare::Jobs(now_ms)) {
        if (std::find(broken.begin(), broken.end(), job) != broken.end()) {
            continue;   // that puppet keeps the game's items (or its last copy)
        }
        std::vector<std::pair<std::uint8_t, std::uint32_t>> wire;
        for (const auto& p : job.parts) {
            wire.emplace_back(p.part, p.colour);
        }
        if (!PaintPuppetCopy(job.copy, CoopLook::FromWire(wire))) {
            broken.push_back(job);
            LOG_WARNING(Core, "coop: look: P{}'s gear colours not painted (copy {}: the pack's coop_look/gear.bin "
                        "cannot be read; make the puppet pack again): their puppet keeps the game's items",
                        job.player, job.copy);
            continue;
        }
        CoopLookShare::Done(job, now_ms);
        LOG_INFO(Core, "coop: look: P{}'s gear colours painted for their puppet (copy {})", job.player, job.copy);
    }
}

CoopLookShare::Parts Share(const CoopLook::Look& look) {
    CoopLookShare::Parts parts;
    for (const auto& [number, colour] : CoopLook::Wire(look)) {
        parts.push_back({number, colour});
    }
    return parts;
}

std::unique_ptr<GameTextures> g_textures;
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
        PaintPuppets(static_cast<std::uint64_t>(ms.count()));   // before their puppets wear the copies
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
    // The wardrobes paint the textures (coop_look_paint) and have the game draw them through Longshot's texture
    // overrides (patch 0020); nothing is written over on the heap.
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

    g_textures = std::make_unique<GameTextures>();
    if (!g_textures->Known()) {
        LOG_WARNING(Core, "coop: look: this Longshot has no texture overrides (patch 0020): your own Link keeps the "
                          "game's colours (the others see yours all the same)");
    }
    CoopLookTunic::Wardrobe::Host host;
    host.textures = g_textures.get();
    host.file = [](const std::string& colour) { return ReadFile(LookDir() / (colour + ".bin")); };
    host.log = [](const std::string& line) { LOG_INFO(Core, "{}", line); };
    CoopLookTunic::Wardrobe::Host gear = host;
    gear.file = GearFile;
    gear.base = CoopLookParts::kBase;
    gear.what = "gear";
    CoopLookTunic::Wardrobe::Host puppets = host;
    puppets.file = CopiesFile;
    puppets.base = CoopLookParts::kCopiesBase;
    puppets.what = "puppets' gear";
    g_wardrobe = std::make_unique<CoopLookTunic::Wardrobe>(std::move(host));
    g_gear = std::make_unique<CoopLookTunic::Wardrobe>(std::move(gear));
    g_puppets = std::make_unique<CoopLookTunic::Wardrobe>(std::move(puppets));
    // The others' puppets wear their own gear colours only where the pack has
    // the gear's texels to paint their copies with, and a Longshot that draws
    // them (patch 0020).
    const bool gear_file = ReadFile(LookDir() / (std::string(CoopLookParts::kBase) + ".bin")).has_value();
    const bool slots = gear_file && g_textures->Known();
    CoopLookShare::EnableSlots(slots);
    LOG_INFO(Core, "coop: look: other players' gear colours on their puppets {}",
             slots       ? "on (pack copies, drawn by Longshot's texture overrides)"
             : gear_file ? "off (this Longshot has no texture overrides, patch 0020)"
                         : "off (the puppet pack has no coop_look/gear.bin; make it again)");
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
    g_wardrobe->Clear();   // the colours worn back to the game's own: no override left behind
    g_gear->Clear();
    g_puppets->Clear();
    g_wardrobe.reset();   // a later StartModule starts a new worker
    g_gear.reset();
    g_puppets.reset();
    {
        std::lock_guard lock(g_copies_mutex);
        g_copies.clear();
        g_copies_named.clear();
    }
    CoopLookShare::EnableSlots(false);
    g_textures.reset();
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
