// coop_mods_tab.h: the list page and a mod's page, built fresh from the
// Loader at every view; the module's glue at the end.
#include "coop_mods_tab.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <utility>


namespace CoopModsTab {

namespace {

using CoopPauseMenu::Row;
using CoopPauseMenu::RowKind;

std::string NumberText(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

void AppendUtf8(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void EraseUtf8(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) return;
    }
}

Row Make(std::string name, std::string value, RowKind kind) {
    Row r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.kind = kind;
    return r;
}

}  // namespace

Tab::Tab(Sources sources) : src_(std::move(sources)) {}

void Tab::Say(std::string line) {
    note_ = std::move(line);
}

void Tab::Show() {
    if (src_.loader != nullptr) src_.loader->Refresh();
    note_.clear();
    Clamp();
}

void Tab::Hide() {
    typing_ = false;
    typed_.clear();
}

std::vector<Tab::Line> Tab::Lines() const {
    std::vector<Line> out;
    if (src_.loader == nullptr) return out;
    const std::vector<CoopMods::Row> rows = src_.loader->Rows();
    const auto note = [&](std::string text) {
        Line l;
        l.row = Make(std::move(text), "", RowKind::kNote);
        l.act = Act::kNote;
        out.push_back(std::move(l));
    };
    if (mod_.empty()) {
        const CoopMods::Restart restart = src_.loader->RestartNeeded();
        for (const std::string& why : restart.why) note("Restart needed: " + why);
        if (src_.coop) {
            for (std::string& line : src_.coop()) note(std::move(line));
        }
        if (src_.rando) {
            if (std::string r = src_.rando(); !r.empty()) note("Randomizer: " + r);
        }
        if (rows.empty()) {
            note("No mods in " + CoopMods::ToUtf8(src_.loader->Root()) + ". Put each mod in its own folder there.");
        }
        for (const CoopMods::Row& m : rows) {
            Line l;
            l.row = Make(m.title + (m.version.empty() || m.version == "0" ? "" : " " + m.version), "",
                         RowKind::kToggle);
            l.row.on = m.on;
            l.row.fixed = !m.can_switch;
            l.act = Act::kMod;
            l.name = m.name;
            l.help = m.description;
            if (!m.note.empty()) l.help += (l.help.empty() ? "" : "\n") + m.note;
            out.push_back(std::move(l));
        }
        return out;
    }
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const CoopMods::Row& r) { return r.name == mod_; });
    if (it == rows.end()) {
        note("This mod is gone from the folder.");
        return out;
    }
    const CoopMods::Row& m = *it;
    Line head;
    head.row = Make(m.title, "", RowKind::kHeading);
    out.push_back(head);
    if (!m.description.empty()) note(m.description);
    if (!m.author.empty()) note("By " + m.author + (m.version.empty() ? "" : ", version " + m.version));
    if (!m.note.empty()) note(m.note);
    Line on;
    on.row = Make("On", "", RowKind::kToggle);
    on.row.on = m.on;
    on.row.fixed = !m.can_switch;
    on.act = Act::kOn;
    on.help = m.can_switch ? "Off: the game leaves this mod out from its next start." : m.note;
    out.push_back(on);
    for (const auto& [act, name, help] :
         {std::tuple{Act::kUp, "Move up", "Up: it wins a file it shares with the mods under it."},
          std::tuple{Act::kDown, "Move down", "Down: the mods above it win a file they share."}}) {
        Line l;
        l.row = Make(name, "", RowKind::kAction);
        l.row.fixed = !m.can_move;
        l.act = act;
        l.help = help;
        out.push_back(l);
    }
    const std::vector<CoopMods::SettingRow> settings = src_.loader->Settings(mod_);
    if (!settings.empty()) {
        Line h;
        h.row = Make("Settings", "", RowKind::kHeading);
        out.push_back(h);
    }
    for (const CoopMods::SettingRow& s : settings) {
        Line l;
        l.act = Act::kSetting;
        l.name = s.setting.key;
        const std::string caption = s.setting.name.empty() ? s.setting.key : s.setting.name;
        switch (s.setting.type) {
        case CoopMods::SettingType::Toggle:
            l.row = Make(caption, "", RowKind::kToggle);
            l.row.on = s.value == "1";
            break;
        case CoopMods::SettingType::Choice: {
            std::string shown = s.value;
            std::size_t at = 0;
            for (std::size_t i = 0; i < s.setting.choices.size(); ++i) {
                if (s.setting.choices[i].first == s.value) {
                    shown = s.setting.choices[i].second.empty() ? s.value : s.setting.choices[i].second;
                    at = i;
                }
            }
            l.row = Make(caption, shown, RowKind::kChoice);
            l.row.left = at > 0;
            l.row.right = at + 1 < s.setting.choices.size();
            break;
        }
        case CoopMods::SettingType::Number: {
            l.row = Make(caption, s.value, RowKind::kChoice);
            const double v = std::atof(s.value.c_str());
            l.row.left = v > s.setting.min;
            l.row.right = v < s.setting.max;
            break;
        }
        case CoopMods::SettingType::Text:
            l.row = Make(caption, s.value.empty() ? "(empty)" : s.value, RowKind::kAction);
            break;
        }
        l.row.fixed = s.locked;
        l.help = s.locked ? "In this co-op game the host sets this." : s.setting.help;
        if (s.setting.restart) l.help += (l.help.empty() ? "" : " ") + std::string("Counts after a restart.");
        out.push_back(std::move(l));
    }
    return out;
}

