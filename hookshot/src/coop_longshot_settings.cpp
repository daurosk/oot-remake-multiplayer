// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_longshot_settings.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace CoopLongshotSettings {

namespace {

std::atomic<const HosCoopApi*> g_api{nullptr};

template <std::size_t N>
std::string Text(const char (&field)[N]) {
    return std::string(field, strnlen(field, N));
}

}  // namespace

void Init(const HosCoopApi* api) {
    g_api = api;
}

bool Available() {
    const HosCoopApi* api = g_api.load();
    return api != nullptr && api->size >= offsetof(HosCoopApi, setting_set) + sizeof(api->setting_set) &&
           api->setting != nullptr && api->setting_set != nullptr;
}

Setting FromGame(const HosCoopSetting& s) {
    Setting out;
    out.key = Text(s.key), out.name = Text(s.name), out.text = Text(s.text), out.tab = Text(s.tab);
    out.value = s.value, out.lo = s.lo, out.hi = s.hi, out.step = s.step;
    out.restart = s.restart != 0, out.fixed = s.fixed != 0;
    return out;
}

std::optional<Setting> At(int index) {
    if (index < 0 || !Available()) return std::nullopt;
    HosCoopSetting s{};
    if (!g_api.load()->setting(index, &s)) return std::nullopt;
    return FromGame(s);
}

std::vector<Setting> All() {
    std::vector<Setting> out;
    for (int i = 0; i < kMaxRows; ++i) {
        auto s = At(i);
        if (!s) break;
        out.push_back(std::move(*s));
    }
    return out;
}

std::optional<Setting> Set(const std::string& key, int value, int* code) {
    if (code != nullptr) *code = kNoSuchRow;
    if (key.empty() || !Available()) return std::nullopt;
    HosCoopSetting s{};
    const int r = g_api.load()->setting_set(key.c_str(), value, &s);
    if (code != nullptr) *code = r;
    if (r != kSet) return std::nullopt;   // (-1 fixed, -2 unreachable: the game left it as it was)
    return FromGame(s);
}

}  // namespace CoopLongshotSettings
