#include "coop_cheats.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>

#include "common/logging.h"
#include "coop/host/game_items.hpp"
#include "coop_cheat_give.h"

namespace CoopCheats {
namespace {

std::mutex mutex_;
std::deque<std::string> queue_;
std::string last_ = "nothing given yet";

} // namespace

bool On() {
    const char* value = std::getenv("LICHII_COOP_CHEATS");
    return value && *value && std::string_view(value) != "0";
}

std::vector<std::string> Items() {
    std::vector<std::string> names;
    for (const auto& item : coop::host::item_flags()) {
        if (item.on && item.rule != coop::host::ItemRule::per_player) {
            names.emplace_back(item.name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

void Give(const std::string& name) {
    std::lock_guard lock(mutex_);
    queue_.push_back(name);
}

std::string LastResult() {
    std::lock_guard lock(mutex_);
    return last_;
}

void Frame(coop::host::FlagStore& game) {
    std::deque<std::string> todo;
    {
        std::lock_guard lock(mutex_);
        todo.swap(queue_);
    }
    for (const auto& name : todo) {
        const std::string result = CoopCheatGive::Give(game, name).text;
        LOG_INFO(Core, "coop: cheat: {}", result);
        std::lock_guard lock(mutex_);
        last_ = result;
    }
}

} // namespace CoopCheats
