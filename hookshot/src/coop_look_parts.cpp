// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_parts.h"

#include "coop/protocol/messages.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

namespace CoopLookParts {

using CoopLook::Part;
using CoopLookPaint::Format;
namespace T = CoopLookPaint;

const std::vector<GearTexture>& Gear() {
    // The tones each part's end colours have in the game's textures (their
    // shares, from the textures' end colours: the Hylian Shield 33% grey,
    // 8% blue, 2% red, the rest wood; the Master Sword 63% grey, 22% blue).
    static const std::vector<GearTexture> kGear = {
        {"Mt_Weapon_Shield_Hylian_Alb", Format::bc1,
         {{Part::hylian_frame, T::kGrey}, {Part::hylian_face, T::kBlue | T::kPurple}, {Part::hylian_emblem, T::kRed}}},
        {"Mt_Weapon_Shield_Mirror_Alb", Format::astc,
         {{Part::mirror_rim, T::kRed}, {Part::mirror_face, T::kGrey | T::kBlue | T::kPurple | T::kCyan}}},
        {"Mt_Body_Weapon_Shield_Deku_Alb", Format::astc,
         {{Part::deku_shield, T::kBrown | T::kGold | T::kYellow | T::kRed}}},
        {"Mt_Weapon_Sword_Kokiri_Alb", Format::astc,
         {{Part::kokiri_blade, T::kGrey}, {Part::kokiri_hilt, T::kBrown | T::kRed}}},
        {"Mt_Weapon_Sword_Master_Alb", Format::astc,
         {{Part::master_blade, T::kGrey}, {Part::master_hilt, T::kBlue | T::kPurple}}},
        {"Mt_Weapon_Sword_Biggoron_Alb.00", Format::astc,
         {{Part::biggoron_blade, T::kGrey}, {Part::biggoron_hilt, T::kBlue | T::kPurple}}},
        {"Mt_Weapon_Sword_Biggoron_Alb.01", Format::astc,
         {{Part::biggoron_blade, T::kGrey}, {Part::biggoron_hilt, T::kBlue | T::kPurple}}},
        {"Mt_Boots_Player_Adult_Alb", Format::astc, {{Part::boots, T::kBrown}}},
        {"Mt_Leather_Player_Child_Alb", Format::astc, {{Part::boots, T::kBrown}}},   // and the child's belts
        {"Mt_Boots_Armor_Adult_HoverBoot_Alb", Format::astc, {{Part::boots, T::kBrown}}},
        {"Mt_Boots_Armor_Adult_HeavyBoot_Alb", Format::astc, {{Part::boots, T::kBrown}}},
        {"Mt_Silver_Armor_Adult_Gauntlet_Silver_Alb", Format::astc, {{Part::gauntlets, T::kGrey}}},
        {"Mt_Gold_Armor_Adult_Gauntlet_Gold_Alb", Format::astc, {{Part::gauntlets, T::kGold | T::kBrown}}},
        {"Mt_Angin_Armor_Adult_Goron_Alb", Format::astc, {{Part::goron_tunic, T::kRed}}},
        {"Mt_Vest_Armor_Adult_Goron_Alb", Format::astc, {{Part::goron_tunic, T::kRed}}},
        {"Mt_Angin_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
        {"Mt_Vest_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
        {"Mt_Angin_Acs_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
        // The rest of the cloth (the dump's end colours: the Goron inner 82% crimson, which is pink by
        // hue; the Zora parts 45% blue beside grey scales; the Zora feather 49% blue and cyan).
        {"Mt_Inner_Armor_Adult_Goron_Alb", Format::astc, {{Part::goron_tunic, T::kRed | T::kPink}}},
        {"Mt_Parts_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
        {"Mt_Feather_Armor_Adult_Zora_Alb", Format::bc1, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
    };
    return kGear;
}

namespace {
// The parts the gear wardrobe paints (the tunic and the Cap trim are the tunic wardrobe's).
bool IsGear(Part p) { return p != Part::tunic && p != Part::cap_trim; }
}  // namespace

bool AnyGear(const CoopLook::Look& look) {
    for (const CoopLook::PartInfo& info : CoopLook::Parts()) {
        if (IsGear(info.part) && look[info.part]) {
            return true;
        }
    }
    return false;
}

std::string Name(const CoopLook::Look& look) {
    if (!AnyGear(look)) {
        return kBase;
    }
    std::string name = kBase;
    for (const CoopLook::PartInfo& info : CoopLook::Parts()) {
        if (!IsGear(info.part)) {
            continue;
        }
        char piece[8] = "-";
        if (const auto& c = look[info.part]) {
            std::snprintf(piece, sizeof piece, "%06X", static_cast<unsigned>(*c & 0xFFFFFFu));
        }
        name += '.';
        name += piece;
    }
    return name;
}

namespace {
std::mutex g_metal_mutex;
std::shared_ptr<const Metal> g_metal;   // guarded

std::shared_ptr<const Metal> CurrentMetal() {
    std::lock_guard lock(g_metal_mutex);
    return g_metal;
}
}  // namespace

std::optional<Metal> ParseMetal(const std::vector<std::uint8_t>& file, std::string* why) {
    const auto fail = [&](const char* w) -> std::optional<Metal> {
        if (why != nullptr) {
            *why = w;
        }
        return std::nullopt;
    };
    constexpr std::size_t kName = 64;
    static constexpr char kMagic[] = "COOPMTL1";
    if (file.size() < 12 || std::memcmp(file.data(), kMagic, 8) != 0) {
        return fail("not a metal file");
    }
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, file.data() + at, 4);
        return v;   // (little-endian hosts only, as the look files)
    };
    const std::uint32_t count = u32(8);
    Metal out;
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (file.size() - at < kName + 8) {
            return fail("the metal file is cut short");
        }
        const auto* name = file.data() + at;
        MetalBlocks b;
        b.texture.assign(name, std::find(name, name + kName, std::uint8_t{0}));
        const std::uint32_t part = u32(at + kName);
        const std::uint32_t n = u32(at + kName + 4);
        at += kName + 8;
        if (part >= CoopLook::kParts) {
            return fail("a metal entry names no part");
        }
        if ((file.size() - at) / 4 < n) {
            return fail("the metal file is cut short");
        }
        b.part = static_cast<CoopLook::Part>(part);
        b.offsets.resize(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            b.offsets[k] = u32(at + 4 * std::size_t{k});
        }
        at += 4 * std::size_t{n};
        out.push_back(std::move(b));
    }
    return out;
}

void UseMetal(std::shared_ptr<const Metal> metal) {
    std::lock_guard lock(g_metal_mutex);
    g_metal = std::move(metal);
}

std::size_t PaintMetal(const Metal& metal, const std::string& name, std::vector<std::uint8_t>& texels,
                       const CoopLook::Look& look) {
    std::size_t set = 0;
    for (const MetalBlocks& b : metal) {
        if (b.texture != name || !look[b.part]) {
            continue;
        }
        for (const std::uint32_t at : b.offsets) {
            // A BC4 block with both ends 0 is 0 in every texel.
            if (at % 8 == 0 && at <= texels.size() && texels.size() - at >= 8) {
                std::memset(texels.data() + at, 0, 8);
                ++set;
            }
        }
    }
    return set;
}

CoopLookTunic::Texels Painted(const CoopLookTunic::Texels& gear, const CoopLook::Look& look) {
    CoopLookTunic::Texels out = gear;
    const std::shared_ptr<const Metal> metal = CurrentMetal();
    for (CoopLookTunic::Texture& t : out) {
        if (metal) {
            PaintMetal(*metal, t.name, t.texels, look);
        }
        const GearTexture* entry = nullptr;
        for (const GearTexture& g : Gear()) {
            if (t.name == g.name) {
                entry = &g;
                break;
            }
        }
        if (entry == nullptr) {
            continue;
        }
        std::vector<CoopLookPaint::Rule> rules;
        for (const GearRule& r : entry->rules) {
            if (const auto& c = look[r.part]) {
                rules.push_back({r.tones, *c});
            }
        }
        if (!rules.empty()) {
            CoopLookPaint::Recolour(entry->format, t.texels, t.texels, rules);
        }
    }
    return out;
}

const std::vector<TunicTexture>& TunicTextures() {
    // Formats as the game's files have them (green.bin holds those texels).
    static const std::vector<TunicTexture> kTunic = {
        {kTunicTile, Format::bc3, T::Takes::all},   // hat and upper body, both ages
        // The child's tunic body: the game's is ASTC 5x4 and some 0.64 as bright as the tile; shaded against
        // the tile it stays that much darker (the pack's own colour files hold a BC3 copy of the tile instead).
        {"Angin_Tile_A_Dark_Alb", Format::astc, T::Takes::all},
        {"Mt_Vest_Player_Adult_Alb", Format::astc, T::Takes::green},        // lower tunic and back
        {"Mt_Angin_Acs_Player_Adult_Alb", Format::bc1, T::Takes::green},    // rope and hood parts
        {"Mt_Cloth_Parts_Player_Child_Alb", Format::astc, T::Takes::green}, // cap knot, headband, collar
        {"Mt_Vest_Player_Child_Alb", Format::astc, T::Takes::green},        // the string at the back
        {"Mt_PantsParts_Player_Child_Alb", Format::bc1, T::Takes::green},   // straps
    };
    return kTunic;
}

namespace {
constexpr std::string_view kTunicPrefix = "tunic-";
constexpr std::string_view kCapInfix = "-cap-";
constexpr std::string_view kGameGreen = "green";

std::string Hex(std::uint32_t colour) {
    char piece[8];
    std::snprintf(piece, sizeof piece, "%06X", static_cast<unsigned>(colour & 0xFFFFFFu));
    return piece;
}

// Six upper-case hex digits (as Hex writes them).
std::optional<std::uint32_t> HexOf(std::string_view text) {
    if (text.size() != 6) {
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (const char c : text) {
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return std::nullopt;
        }
        v = v << 4 | static_cast<std::uint32_t>(d);
    }
    return v;
}
}  // namespace

std::string TunicPainting(const std::optional<std::uint32_t>& colour, const std::optional<std::uint32_t>& cap) {
    // (none, and the preset green: the game's own green, its own texels; painted, the preset's 30D040 was a lime
    // green brighter than the game's tunic, seen in a test run 2026-11-20)
    const bool own = !colour || (*colour & 0xFFFFFFu) == coop::protocol::slot_color(3);
    if (!cap) {
        return own ? std::string(kTunicBase) : std::string(kTunicPrefix) + Hex(*colour);
    }
    return std::string(kTunicPrefix) + (own ? std::string(kGameGreen) : Hex(*colour)) + std::string(kCapInfix) +
           Hex(*cap);
}

std::optional<TunicPaint> TunicPaintOf(const std::string& name) {
    const std::string_view n = name;
    if (n.substr(0, kTunicPrefix.size()) != kTunicPrefix) {
        return std::nullopt;
    }
    std::string_view rest = n.substr(kTunicPrefix.size());
    TunicPaint paint;
    const std::size_t at = rest.find(kCapInfix);
    if (at != std::string_view::npos) {
        paint.cap = HexOf(rest.substr(at + kCapInfix.size()));
        if (!paint.cap) {
            return std::nullopt;
        }
        rest = rest.substr(0, at);
        if (rest == kGameGreen) {
            return paint;   // the game's own green, the cap painted
        }
    }
    paint.tunic = HexOf(rest);
    if (!paint.tunic) {
        return std::nullopt;
    }
    return paint;
}

std::optional<std::uint32_t> TunicColour(const std::string& name) {
    if (name.size() != kTunicPrefix.size() + 6 || name.compare(0, kTunicPrefix.size(), kTunicPrefix) != 0) {
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (std::size_t i = kTunicPrefix.size(); i < name.size(); ++i) {
        const char c = name[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return std::nullopt;   // (TunicPainting writes upper case only)
        }
        v = v << 4 | static_cast<std::uint32_t>(d);
    }
    return v;
}

CoopLookTunic::Texels TunicPainted(const CoopLookTunic::Texels& green, std::uint32_t colour) {
    return TunicPainted(green, TunicPaint{colour, std::nullopt}, nullptr);
}

std::optional<Cap> ParseCap(const std::vector<std::uint8_t>& file, std::string* why) {
    const auto fail = [&](const char* w) -> std::optional<Cap> {
        if (why != nullptr) {
            *why = w;
        }
        return std::nullopt;
    };
    constexpr std::size_t kName = 64;
    static constexpr char kMagic[] = "COOPCAP1";
    if (file.size() < 12 || std::memcmp(file.data(), kMagic, 8) != 0) {
        return fail("not a cap file");
    }
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, file.data() + at, 4);
        return v;   // (little-endian hosts only, as the look files)
    };
    const std::uint32_t count = u32(8);
    Cap out;
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (file.size() - at < kName + 8) {
            return fail("the cap file is cut short");
        }
        const auto* name = file.data() + at;
        CapBlocks b;
        b.texture.assign(name, std::find(name, name + kName, std::uint8_t{0}));
        b.block = u32(at + kName);
        const std::uint32_t n = u32(at + kName + 4);
        at += kName + 8;
        if (b.block != 8 && b.block != 16) {
            return fail("a cap entry has no block size");
        }
        if ((file.size() - at) / 4 < n) {
            return fail("the cap file is cut short");
        }
        b.offsets.resize(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            b.offsets[k] = u32(at + 4 * std::size_t{k});
        }
        at += 4 * std::size_t{n};
        out.push_back(std::move(b));
    }
    return out;
}

std::size_t PaintCap(const Cap& cap, const std::string& name, CoopLookPaint::Format format,
                     std::span<const std::uint8_t> green, std::vector<std::uint8_t>& texels, std::uint32_t colour,
                     double base) {
    const std::size_t step = CoopLookPaint::BlockBytes(format);
    const std::size_t size = std::min(green.size(), texels.size());
    const std::span<std::uint8_t> out(texels);
    std::size_t changed = 0;
    for (const CapBlocks& b : cap) {
        if (b.texture != name || b.block != step) {
            continue;
        }
        for (const std::uint32_t at : b.offsets) {
            if (at % step != 0 || at > size || size - at < step) {
                continue;
            }
            changed += CoopLookPaint::Recolour(format, green.subspan(at, step), out.subspan(at, step),
                                               {{0, colour & 0xFFFFFFu, T::Takes::green, base}});
        }
    }
    return changed;
}

CoopLookTunic::Texels TunicPainted(const CoopLookTunic::Texels& green, const TunicPaint& paint, const Cap* cap) {
    // The tile's average (tunic.py average_luma); with no tile in the file each texture its own.
    double base = 0;
    for (const CoopLookTunic::Texture& t : green) {
        if (t.name == kTunicTile) {
            base = CoopLookPaint::AverageLuma(Format::bc3, t.texels);
            break;
        }
    }
    CoopLookTunic::Texels out = green;
    for (std::size_t i = 0; i < out.size(); ++i) {
        CoopLookTunic::Texture& t = out[i];
        for (const TunicTexture& entry : TunicTextures()) {
            if (t.name != entry.name) {
                continue;
            }
            if (paint.tunic) {
                CoopLookPaint::Recolour(entry.format, t.texels, t.texels,
                                        {{0, *paint.tunic & 0xFFFFFFu, entry.takes, base}});
            }
            // Then the cap's own blocks, from green's texels: over the tunic's colour.
            if (paint.cap && cap != nullptr) {
                PaintCap(*cap, t.name, entry.format, green[i].texels, t.texels, *paint.cap, base);
            }
            break;
        }
    }
    return out;
}

std::string SlotName(const std::string& name, std::size_t slot) {
    if (slot < 1 || slot > 9 || name.size() < 4 || name.compare(0, 3, "Mt_") != 0 ||
        name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos) {
        return {};
    }
    return "C" + std::to_string(slot) + "_" + name.substr(3);
}

CoopLookTunic::Texels CopyTexels(const CoopLookTunic::Texels& gear, std::size_t copy,
                                 const std::optional<CoopLook::Look>& look) {
    CoopLookTunic::Texels out = look && AnyGear(*look) ? Painted(gear, *look) : gear;
    std::erase_if(out, [&](CoopLookTunic::Texture& t) {
        std::string name = SlotName(t.name, copy);
        if (name.empty()) {
            return true;   // not a gear texture: no copy of it
        }
        t.name = std::move(name);
        Mark(t.texels, copy);
        return false;
    });
    return out;
}

void Mark(std::vector<std::uint8_t>& texels, std::size_t copy) {
    if (!texels.empty()) {
        texels.back() ^= static_cast<std::uint8_t>(copy);
    }
}

std::string CopiesName(const CopyLooks& looks) {
    std::string name = kCopiesBase;
    bool any = false;
    for (const auto& look : looks) {
        name += '|';
        if (look && AnyGear(*look)) {
            name += Name(*look);
            any = true;
        } else {
            name += '-';
        }
    }
    return any ? name : std::string(kCopiesBase);
}

const std::vector<std::string>& BodyBoots() {
    static const std::vector<std::string> kBoots = {"Mt_Boots_Player_Adult_Alb", "Mt_Leather_Player_Child_Alb"};
    return kBoots;
}

namespace {
struct Letter {
    std::string_view name;
    char letter;
};
constexpr Letter kLetters[] = {{"red", 'R'},    {"orange", 'O'}, {"yellow", 'Y'}, {"cyan", 'C'},  {"blue", 'B'},
                               {"purple", 'P'}, {"pink", 'M'},   {"white", 'W'},  {"black", 'K'}, {"green", 'G'}};
}  // namespace

std::optional<char> LetterOf(std::string_view tunic) {
    for (const Letter& l : kLetters) {
        if (l.name == tunic) {
            return l.letter;
        }
    }
    return std::nullopt;
}

std::string BootsName(const std::string& name, char letter) {
    if (std::find(BodyBoots().begin(), BodyBoots().end(), name) == BodyBoots().end()) {
        return {};
    }
    return std::string("P") + letter + "_" + name.substr(3);
}

Wearers Assign(const Wearer& own, const std::vector<Wearer>& others) {
    Wearers out;
    std::vector<char> taken;
    const auto take = [&](const Wearer& w) {
        if (!w.letter || std::find(taken.begin(), taken.end(), *w.letter) != taken.end()) {
            return;
        }
        taken.push_back(*w.letter);   // (with no boots colour: that colour keeps the game's)
        if (w.boots) {
            out[*w.letter] = *w.boots & 0xFFFFFFu;
        }
    };
    take(own);
    for (const Wearer& w : others) {
        take(w);
    }
    return out;
}

std::string BootsPaintingName(const Wearers& wearers) {
    if (wearers.empty()) {
        return kBootsBase;
    }
    std::string name = kBootsBase;
    for (const auto& [letter, colour] : wearers) {
        char piece[16];
        std::snprintf(piece, sizeof piece, ".%c%06X", letter, static_cast<unsigned>(colour & 0xFFFFFFu));
        name += piece;
    }
    return name;
}

CoopLookTunic::Texels BootsTexels(const CoopLookTunic::Texels& gear, const Wearers& wearers) {
    CoopLookTunic::Texels boots;
    for (const CoopLookTunic::Texture& t : gear) {
        if (!BootsName(t.name, 'G').empty()) {
            boots.push_back(t);
        }
    }
    CoopLookTunic::Texels out;
    for (const Letter& l : kLetters) {
        CoopLookTunic::Texels own = boots;
        if (const auto it = wearers.find(l.letter); it != wearers.end()) {
            CoopLook::Look look;
            look[Part::boots] = it->second;
            own = Painted(boots, look);
        }
        for (CoopLookTunic::Texture& t : own) {
            t.name = BootsName(t.name, l.letter);
            Mark(t.texels, static_cast<unsigned char>(l.letter));
            out.push_back(std::move(t));
        }
    }
    return out;
}

}  // namespace CoopLookParts
