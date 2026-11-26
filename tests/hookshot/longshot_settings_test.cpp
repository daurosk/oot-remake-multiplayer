// SPDX-License-Identifier: GPL-3.0-or-later
// Longshot's settings for the module (hookshot/src/coop_longshot_settings.*, patch 0032), through a made-up game table.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "coop_longshot_settings.h"

namespace {

HosCoopSetting Row(const char* key, const char* name, const char* text, const char* tab, int value, int lo, int hi,
                   int restart = 0) {
    HosCoopSetting s{};
    std::strncpy(s.key, key, sizeof s.key);
    std::strncpy(s.name, name, sizeof s.name);
    std::strncpy(s.text, text, sizeof s.text);
    std::strncpy(s.tab, tab, sizeof s.tab);
    s.value = value, s.lo = lo, s.hi = hi, s.step = 1, s.restart = restart;
    return s;
}

HosCoopSetting g_rows[5];
std::string g_set_key;
int g_set_value = 0;

int FakeSetting(int index, HosCoopSetting* out) {
    if (index < 0 || index >= 5) return 0;
    *out = g_rows[index];
    return 1;
}

int FakeSettingSet(const char* key, int value, HosCoopSetting* out) {
    for (HosCoopSetting& r : g_rows) {
        if (std::strncmp(r.key, key, sizeof r.key) == 0) {
            g_set_key = key, g_set_value = value;
            if (out != nullptr) *out = r;   // (-1, -2: the row as it is, as host/control.h says)
            if (r.fixed) return -1;
            if (r.step > 1 && value % r.step != 0) return -2;   // (a list row whose steps never reach value)
            r.value = value < r.lo ? r.lo : value > r.hi ? r.hi : value;   // (clamped, as the menu does)
            if (out != nullptr) *out = r;
            return 1;
        }
    }
    return 0;
}

HosCoopApi FakeApi() {
    g_rows[0] = Row("volume", "Volume", "80", "Audio", 80, 0, 100);
    g_rows[1] = Row("fullscreen", "Fullscreen", "On", "Graphics", 1, 0, 1);
    g_rows[2] = Row("profile", "Profile", "Balanced", "Graphics", 1, 0, 3, 1);
    g_rows[3] = Row("vsync", "VSync", "On", "Graphics", 1, 0, 1);
    g_rows[3].fixed = 1;   // (nothing to choose on this PC)
    g_rows[4] = Row("msaa", "MSAA", "4x", "Graphics", 4, 0, 8);
    g_rows[4].step = 4;
    HosCoopApi api{};
    api.size = sizeof api;
    api.setting = FakeSetting;
    api.setting_set = FakeSettingSet;
    return api;
}

}  // namespace

TEST_CASE("settings: the rows listed until the first index that gives none, their fields kept") {
    const HosCoopApi api = FakeApi();
    CoopLongshotSettings::Init(&api);
    REQUIRE(CoopLongshotSettings::Available());
    const auto all = CoopLongshotSettings::All();
    REQUIRE(all.size() == 5);
    CHECK(all[0].key == "volume");
    CHECK(all[0].name == "Volume");
    CHECK(all[0].text == "80");
    CHECK(all[0].tab == "Audio");
    CHECK(all[0].hi == 100);
    CHECK_FALSE(all[0].restart);
    CHECK(all[2].restart);
    CHECK(all[3].fixed);
    CHECK_FALSE(CoopLongshotSettings::At(5).has_value());
    CHECK_FALSE(CoopLongshotSettings::At(-1).has_value());
    CoopLongshotSettings::Init(nullptr);
}

TEST_CASE("settings: a key set to a value gives the row as it is after (clamped by the game), none for no such key") {
    const HosCoopApi api = FakeApi();
    CoopLongshotSettings::Init(&api);
    const auto v = CoopLongshotSettings::Set("volume", 120);
    REQUIRE(v.has_value());
    CHECK(g_set_key == "volume");
    CHECK(g_set_value == 120);
    CHECK(v->value == 100);
    CHECK_FALSE(CoopLongshotSettings::Set("nope", 1).has_value());
    CHECK_FALSE(CoopLongshotSettings::Set("", 1).has_value());
    CoopLongshotSettings::Init(nullptr);
}

