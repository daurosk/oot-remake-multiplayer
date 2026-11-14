// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_menu.h"

#include <algorithm>
#include <cmath>

#include "coop_colors.h"

namespace CoopLookMenu {

using CoopLook::Part;
using CoopLook::PartInfo;

namespace {

constexpr int kStep = 8;   // a press on Red, Green or Blue

enum class What { kHeading, kPart, kAllGame, kRed, kGreen, kBlue, kPreset, kGameOwn, kDone };

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
    case Part::kokiri_blade:
    case Part::master_blade:
    case Part::biggoron_blade: return "The blade.";
    case Part::kokiri_hilt: return "The leather grip.";
    case Part::master_hilt: return "The blue grip and wings; the gold and the gem stay.";
    case Part::biggoron_hilt: return "The blue grip.";
    case Part::boots: return "The leather of every pair: the Kokiri, Hover and Iron Boots.";
    case Part::gauntlets: return "The metal of the Silver and the Golden Gauntlets.";
    }
    return "";
}

std::string ValueText(const std::optional<std::uint32_t>& colour) {
    return colour ? CoopLook::ColourText(colour) : std::string("Game's own");
}

std::string Title(Part part) {
    const PartInfo& info = CoopLook::InfoOf(part);
    return info.item == std::string(info.name) ? std::string(info.item) : std::string(info.item) + ": " + info.name;
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

const std::vector<std::string>& Presets() {
    static const std::vector<std::string> kPresets = {"",     "red",    "orange", "yellow", "green", "cyan",
                                                      "blue", "purple", "pink",   "white",  "black"};
    return kPresets;
}

std::uint32_t Shown(const CoopLook::Look& look, Part part) {
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
        const std::uint32_t c = Shown(look_, p);
        out.push_back({{Title(p), "", 3}, What::kHeading, p, ""});
        const char* names[3] = {"Red", "Green", "Blue"};
        const What whats[3] = {What::kRed, What::kGreen, What::kBlue};
        for (int i = 0; i < 3; ++i) {
            const int v = static_cast<int>((c >> (16 - 8 * i)) & 0xFF);
            out.push_back({{names[i], std::to_string(v), 1, v > 0, v < 255}, whats[i], p,
                           "Left and Right: less or more of it, " + std::to_string(kStep) + " a press (0 to 255)."});
        }
        std::string preset = "Mixed";
        for (const std::string& name : Presets()) {
            if (!name.empty() && look_[p] && CoopColors::Parse(name) == look_[p]) {
                preset = name;
            }
        }
        if (!look_[p]) {
            preset = "Game's own";
        }
        out.push_back({{"Preset", preset, 1, true, true}, What::kPreset, p, "Left and Right: the preset colours."});
        out.push_back({{"Game's own colour", "", 0}, What::kGameOwn, p, "This part back to the game's own colour."});
        out.push_back({{"Done", "", 0}, What::kDone, p, "Back to the list of parts."});
        return out;
    }
    std::string item;
    for (const PartInfo& info : CoopLook::Parts()) {
        if (item != info.item) {
            item = info.item;
            out.push_back({{item, "", 3}, What::kHeading, info.part, ""});
        }
        out.push_back({{info.name, ValueText(look_[info.part]), 1, true, true}, What::kPart, info.part,
                       std::string(Note(info.part)) + " Left and Right try the colours; Confirm mixes your own. Your Link "
                       "wears it when you close the menu (the other players see only the tunic's colour for now)."});
    }
    out.push_back({{"All parts back to the game's own", "", 0}, What::kAllGame, Part::tunic,
                   "Every part in the game's own colours again (the tunic: your player number's colour for the others)."});
    return out;
}

bool Page::Selectable(std::size_t row) const {
    const auto lines = Lines();
    return row < lines.size() && lines[row].what != What::kHeading;
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
    if (IsTunic(part)) {
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
        if (lines[selected_].what != What::kHeading) {
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
    case What::kPreset: Set(l.part, NextPreset(look_[l.part], step)); break;
    case What::kRed:
    case What::kGreen:
    case What::kBlue: {
        const int shift = l.what == What::kRed ? 16 : l.what == What::kGreen ? 8 : 0;
        const std::uint32_t c = Shown(look_, l.part);
        const int v = std::clamp(static_cast<int>((c >> shift) & 0xFF) + step * kStep, 0, 255);
        Set(l.part, (c & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(v) << shift));
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
        list_selected_ = row;
        editing_ = l.part;
        Focus(l.part);
        selected_ = 1;   // Red
        break;
    case What::kAllGame:
        for (const PartInfo& info : CoopLook::Parts()) {
            Set(info.part, std::nullopt);
        }
        break;
    case What::kGameOwn: Set(l.part, std::nullopt); break;
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
