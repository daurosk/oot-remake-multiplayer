// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module's side of coop_look.h: what applying a look does, and
// the worker that keeps your own Link in it: the tunic and the gear painted
// in any colour (coop_look_tunic.h, coop_look_parts.h),
// both drawn through Longshot's texture overrides (patch 0020), and the
// other players' puppets' own copies of the gear (coop_look_share.h) the same way.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_camera.h"
#include "coop_hook.h"
#include "coop_look.h"
#include "coop_look_view.h"
#include "coop_look_parts.h"
#include "coop_look_share.h"
#include "coop_look_tunic.h"
#include "coop_pause_files.h"
#include "coop_puppet.h"
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
// The wardrobes' step: the game makes Link's textures again (scene changes,
// cutscenes, its streaming) and the override stays with the old one; until a
// step finds the new one the game's colours show (0.11.0: up to a round, seen
// as flicker). A step with nothing new made only asks Longshot's generation.
constexpr auto kStep = std::chrono::milliseconds(20);

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

// The tunic's file of a name the module asked for (CoopLookParts::TunicPainting):
// green.bin's texels painted in its exact colour, and the Cap trim's blocks in
// the cap's (coop_look/cap.bin); another name is the pack's own file (green,
// or the colour the kit put in at the start). The worker's, from the
// wardrobe's Step. The last kTunicKept paintings are kept, so the Customize
// page's Red, Green and Blue rows going back and forth are drawn at once (a
// painting of both ages' seven textures takes a few tenths of a second).
constexpr std::size_t kTunicKept = 8;
std::mutex g_tunic_mutex;
std::shared_ptr<const CoopLookTunic::Texels> g_tunic_green;   // guarded: green.bin, read once
std::vector<std::pair<std::string, std::vector<std::uint8_t>>> g_tunic_kept;   // guarded: the newest first
// guarded: the cap's blocks (cap.bin, read at StartModule); none: the Cap trim changes nothing (an older pack)
std::shared_ptr<const CoopLookParts::Cap> g_tunic_cap;