bool Tab::Selectable(const Line& l) {
    return l.row.kind != RowKind::kHeading && l.row.kind != RowKind::kNote;
}

void Tab::Clamp() {
    const std::vector<Line> lines = Lines();
    if (lines.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = std::min(selected_, lines.size() - 1);
    if (Selectable(lines[selected_])) return;
    for (std::size_t i = selected_; i < lines.size(); ++i) {
        if (Selectable(lines[i])) {
            selected_ = i;
            return;
        }
    }
    for (std::size_t i = selected_; i-- > 0;) {
        if (Selectable(lines[i])) {
            selected_ = i;
            return;
        }
    }
}

void Tab::Move(int step) {
    const std::vector<Line> lines = Lines();
    const auto n = static_cast<std::ptrdiff_t>(lines.size());
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(selected_) + step; i >= 0 && i < n; i += step) {
        if (Selectable(lines[static_cast<std::size_t>(i)])) {
            selected_ = static_cast<std::size_t>(i);
            return;
        }
    }
}

void Tab::Change(const Line& l, int step) {
    CoopMods::Loader& loader = *src_.loader;
    std::string problem;
    bool ok = true;
    switch (l.act) {
    case Act::kMod:
    case Act::kOn: {
        const std::string name = l.act == Act::kMod ? l.name : mod_;
        if (l.row.kind == RowKind::kToggle && ((step > 0) != l.row.on)) {
            ok = loader.SetOn(name, step > 0, &problem);
        }
        break;
    }
    case Act::kSetting: {
        for (const CoopMods::SettingRow& s : loader.Settings(mod_)) {
            if (s.setting.key != l.name) continue;
            if (s.locked) {
                ok = false;
                problem = "In this co-op game the host sets this.";
                break;
            }
            std::string value = s.value;
            if (s.setting.type == CoopMods::SettingType::Toggle) {
                value = step > 0 ? "1" : "0";
            } else if (s.setting.type == CoopMods::SettingType::Choice) {
                const auto& c = s.setting.choices;
                auto at = std::find_if(c.begin(), c.end(), [&](const auto& p) { return p.first == s.value; });
                const std::ptrdiff_t i = at == c.end() ? 0 : (at - c.begin()) + step;
                if (i < 0 || i >= static_cast<std::ptrdiff_t>(c.size())) break;
                value = c[static_cast<std::size_t>(i)].first;
            } else if (s.setting.type == CoopMods::SettingType::Number) {
                const double v = std::clamp(std::atof(s.value.c_str()) + step * s.setting.step, s.setting.min,
                                            s.setting.max);
                value = NumberText(v);
            } else {
                break;
            }
            if (value != s.value) ok = loader.Set(mod_, l.name, value, &problem);
        }
        break;
    }
    default:
        break;
    }
    if (!ok) Say(problem);
}

