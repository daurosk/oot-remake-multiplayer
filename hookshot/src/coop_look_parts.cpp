// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_parts.h"

#include "coop/protocol/messages.hpp"

#include <algorithm>
#include <cmath>
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
        // leather-link (C #132, Clothing Color 3 on Link): the adult's gloves and upper leather (bracers, belts), the
        // rest of Character Select's leather set. The dump's end colours: gloves 84% brown, 11% red; upper leather 73%
        // brown, 24% red (its dark leather), so both tones. Painted only by link_leather (Owners), so a 0.11.12 look
        // leaves them the game's.
        {"Mt_Glove_Player_Adult_Alb", Format::astc, {{Part::link_leather, T::kBrown | T::kRed}}},
        {"Mt_Leater_Upper_Player_Adult_Alb", Format::astc, {{Part::link_leather, T::kBrown | T::kRed}}},
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
        // hair-link: Link's hair (BC7; child Link's Player_Child_Hairs_Alb has no file of its own: the game's
        // System/Gfx/UnifyTex gives it this one), painted whole by Character Select's hair rule (HairRecolour), not
        // by tones. Last, so every other texture keeps its place in gear.bin.
        {"Mt_Hairs_Player_Adult_Alb", Format::bc7, {{Part::hair, 0}}},
    };
    return kGear;
}

namespace {
// The parts the gear wardrobe paints by their own colour (the tunic and the Cap trim are the tunic wardrobe's, as in
// 0.11.12; the eyes are no texture: the model's own parameter, CoopCharColours). The Cap trim reaches the gear only
// through HoodTrim (C4-4: row 278's Goron and Zora hood trims when cap_trim_hoods is on).
bool IsGear(Part p) {
    return p != Part::tunic && p != Part::cap_trim && p != Part::cap_trim_hoods && p != Part::eyes &&
           p != Part::hair && p != Part::link_gear && p != Part::link_vest && !CoopLook::IsCharacterPart(p);
    // (look-link-cs: Link's leather is a gear-painted part; its textures are in the owner table (Owners). vest-link:
    // the vest is the TUNIC wardrobe's, as the Cap trim (its textures are tunic textures, green.bin: TunicPainted),
    // so no gear painting is made for it. hair-link: the hair stays out of the pieces and is named last, only when
    // set (Name), so a 0.11.12 look keeps its names.)
}
}  // namespace

std::optional<std::uint32_t> HoodTrim(const CoopLook::Look& look) {
    if (!look[Part::cap_trim_hoods] || !look[Part::cap_trim]) {
        return std::nullopt;
    }
    return *look[Part::cap_trim] & 0xFFFFFFu;
}

bool AnyGear(const CoopLook::Look& look) {
    for (const CoopLook::PartInfo& info : CoopLook::Parts()) {
        if (IsGear(info.part) && look[info.part]) {
            return true;
        }
    }
    return HoodTrim(look).has_value() || look[Part::hair].has_value();
}