// The Cap trim colour a painting of `look` takes: none without cap.bin.
std::optional<std::uint32_t> CapOf(const CoopLook::Look& look) {
    std::lock_guard lock(g_tunic_mutex);
    return g_tunic_cap ? look[Part::cap_trim] : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> TunicFile(const std::string& name) {
    const auto paint = CoopLookParts::TunicPaintOf(name);
    if (!paint) {
        return ReadFile(LookDir() / (name + ".bin"));
    }
    std::lock_guard lock(g_tunic_mutex);
    for (auto it = g_tunic_kept.begin(); it != g_tunic_kept.end(); ++it) {
        if (it->first == name) {
            std::rotate(g_tunic_kept.begin(), it, it + 1);
            return g_tunic_kept.front().second;
        }
    }
    if (!g_tunic_green) {
        const auto file = ReadFile(LookDir() / (std::string(CoopLookParts::kTunicBase) + ".bin"));
        auto green = file ? CoopLookTunic::Parse(*file) : std::nullopt;
        if (!green) {
            return std::nullopt;   // (the wardrobe says why when it reads green.bin itself)
        }
        g_tunic_green = std::make_shared<const CoopLookTunic::Texels>(std::move(*green));
    }
    g_tunic_kept.insert(g_tunic_kept.begin(),
                        {name, CoopLookTunic::Pack(CoopLookParts::TunicPainted(*g_tunic_green, *paint,
                                                                               g_tunic_cap.get()))});
    if (g_tunic_kept.size() > kTunicKept) {
        g_tunic_kept.pop_back();
    }
    return g_tunic_kept.front().second;
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

// The boots in Link's own model (CoopLookParts::BodyBoots): each tunic
// colour's puppet model names copies of its own, painted here in the boots
// colour of the player who wears that colour. Your own Link's model (the
// game's, or its own copy: colour-agree) names the game's boots, which the
// gear wardrobe paints. Nothing of the game is written.
std::unique_ptr<CoopLookTunic::Wardrobe> g_boots;
std::mutex g_boots_mutex;
std::map<std::string, CoopLookParts::Wearers> g_boots_named;   // guarded: the newest painting's name
std::string g_boots_worn;                                        // the worker's: the name worn

std::optional<std::vector<std::uint8_t>> BootsFile(const std::string& name) {
    const auto gear = GearTexels();
    if (!gear) {
        return std::nullopt;
    }
    CoopLookParts::Wearers wearers;
    if (name != CoopLookParts::kBootsBase) {
        std::lock_guard lock(g_boots_mutex);
        const auto it = g_boots_named.find(name);
        if (it == g_boots_named.end()) {
            return std::nullopt;
        }
        wearers = it->second;
    }
    return CoopLookTunic::Pack(CoopLookParts::BootsTexels(*gear, wearers));
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
// With no colour chosen ("by player number") your own Link wears your player
// number's slot colour, the one the others see you in (OwnTunic): the number
// as the worker last saw it (0: not known, co-op off), and the chosen tunic.
std::atomic<std::uint8_t> g_number{0};
std::mutex g_chosen_mutex;
std::optional<std::uint32_t> g_chosen;   // guarded by g_chosen_mutex
std::optional<std::uint32_t> g_chosen_cap;   // guarded by g_chosen_mutex: the Cap trim worn (CapOf)

// The worker's round: who wears which colour now, and the boots they chose.
void FollowBoots() {
    std::vector<CoopLookParts::Wearer> others;
    for (const auto& row : CoopHook::PlayerList()) {
        if (row.self) {
            continue;
        }
        CoopLookParts::Wearer w;
        const std::uint32_t colour =
            CoopPuppet::PlayerColor(row.number).value_or(coop::protocol::slot_color(row.number));
        w.letter = CoopLookParts::LetterOf(TunicName(colour));
        if (const auto parts = CoopLookShare::Of(row.number)) {
            for (const auto& p : *parts) {
                if (p.part == static_cast<std::uint8_t>(Part::boots)) {
                    w.boots = p.colour;
                }
            }
        }
        others.push_back(w);
    }
    const CoopLookParts::Wearers wearers = CoopLookParts::Assign({}, others);
    const std::string name = CoopLookParts::BootsPaintingName(wearers);
    if (name == g_boots_worn) {
        return;
    }
    g_boots_worn = name;
    {
        std::lock_guard lock(g_boots_mutex);
        g_boots_named.clear();   // only the newest painting is asked for
        g_boots_named[name] = wearers;
    }
    LOG_INFO(Core, "coop: look: the puppets' boots are {} now", name);
    g_boots->Wear(name);
    g_boots->Kick();
}

std::uint8_t OwnNumber() {
    for (const auto& row : CoopHook::PlayerList()) {
        if (row.self) {
            return row.number;
        }
    }
    return 0;
}

// The worker's round: the player number came (welcome) or changed (rejoin),
// or co-op stopped. With no colour chosen, your own Link's tunic follows it.
void FollowNumber() {
    const std::uint8_t number = OwnNumber();
    if (number == g_number.exchange(number)) {
        return;
    }
    std::optional<std::uint32_t> chosen, cap;
    {
        std::lock_guard lock(g_chosen_mutex);
        chosen = g_chosen;
        cap = g_chosen_cap;
    }
    if (chosen) {
        return;   // a chosen colour wins
    }
    const auto tunic = OwnTunic(chosen, number);
    LOG_INFO(Core, "coop: look: player number {}: your own Link wears {}", static_cast<int>(number),
             tunic ? "the slot colour " + ColourText(tunic) : std::string("the game's own colour"));
    g_wardrobe->Wear(CoopLookParts::TunicPainting(tunic, cap));
    g_wardrobe->Kick();
}

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
    auto round = start + kRound;   // the next round of the rest (who wears what)
    std::unique_lock lock(g_wake_mutex);
    while (!g_stop) {
        g_wake.wait_for(lock, kStep, [] { return g_stop || g_kick; });
        if (g_stop) {
            break;
        }
        const bool kicked = std::exchange(g_kick, false);
        lock.unlock();
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        if (kicked || now >= round) {
            round = now + kRound;
            PaintPuppets(static_cast<std::uint64_t>(ms.count()));   // before their puppets wear the copies
            FollowNumber();
            FollowBoots();
        }
        g_wardrobe->Step(static_cast<std::uint64_t>(ms.count()));
        g_gear->Step(static_cast<std::uint64_t>(ms.count()));
        g_puppets->Step(static_cast<std::uint64_t>(ms.count()));
        g_boots->Step(static_cast<std::uint64_t>(ms.count()));
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
    const auto cap = CapOf(look);
    {
        std::lock_guard lock(g_chosen_mutex);
        g_chosen = look[Part::tunic];
        g_chosen_cap = cap;
    }
    // The tunic in its exact colour (the puppets of you: the pack's nearest, TunicName), the Cap trim over it.
    Wear(CoopLookParts::TunicPainting(OwnTunic(look[Part::tunic], g_number.load()), cap), look);
}

void AgreeStartTunic() {
    const char* set = std::getenv("LICHII_COOP_SELF_TUNIC");
    const std::string hint = set != nullptr ? set : "";
    const fs::path pack = LookDir().parent_path();
    // Your own tile (colour-agree packs), else an older pack's tile over the game's.
    std::string own = "Angin_Tile_S_Alb.";
    auto tile = ReadFile(pack / "romfs" / "Tex" / "Angin_Tile_S_Alb.bntx.zs");
    if (!tile) {
        own.clear();
        tile = ReadFile(pack / "romfs" / "Tex" / "Angin_Tile_A_Alb.bntx.zs");
    }
    const std::string start = PackStartTunic(
        tile, [&](const std::string& c) { return ReadFile(pack / "coop_self" / (c + "." + own + "bntx.zs")); },
        hint);
    // The module paints your colour on green's files (CoopLookParts::TunicPainted): the kit's launcher and the
    // autotest put green's in whatever the colour, when the pack has coop_look/green.bin. Another colour's files
    // are left from an older kit or a start without it: your Link starts in that colour and the wardrobe
    // repaints it (your own textures only: the puppets' copies are marked), but a texture of that colour that is
    // not green's size (the child's dark tile: BC3 there, ASTC in green) is not drawn over and keeps it.
    std::vector<std::string> errors;
    const auto chosen = CoopHook::LoadSettings(
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini", errors).color;
    std::error_code no_file;
    if (start != "green" && fs::exists(LookDir() / "green.bin", no_file)) {
        LOG_WARNING(Core, "coop: look: the puppet pack's tunic files are {}, not green: started without this kit's "
                          "launcher? Your Link is repainted in {} once the game's textures are found (a child "
                          "Link's tunic body may stay {})",
                    start, chosen ? ColourText(chosen) : std::string("your colour"), start);
    }
    if (start == (hint.empty() ? std::string("green") : hint)) {
        return;
    }
    LOG_INFO(Core, "coop: look: your Link starts in {} (the puppet pack's tunic files; LICHII_COOP_SELF_TUNIC said "
                   "{}): the switch follows the files",
             start, hint.empty() ? std::string("none") : hint);
#ifdef _MSC_VER
    _putenv_s("LICHII_COOP_SELF_TUNIC", start == "green" ? "" : start.c_str());
#else
    if (start == "green") {
        unsetenv("LICHII_COOP_SELF_TUNIC");
    } else {
        setenv("LICHII_COOP_SELF_TUNIC", start.c_str(), 1);
    }
#endif
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
    host.file = TunicFile;
    // The game's tunic textures are not only your Link's: Angin_Tile_A_Alb is on
    // Mido's model (39 Kokiri actors), the dark tile on Saria's (18 more). The
    // wardrobe finds textures by their texels, so it may only look for your own
    // Link's copies (tools/puppet_pack: marked, green's too). A pack from before
    // npc-colour has no own green copies, and its green.bin holds the game's
    // own texels: with it the tunic is not changed in the game at all.
    std::error_code no_file;
    if (!fs::exists(LookDir().parent_path() / "coop_self" / "green.Angin_Tile_S_Alb.bntx.zs", no_file)) {
        host.file = [](const std::string&) -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; };
        LOG_WARNING(Core, "coop: look: the puppet pack is older than this module (no own green tunic copies): your "
                          "Link's tunic colour is not changed in the game, so the Kokiri keep theirs (make the "
                          "puppet pack again; the others see your colour all the same)");
    }
    host.log = [](const std::string& line) { LOG_INFO(Core, "{}", line); };
    if (const char* self = std::getenv("LICHII_COOP_SELF_TUNIC")) {
        host.start = self;   // (the kit put that colour's files in: Set-KitSelfTunic)
    }
    CoopLookTunic::Wardrobe::Host gear = host;
    gear.file = GearFile;
    gear.base = CoopLookParts::kBase;
    gear.start.clear();   // (the kit changes only the tunic's files)
    gear.what = "gear";
    CoopLookTunic::Wardrobe::Host puppets = host;
    puppets.file = CopiesFile;
    puppets.base = CoopLookParts::kCopiesBase;
    puppets.start.clear();
    puppets.what = "puppets' gear";
    CoopLookTunic::Wardrobe::Host boots = puppets;
    boots.file = BootsFile;
    boots.base = CoopLookParts::kBootsBase;
    boots.what = "puppets' boots";
    g_wardrobe = std::make_unique<CoopLookTunic::Wardrobe>(std::move(host));
    g_gear = std::make_unique<CoopLookTunic::Wardrobe>(std::move(gear));
    g_puppets = std::make_unique<CoopLookTunic::Wardrobe>(std::move(puppets));
    g_boots = std::make_unique<CoopLookTunic::Wardrobe>(std::move(boots));
    g_boots_worn.clear();
    // The others' puppets wear their own gear colours only where the pack has
    // the gear's texels to paint their copies with, and a Longshot that draws
    // them (patch 0020).
    const bool gear_file = ReadFile(LookDir() / (std::string(CoopLookParts::kBase) + ".bin")).has_value();
    const bool slots = gear_file && g_textures->Known();
    // The metal parts' blocks (#202): a painted metal part is painted down to no metal.
    if (const auto metal_file = ReadFile(LookDir() / "metal.bin")) {
        std::string why;
        if (auto metal = CoopLookParts::ParseMetal(*metal_file, &why)) {
            LOG_INFO(Core, "coop: look: {} metal part block lists (a painted metal part loses its metal)",
                     metal->size());
            CoopLookParts::UseMetal(std::make_shared<const CoopLookParts::Metal>(std::move(*metal)));
        } else {
            LOG_WARNING(Core, "coop: look: coop_look/metal.bin: {}; painted metal parts stay metal", why);
        }
    } else if (gear_file) {
        LOG_INFO(Core, "coop: look: the puppet pack has no coop_look/metal.bin: painted metal parts stay metal "
                       "(make it again)");
    }
    // The Cap trim's blocks: without cap.bin (an older pack) the Cap trim row changes nothing.
    if (const auto cap_file = ReadFile(LookDir() / "cap.bin")) {
        std::string why;
        if (auto cap = CoopLookParts::ParseCap(*cap_file, &why)) {
            std::size_t blocks = 0;
            for (const auto& b : *cap) {
                blocks += b.offsets.size();
            }
            LOG_INFO(Core, "coop: look: the Cap trim paints {} blocks of {} tunic textures", blocks, cap->size());
            std::lock_guard lock(g_tunic_mutex);
            g_tunic_cap = std::make_shared<const CoopLookParts::Cap>(std::move(*cap));
        } else {
            LOG_WARNING(Core, "coop: look: coop_look/cap.bin: {}; the Cap trim changes nothing", why);
        }
    } else if (fs::exists(LookDir() / "green.bin", no_file)) {
        LOG_INFO(Core, "coop: look: the puppet pack has no coop_look/cap.bin: the Cap trim changes nothing (make "
                       "the puppet pack again)");
    }
    CoopLookShare::EnableSlots(slots);
    LOG_INFO(Core, "coop: look: other players' gear colours on their puppets {}",
             slots       ? "on (pack copies, drawn by Longshot's texture overrides)"
             : gear_file ? "off (this Longshot has no texture overrides, patch 0020)"
                         : "off (the puppet pack has no coop_look/gear.bin; make it again)");
    // The game started in green's files (the kit puts them in: the colour is
    // painted on them): the worker's first step paints the settings' colour.
    // With another colour's files (LICHII_COOP_SELF_TUNIC) the wardrobe looks
    // for that colour's texels too.
    const auto cap = CapOf(worn);
    g_wardrobe->Wear(CoopLookParts::TunicPainting(worn[Part::tunic], cap));
    {
        std::lock_guard lock(g_chosen_mutex);
        g_chosen = worn[Part::tunic];
        g_chosen_cap = cap;
    }
    g_number = 0;   // the worker's first round puts on the slot colour once the number is known
    // The gear starts in the game's own colours: the settings' look is painted.
    {
        std::lock_guard lock(g_gear_mutex);
        g_gear_looks[CoopLookParts::Name(worn)] = worn;
    }
    g_gear->Wear(CoopLookParts::Name(worn));
    // Always sent, gear colours or not (held-sword): the others give every player whose look
    // arrives their own copy of the items, so a puppet never wears the game's own sword, whose
    // blade hides with their own Link's. A look with no gear paints that copy in the game's colours.
    CoopLookShare::Set(Share(worn));
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
    g_boots->Clear();
    g_wardrobe.reset();   // a later StartModule starts a new worker
    g_gear.reset();
    g_puppets.reset();
    g_boots.reset();
    {
        std::lock_guard lock(g_boots_mutex);
        g_boots_named.clear();
    }
    {
        std::lock_guard lock(g_copies_mutex);
        g_copies.clear();
        g_copies_named.clear();
    }
    {
        std::lock_guard lock(g_tunic_mutex);
        g_tunic_green.reset();   // (the pack may be made again before the next start)
        g_tunic_kept.clear();
        g_tunic_cap.reset();
    }
    CoopLookShare::EnableSlots(false);
    g_number = 0;
    g_textures.reset();
}

}  // namespace CoopLook

// The Customize page's live picture (coop_look_view.h): the module's one orbit,
// with your own Link from the co-op hook.
namespace CoopLookView {

namespace {

std::mutex g_mutex;
Orbit g_orbit;   // guarded
Watch g_watch;   // guarded
std::mutex g_keep_mutex;   // (its own: OwnLink runs outside g_mutex)
Keep g_keep;               // guarded by g_keep_mutex

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Link's top above his feet by age (the name tags' heights, coop_hook.cpp).
constexpr float kHeightChild = 1.35f;
constexpr float kHeightAdult = 1.9f;

// Your own Link, or none; `showing` (when given) says which picture that makes. Live during our own event too
// (#74: the owner's adult Link got the flat picture, and the only flat causes are no Link or our event); a Link
// missing for a moment (a scene entry) is the last one seen (Keep).
std::optional<Subject> OwnLink(Showing* showing = nullptr) {
    thread_local CoopHook::MapView view;   // (keeps its room: no allocation once filled)
    std::optional<Seen> now;
    if (CoopHook::CurrentMapView(view)) {
        const Showing s = view.event ? Showing::live_event : view.adult ? Showing::live_adult : Showing::live_child;
        now = Seen{Subject{view.self, view.adult ? kHeightAdult : kHeightChild}, s};
    }
    std::optional<Seen> kept;
    {
        std::lock_guard lock(g_keep_mutex);
        kept = g_keep.Take(now, NowMs());
    }
    if (showing) {
        *showing = kept ? kept->showing : Showing::flat_no_link;
    }
    return kept ? std::optional<Subject>(kept->link) : std::nullopt;
}

// The game's vertical field of view now: from the camera block Hookshot hands the name tags (CoopCamera), else
// kFovY. Said in the log when first known and when it changes by more than two degrees. The game's thread.
double GameFov() {
    const auto frame = CoopCamera::Latest();
    const auto fov = frame ? FovFromClip(frame->clip_from_world) : std::nullopt;
    static double said = 0;   // (the game's thread only)
    const double now = fov.value_or(kFovY);
    if (std::abs(now - said) > 0.035) {
        said = now;
        LOG_INFO(Core, "coop: look: the Customize camera's field of view is {:.1f} degrees ({})", now * 57.29578,
                 fov ? "the game's camera" : "no camera from the game: the measured one");
    }
    return now;
}

}  // namespace

bool LinkShown() {
    Showing showing = Showing::flat_no_link;
    const bool shown = OwnLink(&showing).has_value();
    std::optional<std::string> line;
    {
        std::lock_guard lock(g_mutex);
        line = g_watch.Drawn(showing, NowMs());
    }
    if (line) {
        LOG_INFO(Core, "{}", *line);
    }
    return shown;
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
    const double fov = GameFov();
    std::lock_guard lock(g_mutex);
    const auto shot = g_orbit.Camera({eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]}, link, now, fov);
    if (!shot) {
        return false;
    }
    g_watch.Framed(now);
    for (int i = 0; i < 3; ++i) {
        eye[i] = shot->eye[i];
        target[i] = shot->target[i];
    }
    return true;
}

}  // namespace CoopLookView
