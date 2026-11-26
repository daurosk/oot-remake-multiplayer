// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_title_native.h"

#include "coop_i18n.h"
#include "coop_list_pages.h"
#include "coop_menu_nav.h"
#include "coop_text_fit.h"

#include <algorithm>
#include <vector>

namespace CoopTitleNative {

namespace {

// At most `n` letters of width (and bytes), cut before a letter's start, with "..." when cut (coop_text_fit.h).
std::string Fit(std::string t, std::size_t n) { return CoopTextFit::Fit(t, n); }

}  // namespace

std::string RowText(const CoopPauseMenu::Row& r) {
    using CoopPauseMenu::RowKind;
    // The row's words in the player's language (coop_i18n.h), before the cut: a
    // cut line is not the table's line any more. The table has no line for the
    // game's own English (the tests read this file's lines as they are written).
    // A player's, a room's or a mod's own text on the row (Row::own_name, own_value: "Your name: Link") is put in as it
    // is: a name that is also a word of the table must not turn into the table's line.
    const std::string name = r.own_name ? r.name : CoopI18n::Tr(r.name);
    const std::string value = r.own_value ? r.value : CoopI18n::Tr(r.value);
    std::string tail;   // (what follows the name: the value, which is what the row is read for)
    switch (r.kind) {
    case RowKind::kToggle: tail = CoopI18n::Tr(": ") + (r.on ? CoopI18n::Tr("On") : CoopI18n::Tr("Off")); break;
    case RowKind::kChoice:
        tail = CoopI18n::Tr(": ") + std::string(r.left ? "< " : "") + value + (r.right ? " >" : "");
        break;
    default:
        // (a value that only says the row's own word again, "Join: Join", is left off)
        if (!value.empty() && (name.empty() || r.name.rfind(r.value, 0) != 0))
            tail = name.empty() ? value : CoopI18n::Tr(": ") + value;
        break;
    }
    return FitRow(name, tail, kTextBytes);
}

std::string FitRow(const std::string& name, const std::string& tail, std::size_t n) {
    const std::string whole = name + tail;
    if (whole.size() <= n && CoopTextFit::Halves(whole) <= 2 * n) return whole;
    // (too long: the name is cut, never the value. A game's row "Kakariko-crew-of-friends (Saria): 3/16  Public" read
    // "...: 3/16..." with the players and the kind gone, the audit's item 16; a value longer than the button alone is
    // cut at its end as before)
    const std::size_t tail_bytes = tail.size(), tail_halves = CoopTextFit::Halves(tail);
    if (name.empty() || tail_bytes + 4 > n || tail_halves + 8 > 2 * n) return Fit(whole, n);
    const std::size_t room = std::min(n - tail_bytes, (2 * n - tail_halves) / 2);
    const std::string cut = Fit(name, room);
    return cut.empty() ? Fit(whole, n) : cut + tail;
}

const std::string& Letters() {
    static const std::string letters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-_@!#$%&*+=? ";
    return letters;
}

int MoveKey(int key, int dy) { return KeyStep(key, dy, false); }   // (a fresh press: round)

int RowStep(const CoopPauseMenu::View& v, int from, int dir, bool repeat) {
    const int n = static_cast<int>(v.rows.size());
    if (from < 0 || from >= n || dir == 0) return from;
    return CoopMenuNav::Step(from, dir, n, !repeat, [&v](int i) {
        const CoopPauseMenu::RowKind k = v.rows[static_cast<std::size_t>(i)].kind;
        return k != CoopPauseMenu::RowKind::kHeading && k != CoopPauseMenu::RowKind::kNote;
    });
}

int KeyStep(int key, int dy, bool repeat) {
    const int n = kLastKey - kFirstKey + 1;
    const int at = key < kFirstKey || key > kLastKey ? 0 : key - kFirstKey;
    return kFirstKey + CoopMenuNav::Step(at, dy, n, !repeat, [](int) { return true; });
}

std::size_t LetterStep(std::size_t at, int dx, bool repeat) {
    const int n = static_cast<int>(Letters().size());
    return static_cast<std::size_t>(
        CoopMenuNav::Step(static_cast<int>(at % static_cast<std::size_t>(n)), dx, n, !repeat, [](int) { return true; }));
}

Look TypingLook(const CoopPauseMenu::View& v, char letter, int key) {
    Look out;
    const bool row = v.selected >= 0 && static_cast<std::size_t>(v.selected) < v.rows.size();
    if (row) {   // (the field's name and what is typed; a long one shows its end, where the typing goes)
        const CoopPauseMenu::Row& r = v.rows[static_cast<std::size_t>(v.selected)];
        out.texts[static_cast<std::size_t>(Key::kField)] =
            r.value.empty() ? (r.own_name ? r.name : CoopI18n::Tr(r.name))
                            : (r.own_name ? r.name : CoopI18n::Tr(r.name)) + CoopI18n::Tr(": ") +
                                  (r.own_value ? r.value : CoopI18n::Tr(r.value));   // (what is typed: as it is, "On_")
    }
    // (the keys in the player's language here, not by the caller: the field's line above is the player's text)
    out.texts[static_cast<std::size_t>(Key::kLetter)] =
        CoopI18n::Tr(std::string("Letter: < ") + (letter == ' ' ? std::string("Space") : std::string(1, letter)) + " >");
    out.texts[static_cast<std::size_t>(Key::kDelete)] = CoopI18n::Tr("Delete");
    out.texts[static_cast<std::size_t>(Key::kOk)] = CoopI18n::Tr("OK");
    for (std::string& t : out.texts) {
        if (t.size() > kTextBytes || CoopTextFit::Halves(t) > 2 * kTextBytes) {   // (the field's end: the typing)
            std::size_t from = t.size() - std::min(t.size(), kTextBytes - 3);
            while (from < t.size() && (CoopTextFit::Halves(t.substr(from)) > 2 * kTextBytes - 6 ||
                                       (static_cast<unsigned char>(t[from]) & 0xC0) == 0x80))
                ++from;   // (never inside a letter; the end that fits the button's width)
            t = "..." + t.substr(from);
        }
    }
    out.focus = key < kFirstKey || key > kLastKey ? kFirstKey : key;
    return out;
}

namespace {

// The view's rows the buttons may show (all but headings), and where the selected one is among them (-1: none).
std::vector<int> Shown(const CoopPauseMenu::View& v, int* at) {
    std::vector<int> shown;
    *at = -1;
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].kind == CoopPauseMenu::RowKind::kHeading) continue;
        if (static_cast<int>(i) == v.selected) *at = static_cast<int>(shown.size());
        shown.push_back(static_cast<int>(i));
    }
    return shown;
}

}  // namespace

