#include "coop_cheats.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>

#include "common/logging.h"
#include "coop/host/game_items.hpp"

namespace CoopCheats {
namespace {

std::mutex mutex_;
std::deque<std::string> queue_;
std::string last_ = "nothing given yet";

const coop::host::ItemFlag* Find(std::string_view name) {
    for (const auto& item : coop::host::item_flags()) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

// Gives one flag; returns what it did.
std::string GiveOne(coop::host::FlagStore& game, const coop::host::ItemFlag& item) {
    using coop::protocol::FlagKind;
    const coop::host::FlagKey key(item.hash, 0);
    const auto now = game.read(key);
    if (!now) {
        return std::string(item.name) + ": not readable (save not loaded?)";
    }
    std::uint32_t value = 1;
    if (item.kind == FlagKind::int32) {
        value = static_cast<std::uint32_t>(static_cast<std::int32_t>(now->value) + 1);
    }
    if (!game.write(coop::protocol::FlagEntry(key, item.kind, value))) {
        return std::string(item.name) + ": write failed";
    }
    return std::string(item.name) + " " + std::to_string(static_cast<std::int32_t>(now->value)) +
           " -> " + std::to_string(static_cast<std::int32_t>(value));
}

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
        const auto* item = Find(name);
        std::string result;
        if (!item) {
            result = name + ": not an item flag";
        } else {
            result = GiveOne(game, *item);
            // A pouch item also gets its IsGet flag, as a pickup sets both.
            constexpr std::string_view pouch = "PouchData.", stock = ".StockNum";
            if (name.starts_with(pouch) && name.ends_with(stock)) {
                const auto base =
                    name.substr(pouch.size(), name.size() - pouch.size() - stock.size());
                if (const auto* is_get = Find("IsGet." + base);
                    is_get && is_get->kind == coop::protocol::FlagKind::boolean) {
                    result += "; " + GiveOne(game, *is_get);
                }
            }
        }
        LOG_INFO(Core, "coop: cheat: {}", result);
        std::lock_guard lock(mutex_);
        last_ = result;
    }
}

} // namespace CoopCheats
