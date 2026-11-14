// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_parts.h"

#include <algorithm>
#include <cstdio>
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
        {"Mt_Boots_Armor_Adult_HoverBoot_Alb", Format::astc, {{Part::boots, T::kBrown}}},
        {"Mt_Boots_Armor_Adult_HeavyBoot_Alb", Format::astc, {{Part::boots, T::kBrown}}},
        {"Mt_Silver_Armor_Adult_Gauntlet_Silver_Alb", Format::astc, {{Part::gauntlets, T::kGrey}}},
        {"Mt_Gold_Armor_Adult_Gauntlet_Gold_Alb", Format::astc, {{Part::gauntlets, T::kGold | T::kBrown}}},
        {"Mt_Angin_Armor_Adult_Goron_Alb", Format::astc, {{Part::goron_tunic, T::kRed}}},
        {"Mt_Vest_Armor_Adult_Goron_Alb", Format::astc, {{Part::goron_tunic, T::kRed}}},
        {"Mt_Angin_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
        {"Mt_Vest_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
        {"Mt_Angin_Acs_Armor_Adult_Zora_Alb", Format::astc, {{Part::zora_tunic, T::kBlue | T::kCyan}}},
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

CoopLookTunic::Texels Painted(const CoopLookTunic::Texels& gear, const CoopLook::Look& look) {
    CoopLookTunic::Texels out = gear;
    for (CoopLookTunic::Texture& t : out) {
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
        return false;
    });
    return out;
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

}  // namespace CoopLookParts
