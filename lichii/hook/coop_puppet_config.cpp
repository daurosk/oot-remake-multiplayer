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
                                     std::vector<std::string>* log) {
    CoopPuppet::PlannerSettings settings;
    // The test puppet has its own switch: the logs alone must not put a
    // second Link in the game (Caleb's run of 2026-11-09 with the logs on).
    settings.debug = On(env("LICHII_COOP_TEST_PUPPET"));
    settings.debug_anim = On(env("LICHII_COOP_DEBUG_ANIM"));
    if (auto actor = env("LICHII_COOP_PUPPET_ACTOR"); !actor.empty()) {
        settings.actor = std::move(actor);
    }
    if (const auto tunics = env("LICHII_COOP_TUNICS"); tunics == "0" || tunics == "off") {
        settings.tunics = false;
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
    if (log != nullptr) {
        log->push_back(std::string("coop: puppets on, actor ") +
                       (settings.actor.empty() ? "by age" : settings.actor) +
                       (settings.tunics ? "" : ", tunics off (LICHII_COOP_TUNICS=0)") +
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

// Any mod folder's coop_puppet.json under LoadDir/<title id> with both green
// puppets (CoopPuppet::PackHasGreenPuppets), as coop_spawn's PackStandIns
// looks. LoadDir is the emulator's load/ or Hookshot's mods\contents
// (hookshot/compat/common/fs/path_util.h).
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

}  // namespace

void Configure() {
    std::vector<std::string> lines;
    auto settings = Settings(Env, PackGreenPuppets, &lines);
    for (const auto& line : lines) {
        LOG_INFO(Core, "{}", line);
    }
    const auto verbose = Env("LICHII_COOP_DEBUG_PUPPET");
    CoopPuppet::Configure(std::move(settings), On(verbose),
                          [](const std::string& line) { LOG_INFO(Core, "{}", line); });
}

}  // namespace CoopPuppetConfig

#endif  // LICHII_COOP
