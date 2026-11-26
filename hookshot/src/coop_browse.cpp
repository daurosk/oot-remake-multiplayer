// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_browse.h"

#include <algorithm>
#include <cctype>

namespace CoopBrowse {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const char* FilterWords(int filter) {
    switch (filter) {
    case kOpen: return "Open games";
    case kSameVersion: return "This version";
    default: return "All games";
    }
}

const char* SortWords(int sort) {
    switch (sort) {
    case kFewestPlayers: return "Fewest players";
    case kByName: return "Name";
    default: return "Most players";
    }
}

bool Joinable(const Entry& e) {
    return e.same_version && (e.max <= 0 || e.players < e.max);
}

std::vector<int> Order(const std::vector<Entry>& all, const std::string& search, int filter, int sort) {
    const std::string want = Lower(search);
    std::vector<int> kept;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Entry& e = all[i];
        if (!want.empty() && Lower(e.name).find(want) == std::string::npos &&
            Lower(e.host).find(want) == std::string::npos) {
            continue;
        }
        if (filter == kOpen && !Joinable(e)) continue;
        if (filter == kSameVersion && !e.same_version) continue;
        kept.push_back(static_cast<int>(i));
    }
    const auto name_less = [&](int a, int b) {
        return Lower(all[static_cast<std::size_t>(a)].name) < Lower(all[static_cast<std::size_t>(b)].name);
    };
    std::stable_sort(kept.begin(), kept.end(), [&](int a, int b) {
        const Entry& x = all[static_cast<std::size_t>(a)];
        const Entry& y = all[static_cast<std::size_t>(b)];
        if (sort == kMostPlayers && x.players != y.players) return x.players > y.players;
        if (sort == kFewestPlayers && x.players != y.players) return x.players < y.players;
        return name_less(a, b);
    });
    return kept;
}

}  // namespace CoopBrowse
