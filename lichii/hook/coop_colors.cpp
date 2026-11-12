#include "coop_colors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::uint64_t PieceOf(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t link_index) {
    if (actor == 0 || link_index >= addr::kArmorLinkCount) return 0;
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    if (!ReadValue(guest, actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
        return 0;
    }
    const std::uint64_t link = eu + addr::kArmorLinks + addr::kLinkStride * link_index;
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
    if (Apply(guest, actor, tint) == Applied::no_model) return "";
    std::uint32_t models = 1;
    for (std::uint32_t link = 0; link < addr::kArmorLinkCount; ++link) {
        const std::uint64_t piece = PieceOf(guest, actor, link);
        if (piece != 0 && Apply(guest, piece, tint) != Applied::no_model) ++models;
    }
    if (color == painted.color && models == painted.models) return "";
    painted.color = color;
    painted.models = models;
    char line[128];
    std::snprintf(line, sizeof(line), " (strength %.2f) on body 0x%llx and %u worn piece%s, model 0x%llx",
                  strength, static_cast<unsigned long long>(actor), models - 1,
                  models == 2 ? "" : "s", static_cast<unsigned long long>(ModelOf(guest, actor)));
    return "colour " + Text(color) + line;
}

}  // namespace CoopColors

// --- Cloth colours.
namespace CoopColors {
namespace {

std::optional<std::string> Env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string s(value);
    std::free(value);
    return s;
#else
    const char* v = std::getenv(name);
    if (v == nullptr) return std::nullopt;
    return std::string(v);
#endif
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + ",") {
        if (c == ',' || c == ';') {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (!(c == ' ' && cur.empty())) {
            cur += c;
        }
    }
    return out;
}

float Linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool Has(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

constexpr std::uint64_t kScratchText = 0x00;   // 0x40 bytes
constexpr std::uint64_t kScratchPtr = 0x40;    // the char* to the text
constexpr std::uint64_t kScratchColor = 0x50;  // 4 x f32

std::string HexText(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

}  // namespace

namespace {
bool SlotOf(CoopPuppet::Guest& guest, std::uint64_t unit, std::int32_t material,
            std::uint64_t& slot) {
    if (material < 0) return false;
    std::uint64_t arr = 0;
    if (!ReadValue(guest, unit + cloth::kUnitSlotArray, arr)) return false;
    if (arr != 0) {
        return ReadValue(guest, arr + 8ull * static_cast<std::uint64_t>(material), slot) && slot != 0;
    }
    std::uint64_t slots = 0;
    if (!ReadValue(guest, unit + cloth::kUnitSlots, slots) || slots == 0) return false;
    slot = slots + cloth::kSlotStride * static_cast<std::uint64_t>(material);
    return true;
}

bool ParamTable(CoopPuppet::Guest& guest, std::uint64_t slot, std::uint64_t& table,
                std::uint64_t& values) {
    std::uint64_t res = 0, a = 0, b = 0;
    return ReadValue(guest, slot, res) && res != 0 &&
           ReadValue(guest, res + cloth::kResShader, a) && a != 0 && ReadValue(guest, a, b) &&
           b != 0 && ReadValue(guest, b + cloth::kShaderParams, table) && table != 0 &&
           ReadValue(guest, slot + cloth::kSlotValues, values) && values != 0;
}
}  // namespace

std::optional<Rgba> ReadParam(CoopPuppet::Guest& guest, std::uint64_t unit, std::int32_t material,
                              std::int32_t param) {
    std::uint64_t slot = 0, table = 0, values = 0;
    if (param < 0 || !SlotOf(guest, unit, material, slot) || !ParamTable(guest, slot, table, values)) {
        return std::nullopt;
    }
    std::uint16_t offset = 0;
    if (!ReadValue(guest, table + cloth::kParamStride * static_cast<std::uint64_t>(param) +
                              cloth::kParamOffset, offset)) {
        return std::nullopt;
    }
    Rgba v{};
    if (!guest.Read(values + offset, v.data(), sizeof(v))) return std::nullopt;
    return v;
}

std::string DumpParams(CoopPuppet::Guest& guest, std::uint64_t unit, std::int32_t material,
                       std::int32_t first, std::int32_t count) {
    std::uint64_t slot = 0, table = 0, values = 0;
    if (!SlotOf(guest, unit, material, slot) || !ParamTable(guest, slot, table, values)) {
        return "no table";
    }
    std::string s = "table " + HexText(table) + " values " + HexText(values) + ":";
    for (std::int32_t p = first; p < first + count; ++p) {
        const std::uint64_t e = table + cloth::kParamStride * static_cast<std::uint64_t>(p);
        std::uint64_t name_ptr = 0;
        std::uint16_t offset = 0;
        if (!ReadValue(guest, e + cloth::kParamOffset, offset)) break;
        std::string name;
        if (ReadValue(guest, e, name_ptr) && name_ptr != 0) {
            for (int i = 0; i < 48; ++i) {
                char ch = 0;
                if (!ReadValue(guest, name_ptr + static_cast<std::uint64_t>(i), ch) || ch == 0) break;
                if (ch < 0x20 || ch > 0x7e) {
                    name.clear();
                    break;
                }
                name += ch;
            }
        }
        Rgba v{};
        guest.Read(values + offset, v.data(), sizeof(v));
        char b[160];
        std::snprintf(b, sizeof(b), " [%d %s +0x%x %.3f %.3f %.3f %.3f]", p,
                      name.empty() ? "-" : name.c_str(), offset, v[0], v[1], v[2], v[3]);
        s += b;
    }
    return s;
}

ClothSettings ClothSettings::FromEnvironment() {
    ClothSettings s;
    if (auto v = Env("LICHII_COOP_COLOR_STRONG")) s.strong = SplitList(*v);
    if (auto v = Env("LICHII_COOP_COLOR_LIGHT")) s.light = SplitList(*v);
    if (auto v = Env("LICHII_COOP_COLOR_PARAM"); v && !v->empty()) s.param = *v;
    if (auto v = Env("LICHII_COOP_COLOR_LIGHTEN")) {
        char* end = nullptr;
        const float f = std::strtof(v->c_str(), &end);
        if (end != v->c_str() && std::isfinite(f) && f >= 0.0f && f <= 1.0f) s.lighten = f;
    }
    if (auto v = Env("LICHII_COOP_DEBUG_COLOR")) s.probe = !v->empty() && *v != "0";
    return s;
}

Rgba ClothColor(std::uint32_t rgb, float lighten) {
    lighten = std::isfinite(lighten) ? std::clamp(lighten, 0.0f, 1.0f) : 0.0f;
    Rgba out{0, 0, 0, 1.0f};
    for (int i = 0; i < 3; ++i) {
        const float c = static_cast<float>((rgb >> (16 - 8 * i)) & 0xFF) / 255.0f;
        out[static_cast<std::size_t>(i)] = Linear(c + (1.0f - c) * lighten);
    }
    return out;
}

std::vector<std::string> Cloth::MaterialNames(const ClothSettings& settings) const {
    std::vector<std::string> names = settings.strong;
    for (const auto& n : settings.light) {
        if (!Has(names, n)) names.push_back(n);
    }
    if (settings.probe) {
        for (const char* n : kProbeMaterials) {
            if (!Has(names, n)) names.emplace_back(n);
        }
    }
    return names;
}

std::vector<std::string> Cloth::ParamNames(const ClothSettings& settings) const {
    std::vector<std::string> names{settings.param};
    if (settings.probe) {
        for (int i = 0; i < 6; ++i) {
            std::string n = "ns_param_color" + std::to_string(i);
            if (!Has(names, n)) names.push_back(n);
        }
    }
    return names;
}

std::optional<ClothCall> Cloth::Next(CoopPuppet::Guest& guest, std::uint64_t main_base,
                                     std::uint64_t actor, std::uint32_t color,
                                     const ClothSettings& settings, std::uint64_t scratch,
                                     std::string& line) {
    line.clear();
    if (actor == 0 || scratch == 0) return std::nullopt;
    if (actor != actor_) {
        *this = Cloth{};
        actor_ = actor;
    }
    if (waiting_) return std::nullopt;
    const auto write_text = [&](const std::string& text) {
        std::array<char, 0x40> cell{};
        std::memcpy(cell.data(), text.data(), std::min<std::size_t>(text.size(), cell.size() - 1));
        guest.Write(scratch + kScratchText, cell.data(), cell.size());
        const std::uint64_t ptr = scratch + kScratchText;
        guest.Write(scratch + kScratchPtr, &ptr, sizeof(ptr));
    };
    const auto virtual_at = [&](const Unit& u, std::uint64_t offset) {
        std::uint64_t f = 0;
        ReadValue(guest, u.vtable + offset, f);
        return f;
    };
    for (;;) {
        switch (step_) {
        case Step::units: {
            std::uint64_t parts = 0, holder = 0, hv = 0, get = 0, set = 0, arr = 0;
            std::int32_t count = 0;
            if (!ReadValue(guest, actor + addr::kActorParts, parts) || parts == 0 ||
                !ReadValue(guest, parts + cloth::kPartsHolder, holder) || holder == 0 ||
                !ReadValue(guest, holder, hv) || hv == 0 ||
                !ReadValue(guest, hv + cloth::kHolderGetSet, get) ||
                get != main_base + cloth::kHolderGetSetDefault ||
                !ReadValue(guest, holder + cloth::kHolderSet, set) || set == 0 ||
                !ReadValue(guest, set + cloth::kSetCount, count) || count <= 0 ||
                !ReadValue(guest, set + cloth::kSetUnits, arr) || arr == 0) {
                return std::nullopt;  // not made yet; try again next frame
            }
            units_.clear();
            for (std::int32_t i = 0; i < count && i < static_cast<std::int32_t>(cloth::kMaxUnits);
                 ++i) {
                std::uint64_t entry = 0;
                Unit u;
                if (ReadValue(guest, arr + 8ull * static_cast<std::uint64_t>(i), entry) &&
                    entry != 0 && ReadValue(guest, entry, u.object) && u.object != 0 &&
                    ReadValue(guest, u.object, u.vtable) && u.vtable != 0) {
                    units_.push_back(u);
                }
            }
            if (units_.empty()) return std::nullopt;
            step_ = Step::material;
            unit_i_ = name_i_ = 0;
            break;
        }
        case Step::material: {
            const auto names = MaterialNames(settings);
            if (unit_i_ >= units_.size() || names.empty()) {
                step_ = Step::param;
                found_i_ = param_i_ = 0;
                break;
            }
            write_text(names[name_i_]);
            ClothCall c;
            c.function = virtual_at(units_[unit_i_], cloth::kFindMaterial);
            c.x[0] = units_[unit_i_].object;
            c.x[1] = scratch + kScratchPtr;
            if (c.function == 0) return std::nullopt;
            waiting_ = true;
            return c;
        }
        case Step::param: {
            const auto names = ParamNames(settings);
            if (found_i_ >= found_.size()) {
                step_ = Step::paint;
                found_i_ = 0;
                paint_color_ = color;
                break;
            }
            const Found& f = found_[found_i_];
            write_text(names[param_i_]);
            ClothCall c;
            c.function = virtual_at(units_[f.unit], cloth::kFindParam);
            c.x[0] = units_[f.unit].object;
            c.x[1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(f.index));
            c.x[2] = scratch + kScratchPtr;
            if (c.function == 0) return std::nullopt;
            waiting_ = true;
            return c;
        }
        case Step::paint: {
            while (found_i_ < found_.size() &&
                   (found_[found_i_].params.empty() || found_[found_i_].params[0] < 0 ||
                    !(Has(settings.strong, found_[found_i_].material) ||
                      Has(settings.light, found_[found_i_].material)))) {
                ++found_i_;
            }
            if (found_i_ >= found_.size()) {
                step_ = Step::done;
                color_ = paint_color_;
                line = Report(settings);
                if (settings.probe) {
                    const auto names = ParamNames(settings);
                    for (const auto& f : found_) {
                        line += "\n  " + f.material + " now:";
                        for (std::size_t p = 0; p < f.params.size() && p < names.size(); ++p) {
                            if (const auto v = ReadParam(guest, units_[f.unit].object, f.index, f.params[p])) {
                                char b[96];
                                std::snprintf(b, sizeof(b), " %s (%.3f %.3f %.3f %.3f)", names[p].c_str(),
                                              (*v)[0], (*v)[1], (*v)[2], (*v)[3]);
                                line += b;
                            }
                        }
                    }
                    if (!found_.empty()) {
                        line += "\n  " + found_[0].material + " params " +
                                DumpParams(guest, units_[found_[0].unit].object, found_[0].index, 0, 160);
                    }
                }
                return std::nullopt;
            }
            const Found& f = found_[found_i_];
            const Rgba value = ClothColor(paint_color_, f.light ? settings.lighten : 0.0f);
            guest.Write(scratch + kScratchColor, value.data(), sizeof(value));
            ClothCall c;
            c.function = virtual_at(units_[f.unit], cloth::kSetColor);
            c.x[0] = units_[f.unit].object;
            c.x[1] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(f.index));
            c.x[2] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(f.params[0]));
            c.x[3] = scratch + kScratchColor;
            if (c.function == 0) return std::nullopt;
            waiting_ = true;
            return c;
        }
        case Step::done:
            if (color == color_) return std::nullopt;
            step_ = Step::paint;
            found_i_ = 0;
            paint_color_ = color;
            break;
        }
    }
}

