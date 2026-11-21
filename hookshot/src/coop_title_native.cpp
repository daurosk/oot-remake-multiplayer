// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_title_native.h"

#include <algorithm>
#include <vector>

namespace CoopTitleNative {

namespace {

// At most `n` bytes, cut before a UTF-8 sequence's start, with "..." when cut.
std::string Fit(std::string t, std::size_t n) {
    if (t.size() <= n) return t;
    std::size_t k = n - 3;
    while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xc0) == 0x80) --k;
    return t.substr(0, k) + "...";
}

}  // namespace

std::string RowText(const CoopPauseMenu::Row& r) {
    using CoopPauseMenu::RowKind;
    std::string t = r.name;
    switch (r.kind) {
    case RowKind::kToggle: t += std::string(": ") + (r.on ? "On" : "Off"); break;
    case RowKind::kChoice:
        t += ": " + std::string(r.left ? "< " : "") + r.value + (r.right ? " >" : "");
        break;
    default:
        // (a value that only says the row's own word again, "Join: Join", is left off)
        if (!r.value.empty() && (t.empty() || t.rfind(r.value, 0) != 0)) t += t.empty() ? r.value : ": " + r.value;
        break;
    }
    return Fit(t, kTextBytes);
}

const std::string& Letters() {
    static const std::string letters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-_@!#$%&*+=? ";
    return letters;
}

int MoveKey(int key, int dy) {
    const int n = kLastKey - kFirstKey + 1;
    const int at = (key < kFirstKey || key > kLastKey ? 0 : key - kFirstKey) + dy;
    return kFirstKey + ((at % n) + n) % n;
}

Look TypingLook(const CoopPauseMenu::View& v, char letter, int key) {
    Look out;
    const bool row = v.selected >= 0 && static_cast<std::size_t>(v.selected) < v.rows.size();
    if (row) {   // (the field's name and what is typed; a long one shows its end, where the typing goes)
        const CoopPauseMenu::Row& r = v.rows[static_cast<std::size_t>(v.selected)];
        out.texts[static_cast<std::size_t>(Key::kField)] = r.value.empty() ? r.name : r.name + ": " + r.value;
    }
    out.texts[static_cast<std::size_t>(Key::kLetter)] =
        std::string("Letter: < ") + (letter == ' ' ? std::string("Space") : std::string(1, letter)) + " >";
    out.texts[static_cast<std::size_t>(Key::kDelete)] = "Delete";
    out.texts[static_cast<std::size_t>(Key::kOk)] = "OK";
    for (std::string& t : out.texts) {
        if (t.size() > kTextBytes) {   // (the field's end: the typing; never inside a letter of more bytes)
            std::size_t from = t.size() - (kTextBytes - 3);
            while (from < t.size() && (static_cast<unsigned char>(t[from]) & 0xC0) == 0x80) ++from;
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
        l.dim[static_cast<std::size_t>(b)] = v.rows[static_cast<std::size_t>(row)].fixed;
    }
    // (Back right under the page's last row, the buttons after it not shown; never under the third button: with two
    // shown, the game put its own word on the second, "Join" for our Back. A page of one row had Back three rows
    // down, a tester's "super janky")
    l.back = std::max(kRowButtons - 1, on_page);
    l.buttons = l.back + 1;
    for (int b = 0; b < l.back; ++b) {
        if (l.texts[static_cast<std::size_t>(b)].empty()) l.texts[static_cast<std::size_t>(b)] = " ";
    }
    l.texts[static_cast<std::size_t>(l.back)] = "Back";
    l.rows[static_cast<std::size_t>(l.back)] = -1;
    l.focus = at >= 0 ? at - l.top : l.back;
    // (more than one page: the page first, then what the row does)
    const std::string paged =
        l.pages > 1 ? "Page " + std::to_string(l.page + 1) + " of " + std::to_string(l.pages) + " (ZL/ZR)" : std::string();
    l.hint = Fit(paged.empty() ? v.help : v.help.empty() ? paged : paged + "   " + v.help, kHintBytes);
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

int PageJump(const CoopPauseMenu::View& v, const Look& l, int dir) {
    int at = -1;
    const std::vector<int> shown = Shown(v, &at);
    const int page = l.page + (dir < 0 ? -1 : 1);
    if (dir == 0 || l.pages <= 1 || page < 0 || page >= l.pages) return -1;
    const int per_page = std::max(1, l.per_page);
    const std::size_t first = static_cast<std::size_t>(page) * static_cast<std::size_t>(per_page);
    return first < shown.size() ? shown[first] : -1;
}

}  // namespace CoopTitleNative
