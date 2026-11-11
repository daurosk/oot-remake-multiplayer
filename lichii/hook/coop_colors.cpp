#include "coop_colors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "coop_puppet.h"

namespace CoopColors {
namespace {

struct Named {
    const char* name;
    std::uint32_t rgb;
};
// Also in tools/release/kit/scripts/coop-ini.ps1 (Test-CoopColor) and
// docs/play-online.md: keep the three the same.
constexpr Named kNamed[] = {
    {"red", 0xFF3030},    {"orange", 0xFF8020}, {"yellow", 0xFFC020}, {"green", 0x30D040},
    {"cyan", 0x20D0E0},   {"blue", 0x3070FF},   {"purple", 0xA040FF}, {"pink", 0xFF60C0},
    {"white", 0xFFFFFF},  {"black", 0x202020},  {"brown", 0x8B5A2B},  {"gray", 0xA0A0A0},
    {"grey", 0xA0A0A0},
};

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = Lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

std::optional<std::uint32_t> Parse(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    for (const Named& n : kNamed) {
        const std::string_view name(n.name);
        if (name.size() == text.size() &&
            std::equal(name.begin(), name.end(), text.begin(),
                       [](char a, char b) { return a == Lower(b); })) {
            return n.rgb;
        }
    }
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6) return std::nullopt;
    std::uint32_t rgb = 0;
    for (const char c : text) {
        const int d = HexDigit(c);
        if (d < 0) return std::nullopt;
        rgb = (rgb << 4) | static_cast<std::uint32_t>(d);
    }
    return rgb;
}

const char* Names() {
    return "red, orange, yellow, green, cyan, blue, purple, pink, white, black, brown, gray";
}

std::string Text(std::uint32_t rgb) {
    char out[8];
    std::snprintf(out, sizeof(out), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFF));
    return out;
}

Rgba TintOf(std::uint32_t rgb, float strength) {
    if (!std::isfinite(strength)) strength = 0.0f;
    strength = std::clamp(strength, 0.0f, 1.0f);
    if (strength == 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, strength};
}

float StrengthFrom(const char* text) {
    if (text == nullptr || *text == '\0') return kDefaultStrength;
    const std::string_view t(text);
    if (t == "off" || t == "OFF" || t == "Off") return 0.0f;
    char* end = nullptr;
    const float f = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(f) || f < 0.0f || f > 1.0f) {
        return kDefaultStrength;
    }
    return f;
}

std::uint64_t ModelOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    if (actor == 0) return 0;
    std::uint64_t parts = 0;
    if (!guest.Read(actor + addr::kActorParts, &parts, sizeof(parts)) || parts == 0) return 0;
    std::uint64_t model = 0;
    if (!guest.Read(parts + addr::kPartsModel, &model, sizeof(model))) return 0;
    return model;
}

Applied Apply(CoopPuppet::Guest& guest, std::uint64_t actor, const Rgba& tint) {
    const std::uint64_t model = ModelOf(guest, actor);
    if (model == 0) return Applied::no_model;
    Rgba now{};
    if (!guest.Read(model + addr::kModelColor, now.data(), sizeof(now))) return Applied::no_model;
    if (now == tint) return Applied::same;
    std::uint32_t dirty = 0;
    if (!guest.Read(model + addr::kModelDirty, &dirty, sizeof(dirty))) return Applied::no_model;
    guest.Write(model + addr::kModelColor, tint.data(), sizeof(tint));
    dirty |= addr::kDirtyColor;
    guest.Write(model + addr::kModelDirty, &dirty, sizeof(dirty));
    return Applied::written;
}

}  // namespace CoopColors
