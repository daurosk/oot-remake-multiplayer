// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_puppet_config.h"

#include <utility>

namespace CoopPuppetConfig {
namespace {

bool On(const std::string& v) {
    return !v.empty() && v != "0";
}

}  // namespace

CoopPuppet::PlannerSettings Settings(const EnvFn& env, const std::function<bool()>& pack_green,
                                     std::vector<std::string>* log, const PackTunicsFn& pack_tunics,
                                     const PackLookFn& pack_look) {
    CoopPuppet::PlannerSettings settings;
    // The test puppet has its own switch: the logs alone must not put a
    // second Link in the game (Alex's run of 2026-11-09 with the logs on).
    settings.debug = On(env("LICHII_COOP_TEST_PUPPET"));
    settings.debug_anim = On(env("LICHII_COOP_DEBUG_ANIM"));
    if (auto actor = env("LICHII_COOP_PUPPET_ACTOR"); !actor.empty()) {
        settings.actor = std::move(actor);
    }
    settings.native_player = On(env("LICHII_COOP_NATIVE_PLAYER"));
    bool pack_plain = false;
    if (const auto tunics = env("LICHII_COOP_TUNICS"); tunics == "0" || tunics == "off") {
        settings.tunics = false;
    } else if (pack_tunics) {
        // A pack without the tunic puppets: the game refuses CoopPuppet_<Age>_<Colour>
        // (not in its ActorInfo), so every puppet is the plain one it has.
        if (const auto has = pack_tunics(); has && !*has) {
            settings.tunics = false;
            pack_plain = true;
            if (log != nullptr) {
                log->push_back("coop: the puppet pack has no tunic colours (made before them): every puppet is "
                               "the plain one; run make-puppet-pack again for the colours");
            }
        }
    }
    // The kit's tools/release/kit/scripts/kit-paths.ps1 Set-KitSelfTunic. Only
    // with a pack that has the green puppets (a hand-set switch and an older
    // pack must not leave green players without a puppet).
    if (const auto self = env("LICHII_COOP_SELF_TUNIC"); !self.empty() && self != "green") {
        settings.green_puppet = pack_green();
        if (log != nullptr) {
            log->push_back("coop: your Link wears " + self + "; green players " +
                           (settings.green_puppet
                                ? "get CoopPuppet_<Age>_Green"
                                : "get the plain puppet (the pack has no green puppet; make it again)"));
        }
    }
    if (const auto items = env("LICHII_COOP_LOOK_ITEMS"); pack_look && items != "0" && items != "off") {
        auto look = pack_look();
        settings.look_items = std::move(look.actors);
        settings.look_slots = settings.look_items.empty() ? 0 : look.slots;
        if (log != nullptr && settings.look_slots > 0) {
            log->push_back("coop: the puppet pack has " + std::to_string(settings.look_items.size()) +
                           " coloured items for " + std::to_string(settings.look_slots) +
                           " players (gear colours on the puppets)");
        }
    }
    if (log != nullptr) {
        log->push_back(std::string("coop: puppets on, actor ") +
                       (settings.actor.empty() ? "by age" : settings.actor) +
                       (settings.tunics  ? ""
                        : pack_plain     ? ", tunics off (the pack has none)"
                                         : ", tunics off (LICHII_COOP_TUNICS=0)") +
                       (settings.native_player && settings.actor.empty()
                            ? ", NATIVE PLAYER TEST (LICHII_COOP_NATIVE_PLAYER): the game's own "
                              "Player_<Age>, left alone"
                            : "") +
                       (settings.debug ? ", test puppet beside you" : ""));
    }
    return settings;
}

}  // namespace CoopPuppetConfig

#ifdef LICHII_COOP

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "common/fs/path_util.h"
#include "common/logging.h"

namespace CoopPuppetConfig {
namespace {

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

// Every mod folder's coop_puppet.json under LoadDir/<title id>, as
// coop_spawn's PackStandIns looks. LoadDir is the emulator's load/ or
// Hookshot's mods\contents (hookshot/compat/common/fs/path_util.h).
std::vector<std::string> PackManifests() {
    std::vector<std::string> out;
    std::error_code ec;
    const auto title = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400486024DEC000";
    for (const auto& mod : std::filesystem::directory_iterator(title, ec)) {
        std::ifstream in(mod.path() / "coop_puppet.json", std::ios::binary);
        if (!in) {
            continue;
        }
        out.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return out;
}

bool PackGreenPuppets() {
    for (const auto& text : PackManifests()) {
        if (CoopPuppet::PackHasGreenPuppets(text)) {
            return true;
        }
    }
    return false;
}

// Any manifest with tunic puppets; std::nullopt with no manifest at all.
std::optional<bool> PackTunicPuppets() {
    const auto manifests = PackManifests();
    if (manifests.empty()) {
        return std::nullopt;
    }
    for (const auto& text : manifests) {
        if (CoopPuppet::PackHasTunicPuppets(text)) {
            return true;
        }
    }
    return false;
}

// The first manifest with item copies.
CoopPuppet::LookItems PackLook() {
    for (const auto& text : PackManifests()) {
        if (auto items = CoopPuppet::PackLookItems(text); items.slots > 0) {
            return items;
        }
    }
    return {};
}

}  // namespace

void Configure() {
    std::vector<std::string> lines;
    auto settings = Settings(Env, PackGreenPuppets, &lines, PackTunicPuppets, PackLook);
    for (const auto& line : lines) {
        LOG_INFO(Core, "{}", line);
    }
    const auto verbose = Env("LICHII_COOP_DEBUG_PUPPET");
    CoopPuppet::Configure(std::move(settings), On(verbose),
                          [](const std::string& line) { LOG_INFO(Core, "{}", line); });
}

}  // namespace CoopPuppetConfig

#endif  // LICHII_COOP
