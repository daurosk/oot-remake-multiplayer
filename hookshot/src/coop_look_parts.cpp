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
    if (slot < 1 || slot > 9 || name.size() < 4 || name.compare(0, 3, "Mt_") != 0) {
        return {};
    }
    return "C" + std::to_string(slot) + "_" + name.substr(3);
}

namespace {
constexpr std::uint32_t kZstdMagic = 0xFD2FB528u;
constexpr std::uint8_t kRawHeader = 0xE0;   // single segment, an 8-byte content size, no dictionary or checksum
constexpr std::size_t kRawBlock = 128 * 1024;

void PutLe(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
std::uint64_t GetLe(const std::vector<std::uint8_t>& in, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= std::uint64_t{in[at + static_cast<std::size_t>(i)]} << (8 * i);
    }
    return v;
}
}  // namespace

std::vector<std::uint8_t> RawZstd(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    out.reserve(data.size() + data.size() / kRawBlock * 3 + 16);
    PutLe(out, kZstdMagic, 4);
    out.push_back(kRawHeader);
    PutLe(out, data.size(), 8);
    std::size_t at = 0;
    do {
        const std::size_t n = std::min(kRawBlock, data.size() - at);
        const bool last = at + n >= data.size();
        PutLe(out, (std::uint64_t{n} << 3) | (last ? 1u : 0u), 3);
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < data.size());
    return out;
}

std::optional<std::vector<std::uint8_t>> UnrawZstd(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 13 || GetLe(frame, 0, 4) != kZstdMagic || frame[4] != kRawHeader) {
        return std::nullopt;
    }
    const std::uint64_t size = GetLe(frame, 5, 8);
    if (size > frame.size()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(size));
    std::size_t at = 13;
    while (true) {
        if (at + 3 > frame.size()) {
            return std::nullopt;
        }
        const std::uint64_t h = GetLe(frame, at, 3);
        at += 3;
        const std::size_t n = static_cast<std::size_t>(h >> 3);
        if (((h >> 1) & 3) != 0 || n > frame.size() - at) {
            return std::nullopt;   // not a raw block, or cut short
        }
        out.insert(out.end(), frame.begin() + static_cast<std::ptrdiff_t>(at),
                   frame.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (h & 1) {
            break;
        }
    }
    if (out.size() != size) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> Repainted(const std::vector<std::uint8_t>& frame,
                                                   const CoopLookTunic::Texture& texture) {
    auto file = UnrawZstd(frame);
    if (!file || texture.offset > file->size() || texture.texels.size() > file->size() - texture.offset) {
        return std::nullopt;
    }
    std::copy(texture.texels.begin(), texture.texels.end(), file->begin() + static_cast<std::ptrdiff_t>(texture.offset));
    return RawZstd(*file);
}

}  // namespace CoopLookParts
