// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look.h"

#include <cctype>
#include <mutex>
#include <utility>

#include "coop_colors.h"
#include "coop_title_menu.h"

namespace CoopLook {

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

bool WriteSettings(const Look& look, std::string& coop_ini, std::string* options_ini) {
    std::string& text = options_ini != nullptr ? *options_ini : coop_ini;
    const std::string colour = ColourText(look[Part::tunic]);
    std::string out = colour.empty() ? CoopTitleMenu::IniUnset(text, "color")
                                     : CoopTitleMenu::IniSet(text, "color", colour, true);
    if (out == text) {
        return false;
    }
    text = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------

namespace {
std::mutex g_mutex;
Choice g_choice;           // guarded
Apply g_apply;             // guarded
bool g_to_apply = false;   // guarded: ApplyOnUnpause's, for the next Frame
}  // namespace

void Init(const Look& worn, Apply apply) {
    std::lock_guard lock(g_mutex);
    g_choice = Choice(worn);
    g_apply = std::move(apply);
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
    {
        std::lock_guard lock(g_mutex);
        if (!std::exchange(g_to_apply, false)) {
            return;
        }
        look = g_choice.Unpause();
        apply = g_apply;
    }
    if (look && apply) {
        apply(*look);   // outside the lock: it may read and write files
    }
}

}  // namespace CoopLook
