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

struct Tunic {
    std::uint32_t rgb;     // the kNamed colour it stands for
    std::string_view suffix;
};
// The puppet pack's tunic presets (tools/puppet_pack/tunic.py COLORS; keep the
// two the same), each at the colour of its name above. Green is the game's own.
constexpr Tunic kTunics[] = {
    {0xFF3030, "Red"},  {0xFF8020, "Orange"}, {0xFFC020, "Yellow"}, {0x30D040, ""},
    {0x20D0E0, "Cyan"}, {0x3070FF, "Blue"},   {0xA040FF, "Purple"}, {0xFF60C0, "Pink"},
    {0xFFFFFF, "White"}, {0x202020, "Black"},
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
    const auto trim = [&text] {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
    };
    trim();
    // Quotes around it ("#FF8020", as players type them in an ini).
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
        text = text.substr(1, text.size() - 2);
        trim();
    }
    for (const Named& n : kNamed) {
        const std::string_view name(n.name);
        if (name.size() == text.size() &&
            std::equal(name.begin(), name.end(), text.begin(),
                       [](char a, char b) { return a == Lower(b); })) {
            return n.rgb;
        }
    }
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    } else if (text.size() == 8 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);   // 0xRRGGBB
    }
    if (text.size() == 3) {   // #RGB: each digit twice
        std::uint32_t rgb = 0;
        for (const char c : text) {
            const int d = HexDigit(c);
            if (d < 0) return std::nullopt;
            rgb = (rgb << 8) | static_cast<std::uint32_t>(d * 17);
        }
        return rgb;
    }
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

std::string_view TunicOf(std::uint32_t rgb) {
    // Brightness (luma) and colour (chroma) apart, chroma counted double: a
    // plain RGB distance puts gray (A0A0A0) nearer pink than white.
    struct Ycc {
        double y, cb, cr;
    };
    const auto ycc = [](std::uint32_t c) {
        const double r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        const double y = 0.299 * r + 0.587 * g + 0.114 * b;
        return Ycc{y, 0.564 * (b - y), 0.713 * (r - y)};
    };
    const Ycc want = ycc(rgb);
    std::string_view best;
    double best_d = -1.0;
    for (const auto& t : kTunics) {
        const Ycc have = ycc(t.rgb);
        const double dy = want.y - have.y, db = want.cb - have.cb, dr = want.cr - have.cr;
        const double d = dy * dy + 2.0 * (db * db + dr * dr);
        if (best_d < 0.0 || d < best_d) {
            best_d = d;
            best = t.suffix;
        }
    }
    return best;
}

const std::vector<std::string_view>& Tunics() {
    static const std::vector<std::string_view> all = [] {
        std::vector<std::string_view> v;
        for (const auto& t : kTunics) {
            if (!t.suffix.empty()) {
                v.push_back(t.suffix);
            }
        }
        return v;
    }();
    return all;
}

}  // namespace CoopColors
