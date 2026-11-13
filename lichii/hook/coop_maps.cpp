// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_maps.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"

namespace CoopMaps {
namespace {

namespace map = coop::overlay::map;

// coop_maps.txt in each mod folder under load/<title id> (as coop_rooms.txt, CoopFairy's LoadRooms).
map::Table Load() {
    std::vector<map::Capture> all;
    int bad_all = 0;
    std::error_code ec;
    const auto title = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400486024DEC000";
    for (const auto& mod : std::filesystem::directory_iterator(title, ec)) {
        std::ifstream in(mod.path() / "coop_maps.txt", std::ios::binary);
        if (!in) {
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int bad = 0;
        auto more = map::parse_captures(text, &bad);
        bad_all += bad;
        all.insert(all.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    map::Table table(std::move(all));
    if (table.empty()) {
        LOG_INFO(Core, "coop: no map table, make the puppet pack again (no players on the minimap or the pause map)");
    } else {
        LOG_INFO(Core, "coop: map table: {} maps{}", table.size(),
                 bad_all ? ", " + std::to_string(bad_all) + " bad lines skipped" : std::string());
    }
    return table;
}

}  // namespace

const map::Table& Maps() {
    static const map::Table table = Load();
    return table;
}

}  // namespace CoopMaps
