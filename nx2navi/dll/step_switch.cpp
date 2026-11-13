// SPDX-License-Identifier: GPL-3.0-or-later
#include "step_switch.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace CoopNx2navi::StepSwitch {
namespace {
std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}
} // namespace

std::vector<std::uint8_t> Encode(const Note& n) {
    std::vector<std::uint8_t> out;
    out.push_back(n.pressed ? 1 : 0);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(n.hash >> (8 * i)));
    }
    const std::size_t room = 96 - out.size();
    out.insert(out.end(), n.scene.begin(), n.scene.begin() + static_cast<std::ptrdiff_t>(std::min(room, n.scene.size())));
    return out;
}

std::optional<Note> Decode(const std::vector<std::uint8_t>& data) {
    if (data.size() < 10 || data[0] > 1) {
        return std::nullopt;
    }
    Note n;
    n.pressed = data[0] == 1;
    for (int i = 0; i < 8; ++i) {
        n.hash |= static_cast<std::uint64_t>(data[1 + i]) << (8 * i);
    }
    n.scene.assign(data.begin() + 9, data.end());
    if (n.hash == 0 || n.hash == ~std::uint64_t{0}) {
        return std::nullopt;
    }
    return n;
}

void Switches::SetScene(const std::string& scene) {
    std::lock_guard lock(mutex_);
    if (scene == scene_) {
        return;
    }
    scene_ = scene;
    ours_.clear();
    remote_.clear();
    forced_us_.clear();
    forced_said_.clear();
}

bool Switches::Force(std::uint64_t hash, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    const auto r = remote_.find(hash);
    if (r == remote_.end()) {
        return false;
    }
    bool any = false;
    for (const auto& [player, until] : r->second) {
        any = any || now_us < until;
    }
    if (any) {
        forced_us_[hash] = now_us;
        if (forced_said_.insert(hash).second) {
            lines_.push_back("switch " + Hex(hash) + ": held down here for another player");
        }
    } else if (forced_said_.erase(hash) > 0) {
        lines_.push_back("switch " + Hex(hash) + ": nobody else holds it any more: left to this game");
    }
    return any;
}

void Switches::Seen(std::uint64_t hash, bool on, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (const auto f = forced_us_.find(hash); f != forced_us_.end() && now_us - f->second < kForcedQuietUs) {
        return;  // down because we held it for another player: not our press (no echo)
    }
    Ours& o = ours_[hash];
    if (o.on != on || !o.said) {
        if (o.on != on || on) {
            o.changed = true;
        }
        if (o.on != on) {
            lines_.push_back("switch " + Hex(hash) + (on ? ": pressed here" : ": up again here"));
        }
        o.on = on;
    }
}

void Switches::Remote(std::uint8_t player, const Note& n, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (n.scene != scene_ || player == 0) {
        return;
    }
    auto& who = remote_[n.hash];
    const bool before = who.contains(player) && now_us < who[player];
    if (n.pressed) {
        who[player] = now_us + kHoldUs;
    } else {
        who.erase(player);
    }
    if (before != n.pressed) {
        lines_.push_back("switch " + Hex(n.hash) + ": player " + std::to_string(player) +
                         (n.pressed ? " stands on it (pressed in their game)" : " left it (up in their game)"));
    }
}

std::vector<Note> Switches::TakeSends(std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    std::vector<Note> out;
    for (auto& [hash, o] : ours_) {
        if (o.changed || (o.on && now_us - o.sent_us >= kAgainUs)) {
            out.push_back({scene_, hash, o.on});
            o.changed = false;
            o.said = true;
            o.sent_us = now_us;
        }
    }
    return out;
}

std::vector<std::string> Switches::TakeLines() {
    std::lock_guard lock(mutex_);
    return std::exchange(lines_, {});
}

std::size_t Switches::Forced() const {
    std::lock_guard lock(mutex_);
    return forced_said_.size();
}

} // namespace CoopNx2navi::StepSwitch