void Tab::Decide(const Line& l) {
    std::string problem;
    switch (l.act) {
    case Act::kMod:
        list_selected_ = selected_;
        mod_ = l.name;
        selected_ = 0;
        Clamp();
        return;
    case Act::kOn:
        Change(l, l.row.on ? -1 : 1);
        return;
    case Act::kUp:
    case Act::kDown:
        if (!src_.loader->Move(mod_, l.act == Act::kUp ? -1 : 1, &problem)) Say(problem);
        return;
    case Act::kSetting:
        if (l.row.fixed) {
            Say("In this co-op game the host sets this.");
            return;
        }
        if (l.row.kind == RowKind::kToggle) {
            Change(l, l.row.on ? -1 : 1);
        } else if (l.row.kind == RowKind::kAction) {  // text: type it
            for (const CoopMods::SettingRow& s : src_.loader->Settings(mod_)) {
                if (s.setting.key == l.name) typed_ = s.value;
            }
            typing_ = true;
        } else {
            Change(l, 1);
        }
        return;
    default:
        return;
    }
}

bool Tab::Press(Input input) {
    if (src_.loader == nullptr) return false;
    const std::vector<Line> lines = Lines();
    if (typing_) {
        if (input == Input::kErase) {
            EraseUtf8(typed_);
        } else if (input == Input::kDecide && selected_ < lines.size()) {
            std::string problem;
            if (!src_.loader->Set(mod_, lines[selected_].name, typed_, &problem)) Say(problem);
            typing_ = false;
        } else if (input == Input::kBack) {
            typing_ = false;
        }
        return true;
    }
    note_.clear();
    switch (input) {
    case Input::kUp: Move(-1); break;
    case Input::kDown: Move(1); break;
    case Input::kLeft:
    case Input::kRight:
        if (selected_ < lines.size()) Change(lines[selected_], input == Input::kRight ? 1 : -1);
        break;
    case Input::kDecide:
        if (selected_ < lines.size()) Decide(lines[selected_]);
        break;
    case Input::kBack:
        if (mod_.empty()) return false;
        mod_.clear();
        selected_ = list_selected_;
        break;
    case Input::kErase: break;
    }
    Clamp();
    return true;
}

void Tab::Select(std::size_t row) {
    if (typing_) return;
    const std::vector<Line> lines = Lines();
    if (row < lines.size() && Selectable(lines[row])) selected_ = row;
}

void Tab::Click(std::size_t row, int side) {
    Select(row);
    const std::vector<Line> lines = Lines();
    if (typing_ || row != selected_ || row >= lines.size()) return;
    note_.clear();
    if (side != 0 && (lines[row].row.kind == RowKind::kToggle || lines[row].row.kind == RowKind::kChoice)) {
        Change(lines[row], side);
    } else {
        Decide(lines[row]);
    }
    Clamp();
}

bool Tab::Char(std::uint32_t cp) {
    if (!typing_) return false;
    if (cp >= 0x20 && cp != 0x7F && typed_.size() < CoopMods::kMaxSettingValue) AppendUtf8(typed_, cp);
    return true;
}

CoopPauseMenu::View Tab::view() const {
    CoopPauseMenu::View v;
    const std::vector<Line> lines = Lines();
    for (const Line& l : lines) v.rows.push_back(l.row);
    if (selected_ < lines.size() && Selectable(lines[selected_])) {
        v.selected = static_cast<int>(selected_);
        const Line& l = lines[selected_];
        v.help_title = l.row.name;
        v.help = l.help;
        if (typing_) {
            v.rows[selected_].value = typed_ + "_";
            v.typing = true;
            v.help = "Type, then Enter. Esc: as it was.";
        }
    }
    if (!note_.empty()) {
        v.gold = note_;
    } else if (src_.loader != nullptr && src_.loader->RestartNeeded().needed) {
        v.gold = "Restart the game to use the changes.";
    }
    v.change = typing_ ? "" : "Switch";
    v.confirm = typing_ ? "Done" : mod_.empty() ? "Open" : "Choose";
    v.back = mod_.empty() && !typing_ ? "Close" : "Back";
    return v;
}

}  // namespace CoopModsTab
