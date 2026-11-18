// coop_mods_tab.h: the list page and a mod's page, built fresh from the
// Loader at every view; the module's glue at the end.
#include "coop_mods_tab.h"

#include "coop_rando.h"
#include "coop_i18n.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
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

// Text from the clipboard made fit for one setting: valid UTF-8 only (a broken byte is dropped), no control
// characters (a pasted line break or tab is dropped, so a string copied over two lines joins), no spaces at
// either end. It stops on a character's edge, never inside one, once it passes `room` bytes (`cut` says so).
std::string CleanPaste(std::string_view in, std::size_t room, bool* cut) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3
                              : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        bool ok = n != 0 && i + n <= in.size();
        for (std::size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (ok && n >= 3) {   // no overlong forms, no surrogates, nothing past U+10FFFF
            const unsigned char d = static_cast<unsigned char>(in[i + 1]);
            if (n == 3) ok = !(c == 0xE0 && d < 0xA0) && !(c == 0xED && d >= 0xA0);
            else ok = !(c == 0xF0 && d < 0x90) && !(c == 0xF4 && d >= 0x90);
        }
        if (!ok || (n == 1 && (c < 0x20 || c == 0x7F))) {
            ++i;
            continue;
        }
        if (out.size() + n > room) {
            if (cut != nullptr) *cut = true;
            break;
        }
        out.append(in.substr(i, n));
        i += n;
    }
    std::size_t b = 0, e = out.size();
    while (b < e && out[b] == ' ') ++b;
    while (e > b && out[e - 1] == ' ') --e;
    return out.substr(b, e - b);
}

// The text's characters (code points) as start offsets, for cutting a long value on a character's edge.
std::vector<std::size_t> CharStarts(const std::string& s) {
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) at.push_back(i);
    }
    return at;
}

constexpr std::size_t kShownChars = 40;   // a long value shows this many characters, not the whole of it

// While typing: the END of the text (that is where the next character goes), "..." before it when cut.
std::string TailShown(const std::string& s) {
    const std::vector<std::size_t> at = CharStarts(s);
    if (at.size() <= kShownChars) return s;
    return "..." + s.substr(at[at.size() - (kShownChars - 3)]);
}

// Not typing: a long value shows its start and its end, so one pasted string can be told from another.
std::string EndsShown(const std::string& s) {
    const std::vector<std::size_t> at = CharStarts(s);
    if (at.size() <= kShownChars) return s;
    const std::size_t part = (kShownChars - 3) / 2;
    return s.substr(0, at[part]) + "..." + s.substr(at[at.size() - part]);
}

Row Make(std::string name, std::string value, RowKind kind) {
    Row r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.kind = kind;
    return r;
}

// A glyph and what it means for a break: its bytes, the cells it takes (a Chinese
// character is two), and whether a line may end after it.
struct Piece {
    std::string bytes;
    int cells = 1;
    bool space = false;
    bool stops = false;
};

int Cells(std::uint32_t cp) {
    return cp >= 0x1100 ? 2 : 1;   // the wide scripts: CJK, kana, hangul, the fullwidth forms
}

bool Stops(std::uint32_t cp) {
    switch (cp) {
    case ',':
    case '.':
    case ';':
    case ':':
    case '!':
    case '?':
    case 0x3001:   // 、
    case 0x3002:   // 。
    case 0xFF0C:   // ，
    case 0xFF1B:   // ；
    case 0xFF1A:   // ：
    case 0xFF01:   // ！
    case 0xFF1F:   // ？
    case 0x300D:   // 」
    case 0x300F:   // 』
    case 0x3011:   // 】
    case 0xFF09:   // ）
    case 0xFF3D:   // ］
        return true;
    default:
        return false;
    }
}

std::vector<Piece> Pieces(const std::string& text) {
    std::vector<Piece> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = b;
        std::size_t n = 1;
        if ((b & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = b & 0x1Fu;
            n = 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = b & 0x0Fu;
            n = 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = b & 0x07u;
            n = 4;
        }
        for (std::size_t k = 1; k < n; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        Piece p;
        p.bytes = text.substr(i, n);
        p.cells = Cells(cp);
        p.space = cp == ' ';
        p.stops = Stops(cp);
        out.push_back(std::move(p));
        i += n;
    }
    return out;
}

// A row takes about 85 cells at the menu's usual scale (a line of 44 Chinese
// characters before the "…"); 76 keeps a margin for a larger UI scale.
constexpr std::size_t kNoteCells = 76;

}  // namespace

