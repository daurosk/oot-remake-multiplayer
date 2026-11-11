#include "coop_colors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace CoopColors {
namespace {

struct Named {
    const char* name;
    std::uint32_t rgb;
};
// The first four are the player number colours (coop::protocol::slot_color).
// Also in tools/release/kit/scripts/coop-ini.ps1 (Test-CoopColor) and
// docs/play-online.md: keep the three the same.
constexpr Named kNamed[] = {
    {"green", 0x20C040},  {"red", 0xE02020},    {"blue", 0x2050FF},   {"purple", 0x9030E0},
    {"yellow", 0xFFD020}, {"orange", 0xFF8000}, {"cyan", 0x00C8E0},   {"pink", 0xFF50B0},
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

template <typename T>
bool ReadValue(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return guest.Read(address, &out, sizeof(T));
}

std::mutex g_mutex;
std::optional<std::uint32_t> g_own;                      // guarded
std::array<std::optional<std::uint32_t>, 256> g_chosen;  // by player number; guarded

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
    return "green, red, blue, purple, yellow, orange, cyan, pink, white, black, brown, gray";
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

float StrengthFromEnvironment() {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "LICHII_COOP_PUPPET_TINT") != 0 || value == nullptr) {
        return kDefaultStrength;
    }
    const float strength = StrengthFrom(value);
    std::free(value);
    return strength;
#else
    return StrengthFrom(std::getenv("LICHII_COOP_PUPPET_TINT"));
#endif
}

void SetOwn(std::optional<std::uint32_t> rgb) {
    std::lock_guard lock(g_mutex);
    g_own = rgb;
}

std::optional<std::uint32_t> Own() {
    std::lock_guard lock(g_mutex);
    return g_own;
}

void SetChosen(std::uint8_t player, std::optional<std::uint32_t> rgb) {
    std::lock_guard lock(g_mutex);
    g_chosen[player] = rgb;
}

std::optional<std::uint32_t> Chosen(std::uint8_t player) {
    std::lock_guard lock(g_mutex);
    return g_chosen[player];
}

void ForgetAll() {
    std::lock_guard lock(g_mutex);
    g_chosen = {};
}

std::uint32_t ForPlayer(std::uint8_t player, std::uint8_t self) {
    std::lock_guard lock(g_mutex);
    const auto& chosen = (self != 0 && player == self) ? g_own : g_chosen[player];
    return chosen ? *chosen : coop::protocol::slot_color(player);
}

std::uint64_t ModelOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    if (actor == 0) return 0;
    std::uint64_t parts = 0;
    if (!ReadValue(guest, actor + addr::kActorParts, parts) || parts == 0) return 0;
    std::uint64_t model = 0;
    if (!ReadValue(guest, parts + addr::kPartsModel, model)) return 0;
    return model;
}

std::uint64_t TunicOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    if (actor == 0) return 0;
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    if (!ReadValue(guest, actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
        return 0;
    }
    const std::uint64_t link = eu + addr::kArmorLinks + addr::kLinkStride * addr::kTunicLink;
    std::uint32_t id = 0xFFFFFFFFu;
    std::uint64_t holder = 0;
    std::uint64_t piece = 0;
    if (!ReadValue(guest, link + addr::kLinkId, id) || id == 0xFFFFFFFFu ||
        !ReadValue(guest, link + addr::kLinkHolder, holder) || holder == 0 ||
        !ReadValue(guest, holder + addr::kHolderActor, piece) || piece == actor) {
        return 0;
    }
    return piece;
}

Applied Apply(CoopPuppet::Guest& guest, std::uint64_t actor, const Rgba& tint) {
    const std::uint64_t model = ModelOf(guest, actor);
    if (model == 0) return Applied::no_model;
    Rgba now{};
    if (!guest.Read(model + addr::kModelColor, now.data(), sizeof(now))) return Applied::no_model;
    if (now == tint) return Applied::same;
    std::uint32_t dirty = 0;
    if (!ReadValue(guest, model + addr::kModelDirty, dirty)) return Applied::no_model;
    guest.Write(model + addr::kModelColor, tint.data(), sizeof(tint));
    dirty |= addr::kDirtyColor;
    guest.Write(model + addr::kModelDirty, &dirty, sizeof(dirty));
    return Applied::written;
}

std::string Paint(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t color,
                  float strength, Painted& painted) {
    if (painted.actor != actor) {
        painted = Painted{actor, 0, 0};  // a new actor: nothing painted on it yet
    }
    const Rgba tint = TintOf(color, strength);
    std::uint64_t target = TunicOf(guest, actor);
    if (target == 0 || Apply(guest, target, tint) == Applied::no_model) {
        target = actor;  // no tunic piece (a child), or not made yet
        if (Apply(guest, actor, tint) == Applied::no_model) return "";
    }
    if (target == painted.target && color == painted.color) return "";
    if (painted.target == actor && target != actor) {
        Apply(guest, actor, Rgba{0, 0, 0, 0});  // the tunic came: the body back to its own colours
    }
    painted.target = target;
    painted.color = color;
    char s[16];
    std::snprintf(s, sizeof(s), "%.2f", strength);
    char where[48];
    std::snprintf(where, sizeof(where), "%s 0x%llx", target == actor ? "body" : "tunic",
                  static_cast<unsigned long long>(target));
    return "colour " + Text(color) + " (strength " + s + ") on " + where + ", model 0x" +
           [&] {
               char m[24];
               std::snprintf(m, sizeof(m), "%llx",
                             static_cast<unsigned long long>(ModelOf(guest, target)));
               return std::string(m);
           }();
}

}  // namespace CoopColors
