// SPDX-License-Identifier: GPL-3.0-or-later
// Readable names for Character Select's unnamed characters (menu-customize-01113, audit item 14).
//
// OoT Character Select (snaildev99, MIT) names its game characters (Saria, Rauru, ...) but gives the game's unnamed
// townsfolk their model ids ("Gerudo_GEN_002", "Hylia_MN_005", "Kokiri_WC_001") in his characters.json and his
// picker. Co-op's Settings rows "Character by age" list them by that name (coop_pause.cpp FeedCharacterAges), and the
// kit's merge renames them in his picker (tools/puppet_pack/charsel_names.py). This is the same rule as that file,
// so a character has one name everywhere; tests/hookshot/charsel_names_test.cpp checks the names both give.
//
// A name <Race>_<CODE>_<digits> (an optional _<Word> after it) is a model id; any other name is his and stays.
//   Hylia M.. -> Townsman, W.. -> Townswoman, MO -> Old man, WO -> Old woman, MS -> Soldier
//   Kokiri MC -> Kokiri boy, WC -> Kokiri girl
//   any other race -> the race's word (Hylia -> Hylian); a trailing word -> "<kind> <word in lower case>"
// A kind with more than one character is numbered 1, 2, ... by its model ids in order (the code, the value of the
// digits, the word), never by where they stand in a list: his picker and his characters.json are two lists
// (review-01112-h), and a release ordering one of them otherwise still names each character alike in both. A kind of
// one has no number.
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace CoopCharselNames {

namespace detail {
inline bool Upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool Letter(char c) { return Upper(c) || (c >= 'a' && c <= 'z'); }
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
// The run of characters from `at` for which `ok` holds; its end.
template <class F>
std::size_t Run(const std::string& s, std::size_t at, F ok) {
    while (at < s.size() && ok(s[at])) ++at;
    return at;
}

// A model id's parts (the word as written; "" none).
struct Id {
    std::string race, code, word;
    unsigned long long number = 0;
};
inline std::optional<Id> Parse(const std::string& name) {
    const std::size_t race_end = Run(name, 0, Letter);
    if (race_end == 0 || race_end >= name.size() || name[race_end] != '_') return std::nullopt;
    const std::size_t code_end = Run(name, race_end + 1, Upper);
    if (code_end == race_end + 1 || code_end >= name.size() || name[code_end] != '_') return std::nullopt;
    const std::size_t digits_end = Run(name, code_end + 1, Digit);
    if (digits_end == code_end + 1 || digits_end - code_end - 1 > 18) return std::nullopt;
    Id id;
    if (digits_end < name.size()) {
        if (name[digits_end] != '_') return std::nullopt;
        const std::size_t word_end = Run(name, digits_end + 1, Letter);
        if (word_end == digits_end + 1 || word_end != name.size()) return std::nullopt;
        id.word = name.substr(digits_end + 1);
    }
    id.race = name.substr(0, race_end);
    id.code = name.substr(race_end + 1, code_end - race_end - 1);
    id.number = std::stoull(name.substr(code_end + 1, digits_end - code_end - 1));
    return id;
}

inline std::string Kind(const Id& id) {
    const bool man = id.code[0] == 'M', woman = id.code[0] == 'W';
    std::string kind;
    if (id.race == "Hylia" && (man || woman)) {
        if (id.code.size() > 1 && id.code[1] == 'O') {
            kind = man ? "Old man" : "Old woman";
        } else if (id.code == "MS") {
            kind = "Soldier";
        } else {
            kind = man ? "Townsman" : "Townswoman";
        }
    } else if (id.race == "Kokiri" && (id.code == "MC" || id.code == "WC")) {
        kind = man ? "Kokiri boy" : "Kokiri girl";
    } else {
        kind = id.race == "Hylia" ? "Hylian" : id.race;
    }
    if (id.word.empty()) return kind;
    std::string word = id.word;
    for (char& c : word) {
        if (Upper(c)) c = static_cast<char>(c - 'A' + 'a');
    }
    return kind + " " + word;
}
}  // namespace detail

// The kind (the readable name before its number) of a model id, or nullopt for a name of his own.
inline std::optional<std::string> KindOf(const std::string& name) {
    const auto id = detail::Parse(name);
    if (!id) return std::nullopt;
    return detail::Kind(*id);
}

// Each name of the list (in its order) as shown: model ids as their kind (numbered by their ids' order when the kind
// has more than one, whatever the list's order), the others unchanged.
inline std::vector<std::string> Readable(const std::vector<std::string>& names) {
    std::vector<std::optional<detail::Id>> ids;
    std::map<std::string, std::vector<std::size_t>> of_kind;
    for (std::size_t i = 0; i < names.size(); ++i) {
        ids.push_back(detail::Parse(names[i]));
        if (ids.back()) of_kind[detail::Kind(*ids.back())].push_back(i);
    }
    std::vector<std::string> out = names;
    for (auto& [kind, members] : of_kind) {
        std::stable_sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
            return std::tie(ids[a]->code, ids[a]->number, ids[a]->word) <
                   std::tie(ids[b]->code, ids[b]->number, ids[b]->word);
        });
        for (std::size_t rank = 0; rank < members.size(); ++rank) {
            out[members[rank]] = members.size() == 1 ? kind : kind + " " + std::to_string(rank + 1);
        }
    }
    return out;
}

}  // namespace CoopCharselNames