std::string Cloth::Done(std::uint64_t result, const ClothSettings& settings) {
    if (!waiting_) return "";
    waiting_ = false;
    const auto index = static_cast<std::int32_t>(static_cast<std::uint32_t>(result));
    switch (step_) {
    case Step::material: {
        const auto names = MaterialNames(settings);
        if (index >= 0) {
            Found f;
            f.unit = static_cast<std::uint32_t>(unit_i_);
            f.material = names[name_i_];
            f.index = index;
            f.light = !Has(settings.strong, f.material) && Has(settings.light, f.material);
            found_.push_back(f);
        }
        if (++name_i_ >= names.size()) {
            name_i_ = 0;
            ++unit_i_;
        }
        return "";
    }
    case Step::param: {
        const auto names = ParamNames(settings);
        found_[found_i_].params.push_back(index);
        if (++param_i_ >= names.size()) {
            param_i_ = 0;
            ++found_i_;
        }
        return "";
    }
    case Step::paint:
        found_[found_i_].painted = true;
        ++found_i_;
        return "";
    default:
        return "";
    }
}

std::string Cloth::Report(const ClothSettings& settings) const {
    const auto params = ParamNames(settings);
    std::string s = "cloth " + Text(color_) + ": " + std::to_string(units_.size()) + " unit(s)";
    for (std::size_t i = 0; i < units_.size(); ++i) {
        s += " u" + std::to_string(i) + " " + HexText(units_[i].object) + " vtable " +
             HexText(units_[i].vtable);
    }
    int strong = 0, light = 0;
    for (const auto& f : found_) {
        s += "; " + f.material + " u" + std::to_string(f.unit) + " m" + std::to_string(f.index);
        for (std::size_t p = 0; p < f.params.size() && p < params.size(); ++p) {
            if (f.params[p] >= 0) s += " " + params[p] + "=" + std::to_string(f.params[p]);
        }
        if (f.painted) {
            s += f.light ? " (light)" : " (strong)";
            (f.light ? light : strong)++;
        }
    }
    s += "; painted " + std::to_string(strong) + " strong, " + std::to_string(light) + " light";
    return s;
}

}  // namespace CoopColors
