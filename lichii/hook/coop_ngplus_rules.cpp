// SPDX-License-Identifier: GPL-3.0-or-later
//
// (Navi) New Game+'s rules (coop_ngplus.h), with no game code, so the unit tests build them on their own.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "coop/protocol/difficulty.hpp"
#include "coop_ngplus.h"

namespace CoopNgPlus {
namespace {

namespace proto = coop::protocol;

int IntOf(std::string_view v) {
    int n = 0;
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    std::from_chars(v.data(), v.data() + v.size(), n);
    return n;
}


}  // namespace

Saved Parse(std::string_view text) {
    Saved s;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const auto eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        std::string_view key = line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
        const int v = IntOf(line.substr(eq + 1));
        if (key == "level") s.level = static_cast<std::uint8_t>(std::clamp(v, 0, int{proto::kMaxNgPlus}));
        if (key == "unlocked") s.unlocked = static_cast<std::uint8_t>(std::clamp(v, 0, int{proto::kMaxNgPlus}));
        if (key == "hearts") s.hearts = std::clamp(v, 0, kMostQuarters);
    }
    s.level = std::min(s.level, s.unlocked);
    return s;
}

std::string Format(const Saved& s) {
    return "# New Game+ (Navi): the level chosen (0 off), the highest unlocked by beating Ganon, the hearts carried\n"
           "# (quarter hearts). Chosen on the Multiplayer tab's Settings page.\n"
           "level = " + std::to_string(s.level) + "\nunlocked = " + std::to_string(s.unlocked) +
           "\nhearts = " + std::to_string(s.hearts) + "\n";
}

Saved Beaten(Saved s, std::int32_t max_life) {
    const int next = std::min(int{s.level} + 1, int{proto::kMaxNgPlus});
    s.unlocked = static_cast<std::uint8_t>(std::max(int{s.unlocked}, next));
    if (max_life > 0) {
        s.hearts = std::max(s.hearts, std::min(max_life, kMostQuarters));
    }
    return s;
}

Saved Choose(Saved s, int level) {
    s.level = static_cast<std::uint8_t>(std::clamp(level, 0, int{s.unlocked}));
    return s;
}

bool IsGanonDeath(std::string_view event) {
    // Ganon's death setting and flow (coop_boss_scenes.cpp: "Ganon"), and the ending's flow.
    return event == "InsideGanonCastle_Dungeon_BossDie_2" || event == "Dm_FinalBattle_BossOut" ||
           event == "Defeat_Ganon";
}

bool IsNewFileOpening(std::string_view event) {
    // The opening of a new game (docs/flags.md, "Quests that follow the story": Dm_Kokiri_0000 to _0030). Only a new
    // file plays its first part; a loaded save starts after it.
    // The opening's settings by name too (kOpeningPlayed), should a game ask for one: the flag is the main way.
    return event == "Dm_Kokiri_0000" || event == "Opening_LoadingKokiriForest" || event == "Opening_Introduction";
}

bool Rose(Played& p, std::optional<bool> now) {
    if (!now) {
        return false;
    }
    const bool rose = p.known && !p.was && *now;
    p.known = true;
    p.was = *now;
    return rose;
}

bool NewFileNow(const FileMark& m) {
    if (!m.native_loads) {
        return false;
    }
    bool now = m.was && !m.title && !m.load_started;
    if (m.opening) {
        now = true;   // (its load came first: a load and the opening in one frame are a new file)
    }
    return now;
}

std::optional<std::int32_t> HeartsToGive(const Give& g) {
    if (g.level == 0 || g.hearts <= 0 || !g.may_give || !g.new_file || g.link_made || !g.max_life ||
        *g.max_life <= 0) {
        return std::nullopt;
    }
    const std::int32_t to = std::min(g.hearts, kMostQuarters);
    if (*g.max_life >= to) {
        return std::nullopt;  // never lowered, and nothing to do once it holds
    }
    return to;
}

}  // namespace CoopNgPlus