std::vector<std::string> NoteLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        std::vector<Piece> left = Pieces(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        while (!left.empty()) {
            std::size_t cells = 0;
            std::size_t stop = 0;
            std::size_t space = 0;
            std::size_t take = 0;
            for (; take < left.size(); ++take) {
                if (cells + static_cast<std::size_t>(left[take].cells) > kNoteCells && take > 0) {
                    break;
                }
                cells += static_cast<std::size_t>(left[take].cells);
                if (left[take].stops) stop = take + 1;
                if (left[take].space) space = take + 1;
            }
            std::size_t cut = take;
            if (take < left.size()) {
                // A break that reads: after a full stop or a comma near the end, else at a space.
                if (stop > 0 && stop * 2 >= take) {
                    cut = stop;
                } else if (space > 0 && space * 2 >= take) {
                    cut = space;
                }
            }
            if (cut == 0) {
                cut = 1;
            }
            std::string line;
            for (std::size_t i = 0; i < cut; ++i) {
                line += left[i].bytes;
            }
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            if (!line.empty()) {
                out.push_back(std::move(line));
            }
            left.erase(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(cut));
            while (!left.empty() && left.front().space) {
                left.erase(left.begin());
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

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
    paste_wanted_ = false;
    paste_for_.clear();
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
        for (const std::string& why : restart.why) {
            for (const std::string& line : NoteLines("Restart needed: " + why)) note(line);
        }
        if (src_.coop) {
            for (const std::string& line : src_.coop()) {
                for (const std::string& part : NoteLines(line)) note(part);
            }
        }
        if (rows.empty()) {
            for (const std::string& line : NoteLines("No mods in " + CoopMods::ToUtf8(src_.loader->Root()) +
                                                     ". Put each mod in its own folder there.")) {
                note(line);
            }
        }
        for (const CoopMods::Row& m : rows) {
            Line l;
            if (m.id == CoopRando::kId) {
                // The randomizer's row is its name and a Settings button: its own switch ("Randomizer") is on its
                // page, so an Off / On here (the mod's) would read as the randomizer's.
                l.row = Make(m.title, "Settings", RowKind::kAction);
            } else {
                l.row = Make(m.title + (m.version.empty() || m.version == "0" ? "" : " " + m.version), "",
                             RowKind::kToggle);
                l.row.on = m.on;
                l.row.fixed = !m.can_switch;
            }
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
    if (!m.description.empty()) {
        for (const std::string& line : NoteLines(m.description)) note(line);
    }
    if (!m.author.empty()) {
        // The table's words around the author and the version, and the author's own
        // text through it too (the kit's randomizer writes a sentence there, which
        // the table has; a mod's own name is not in it and stays as it is).
        std::string by = CoopI18n::Tr("By") + " " + CoopI18n::Tr(m.author);
        if (!m.version.empty()) {
            by += CoopI18n::Tr(", version") + " " + m.version;
        }
        note(std::move(by));
    }
    if (!m.note.empty()) {
        for (const std::string& line : NoteLines(m.note)) note(line);
    }
    if (m.id == CoopRando::kId && src_.rando) {
        if (std::string r = src_.rando(); !r.empty()) note("Randomizer: " + r);
    }
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
            l.row = Make(caption, s.value.empty() ? "(empty)" : EndsShown(s.value), RowKind::kAction);
            break;
        }
        l.row.fixed = s.locked;
        // The setting's own help and the restart note are two lines of the table,
        // put together here: the joined paragraph ("... (for it). Counts after a
        // restart.") is not a line of any table, so it would stay English.
        l.help = s.locked ? CoopI18n::Tr("In this co-op game the host sets this.") : s.setting.help;
        if (s.setting.restart) {
            l.help += (l.help.empty() ? "" : " ") + CoopI18n::Tr("Counts after a restart.");
        }
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
        if (selected_ < lines.size()) {
            if (IsText(lines[selected_])) {   // a text row has no sides: Left / Right is Paste (the pad's way)
                AskPaste(lines[selected_]);
            } else {
                Change(lines[selected_], input == Input::kRight ? 1 : -1);
            }
        }
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
    } else if (side != 0 && IsText(lines[row])) {
        AskPaste(lines[row]);
    } else {
        Decide(lines[row]);
    }
    Clamp();
}

bool Tab::IsText(const Line& l) {
    return l.act == Act::kSetting && l.row.kind == RowKind::kAction;
}

std::size_t Tab::MaxLength(const std::string& key) const {
    if (src_.loader != nullptr) {
        for (const CoopMods::SettingRow& s : src_.loader->Settings(mod_)) {
            if (s.setting.key == key) return std::min(s.setting.max_length, CoopMods::kMaxSettingValue);
        }
    }
    return CoopMods::kMaxSettingValue;
}

void Tab::AskPaste(const Line& l) {
    if (l.row.fixed) {
        Say("In this co-op game the host sets this.");
        return;
    }
    paste_wanted_ = true;
    paste_for_ = mod_ + "\n" + l.name;
}

bool Tab::TakePasteRequest() {
    return std::exchange(paste_wanted_, false);
}

void Tab::Paste(const std::optional<std::string>& clip, bool into_typing) {
    if (src_.loader == nullptr) return;
    const std::vector<Line> lines = Lines();
    if (selected_ >= lines.size() || !IsText(lines[selected_])) return;
    const Line& l = lines[selected_];
    if (into_typing ? !typing_ : (typing_ || paste_for_ != mod_ + "\n" + l.name)) {
        return;   // the player left the field (or the row, or the page) while the text came: it is not for this one
    }
    if (!clip) {
        Say("The clipboard could not be read: copy the text again, then Paste.");
        return;
    }
    const std::size_t max = MaxLength(l.name);
    const std::size_t room = into_typing ? max - std::min(typed_.size(), max) : max;
    bool cut = false;
    const std::string text = CleanPaste(*clip, room, &cut);
    if (text.empty()) {
        Say(cut ? "No room left in this field."
                : clip->empty() ? "The clipboard has no text." : "The clipboard has no text this setting can take.");
        return;
    }
    if (into_typing) {   // Ctrl+V while typing: after what is typed, as a text field does
        typed_ += text;
        if (cut) Say("The text was longer than this setting takes (" + std::to_string(max) + "): the end was left out.");
        return;
    }
    if (cut) {   // not half a settings string: a cut one would be taken for the real thing
        Say("That text is longer than this setting takes (" + std::to_string(max) + "): nothing was changed.");
        return;
    }
    std::string problem;
    if (src_.loader->Set(mod_, l.name, text, &problem)) {
        Say("Pasted: " + std::to_string(CharStarts(text).size()) + " characters.");
    } else {
        Say(problem);
    }
}

bool Tab::Char(std::uint32_t cp) {
    if (!typing_) return false;
    if (cp < 0x20 || cp == 0x7F || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return true;
    const std::vector<Line> lines = Lines();
    const std::size_t max = selected_ < lines.size() ? MaxLength(lines[selected_].name) : CoopMods::kMaxSettingValue;
    std::string one;
    AppendUtf8(one, cp);
    if (typed_.size() + one.size() <= max) typed_ += one;   // (the setting's own max_length, not only the 512 cap)
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
            v.rows[selected_].value = TailShown(typed_) + "_";   // (the end: where the next character goes)
            v.typing = true;
            v.help = "Type or Ctrl+V, then Enter. Esc: as it was.";   // (a line of the translation table: keep it whole)
            v.hint = std::to_string(CharStarts(typed_).size()) + " characters (" + std::to_string(typed_.size()) +
                     " of " + std::to_string(MaxLength(l.name)) + " bytes)";
        }
    }
    if (!note_.empty()) {
        v.gold = note_;
    } else if (src_.loader != nullptr && src_.loader->RestartNeeded().needed) {
        v.gold = "Restart the game to use the changes.";
    }
    const bool on_text = !typing_ && selected_ < lines.size() && IsText(lines[selected_]) && !lines[selected_].row.fixed;
    v.change = typing_ ? "" : on_text ? "Paste" : "Switch";
    v.confirm = typing_ ? "Done" : mod_.empty() ? "Open" : "Choose";
    v.back = mod_.empty() && !typing_ ? "Close" : "Back";
    return v;
}

}  // namespace CoopModsTab
