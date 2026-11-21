// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_original.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "coop_title_menu.h"
#include "hos_coop_mods.h"

namespace fs = std::filesystem;

namespace CoopOriginal {
namespace {

bool Hidden(const std::string& name) {
    return name.empty() || name.front() == '_' || name.front() == '-';
}

std::string Utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool Read(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

bool Write(const fs::path& p, const std::string& text) {
    const fs::path tmp = p.string() + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << text)) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

bool OnIn(std::string_view options_ini) {
    const auto v = CoopTitleMenu::IniGet(options_ini, kOptionKey);
    if (!v) return false;
    std::string t(CoopTitleMenu::Trim(*v));
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t == "on" || t == "1" || t == "yes" || t == "true";
}

std::string With(std::string_view options_ini, bool on) {
    return on ? CoopTitleMenu::IniSet(options_ini, kOptionKey, "on", true)
              : CoopTitleMenu::IniUnset(options_ini, kOptionKey);
}

std::vector<std::string> ModNames(const fs::path& mods) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& top : fs::directory_iterator(mods, ec)) {
        if (!top.is_directory(ec)) continue;
        const std::string name = Utf8(top.path().filename());
        if (Hidden(name)) continue;
        if (name != "contents") {
            out.push_back(name);
            continue;
        }
        for (const auto& title : fs::directory_iterator(top.path(), ec)) {
            if (!title.is_directory(ec)) continue;
            for (const auto& mod : fs::directory_iterator(title.path(), ec)) {
                const std::string m = Utf8(mod.path().filename());
                if (mod.is_directory(ec) && !Hidden(m)) {
                    out.push_back("contents/" + Utf8(title.path().filename()) + "/" + m);
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string OffList(const std::vector<std::string>& mods) {
    std::string out = "coop-mods " + std::to_string(HOS_COOP_MODS_VERSION) + "\n";
    out += "# Original game (co-op's Co-op settings): every mod left out. The list before is in ";
    out += kBackupFile;
    out += "; it comes back when the switch is turned off.\n";
    for (const auto& m : mods) out += "off " + m + "\n";
    return out;
}

bool ApplyMods(const fs::path& mods, bool on, std::string& why) {
    const fs::path list = mods / HOS_COOP_MODS_LIST_FILE, kept = mods / kBackupFile;
    std::error_code ec;
    if (on) {
        if (!fs::exists(kept, ec)) {
            std::string before;
            // (no list: an empty file marks it, so the way back removes ours instead of keeping it)
            if (!Read(list, before)) before.clear();
            if (!Write(kept, before)) {
                why = "could not keep " + Utf8(list) + " as " + kBackupFile;
                return false;
            }
        }
        if (!Write(list, OffList(ModNames(mods)))) {
            why = "could not write " + Utf8(list);
            return false;
        }
        return true;
    }
    if (!fs::exists(kept, ec)) return true;   // (never original here: nothing to put back)
    std::string before;
    if (!Read(kept, before)) {
        why = "could not read " + Utf8(kept);
        return false;
    }
    if (before.empty()) {
        fs::remove(list, ec);
    } else if (!Write(list, before)) {
        why = "could not write " + Utf8(list);
        return false;
    }
    fs::remove(kept, ec);
    return true;
}

void Tab::Open(fs::path options, fs::path mods) {
    options_ = std::move(options);
    mods_ = std::move(mods);
    std::string text;
    Read(options_, text);
    on_ = OnIn(text);
}

void Tab::Flip() {
    std::string text;
    Read(options_, text);
    const bool want = !on_;
    std::string why;
    if (!Write(options_, With(text, want))) {
        note_ = "Could not write " + Utf8(options_) + ".";
        return;
    }
    if (!ApplyMods(mods_, want, why)) {
        note_ = "The switch is saved, but " + why + ".";
    } else {
        note_.clear();
    }
    on_ = want;
    changed_ = !changed_;
}

bool Tab::Page(HosCoopMenuPage* out) {
    if (out == nullptr || !Shown()) return false;
    status_ = !note_.empty() ? note_
              : changed_     ? std::string("Start the game again to finish: ") +
                               (on_ ? "it comes back as the original game." : "co-op and its extras come back.")
              : on_ ? std::string("Co-op, its menus, Link's look, the pack and mods are all off.")
                    : std::string("Co-op and its extras are on.");
    rows_.clear();
    HosCoopMenuRow r{};
    r.name = "Original game";
    r.value = "";
    r.kind = HOS_COOP_MENU_TOGGLE;
    r.on = on_ ? 1 : 0;
    rows_.push_back(r);
    HosCoopMenuRow n{};
    n.name = status_.c_str();
    n.value = "";
    n.kind = HOS_COOP_MENU_NOTE;
    rows_.push_back(n);
    HosCoopMenuRow w{};
    w.name = "Turn it off and start the game again to have co-op and the extras back.";
    w.value = "";
    w.kind = HOS_COOP_MENU_NOTE;
    if (on_ && !changed_) rows_.push_back(w);
    out->rows_n = static_cast<int32_t>(rows_.size());
    out->rows = rows_.data();
    out->selected = 0;
    out->typing = 0;
    out->help_title = "Original game";
    out->help = "Everything this kit adds stays off: the game plays as it came. Your saves are not touched.";
    out->hint = "It counts from the next start.";
    out->gold = "";
    out->change = "Change";
    out->confirm = "Change";
    out->back = "Close";
    return true;
}

bool Tab::Input(int event, int arg, int arg2) {
    (void)arg2;
    switch (event) {
    case HOS_COOP_MENU_LEFT:
        if (on_) Flip();
        return true;
    case HOS_COOP_MENU_RIGHT:
        if (!on_) Flip();
        return true;
    case HOS_COOP_MENU_CONFIRM:
        Flip();
        return true;
    case HOS_COOP_MENU_CLICK:   // (the value's left half Off, its right half On, the name flips)
        if (arg == 0 && (arg2 == 0 || (arg2 < 0) == on_)) Flip();
        return true;
    case HOS_COOP_MENU_BACK:
        return false;   // (nothing to go back to: Longshot closes the menu)
    default:
        return true;
    }
}

}  // namespace CoopOriginal
