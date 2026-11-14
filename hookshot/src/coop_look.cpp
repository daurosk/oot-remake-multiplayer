// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look.h"

#include <cctype>
#include <mutex>
#include <utility>

#include "coop_colors.h"
#include "coop_title_menu.h"

namespace CoopLook {

const std::array<PartInfo, kParts>& Parts() {
    static const std::array<PartInfo, kParts> kTable{{
        {Part::tunic, "tunic", "Tunic", "Kokiri Tunic"},
        {Part::goron_tunic, "goron_tunic", "Tunic", "Goron Tunic"},
        {Part::zora_tunic, "zora_tunic", "Tunic", "Zora Tunic"},
        {Part::hylian_frame, "hylian_frame", "Hylian Shield", "Frame"},
        {Part::hylian_face, "hylian_face", "Hylian Shield", "Face"},
        {Part::hylian_emblem, "hylian_emblem", "Hylian Shield", "Emblem"},
        {Part::mirror_rim, "mirror_rim", "Mirror Shield", "Rim"},
        {Part::mirror_face, "mirror_face", "Mirror Shield", "Mirror"},
        {Part::deku_shield, "deku_shield", "Deku Shield", "Wood"},
        {Part::kokiri_blade, "kokiri_blade", "Kokiri Sword", "Blade"},
        {Part::kokiri_hilt, "kokiri_hilt", "Kokiri Sword", "Hilt"},
        {Part::master_blade, "master_blade", "Master Sword", "Blade"},
        {Part::master_hilt, "master_hilt", "Master Sword", "Hilt"},
        {Part::biggoron_blade, "biggoron_blade", "Biggoron's Sword", "Blade"},
        {Part::biggoron_hilt, "biggoron_hilt", "Biggoron's Sword", "Hilt"},
        {Part::boots, "boots", "Boots", "Leather"},
        {Part::gauntlets, "gauntlets", "Gauntlets", "Metal"},
    }};
    return kTable;
}

const PartInfo& InfoOf(Part part) {
    for (const PartInfo& info : Parts()) {
        if (info.part == part) {
            return info;
        }
    }
    return Parts()[0];   // (every Part is in the table: look_test checks it)
}

std::optional<Part> PartOf(std::uint8_t number) {
    if (number >= kParts) {
        return std::nullopt;
    }
    return static_cast<Part>(number);
}

std::string ColourText(const std::optional<std::uint32_t>& colour) {
    if (!colour) {
        return {};
    }
    // A colour that has a name is written by its name, as a player writes it.
    const std::string_view names = CoopColors::Names();
    for (std::size_t at = 0; at < names.size();) {
        std::size_t end = names.find(',', at);
        if (end == std::string_view::npos) {
            end = names.size();
        }
        std::string_view name = names.substr(at, end - at);
        while (!name.empty() && name.front() == ' ') {
            name.remove_prefix(1);
        }
        if (!name.empty() && CoopColors::Parse(name) == colour) {
            return std::string(name);
        }
        at = end + 1;
    }
    return CoopColors::Text(*colour);
}

std::string TunicName(const std::optional<std::uint32_t>& colour) {
    std::string name(colour ? CoopColors::TunicOf(*colour) : std::string_view());
    if (name.empty()) {
        return "green";
    }
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

void Choice::SetPending(const Look& look) {
    if (look == worn_) {
        pending_.reset();
    } else {
        pending_ = look;
    }
}

std::optional<Look> Choice::Unpause() {
    if (!pending_) {
        return std::nullopt;
    }
    worn_ = *std::exchange(pending_, std::nullopt);
    return worn_;
}

namespace {
std::string SettingsKey(const PartInfo& info) {
    return info.part == Part::tunic ? std::string("color") : "look_" + std::string(info.key);
}
}  // namespace

bool WriteSettings(const Look& look, std::string& coop_ini, std::string* options_ini) {
    std::string& text = options_ini != nullptr ? *options_ini : coop_ini;
    std::string out = text;
    for (const PartInfo& info : Parts()) {
        const std::string colour = ColourText(look[info.part]);
        const std::string key = SettingsKey(info);
        if (colour.empty()) {
            // An unset part other than the tunic is left out of a file that never had it.
            if (info.part == Part::tunic || CoopTitleMenu::IniGet(out, key)) {
                out = CoopTitleMenu::IniUnset(out, key);
            }
        } else {
            out = CoopTitleMenu::IniSet(out, key, colour, true);
        }
    }
    if (out == text) {
        return false;
    }
    text = std::move(out);
    return true;
}

Look ReadParts(std::string_view text, std::vector<std::string>* errors) {
    Look look;
    for (const PartInfo& info : Parts()) {
        if (info.part == Part::tunic) {
            continue;
        }
        const std::string key = SettingsKey(info);
        const auto value = CoopTitleMenu::IniGet(text, key);
        if (!value || value->empty()) {
            continue;
        }
        if (const auto colour = CoopColors::Parse(*value)) {
            look[info.part] = colour;
        } else if (errors != nullptr) {
            errors->push_back(key + " = " + *value + " is not a colour (a name such as red, or #RRGGBB)");
        }
    }
    return look;
}

std::vector<std::pair<std::uint8_t, std::uint32_t>> Wire(const Look& look) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> out;
    for (std::size_t i = 0; i < kParts; ++i) {
        if (look.colour[i]) {
            out.emplace_back(static_cast<std::uint8_t>(i), *look.colour[i] & 0xFFFFFFu);
        }
    }
    return out;
}

Look FromWire(const std::vector<std::pair<std::uint8_t, std::uint32_t>>& parts) {
    Look look;
    for (const auto& [number, colour] : parts) {
        if (const auto part = PartOf(number)) {
            look[*part] = colour & 0xFFFFFFu;
        }
    }
    return look;
}

// ---------------------------------------------------------------------------

namespace {
std::mutex g_mutex;
Choice g_choice;           // guarded
Apply g_apply;             // guarded
std::function<void()> g_unpaused;   // guarded
bool g_to_apply = false;   // guarded: ApplyOnUnpause's, for the next Frame
}  // namespace

void Init(const Look& worn, Apply apply, std::function<void()> unpaused) {
    std::lock_guard lock(g_mutex);
    g_choice = Choice(worn);
    g_apply = std::move(apply);
    g_unpaused = std::move(unpaused);
    g_to_apply = false;
}

Look Worn() {
    std::lock_guard lock(g_mutex);
    return g_choice.Worn();
}

Look Shown() {
    std::lock_guard lock(g_mutex);
    return g_choice.Shown();
}

void SetPending(const Look& look) {
    std::lock_guard lock(g_mutex);
    g_choice.SetPending(look);
}

void ApplyOnUnpause() {
    std::lock_guard lock(g_mutex);
    g_to_apply = true;
}

void Frame() {
    std::optional<Look> look;
    Apply apply;
    std::function<void()> unpaused;
    {
        std::lock_guard lock(g_mutex);
        if (!std::exchange(g_to_apply, false)) {
            return;
        }
        look = g_choice.Unpause();
        apply = g_apply;
        unpaused = g_unpaused;
    }
    if (look && apply) {
        apply(*look);   // outside the lock: it may read and write files
    } else if (!look && unpaused) {
        unpaused();
    }
}

}  // namespace CoopLook
