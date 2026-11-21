// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_parts.h"

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
         {{Part::hylian_frame, T::kGrey | T::kWarmGrey}, {Part::hylian_face, T::kBlue | T::kPurple}, {Part::hylian_emblem, T::kRed}}},
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

bool AnyGear(const CoopLook::Look& look) {
    for (const CoopLook::PartInfo& info : CoopLook::Parts()) {
        if (info.part != Part::tunic && look[info.part]) {
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
        if (info.part == Part::tunic) {
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
