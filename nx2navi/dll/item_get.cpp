// SPDX-License-Identifier: GPL-3.0-or-later
#include "item_get.hpp"

#include <cmath>
#include <string_view>

#include "coop/host/game_items.hpp"
#include "coop/host/notify.hpp"

namespace CoopNx2navi::ItemGet {

std::string ActorFor(const std::string& line) {
    if (line.find(" got the map (") != std::string::npos) {
        return "Item_Dungeon_Map";
    }
    if (line.find(" got the compass (") != std::string::npos) {
        return "Item_Dungeon_Compass";
    }
    if (line.find(" got the Boss Key (") != std::string::npos) {
        return "Item_Dungeon_BossKey";
    }
    if (line.find(" found a Small Key (") != std::string::npos) {
        return "Item_Dungeon_SmallKey";
    }
    const std::string_view mark = " got the ";
    const std::string_view tail = " - you got one too!";
    const auto at = line.find(mark);
    const auto end = line.rfind(tail);
    if (at == std::string::npos || end == std::string::npos || end < at + mark.size()) {
        return {};
    }
    const std::string shown = line.substr(at + mark.size(), end - at - mark.size());
    for (const auto& item : coop::host::item_flags()) {
        const std::string_view name = item.name;
        if (!name.starts_with("IsGet.") || name.starts_with("IsGet.Item_Melody_")) {
            continue;
        }
        const std::string_view id = name.substr(6);
        if (coop::host::ItemName(id) == shown) {
            return std::string(id);
        }
    }
    return {};
}

std::string IconPath(const std::string& actor) {
    // romfs UI/Tex/Icon holds one picture per item actor (129 of them; none for the medallions, the stones
    // and the songs, which the game's menus draw as 3D actors).
    if (actor.empty() || actor.find("Medal") != std::string::npos || actor.find("Stone") != std::string::npos ||
        actor.find("Melody") != std::string::npos) {
        return {};
    }
    return "romfs:UI/Tex/Icon/" + actor + ".bntx.zs#" + actor;
}

int FanfareType(const std::string& actor) {
    return actor.find("SmallKey") != std::string::npos || actor.find("Rupee") != std::string::npos ? 0 : 2;
}

void Queue::Offer(const std::string& line, std::uint64_t now_us) {
    if (wants_.size() >= 8) {
        return;
    }
    wants_.push_back({line, ActorFor(line), now_us});
}

std::optional<Queue::Do> Queue::Frame(const Look& look, std::uint64_t now_us) {
    if (!look.in_game) {
        readable_since_ = 0;
        still_since_ = 0;
        return std::nullopt;
    }
    if (readable_since_ == 0) {
        readable_since_ = now_us;
    }
    if (still_since_ == 0 || std::fabs(look.height - still_height_) > kStillM) {
        still_since_ = now_us;
        still_height_ = look.height;
    }
    if (look.life >= 0 && last_life_ >= 0 && look.life < last_life_) {
        hurt_us_ = now_us;
    }
    if (look.life >= 0) {
        last_life_ = look.life;
    }
    if (busy_) {
        return std::nullopt;
    }
    if (failed_) {
        Do d{*failed_, false};
        failed_.reset();
        return d;
    }
    if (wants_.empty() || (done_us_ != 0 && now_us - done_us_ < kApartUs)) {
        return std::nullopt;
    }
    const Want& w = wants_.front();
    const bool safe = !look.event && !look.menu && !look.boss_fight && !look.travelling && now_us - readable_since_ >= kSafeAfterUs &&
                      now_us - still_since_ >= kStillUs && (hurt_us_ == 0 || now_us - hurt_us_ >= kHurtQuietUs);
    if (!w.actor.empty() && safe) {
        Do d{w, true};
        current_ = w;
        wants_.pop_front();
        busy_ = true;
        return d;
    }
    if (w.actor.empty() || look.boss_fight || now_us - w.at_us >= kWaitUs) {
        if (look.event || look.menu) {
            return std::nullopt;  // the fanfare waits for the end of a cutscene or menu too
        }
        Do d{w, false};
        wants_.pop_front();
        done_us_ = now_us;
        return d;
    }
    return std::nullopt;
}

void Queue::Done(bool played, std::uint64_t now_us) {
    busy_ = false;
    done_us_ = now_us;
    if (!played && current_) {
        failed_ = current_;  // its fanfare and line instead
    }
    current_.reset();
}

} // namespace CoopNx2navi::ItemGet