Look LookFor(const CoopPauseMenu::View& v, int top, int buttons) {
    Look l;
    buttons = std::clamp(buttons, kButtons, kMaxButtons);
    const int per_page = buttons - 1;   // (the last button: Back)
    l.per_page = per_page;
    int at = -1;
    const std::vector<int> shown = Shown(v, &at);
    const int n = static_cast<int>(shown.size());
    l.pages = std::max(1, (n + per_page - 1) / per_page);
    // (the page that holds the selected row; nothing selected: the page shown before)
    l.page = std::clamp(at >= 0 ? at / per_page : std::max(0, top) / per_page, 0, l.pages - 1);
    l.top = l.page * per_page;
    const int on_page = std::clamp(n - l.top, 0, per_page);
    for (int b = 0; b < on_page; ++b) {
        const int row = shown[static_cast<std::size_t>(l.top + b)];
        l.rows[static_cast<std::size_t>(b)] = row;
        l.texts[static_cast<std::size_t>(b)] = RowText(v.rows[static_cast<std::size_t>(row)]);
        const CoopPauseMenu::Row& r = v.rows[static_cast<std::size_t>(row)];
        l.dim[static_cast<std::size_t>(b)] = r.fixed;
        if (l.toggle < 0 && (r.cell == 0 || r.cell == 1) && !r.cells[0].empty() && !r.cells[1].empty() && !r.fixed) {
            l.toggle = b, l.toggle_name = r.name, l.toggle_cells = r.cells, l.toggle_cell = r.cell;
        }
    }
    // (Back right under the page's last row, the buttons after it not shown; never under the third button: with two
    // shown, the game put its own word on the second, "Join" for our Back. A page of one row had Back three rows
    // down, a tester's "super janky")
    l.back = std::max(kRowButtons - 1, on_page);
    l.buttons = l.back + 1;
    for (int b = 0; b < l.back; ++b) {
        if (l.texts[static_cast<std::size_t>(b)].empty()) l.texts[static_cast<std::size_t>(b)] = " ";
    }
    l.texts[static_cast<std::size_t>(l.back)] = CoopI18n::Tr("Back");
    l.rows[static_cast<std::size_t>(l.back)] = -1;
    l.focus = at >= 0 ? at - l.top : l.back;
    // (more than one page: the page first, "Page 2/3 (ZL/ZR)", then what the row does; both in the player's language
    // before the cut: the page is the table's shape, the row's help its line. The buttons in words: the title's
    // archive has no icon pane of the game's (coop_list_pages.h), and the game's letters in this words pane drew as
    // empty boxes, row 264)
    const std::string paged = CoopListPages::Line(l.page, l.pages, true);
    l.hint = Fit(paged.empty()   ? CoopI18n::Tr(v.help)
                 : v.help.empty() ? paged
                                  : paged + "   " + CoopI18n::Tr(v.help),
                 kHintBytes);
    return l;
}

float ColumnRaise(float lowest, float highest, float raised, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    float want = raised;
    if (lowest < kScreenFoot) want += (kScreenFoot - lowest) / scale;            // (the foot: up by what is under it)
    else if (lowest > kScreenFoot + 1.0f) want -= (lowest - kScreenFoot) / scale;   // (room under it: back down)
    // (the top kept on the screen: the column taller than the screen is never raised past its first row)
    const float top_after = highest + (want - raised) * scale;
    if (top_after > kScreenTop) want -= (top_after - kScreenTop) / scale;
    return std::max(0.0f, want);
}

float ColumnCentre(float lowest, float highest, float raised, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    float want = raised - (lowest + highest) / 2.0f / scale;   // (its middle on the screen's: y 0)
    // (still on the screen: the foot kept above the screen's foot, then the top below its top, as ColumnRaise)
    const float foot_after = lowest + (want - raised) * scale;
    if (foot_after < kScreenFoot) want += (kScreenFoot - foot_after) / scale;
    const float top_after = highest + (want - raised) * scale;
    if (top_after > kScreenTop) want -= (top_after - kScreenTop) / scale;
    return std::max(0.0f, want);
}

int PageJump(const CoopPauseMenu::View& v, const Look& l, int dir) {
    int at = -1;
    const std::vector<int> shown = Shown(v, &at);
    if (dir == 0 || l.pages <= 1) return -1;
    const int page = CoopListPages::Step(l.page, l.pages, dir);   // (round at both ends, as every list: row 264)
    const int per_page = std::max(1, l.per_page);
    const std::size_t first = static_cast<std::size_t>(page) * static_cast<std::size_t>(per_page);
    return first < shown.size() ? shown[first] : -1;
}

}  // namespace CoopTitleNative
