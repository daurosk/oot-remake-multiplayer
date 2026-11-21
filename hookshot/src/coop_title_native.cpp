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

Look LookFor(const CoopPauseMenu::View& v, int top) {
    Look l;
    std::vector<int> shown;   // (the view's rows the buttons may show: all but headings)
    int at = -1;
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].kind == CoopPauseMenu::RowKind::kHeading) continue;
        if (static_cast<int>(i) == v.selected) at = static_cast<int>(shown.size());
        shown.push_back(static_cast<int>(i));
    }
    const int n = static_cast<int>(shown.size());
    top = std::clamp(top, 0, std::max(0, n - kRowButtons));
    if (at >= 0 && at < top) top = at;
    if (at >= top + kRowButtons) top = at - kRowButtons + 1;
    l.top = top;
    for (int b = 0; b < kRowButtons && top + b < n; ++b) {
        const int row = shown[static_cast<std::size_t>(top + b)];
        l.rows[static_cast<std::size_t>(b)] = row;
        l.texts[static_cast<std::size_t>(b)] = RowText(v.rows[static_cast<std::size_t>(row)]);
    }
    // (fewer rows than buttons: Back right under the last, the buttons after it not shown; a page of one row had Back
    // three rows down, a tester's "super janky")
    // (never under three buttons: with two shown, the game put its own word on the second, "Join" for our Back)
    const int shown_rows = std::max(1, std::min(kRowButtons, n - top));
    l.back = std::max(kRowButtons - 1, shown_rows < kRowButtons ? shown_rows : kButtons - 1);
    l.buttons = l.back + 1;
    for (int b = 0; b < kButtons; ++b) {
        if (l.texts[static_cast<std::size_t>(b)].empty()) l.texts[static_cast<std::size_t>(b)] = " ";
    }
    l.texts[static_cast<std::size_t>(l.back)] = "Back";
    l.rows[static_cast<std::size_t>(l.back)] = -1;
    l.focus = at >= 0 ? at - top : l.back;
    return l;
}

}  // namespace CoopTitleNative