std::string Name(const CoopLook::Look& look) {
    if (!AnyGear(look)) {
        return kBase;
    }
    std::string name = kBase;
    for (const CoopLook::PartInfo& info : CoopLook::Parts()) {
        if (!IsGear(info.part) || info.part == Part::link_leather) {
            continue;   // (leather-link: the leather last, only when set, so every other look keeps its 0.11.12 name)
        }
        char piece[8] = "-";
        if (const auto& c = look[info.part]) {
            std::snprintf(piece, sizeof piece, "%06X", static_cast<unsigned>(*c & 0xFFFFFFu));
        }
        name += '.';
        name += piece;
    }
    // (C4-4: the hood trims last, only when on, so every other look keeps its 0.11.12 name)
    if (const auto trim = HoodTrim(look)) {
        char piece[16];
        std::snprintf(piece, sizeof piece, ".hood-%06X", static_cast<unsigned>(*trim));
        name += piece;
    }
    if (const auto& leather = look[Part::link_leather]) {
        char piece[20];
        std::snprintf(piece, sizeof piece, ".leather-%06X", static_cast<unsigned>(*leather & 0xFFFFFFu));
        name += piece;
    }
    // hair-link: the hair last too, only when set (IsGear keeps it out of the pieces above).
    if (const auto& hair = look[Part::hair]) {
        char piece[16];
        std::snprintf(piece, sizeof piece, ".hair-%06X", static_cast<unsigned>(*hair & 0xFFFFFFu));
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

std::mutex g_region_mutex;
std::shared_ptr<const Region> g_region;   // guarded

std::shared_ptr<const Region> CurrentRegion() {
    std::lock_guard lock(g_region_mutex);
    return g_region;
}

std::mutex g_cap_mutex;
std::shared_ptr<const Cap> g_cap;   // guarded: the Cap trim's blocks (row 278: the gear textures' are Painted's)

std::shared_ptr<const Cap> CurrentCap() {
    std::lock_guard lock(g_cap_mutex);
    return g_cap;
}

const GearTexture* GearOf(const std::string& name) {
    for (const GearTexture& g : Gear()) {
        if (name == g.name) {
            return &g;
        }
    }
    return nullptr;
}

// metal.bin's layout (tools/puppet_pack/look_metal.py pack), under `magic`.
std::optional<Metal> ParseBlocks(const std::vector<std::uint8_t>& file, const char* magic, const char* what,
                                 std::string* why) {
    const auto fail = [&](const char* w) -> std::optional<Metal> {
        if (why != nullptr) {
            *why = w;
        }
        return std::nullopt;
    };
    constexpr std::size_t kName = 64;
    if (file.size() < 12 || std::memcmp(file.data(), magic, 8) != 0) {
        return fail(what);
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
}  // namespace

std::optional<Metal> ParseMetal(const std::vector<std::uint8_t>& file, std::string* why) {
    return ParseBlocks(file, "COOPMTL1", "not a metal file", why);
}

std::optional<Region> ParseRegion(const std::vector<std::uint8_t>& file, std::string* why) {
    return ParseBlocks(file, "COOPRGN1", "not a region file", why);
}

void UseRegion(std::shared_ptr<const Region> region) {
    std::lock_guard lock(g_region_mutex);
    g_region = std::move(region);
}

std::vector<std::int8_t> RegionRules(const Region& region, const std::string& name, std::size_t blocks,
                                     std::size_t block_bytes, const std::vector<CoopLook::Part>& parts) {
    std::vector<std::int8_t> out;
    for (const MetalBlocks& b : region) {
        if (b.texture != name) {
            continue;
        }
        const auto it = std::find(parts.begin(), parts.end(), b.part);
        if (it == parts.end()) {
            continue;   // the part has no colour: its blocks stay the game's
        }
        if (out.empty()) {
            out.assign(blocks, std::int8_t{-1});
        }
        for (const std::uint32_t at : b.offsets) {
            if (block_bytes != 0 && at % block_bytes == 0 && at / block_bytes < blocks) {
                out[at / block_bytes] = static_cast<std::int8_t>(it - parts.begin());
            }
        }
    }
    return out;
}

void UseMetal(std::shared_ptr<const Metal> metal) {
    std::lock_guard lock(g_metal_mutex);
    g_metal = std::move(metal);
}

void UseCap(std::shared_ptr<const Cap> cap) {
    std::lock_guard lock(g_cap_mutex);
    g_cap = std::move(cap);
}

std::size_t GearCapTextures(const Cap& cap) {
    std::size_t n = 0;
    for (const CapBlocks& b : cap) {
        n += GearOf(b.texture) != nullptr ? 1 : 0;
    }
    return n;
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
    const std::shared_ptr<const Region> region = CurrentRegion();
    const std::shared_ptr<const Cap> cap = CurrentCap();
    for (std::size_t i = 0; i < out.size(); ++i) {
        CoopLookTunic::Texture& t = out[i];
        if (metal) {
            PaintMetal(*metal, t.name, t.texels, look);
        }
        const GearTexture* entry = GearOf(t.name);
        if (entry == nullptr) {
            continue;
        }
        if (OwnerOf(t.name) == Part::hair) {
            // hair-link: the hair whole, by Character Select's rule, from the game's own texels; unset: the game's.
            if (const auto& c = look[Part::hair]) {
                CoopLookPaint::HairRecolour(entry->format, gear[i].texels, t.texels, *c & 0xFFFFFFu);
            }
            continue;
        }
        std::vector<CoopLookPaint::Rule> rules;
        std::vector<CoopLook::Part> parts;   // each rule's part
        std::uint32_t tones = 0;             // every part's, painted or not (never taken by a region)
        // leather-link: a texture with an owner (Owners) takes the owner's colour when it is set, every rule of it;
        // unset, each rule its own part's (0.11.12). One painter per texture.
        const std::optional<Part> owner = OwnerOf(t.name);
        const bool owned = owner && look[*owner].has_value();
        for (const GearRule& r : entry->rules) {
            tones |= r.tones;
            if (const auto& c = owned ? look[*owner] : look[r.part]) {
                rules.push_back({r.tones, *c});
                parts.push_back(r.part);
            }
        }
        if (!rules.empty()) {
            const std::size_t step = CoopLookPaint::BlockBytes(entry->format);
            const std::vector<std::int8_t> blocks =
                region ? RegionRules(*region, t.name, t.texels.size() / step, step, parts) : std::vector<std::int8_t>{};
            CoopLookPaint::Recolour(entry->format, t.texels, t.texels, rules, {blocks, tones});
        }
        // Row 278: then the hood's trims (the Goron and Zora tunics' cap blocks) in the Cap trim colour, from the
        // game's own texels, by the texture's own tones, shaded against that part's average as the tunic is: a trim
        // in the tunic's colour is the tunic's painting byte for byte. C4-4: only when cap_trim_hoods is on (HoodTrim);
        // off or no trim colour, they follow the tunic, as in 0.11.12.
        if (const auto trim = HoodTrim(look); trim && cap) {
            const std::span<const std::uint8_t> original(gear[i].texels);
            double sum = 0;
            std::size_t n = 0;
            for (const T::Rgb& e : CoopLookPaint::Ends(entry->format, original)) {
                if ((CoopLookPaint::ToneOf(e) & tones) != 0) {
                    sum += CoopLookPaint::Luma(e);
                    ++n;
                }
            }
            if (n != 0 && sum > 0) {
                PaintCap(*cap, t.name, entry->format, original, t.texels,
                         CoopLookPaint::Rule{tones, *trim, T::Takes::tones, sum / static_cast<double>(n)},
                         tones);
            }
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
constexpr std::string_view kVestInfix = "-vest-";   // (vest-link: before the cap's)
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

std::string TunicPainting(const std::optional<std::uint32_t>& colour, const std::optional<std::uint32_t>& cap,
                          const std::optional<std::uint32_t>& vest) {
    // (none, and the preset green: the game's own green, its own texels; painted, the preset's 30D040 was a lime
    // green brighter than the game's tunic, seen in a test run 2026-11-20)
    const bool own = !colour || (*colour & 0xFFFFFFu) == coop::protocol::slot_color(3);
    if (!cap && !vest) {
        return own ? std::string(kTunicBase) : std::string(kTunicPrefix) + Hex(*colour);
    }
    std::string name = std::string(kTunicPrefix) + (own ? std::string(kGameGreen) : Hex(*colour));
    if (vest) name += std::string(kVestInfix) + Hex(*vest);
    if (cap) name += std::string(kCapInfix) + Hex(*cap);
    return name;
}

std::optional<TunicPaint> TunicPaintOf(const std::string& name) {
    const std::string_view n = name;
    if (n.substr(0, kTunicPrefix.size()) != kTunicPrefix) {
        return std::nullopt;
    }
    std::string_view rest = n.substr(kTunicPrefix.size());
    TunicPaint paint;
    bool painted = false;   // (a cap or vest colour: then "green" is the game's own green)
    const std::size_t at = rest.find(kCapInfix);
    if (at != std::string_view::npos) {
        paint.cap = HexOf(rest.substr(at + kCapInfix.size()));
        if (!paint.cap) {
            return std::nullopt;
        }
        rest = rest.substr(0, at);
        painted = true;
    }
    const std::size_t vat = rest.find(kVestInfix);
    if (vat != std::string_view::npos) {
        paint.vest = HexOf(rest.substr(vat + kVestInfix.size()));
        if (!paint.vest) {
            return std::nullopt;
        }
        rest = rest.substr(0, vat);
        painted = true;
    }
    if (painted && rest == kGameGreen) {
        return paint;   // the game's own green, the cap and/or the vest painted
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

const std::vector<TextureOwner>& Owners() {
    static const std::vector<TextureOwner> kOwners = {
        // Clothing Color 2 on Link (Character Select's notes): the adult's Mt_Vest (mesh Hood_A, the lower tunic and
        // back), the child's Mt_Vest and the hat's band Mt_Band (one texture). Only their green parts are painted
        // (TunicTextures: Takes::green), so the belt, the trim and the hood trims' own blocks (Cap) keep theirs.
        {"Mt_Vest_Player_Adult_Alb", Part::link_vest},
        {"Mt_Vest_Player_Child_Alb", Part::link_vest},
        // leather-link: Clothing Color 3 on Link, the leather and the boots (the materials of Character Select's
        // leather palette, read from its Player_Adult / Player_Child models). The child's boots, belts, buckles and
        // shoulder belt are ONE texture; the adult's boots, gloves and upper leather three. Unset: the boots part
        // paints the two boots textures and the other two keep the game's (0.11.12). Hover / Iron boots: boots only.
        {"Mt_Leather_Player_Child_Alb", Part::link_leather},
        {"Mt_Boots_Player_Adult_Alb", Part::link_leather},
        {"Mt_Glove_Player_Adult_Alb", Part::link_leather},
        {"Mt_Leater_Upper_Player_Adult_Alb", Part::link_leather},
        // hair-link: Link's hair, both ages (the child's name is given this file by the game's UnifyTex).
        {"Mt_Hairs_Player_Adult_Alb", Part::hair},
    };
    return kOwners;
}

std::optional<Part> OwnerOf(std::string_view texture) {
    for (const TextureOwner& o : Owners()) {
        if (texture == o.texture) return o.part;
    }
    return std::nullopt;
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
    return PaintCap(cap, name, format, green, texels, CoopLookPaint::Rule{0, colour & 0xFFFFFFu, T::Takes::green, base},
                    T::kGreen);
}

std::size_t PaintCap(const Cap& cap, const std::string& name, CoopLookPaint::Format format,
                     std::span<const std::uint8_t> original, std::vector<std::uint8_t>& texels,
                     const CoopLookPaint::Rule& rule, std::uint32_t tones) {
    const std::size_t step = CoopLookPaint::BlockBytes(format);
    static constexpr std::int8_t kTheRule[] = {0};   // each cap block, painted alone, is the rule's region
    const CoopLookPaint::Region region{kTheRule, tones};
    const std::size_t size = std::min(original.size(), texels.size());
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
            changed += CoopLookPaint::Recolour(format, original.subspan(at, step), out.subspan(at, step), {rule}, region);
        }
    }
    return changed;
}

namespace {
// The cap's blocks of the texture `name` (blocks of `step` bytes) copied from `from` into `texels`, byte for byte; an
// offset not on a block or past either is passed over (as PaintCap).
void CopyCap(const Cap& cap, const std::string& name, std::size_t step, const std::vector<std::uint8_t>& from,
             std::vector<std::uint8_t>& texels) {
    const std::size_t size = std::min(from.size(), texels.size());
    for (const CapBlocks& b : cap) {
        if (b.texture != name || b.block != step) continue;
        for (const std::uint32_t at : b.offsets) {
            if (at % step != 0 || at > size || size - at < step) continue;
            std::copy_n(from.begin() + at, step, texels.begin() + at);
        }
    }
}
}  // namespace

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
            // vest-link: a texture the vest owns (OwnerOf) takes the vest's colour when it is set, else the tunic's.
            const bool vest = paint.vest && OwnerOf(t.name) == Part::link_vest;
            const std::optional<std::uint32_t> colour = vest ? paint.vest : paint.tunic;
            if (colour) {
                CoopLookPaint::Recolour(entry.format, t.texels, t.texels, {{0, *colour & 0xFFFFFFu, entry.takes, base}});
            }
            // Then the cap's own blocks, from green's texels: over the tunic's colour.
            if (paint.cap && cap != nullptr) {
                PaintCap(*cap, t.name, entry.format, green[i].texels, t.texels, *paint.cap, base);
            } else if (vest && cap != nullptr) {
                // No Cap trim colour: the cap's blocks of a vest-painted texture (the adult's hood trims) follow the
                // tunic, as in 0.11.12: the tunic's painting of them, byte for byte (green's own with no tunic colour).
                std::vector<std::uint8_t> tunic = green[i].texels;
                if (paint.tunic) {
                    CoopLookPaint::Recolour(entry.format, tunic, tunic, {{0, *paint.tunic & 0xFFFFFFu, entry.takes, base}});
                }
                CopyCap(*cap, t.name, CoopLookPaint::BlockBytes(entry.format), tunic, t.texels);
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

namespace {
// An item copy's look: the hair is no item's (hair-link), so it neither names nor paints a copy.
std::optional<CoopLook::Look> ItemLook(const std::optional<CoopLook::Look>& look) {
    std::optional<CoopLook::Look> out = look;
    if (out) {
        (*out)[Part::hair].reset();
    }
    return out;
}
}  // namespace

CoopLookTunic::Texels CopyTexels(const CoopLookTunic::Texels& gear, std::size_t copy,
                                 const std::optional<CoopLook::Look>& given) {
    const std::optional<CoopLook::Look> look = ItemLook(given);
    CoopLookTunic::Texels out = look && AnyGear(*look) ? Painted(gear, *look) : gear;
    std::erase_if(out, [&](CoopLookTunic::Texture& t) {
        std::string name = SlotName(t.name, copy);
        if (name.empty() || OwnerOf(t.name) == Part::hair) {   // (hair-link: no item names Link's hair)
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
    for (const auto& given : looks) {
        const std::optional<CoopLook::Look> look = ItemLook(given);
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
    // (leather-link: and the adult's gloves and upper leather, link_leather's, which the puppets' models name a copy of;
    // hair-link: the hair last, kHairTexture)
    static const std::vector<std::string> kBoots = {"Mt_Boots_Player_Adult_Alb", "Mt_Leather_Player_Child_Alb",
                                                    "Mt_Glove_Player_Adult_Alb", "Mt_Leater_Upper_Player_Adult_Alb",
                                                    "Mt_Hairs_Player_Adult_Alb"};
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

std::string ChildHairName(char letter) {
    return std::string("Coop_") + letter + "_Child_Hairs_Alb";
}

namespace {
// Who has each letter (your own Link first, then the others in order; the first of a letter has it, with or without
// a colour), the walk both Assign and AssignHair use.
template <typename Take>
void ForEachWearer(const Wearer& own, const std::vector<Wearer>& others, Take take) {
    std::vector<char> taken;
    const auto one = [&](const Wearer& w) {
        if (!w.letter || std::find(taken.begin(), taken.end(), *w.letter) != taken.end()) {
            return;
        }
        taken.push_back(*w.letter);
        take(*w.letter, w);
    };
    one(own);
    for (const Wearer& w : others) {
        one(w);
    }
}
}  // namespace

Wearers Assign(const Wearer& own, const std::vector<Wearer>& others) {
    Wearers out;
    ForEachWearer(own, others, [&](char letter, const Wearer& w) {
        if (w.boots || w.leather) {   // (with no boots or leather colour: that colour keeps the game's)
            BootsPaint& p = out[letter];
            if (w.boots) p.boots = *w.boots & 0xFFFFFFu;
            if (w.leather) p.leather = *w.leather & 0xFFFFFFu;
        }
    });
    return out;
}

HairWearers AssignHair(const Wearer& own, const std::vector<Wearer>& others) {
    HairWearers out;
    ForEachWearer(own, others, [&](char letter, const Wearer& w) {
        if (w.hair) {   // (hair-link: with no hair colour, that colour keeps the game's hair)
            out[letter] = *w.hair & 0xFFFFFFu;
        }
    });
    return out;
}

std::string BootsPaintingName(const Wearers& wearers, const HairWearers& hair) {
    std::string name = kBootsBase;
    for (const auto& [letter, paint] : wearers) {
        // (".R3070FF" as 0.11.12 for the boots; leather-link: "L" and the leather's colour after it when set)
        char piece[24];
        std::snprintf(piece, sizeof piece, ".%c", letter);
        name += piece;
        if (paint.boots) {
            std::snprintf(piece, sizeof piece, "%06X", static_cast<unsigned>(*paint.boots & 0xFFFFFFu));
            name += piece;
        }
        if (paint.leather) {
            std::snprintf(piece, sizeof piece, "L%06X", static_cast<unsigned>(*paint.leather & 0xFFFFFFu));
            name += piece;
        }
    }
    for (const auto& [letter, colour] : hair) {   // (hair-link: after the boots, so a boots name stays 0.11.12's)
        char piece[24];
        std::snprintf(piece, sizeof piece, ".%c-hair-%06X", letter, static_cast<unsigned>(colour & 0xFFFFFFu));
        name += piece;
    }
    return name;
}

CoopLookTunic::Texels BootsTexels(const CoopLookTunic::Texels& gear, const Wearers& wearers,
                                  const HairWearers& hair) {
    CoopLookTunic::Texels boots;
    for (const CoopLookTunic::Texture& t : gear) {
        if (!BootsName(t.name, 'G').empty()) {
            boots.push_back(t);
        }
    }
    CoopLookTunic::Texels out;
    for (const Letter& l : kLetters) {
        CoopLookTunic::Texels own = boots;
        CoopLook::Look look;
        if (const auto it = wearers.find(l.letter); it != wearers.end()) {
            look[Part::boots] = it->second.boots;
            look[Part::link_leather] = it->second.leather;   // (leather-link: the owner, Painted)
        }
        if (const auto it = hair.find(l.letter); it != hair.end()) {
            look[Part::hair] = it->second;   // (hair-link)
        }
        if (look[Part::boots] || look[Part::link_leather] || look[Part::hair]) {
            own = Painted(boots, look);
        }
        for (CoopLookTunic::Texture& t : own) {
            const bool is_hair = t.name == kHairTexture;
            CoopLookTunic::Texture child;
            if (is_hair) {
                child = t;   // (the child's copy: the same painting, its own name and mark)
            }
            t.name = BootsName(t.name, l.letter);
            Mark(t.texels, static_cast<unsigned char>(l.letter));
            out.push_back(std::move(t));
            if (is_hair) {
                child.name = ChildHairName(l.letter);
                Mark(child.texels, static_cast<unsigned char>(l.letter - 'A' + 'a'));
                out.push_back(std::move(child));
            }
        }
    }
    return out;
}

std::string PuppetTunicsName(const TunicPaints& paints) {
    if (paints.empty()) return kPuppetTunicsBase;
    std::string name = kPuppetTunicsBase;
    char hex[8];
    for (const auto& [colour, paint] : paints) {
        std::snprintf(hex, sizeof hex, "%06X", static_cast<unsigned>(paint.tunic.value_or(0x30D040u) & 0xFFFFFFu));
        name += "|" + colour + "=" + hex;
        if (paint.cap) {
            std::snprintf(hex, sizeof hex, "%06X", static_cast<unsigned>(*paint.cap & 0xFFFFFFu));
            name += std::string("+cap") + hex;
        }
        if (paint.vest) {
            std::snprintf(hex, sizeof hex, "%06X", static_cast<unsigned>(*paint.vest & 0xFFFFFFu));
            name += std::string("+vest") + hex;
        }
    }
    return name;
}

namespace {
// The game's own child tunic body: the mean colour of its dark tile's green end colours (its own texture, ASTC),
// read from `green`; nothing when the file has no such texture or no green in it.
std::optional<std::uint32_t> GameDarkColour(const CoopLookTunic::Texels& green) {
    for (const CoopLookTunic::Texture& t : green) {
        if (t.name != "Angin_Tile_A_Dark_Alb") continue;
        double sum[3] = {0, 0, 0};
        std::size_t n = 0;
        const auto lin = [](double v) { v /= 255.0; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        for (const CoopLookPaint::Rgb& c : CoopLookPaint::Ends(Format::astc, t.texels)) {
            if (!CoopLookPaint::IsGreen(c)) continue;
            for (int k = 0; k < 3; ++k) sum[k] += lin(c[static_cast<std::size_t>(k)]);
            ++n;
        }
        if (n == 0) return std::nullopt;
        std::uint32_t out = 0;
        for (int k = 0; k < 3; ++k) {
            const double l = sum[k] / static_cast<double>(n);
            const double v = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
            out = out << 8 | static_cast<std::uint32_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
        }
        return out;
    }
    return std::nullopt;
}
}  // namespace

CoopLookTunic::Texels PuppetTunicsTexels(const std::vector<std::pair<std::string, CoopLookTunic::Texels>>& copies,
                                         const CoopLookTunic::Texels& green, const TunicPaints& paints, const Cap* cap) {
    CoopLookTunic::Texels out;
    for (const auto& [colour, texels] : copies) {
        const auto paint = paints.find(colour);
        if (paint == paints.end()) {
            out.insert(out.end(), texels.begin(), texels.end());
            continue;
        }
        // (none, or the preset green 30D040: the game's own green, as the player's own Link wears it (TunicPainting);
        // painted, 30D040 is a lime lighter than the game's tunic, row 243)
        const bool own = !paint->second.tunic || (*paint->second.tunic & 0xFFFFFFu) == coop::protocol::slot_color(3);
        const CoopLookTunic::Texels painted =
            TunicPainted(green, own ? TunicPaint{std::nullopt, paint->second.cap, paint->second.vest} : paint->second,
                         cap);   // (the trim and the vest too)
        std::optional<CoopLookTunic::Texels> darker;   // (the dark tile's: the main tile in the darker colour)
        for (std::size_t i = 0; i < texels.size(); ++i) {
            CoopLookTunic::Texture t = texels[i];   // (its name and offset: the copy's)
            bool from_green = false;
            if (i < painted.size() && painted[i].texels.size() == t.texels.size()) {
                t.texels = painted[i].texels;
                from_green = true;
            } else if (t.name.find("_Dark_") != std::string::npos && !painted.empty() &&   // (by its name: any order)
                       texels[0].texels.size() == t.texels.size()) {
                if (!darker) {
                    const auto scaled = [](std::uint32_t v) {
                        return static_cast<std::uint32_t>(static_cast<float>(v & 0xFF) * kDarkScale + 0.5f);
                    };
                    const auto darkened = [&](std::uint32_t c) { return scaled(c >> 16) << 16 | scaled(c >> 8) << 8 | scaled(c); };
                    // (the game's own: its own dark tile's colour, read from green, as it is; else the preset's, scaled)
                    const std::optional<std::uint32_t> own_dark = own ? GameDarkColour(green) : std::nullopt;
                    darker = TunicPainted(green, own_dark ? *own_dark
                                                           : darkened(own ? 0x30D040u : *paint->second.tunic));
                }
                if (!darker->empty() && (*darker)[0].texels.size() == t.texels.size()) {
                    t.texels = (*darker)[0].texels;
                    from_green = true;
                }
            }
            if (from_green && !t.texels.empty()) {
                t.texels.back() ^= kSelfMark ^ kTunicMark;   // (from green: marked as the copy's, never your Link's; row 198)
            }
            out.push_back(std::move(t));
        }
    }
    return out;
}

}  // namespace CoopLookParts