TEST_CASE("settings: only the game's 1 is set: a fixed row (-1) or a value no step reaches (-2) is not") {
    const HosCoopApi api = FakeApi();
    CoopLongshotSettings::Init(&api);
    int code = 99;
    REQUIRE(CoopLongshotSettings::Set("msaa", 8, &code).has_value());
    CHECK(code == CoopLongshotSettings::kSet);
    CHECK_FALSE(CoopLongshotSettings::Set("vsync", 0, &code).has_value());
    CHECK(code == CoopLongshotSettings::kFixed);
    CHECK(g_rows[3].value == 1);   // (left as it was)
    CHECK_FALSE(CoopLongshotSettings::Set("msaa", 3, &code).has_value());
    CHECK(code == CoopLongshotSettings::kUnreachable);
    CHECK(g_rows[4].value == 8);
    CHECK_FALSE(CoopLongshotSettings::Set("nope", 1, &code).has_value());
    CHECK(code == CoopLongshotSettings::kNoSuchRow);
    CoopLongshotSettings::Init(nullptr);
    code = 99;
    CHECK_FALSE(CoopLongshotSettings::Set("msaa", 8, &code).has_value());   // (without them)
    CHECK(code == CoopLongshotSettings::kNoSuchRow);
}

TEST_CASE("settings: the module's HosCoopSetting and HosCoopApi keep patch 0032's layout") {
    // (patch 0032's coop_bridge.c checks HosCoopSetting against host/control.h HosControlSetting the same way)
    static_assert(sizeof(HosCoopSetting) == 32 + 48 + 64 + 16 + 6 * sizeof(int));
    static_assert(offsetof(HosCoopSetting, name) == 32);
    static_assert(offsetof(HosCoopSetting, text) == 80);
    static_assert(offsetof(HosCoopSetting, tab) == 144);
    static_assert(offsetof(HosCoopSetting, value) == 160);
    static_assert(offsetof(HosCoopSetting, fixed) == 160 + 5 * sizeof(int));
    static_assert(offsetof(HosCoopApi, setting) == offsetof(HosCoopApi, scratch) + sizeof(void*));
    static_assert(offsetof(HosCoopApi, setting_set) == offsetof(HosCoopApi, setting) + sizeof(void*));
    static_assert(sizeof(HosCoopApi) == offsetof(HosCoopApi, setting_set) + sizeof(void*));
    CHECK(true);
}

TEST_CASE("settings: an older Hookshot's shorter table, or none, gives no settings") {
    HosCoopApi api = FakeApi();
    api.size = static_cast<std::uint32_t>(offsetof(HosCoopApi, setting));   // (before patch 0032)
    CoopLongshotSettings::Init(&api);
    CHECK_FALSE(CoopLongshotSettings::Available());
    CHECK(CoopLongshotSettings::All().empty());
    CHECK_FALSE(CoopLongshotSettings::Set("volume", 1).has_value());
    api.size = sizeof api;
    api.setting_set = nullptr;
    CHECK_FALSE(CoopLongshotSettings::Available());
    CoopLongshotSettings::Init(nullptr);
    CHECK_FALSE(CoopLongshotSettings::Available());
}

TEST_CASE("settings: a text field filled to its size without its 0 is cut at its size") {
    HosCoopSetting s{};
    std::memset(s.name, 'x', sizeof s.name);   // (no 0 at the end)
    std::strncpy(s.key, "k", sizeof s.key);
    const auto row = CoopLongshotSettings::FromGame(s);
    CHECK(row.name.size() == sizeof s.name);
    CHECK(row.key == "k");
}
