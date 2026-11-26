// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_menu.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "coop_colors.h"
#include "coop_i18n.h"

namespace CoopLookMenu {

using CoopLook::Part;
using CoopLook::PartInfo;

namespace {

// 0.11.9 (the owner: any colour, not a palette): the mixer is hue, saturation and brightness, the whole range.
constexpr int kHueStep = 10;    // degrees a press (wraps round)
constexpr int kLevelStep = 5;   // percent a press on Saturation or Brightness

enum class What { kHeading, kPart, kAllGame, kHue, kSaturation, kBrightness, kPreset, kGameOwn, kDone, kNote };

// The game's own colours, near enough for the picture (the textures' averages).
std::uint32_t GameColour(Part part) {
    switch (part) {
    case Part::tunic: return 0x4C8C3C;
    case Part::goron_tunic: return 0xA83030;
    case Part::zora_tunic: return 0x3C5A96;
    case Part::hylian_frame: return 0xA29B99;
    case Part::hylian_face: return 0x27357A;
    case Part::hylian_emblem: return 0xAA2A1E;
    case Part::mirror_rim: return 0xA0322A;
    case Part::mirror_face: return 0xBEC3DC;
    case Part::deku_shield: return 0x96704A;
    case Part::kokiri_blade: return 0xC8C8C8;
    case Part::kokiri_hilt: return 0x6E5032;
    case Part::master_blade: return 0xDDE4E8;
    case Part::master_hilt: return 0x504AA0;
    case Part::biggoron_blade: return 0xC4C5C4;
    case Part::biggoron_hilt: return 0x344F93;
    case Part::boots: return 0x634D38;
    case Part::gauntlets: return 0xAAAAAA;
    case Part::cap_trim: return 0x4C8C3C;   // (unset it is the tunic's: Shown)
    case Part::eyes: return 0x3C6EC8;       // (Link's blue)
    case Part::hair: return 0xD8A848;       // (Link's blond)
    case Part::char_hair: return 0x6A4428;  // (a character's: brown, where the mixer starts)
    case Part::char_eyes: return 0x3C6EC8;
    case Part::char_cloth1:
    case Part::char_cloth2:
    case Part::char_cloth3: return 0x808080;
    case Part::char_gear: return 0x000000;
    }
    return 0x808080;
}

// What a part row's help adds: what of the item takes the colour.
const char* Note(Part part) {
    switch (part) {
    case Part::tunic: return "The tunic and the cap (one piece on adult Link).";
    case Part::goron_tunic: return "The red cloth; the gold trim and the plates stay.";
    case Part::zora_tunic: return "The blue cloth and the scale pattern.";
    case Part::hylian_frame: return "The metal rim, and the cross on the back.";
    case Part::hylian_face: return "The blue face.";
    case Part::hylian_emblem: return "The red bird; the gold Triforce stays.";
    case Part::mirror_rim: return "The red rim.";
    case Part::mirror_face: return "The mirror.";
    case Part::deku_shield: return "The whole shield: it is one piece of wood.";
    // (issue #77: a coloured blade left the hilt blue; the hilt is the row below, the gold guard stays.)
    case Part::kokiri_blade:
    case Part::biggoron_blade: return "The blade only: the grip is the Hilt below.";
    case Part::master_blade: return "The blade only: the grip and wings are the Hilt below; the gold stays.";
    case Part::kokiri_hilt: return "The leather grip.";
    case Part::master_hilt: return "The blue grip and wings; the gold and the gem stay.";
    case Part::biggoron_hilt: return "The blue grip.";
    case Part::boots: return "The leather of every pair: the Kokiri, Hover and Iron Boots (young Link's belts too).";
    case Part::gauntlets: return "The metal of the Silver and the Golden Gauntlets.";
    case Part::cap_trim:
        return "The cap's own trims: young Link's headband and knot, adult Link's hood trims (the cap's cloth is the tunic's). The other players see it on you too.";
    case Part::eyes: return "The colour of the eyes, on your Link and seen by the other players.";
    case Part::hair: return "The colour of the hair and the brows, on your Link and seen by the other players.";
    case Part::char_hair: return "The colour of your character's hair and brows. The other players see it too.";
    case Part::char_eyes: return "The colour of your character's eyes. The other players see it too.";
    case Part::char_cloth1:
    case Part::char_cloth2:
    case Part::char_cloth3:
        return "A part of your character's clothes, in one of seven colours. The other players see it too.";
    case Part::char_gear: return "Your character's sword, shield and other gear shown or hidden.";
    }
    return "";
}

// What an unset part shows: the game's own colour; the Cap trim follows the tunic.
const char* Unset(Part part) { return part == Part::cap_trim ? "Same as tunic" : "Default"; }

std::string ValueText(Part part, const std::optional<std::uint32_t>& colour) {
    if (part == Part::char_gear) return CoopI18n::Tr(colour ? "Hidden" : "Shown");
    return colour ? CoopLook::ColourText(colour) : CoopI18n::Tr(Unset(part));
}

// npc-customize: a character's clothing rows take the mod's seven palette colours only (CoopLook::kClothPresets),
// and its Equipment row is shown or hidden; neither opens the mixer.
bool IsCloth(Part p) { return p == Part::char_cloth1 || p == Part::char_cloth2 || p == Part::char_cloth3; }
bool PresetOnly(Part p) { return IsCloth(p) || p == Part::char_gear; }
std::optional<std::uint32_t> NextCloth(const std::optional<std::uint32_t>& colour, int step) {
    const int n = static_cast<int>(CoopLook::kClothPresets.size()) + 1;   // (the game's own first)
    int at = 0;
    for (int i = 1; i < n; ++i) {
        if (colour && CoopColors::Parse(CoopLook::kClothPresets[static_cast<std::size_t>(i - 1)]) == colour) at = i;
    }
    at = ((at + step) % n + n) % n;
    return at == 0 ? std::nullopt : CoopColors::Parse(CoopLook::kClothPresets[static_cast<std::size_t>(at - 1)]);
}
// A character part's row on the model shown (CoopCharColours::Rows' bits; 0: not known yet, every row).
bool RowOnModel(Part p, std::uint8_t rows) {
    if (!CoopLook::IsCharacterPart(p) || rows == 0) return true;
    switch (p) {
    case Part::char_hair: return (rows & 1) != 0;
    case Part::char_eyes: return (rows & 2) != 0;
    case Part::char_cloth1: return (rows & 4) != 0;
    case Part::char_cloth2: return (rows & 8) != 0;
    case Part::char_cloth3: return (rows & 16) != 0;
    default: return true;   // (the equipment: every model)
    }
}

std::string Title(Part part) {
    const PartInfo& info = CoopLook::InfoOf(part);
    // Both words are the table's ("Kokiri Tunic", "Frame"), and so is the
    // separator: the sentence is put together after each was looked up, since
    // the joined line ("Kokiri Tunic: Frame") is not a line of any table.
    return info.item == std::string(info.name)
               ? CoopI18n::Tr(info.item)
               : CoopI18n::Tr(info.item) + CoopI18n::Tr(": ") + CoopI18n::Tr(info.name);
}

// The preset after (step 1) or before (-1) a colour; a mixed colour steps from the game's own.
std::optional<std::uint32_t> NextPreset(const std::optional<std::uint32_t>& colour, int step) {
    const auto& presets = Presets();
    const int n = static_cast<int>(presets.size());
    int at = 0;
    for (int i = 0; i < n; ++i) {
        const auto c = presets[static_cast<std::size_t>(i)].empty() ? std::optional<std::uint32_t>()
                                                                     : CoopColors::Parse(presets[static_cast<std::size_t>(i)]);
        if (c == colour) {
            at = i;
            break;
        }
    }
    at = ((at + step) % n + n) % n;
    const std::string& name = presets[static_cast<std::size_t>(at)];
    return name.empty() ? std::nullopt : CoopColors::Parse(name);
}

bool IsTunic(Part p) { return p == Part::tunic || p == Part::goron_tunic || p == Part::zora_tunic; }
bool IsShield(Part p) {
    return p == Part::hylian_frame || p == Part::hylian_face || p == Part::hylian_emblem || p == Part::mirror_rim ||
           p == Part::mirror_face || p == Part::deku_shield;
}
bool IsSword(Part p) {
    return p == Part::kokiri_blade || p == Part::kokiri_hilt || p == Part::master_blade || p == Part::master_hilt ||
           p == Part::biggoron_blade || p == Part::biggoron_hilt;
}

// ---- the picture ----

struct Drawn {
    std::optional<Part> part;
    std::uint32_t rgb;
    std::vector<std::pair<float, float>> points;
};

std::vector<std::pair<float, float>> Octagon(float cx, float cy, float r) {
    std::vector<std::pair<float, float>> p;
    for (int i = 0; i < 8; ++i) {
        const double a = (i + 0.5) * 3.14159265358979 / 4;
        p.emplace_back(cx + r * static_cast<float>(std::cos(a)), cy + r * static_cast<float>(std::sin(a)));
    }
    return p;
}

std::vector<Drawn> Doll(const CoopLook::Look& look, Part tunic, Part shield, Part sword) {
    constexpr std::uint32_t kSkin = 0xF0C8A0, kHair = 0xE0B040, kCloth = 0xEDE6D6, kBelt = 0x6B4A2A, kGold = 0xD8B048;
    const std::uint32_t t = Shown(look, tunic);
    std::vector<Drawn> d;
    auto add = [&](std::optional<Part> part, std::uint32_t rgb, std::vector<std::pair<float, float>> pts) {
        d.push_back({part, rgb, std::move(pts)});
    };
    // Link, from the front: his sword in his right hand (the picture's left), his shield on his left arm.
    add(std::nullopt, kCloth, {{0.42f, 0.58f}, {0.49f, 0.58f}, {0.48f, 0.80f}, {0.42f, 0.80f}});   // legs
    add(std::nullopt, kCloth, {{0.51f, 0.58f}, {0.58f, 0.58f}, {0.58f, 0.80f}, {0.52f, 0.80f}});
    add(Part::boots, Shown(look, Part::boots), {{0.41f, 0.76f}, {0.49f, 0.76f}, {0.50f, 0.90f}, {0.38f, 0.90f}});
    add(Part::boots, Shown(look, Part::boots), {{0.51f, 0.76f}, {0.59f, 0.76f}, {0.62f, 0.90f}, {0.50f, 0.90f}});
    add(std::nullopt, kCloth, {{0.33f, 0.31f}, {0.40f, 0.31f}, {0.37f, 0.47f}, {0.30f, 0.46f}});   // arms
    add(std::nullopt, kCloth, {{0.60f, 0.31f}, {0.67f, 0.31f}, {0.70f, 0.46f}, {0.63f, 0.47f}});
    add(tunic, t, {{0.40f, 0.29f}, {0.60f, 0.29f}, {0.65f, 0.60f}, {0.35f, 0.60f}});                 // tunic
    add(std::nullopt, kBelt, {{0.38f, 0.47f}, {0.62f, 0.47f}, {0.625f, 0.505f}, {0.375f, 0.505f}});
    add(std::nullopt, kGold, {{0.48f, 0.465f}, {0.52f, 0.465f}, {0.52f, 0.51f}, {0.48f, 0.51f}});   // buckle
    add(std::nullopt, kSkin, {{0.43f, 0.14f}, {0.57f, 0.14f}, {0.59f, 0.24f}, {0.50f, 0.29f}, {0.41f, 0.24f}});  // face
    add(std::nullopt, kHair, {{0.41f, 0.145f}, {0.59f, 0.145f}, {0.595f, 0.185f}, {0.405f, 0.185f}});
    add(tunic, t, {{0.40f, 0.155f}, {0.45f, 0.075f}, {0.56f, 0.065f}, {0.61f, 0.155f}});            // cap
    add(tunic, t, {{0.405f, 0.12f}, {0.44f, 0.15f}, {0.30f, 0.32f}, {0.27f, 0.30f}});               // its tail
    if (tunic == Part::tunic) {   // the Kokiri cap's trim: the band round its bottom
        add(Part::cap_trim, Shown(look, Part::cap_trim),
            {{0.40f, 0.145f}, {0.61f, 0.145f}, {0.605f, 0.17f}, {0.405f, 0.17f}});
    }
    add(Part::gauntlets, Shown(look, Part::gauntlets), {{0.29f, 0.445f}, {0.37f, 0.455f}, {0.365f, 0.51f}, {0.28f, 0.50f}});
    add(Part::gauntlets, Shown(look, Part::gauntlets), {{0.63f, 0.455f}, {0.71f, 0.445f}, {0.72f, 0.50f}, {0.635f, 0.51f}});

    // The sword.
    const bool kokiri = sword == Part::kokiri_blade || sword == Part::kokiri_hilt;
    const bool biggoron = sword == Part::biggoron_blade || sword == Part::biggoron_hilt;
    const Part blade = kokiri ? Part::kokiri_blade : biggoron ? Part::biggoron_blade : Part::master_blade;
    const Part hilt = kokiri ? Part::kokiri_hilt : biggoron ? Part::biggoron_hilt : Part::master_hilt;
    const float tip = kokiri ? 0.22f : biggoron ? 0.03f : 0.10f, half = biggoron ? 0.022f : 0.015f;
    add(blade, Shown(look, blade), {{0.32f - half, tip + 0.03f}, {0.32f, tip}, {0.32f + half, tip + 0.03f},
                                    {0.32f + half, 0.44f}, {0.32f - half, 0.44f}});
    add(hilt, Shown(look, hilt), {{0.27f, 0.435f}, {0.37f, 0.435f}, {0.37f, 0.46f}, {0.27f, 0.46f}});   // guard
    add(hilt, Shown(look, hilt), {{0.31f, 0.46f}, {0.33f, 0.46f}, {0.33f, 0.535f}, {0.31f, 0.535f}});   // grip

    // The shield.
    if (shield == Part::deku_shield) {
        add(Part::deku_shield, Shown(look, Part::deku_shield), Octagon(0.75f, 0.46f, 0.115f));
        add(std::nullopt, 0x4A3420, Octagon(0.75f, 0.46f, 0.02f));
    } else if (shield == Part::mirror_rim || shield == Part::mirror_face) {
        add(Part::mirror_rim, Shown(look, Part::mirror_rim),
            {{0.63f, 0.28f}, {0.87f, 0.28f}, {0.87f, 0.56f}, {0.75f, 0.66f}, {0.63f, 0.56f}});
        add(Part::mirror_face, Shown(look, Part::mirror_face),
            {{0.66f, 0.315f}, {0.84f, 0.315f}, {0.84f, 0.535f}, {0.75f, 0.615f}, {0.66f, 0.535f}});
    } else {
        add(Part::hylian_frame, Shown(look, Part::hylian_frame),
            {{0.64f, 0.30f}, {0.86f, 0.30f}, {0.86f, 0.52f}, {0.75f, 0.64f}, {0.64f, 0.52f}});
        add(Part::hylian_face, Shown(look, Part::hylian_face),
            {{0.655f, 0.315f}, {0.845f, 0.315f}, {0.845f, 0.51f}, {0.75f, 0.62f}, {0.655f, 0.51f}});
        add(std::nullopt, kGold, {{0.72f, 0.41f}, {0.75f, 0.355f}, {0.78f, 0.41f}});   // the Triforce
        add(Part::hylian_emblem, Shown(look, Part::hylian_emblem),
            {{0.69f, 0.44f}, {0.81f, 0.44f}, {0.75f, 0.54f}});                         // the bird
    }
    return d;
}

}  // namespace

Hsv ToHsv(std::uint32_t rgb) {
    const int r = static_cast<int>((rgb >> 16) & 0xFF), g = static_cast<int>((rgb >> 8) & 0xFF),
              b = static_cast<int>(rgb & 0xFF);
    const int hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    Hsv out;
    out.v = (hi * 100 + 127) / 255;
    out.s = hi == 0 ? 0 : (d * 100 + hi / 2) / hi;
    if (d == 0) return out;
    double h = hi == r ? std::fmod(static_cast<double>(g - b) / d, 6.0) : hi == g ? static_cast<double>(b - r) / d + 2.0
                                                                                  : static_cast<double>(r - g) / d + 4.0;
    h *= 60.0;
    if (h < 0) h += 360.0;
    out.h = static_cast<int>(std::lround(h)) % 360;
    return out;
}

std::uint32_t ToRgb(const Hsv& c) {
    const double h = ((c.h % 360) + 360) % 360, s = std::clamp(c.s, 0, 100) / 100.0, v = std::clamp(c.v, 0, 100) / 100.0;
    const double chroma = v * s, x = chroma * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1)), m = v - chroma;
    double r = 0, g = 0, b = 0;
    if (h < 60) r = chroma, g = x;
    else if (h < 120) r = x, g = chroma;
    else if (h < 180) g = chroma, b = x;
    else if (h < 240) g = x, b = chroma;
    else if (h < 300) r = x, b = chroma;
    else r = chroma, b = x;
    const auto byte = [m](double t) { return static_cast<std::uint32_t>(std::lround(std::clamp(t + m, 0.0, 1.0) * 255)); };
    return (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

const std::vector<Group>& Groups() {
    // (0.11.9: Link's own colours first, his hair and eyes among them)
    static const std::vector<Group> kGroups = {
        // (no Hair row: the lane run showed the hair parameters' tint too weak on Link; never a row that does nothing)
        {"Link", {Part::eyes, Part::tunic, Part::cap_trim, Part::goron_tunic, Part::zora_tunic}},
        {"Shields", {Part::hylian_frame, Part::hylian_face, Part::hylian_emblem, Part::mirror_rim, Part::mirror_face,
                     Part::deku_shield}},
        {"Swords", {Part::kokiri_blade, Part::kokiri_hilt, Part::master_blade, Part::master_hilt, Part::biggoron_blade,
                    Part::biggoron_hilt}},
        {"Boots and gauntlets", {Part::boots, Part::gauntlets}},
        // (npc-customize: a Character Select character's own parts; only the rows its model has are shown)
        {"Character", {Part::char_hair, Part::char_eyes, Part::char_cloth1, Part::char_cloth2, Part::char_cloth3,
                       Part::char_gear}},
    };
    return kGroups;
}

std::string PagedName(Part part) {
    switch (part) {
    case Part::tunic: return "Kokiri Tunic";
    case Part::cap_trim: return "Cap Trim";
    case Part::goron_tunic: return "Goron Tunic";
    case Part::zora_tunic: return "Zora Tunic";
    case Part::boots: return "Boots";
    case Part::gauntlets: return "Gauntlets";
    case Part::deku_shield: return "Deku Shield";
    case Part::eyes: return "Eye Colour";
    case Part::hair: return "Hair Colour";
    case Part::char_hair: return "Hair Colour";
    case Part::char_eyes: return "Eye Colour";
    case Part::char_cloth1: return "Clothing Colour";
    case Part::char_cloth2: return "Clothing Colour 2";
    case Part::char_cloth3: return "Clothing Colour 3";
    case Part::char_gear: return "Equipment";
    default: break;
    }
    const PartInfo& info = CoopLook::InfoOf(part);
    return std::string(info.item) + " " + info.name;   // ("Hylian Shield Frame", "Master Sword Hilt")
}

const std::vector<std::string>& Presets() {
    static const std::vector<std::string> kPresets = {"",     "red",    "orange", "yellow", "green", "cyan",
                                                      "blue", "purple", "pink",   "white",  "black"};
    return kPresets;
}

std::uint32_t Shown(const CoopLook::Look& look, Part part) {
    if (part == Part::cap_trim && !look[part]) {
        return Shown(look, Part::tunic);   // the cap follows the tunic
    }
    return look[part] ? (*look[part] & 0xFFFFFFu) : GameColour(part);
}

struct Page::Line {
    Row row;
    What what = What::kHeading;
    Part part = Part::tunic;
    std::string help;
};

std::vector<Page::Line> Page::Lines() const {
    std::vector<Line> out;
    if (editing_) {
        const Part p = *editing_;
        out.push_back({{Title(p), "", 3}, What::kHeading, p, ""});
        // (any colour: hue round the wheel, then how strong and how bright; the picture and Link show it at once)
        out.push_back({{"Hue", std::to_string(hsv_.h) + " degrees", 1, true, true}, What::kHue, p,
                       "Left and Right: round the colour wheel, " + std::to_string(kHueStep) +
                           " degrees a press (red, yellow, green, cyan, blue, magenta, red)."});
        out.push_back({{"Saturation", std::to_string(hsv_.s) + "%", 1, hsv_.s > 0, hsv_.s < 100}, What::kSaturation, p,
                       "Left and Right: from grey (0%) to the full colour (100%), " + std::to_string(kLevelStep) +
                           "% a press."});
        out.push_back({{"Brightness", std::to_string(hsv_.v) + "%", 1, hsv_.v > 0, hsv_.v < 100}, What::kBrightness, p,
                       "Left and Right: from black (0%) to the brightest (100%), " + std::to_string(kLevelStep) +
                           "% a press."});
        std::string preset = "Mixed";
        for (const std::string& name : Presets()) {
            if (!name.empty() && look_[p] && CoopColors::Parse(name) == look_[p]) {
                preset = name;
            }
        }
        if (!look_[p]) {
            preset = Unset(p);
        }
        out.push_back({{"Preset", preset, 1, true, true}, What::kPreset, p, "Left and Right: the preset colours."});
        out.push_back({{"Default colour", "", 0}, What::kGameOwn, p, "This part back to the game's own colour."});
        out.push_back({{"Done", "", 0}, What::kDone, p, "Back to the list of parts."});
        return out;
    }
    if (paged_) {
        const auto offered = Offered();
        if (offered.empty()) {
            out.push_back({{"Customize", "", 3}, What::kHeading, Part::tunic, ""});
            out.push_back({{kNoColourOptions, "", 4}, What::kNote, Part::tunic, ""});
            return out;
        }
        const std::size_t shown = std::min(group_, offered.size() - 1);
        const Group& g = Groups()[offered[shown]];
        // (a lone page is only its name: "(page 1 of 1)" said nothing)
        out.push_back({{offered.size() < 2 ? std::string(g.name)
                                           : std::string(g.name) + " (page " + std::to_string(shown + 1) + " of " +
                                                 std::to_string(offered.size()) + ")",
                        "", 3},
                       What::kHeading, g.parts[0], ""});
        for (const Part p : g.parts) {
            if (!RowOnModel(p, rows_)) continue;   // (never a row that does nothing: one the model has not)
            out.push_back({{PagedName(p), ValueText(p, look_[p]), 1, true, true}, What::kPart, p,
                           CoopI18n::Tr(Note(p)) + " " +
                               CoopI18n::Tr(PresetOnly(p) ? "Left and Right change it."
                                                          : "Left and Right try the colours; A picks any colour of your own.")});
        }
        return out;
    }
    std::string item;
    for (const PartInfo& info : CoopLook::Parts()) {
        if (CoopLook::IsCharacterPart(info.part)) continue;   // (Link's list: a character's are on its own page)
        if (item != info.item) {
            item = info.item;
            out.push_back({{item, "", 3}, What::kHeading, info.part, ""});
        }
        // What the colour covers, then how the row works: two lines of the
        // table put together here, because the joined paragraph is not one
        // (both go through Tr again by the page's Keep, which changes nothing
        // once they are Chinese).
        const std::string note = CoopI18n::Tr(Note(info.part));
        const std::string how = CoopI18n::Tr(
            "Left and Right try the colours; Confirm picks any colour of your own. Your Link wears it when you close "
            "the menu. The other players see your gear in this colour, and your tunic in the nearest of the ten tunic colours.");
        out.push_back(
            {{info.name, ValueText(info.part, look_[info.part]), 1, true, true}, What::kPart, info.part, note + " " + how});
    }
    out.push_back({{"All parts to the game's own", "", 0}, What::kAllGame, Part::tunic,
                   "Every part in the game's own colours again (the tunic: your player number's colour for the others)."});
    return out;
}

bool Page::Selectable(std::size_t row) const {
    const auto lines = Lines();
    return row < lines.size() && lines[row].what != What::kHeading && lines[row].what != What::kNote;
}

void Page::SetPaged(bool on) {
    paged_ = on;
    group_ = 0;
    selected_ = 1;
}

void Page::SetCharacter(std::uint16_t character, bool gear_shows) {
    if (character == character_ && gear_shows == gear_shows_) return;
    character_ = character, gear_shows_ = gear_shows;
    editing_.reset();
    group_ = 0;
    selected_ = 1;
}

void Page::ResetAll() {
    // (the parts of the model shown: Link's page leaves a character's parts, a character's page Link's; npc-customize)
    for (const PartInfo& info : CoopLook::Parts()) {
        if (CoopLook::IsCharacterPart(info.part) == (character_ != 0)) Set(info.part, std::nullopt);
    }
}

void Page::SetRows(std::uint8_t rows) {
    if (rows == rows_) return;
    rows_ = rows;
    if (editing_ && !RowOnModel(*editing_, rows_)) editing_.reset();
    if (!Selectable(selected_)) selected_ = 1;
}

std::vector<std::size_t> Page::Offered() const {
    if (character_ == 0) return {0, 1, 2, 3};   // Link: every part but a character's
    // A character: its own parts (npc-customize; the Equipment row is on every model), then its held gear once shown
    // to work.
    if (gear_shows_) return {kCharacterGroup, 1, 2};
    return {kCharacterGroup};
}

std::size_t Page::GroupShown() const {
    const auto offered = Offered();
    return offered.empty() ? 0 : offered[std::min(group_, offered.size() - 1)];
}

std::size_t Page::PageIndex() const {
    const std::size_t n = Offered().size();
    return n == 0 ? 0 : std::min(group_, n - 1);
}

std::size_t Page::GroupsOffered() const {
    return Offered().size();
}

void Page::TurnPage(int step) {
    const std::size_t n = Offered().size();
    if (!paged_ || editing_ || n == 0) return;
    group_ = (group_ + n + static_cast<std::size_t>(step > 0 ? 1 : n - 1)) % n;
    selected_ = 1;
    Focus(Lines()[1].part);
}

void Page::Open(const CoopLook::Look& look) {
    look_ = look;
    editing_.reset();
    changed_ = false;
    if (!Selectable(selected_)) {
        selected_ = 1;   // the first part, under its heading
    }
    const auto lines = Lines();
    if (selected_ < lines.size()) {
        Focus(lines[selected_].part);
    }
}

void Page::Focus(Part part) {
    if (part == Part::cap_trim) {
        tunic_ = Part::tunic;   // (the trim is on the Kokiri cap)
    } else if (IsTunic(part)) {
        tunic_ = part;
    } else if (IsShield(part)) {
        shield_ = part;
    } else if (IsSword(part)) {
        sword_ = part;
    }
}

void Page::Move(int step) {
    const auto lines = Lines();
    const std::size_t n = lines.size();
    for (std::size_t tries = 0; tries < n; ++tries) {
        selected_ = step > 0 ? (selected_ + 1) % n : (selected_ + n - 1) % n;
        if (lines[selected_].what != What::kHeading && lines[selected_].what != What::kNote) {
            Focus(lines[selected_].part);
            return;
        }
    }
}

void Page::Set(Part part, std::optional<std::uint32_t> colour) {
    if (look_[part] != colour) {
        look_[part] = colour;
        changed_ = true;
    }
}

void Page::Change(std::size_t row, int step) {
    const auto lines = Lines();
    if (row >= lines.size()) {
        return;
    }
    const Line& l = lines[row];
    switch (l.what) {
    case What::kPart:
        if (l.part == Part::char_gear) {
            Set(l.part, look_[l.part] ? std::nullopt : std::optional<std::uint32_t>(CoopLook::kGearHidden));
        } else if (IsCloth(l.part)) {
            Set(l.part, NextCloth(look_[l.part], step));
        } else {
            Set(l.part, NextPreset(look_[l.part], step));
        }
        break;
    case What::kPreset:
        Set(l.part, NextPreset(look_[l.part], step));
        hsv_ = ToHsv(Shown(look_, l.part));   // (the sliders follow the preset chosen)
        break;
    case What::kHue:
    case What::kSaturation:
    case What::kBrightness: {
        // (kept as hue, saturation and brightness while the mixer is open: a grey keeps its hue, and no press is
        // lost to rounding through red, green and blue)
        if (l.what == What::kHue) hsv_.h = ((hsv_.h + step * kHueStep) % 360 + 360) % 360;
        if (l.what == What::kSaturation) hsv_.s = std::clamp(hsv_.s + step * kLevelStep, 0, 100);
        if (l.what == What::kBrightness) hsv_.v = std::clamp(hsv_.v + step * kLevelStep, 0, 100);
        Set(l.part, ToRgb(hsv_));
        break;
    }
    default: break;
    }
}

void Page::Decide(std::size_t row) {
    const auto lines = Lines();
    if (row >= lines.size()) {
        return;
    }
    const Line& l = lines[row];
    switch (l.what) {
    case What::kPart:
        if (PresetOnly(l.part)) {   // (no mixer: A steps on, as Right)
            Change(row, 1);
            break;
        }
        list_selected_ = row;
        editing_ = l.part;
        hsv_ = ToHsv(Shown(look_, l.part));   // (from the colour it shows: its own, the game's, or the tunic's)
        Focus(l.part);
        selected_ = 1;   // Hue
        break;
    case What::kAllGame:
        ResetAll();
        break;
    case What::kGameOwn:   // (back to the game's colour and to the list of parts: nothing is left to mix)
        Set(l.part, std::nullopt);
        [[fallthrough]];
    case What::kDone:
        editing_.reset();
        selected_ = list_selected_;
        break;
    default: break;
    }
}

bool Page::Press(Input input) {
    switch (input) {
    case Input::kUp: Move(-1); return true;
    case Input::kDown: Move(1); return true;
    case Input::kLeft: Change(selected_, -1); return true;
    case Input::kRight: Change(selected_, 1); return true;
    case Input::kDecide: Decide(selected_); return true;
    case Input::kPagePrev: TurnPage(-1); return true;
    case Input::kPageNext: TurnPage(1); return true;
    case Input::kResetAll:
        if (!editing_) ResetAll();
        return true;
    case Input::kBack:
        if (editing_) {
            editing_.reset();
            selected_ = list_selected_;
            return true;
        }
        return false;
    }
    return false;
}

void Page::Select(std::size_t row) {
    if (Selectable(row)) {
        selected_ = row;
        Focus(Lines()[row].part);
    }
}

void Page::Click(std::size_t row, int side) {
    if (!Selectable(row)) {
        return;
    }
    Select(row);
    if (side == 0) {
        Decide(row);
    } else {
        Change(row, side);
    }
}

std::optional<CoopLook::Look> Page::TakeChange() {
    if (!changed_) {
        return std::nullopt;
    }
    changed_ = false;
    return look_;
}

View Page::view() const {
    const auto lines = Lines();
    View v;
    for (const Line& l : lines) {
        v.rows.push_back(l.row);
    }
    const std::size_t sel = std::min(selected_, lines.size() - 1);
    v.selected = static_cast<int>(sel);
    const Line& l = lines[sel];
    v.help_title = l.what == What::kAllGame ? std::string("Link's look") : Title(l.part);
    v.help = l.help;
    v.change = l.row.kind == 1 ? "Colour" : "";
    v.confirm = l.what == What::kPart ? "Mix" : l.row.kind == 0 ? "Choose" : "";
    v.back = "Back";
    // The picture, the part looked at outlined.
    const std::optional<Part> mark = editing_ ? editing_ : l.what == What::kPart ? std::optional<Part>(l.part) : std::nullopt;
    const auto doll = Doll(look_, tunic_, shield_, sword_);
    for (const Drawn& d : doll) {
        v.picture.push_back({(d.rgb << 8) | 0xFFu, false, d.points});
    }
    for (const Drawn& d : doll) {
        if (mark && d.part == mark) {
            v.picture.push_back({0xF5D061FFu, true, d.points});
        }
    }
    return v;
}

}  // namespace CoopLookMenu
