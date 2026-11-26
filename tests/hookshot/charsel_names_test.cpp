// SPDX-License-Identifier: GPL-3.0-or-later
// Readable names for Character Select's unnamed characters (hookshot/src/coop_charsel_names.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "coop_charsel_names.h"

using CoopCharselNames::KindOf;
using CoopCharselNames::Readable;

TEST_CASE("charsel names: his own names stay, model ids get their kind") {
    CHECK_FALSE(KindOf("Saria").has_value());
    CHECK_FALSE(KindOf("Zelda (child)").has_value());
    CHECK_FALSE(KindOf("Iron Knuckle (Nabooru, cutscene)").has_value());
    CHECK_FALSE(KindOf("Hylia_mn_001").has_value());   // (a code is capitals)
    CHECK_FALSE(KindOf("Hylia_MN_").has_value());
    CHECK_FALSE(KindOf("Hylia_MN_001_").has_value());
    CHECK_FALSE(KindOf("_MN_001").has_value());
    CHECK(KindOf("Hylia_MN_005") == "Townsman");
    CHECK(KindOf("Hylia_WF_002") == "Townswoman");
    CHECK(KindOf("Hylia_MO_001") == "Old man");
    CHECK(KindOf("Hylia_WO_001") == "Old woman");
    CHECK(KindOf("Hylia_MS_001") == "Soldier");
    CHECK(KindOf("Kokiri_MC_003") == "Kokiri boy");
    CHECK(KindOf("Kokiri_WC_001") == "Kokiri girl");
    CHECK(KindOf("Gerudo_GEN_011_Leader") == "Gerudo leader");
    CHECK(KindOf("Hylia_ZZ_001") == "Hylian");   // (a code not known: the race's word)
    CHECK(KindOf("Sheikah_SN_001") == "Sheikah");
}

TEST_CASE("charsel names: a kind of more than one is numbered by its ids' order, a kind of one is not") {
    CHECK(Readable({"Goron_GON_001", "Saria", "Goron_GON_006", "Zora_ZN_001", "Goron_GON_002"}) ==
          std::vector<std::string>{"Goron 1", "Saria", "Goron 3", "Zora", "Goron 2"});
    // (one kind over several codes: the code first, then the number's value, then the word)
    CHECK(Readable({"Hylia_MN_010", "Hylia_MF_002", "Hylia_MN_9", "Gerudo_GEN_011_Leader", "Gerudo_GEN_011_Chief",
                    "Gerudo_GEN_011_Leader"}) ==
          std::vector<std::string>{"Townsman 3", "Townsman 1", "Townsman 2", "Gerudo leader 1", "Gerudo chief",
                                   "Gerudo leader 2"});
    CHECK(Readable({}).empty());
    CHECK_FALSE(KindOf("Hylia_MN_1234567890123456789").has_value());   // (19 digits: not an id, as the Python side)
}

TEST_CASE("charsel names: a list in another order names each character alike (review-01112-h: picker vs json)") {
    std::vector<std::string> names{"Goron_GON_001", "Goron_GON_006", "Goron_GON_002", "Hylia_WO_001",
                                   "Hylia_MN_014", "Hylia_MF_003", "Saria", "Hylia_MN_003", "Kokiri_WC_002",
                                   "Kokiri_WC_001"};
    const auto shown = Readable(names);
    std::map<std::string, std::string> by_id;
    for (std::size_t i = 0; i < names.size(); ++i) by_id[names[i]] = shown[i];
    std::vector<std::string> other(names.rbegin(), names.rend());
    std::rotate(other.begin(), other.begin() + 3, other.end());
    const auto again = Readable(other);
    for (std::size_t i = 0; i < other.size(); ++i) CHECK(again[i] == by_id[other[i]]);
}

TEST_CASE("charsel names: his v0.2.2 list gives the names the kit's merge puts in his picker") {
    // (tests/hookshot/charsel-names.txt: tools/puppet_pack/tests/test_charsel_names.py checks the Python side gives
    // the same, so the Settings rows and his picker name each character alike)
    std::ifstream in(std::string(COOP_TEST_SOURCE_DIR) + "/hookshot/charsel-names.txt", std::ios::binary);
    REQUIRE(in);
    std::vector<std::string> his, want;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        REQUIRE(tab != std::string::npos);
        his.push_back(line.substr(0, tab));
        want.push_back(line.substr(tab + 1));
    }
    REQUIRE(his.size() == 117);
    CHECK(Readable(his) == want);
}
