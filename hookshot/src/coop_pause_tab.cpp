// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h's logic: no game reads or calls, no log (the tests drive it).
#include "coop_pause_tab.h"

#include "coop_browse.h"

#include "hos_coop_host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace CoopPauseTab {

namespace {

// A screen whose updates stopped this long ago is taken as gone (the game updates an open menu once a frame).
constexpr std::uint64_t kStaleMs = 250;
constexpr std::uint64_t kTypingStartMs = 400;   // (an A this soon after typing began is the one that began it)
// A window over the page keeps it away this long after its last opening, open or closing update.
constexpr std::uint64_t kBlockMs = 250;

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

// What a screen shows, as one line (the part that is compared).
std::string Shows(const Facts& f) {
    char b[160];
    if (f.kind == Kind::kStrip) {
        std::snprintf(b, sizeof b, "%d tabs, %s menu, lights category %d, state %d", f.tabs,
                      f.mode == kPlusMode ? "the Plus" : "the Minus", f.lit, f.state);
    } else {
        std::snprintf(b, sizeof b, "page %d, asked %d, state %d", f.page, f.asked, f.state);
    }
    return b;
}

}  // namespace

Kind KindOf(std::uint64_t vtable_offset) {
    if (vtable_offset == kPagesVtable) return Kind::kPages;
    if (vtable_offset == kStripVtable) return Kind::kStrip;
    for (const std::uint64_t v : kBlockerVtables) {
        if (vtable_offset == v) return Kind::kBlocker;
    }
    return Kind::kOther;
}

// --- TextField ---

const std::string& TextField::Letters() const {
    static const std::string room = "abcdefghijklmnopqrstuvwxyz0123456789.:-_";   // (. and : for an address)
    // (a host's own password is 20 of the first 32, the letters of its codes, coop::server::nat::kLetters: those come
    // first, so the pad's up and down reach them soonest)
    static const std::string password =
        "ABCDEFGHJKLMNPQRSTUVWXYZ23456789IO01abcdefghijklmnopqrstuvwxyz!#$%&*+-.=?@_";
    static const std::string port = "0123456789";
    static const std::string address = "abcdefghijklmnopqrstuvwxyz0123456789.-:";
    return kind_ == Kind::kRoom ? room : kind_ == Kind::kPort ? port : kind_ == Kind::kAddress ? address : password;
}

int TextField::PortValue() const {
    if (kind_ != Kind::kPort || text_.empty() || text_.size() > 5) return 0;
    const int v = std::atoi(text_.c_str());
    return v >= 1 && v <= 65535 ? v : 0;
}

bool TextField::Allowed(char c) const {
    if (kind_ != Kind::kPassword) return Letters().find(c) != std::string::npos;
    return c > 0x20 && c < 0x7f;
}

void TextField::Cycle(int dir) {
    const std::string& letters = Letters();
    if (cursor_ >= text_.size()) {
        if (text_.size() >= Max()) return;
        text_.push_back(dir >= 0 ? letters.front() : letters.back());
        return;
    }
    const auto at = letters.find(text_[cursor_]);
    const std::size_t n = letters.size();
    const std::size_t i = at == std::string::npos ? 0 : (at + n + (dir >= 0 ? 1 : n - 1)) % n;
    text_[cursor_] = letters[i];
}

void TextField::Move(int dir) {
    if (dir < 0) {
        if (cursor_ > 0) --cursor_;
        return;
    }
    const std::size_t last = text_.size() < Max() ? text_.size() : text_.size() - 1;
    if (cursor_ < last) {
        ++cursor_;
    } else {
        cursor_ = last;   // (a full text: the cursor on its last letter, not past it)
    }
}

void TextField::Delete() {
    if (text_.empty()) return;
    if (cursor_ >= text_.size()) cursor_ = text_.size() - 1;
    text_.erase(cursor_, 1);
}

void TextField::Backspace() {
    if (cursor_ == 0 || text_.empty()) return;
    --cursor_;
    text_.erase(cursor_, 1);
}

bool TextField::Type(std::uint32_t cp) {
    if (cp >= 0x80) return false;
    char c = static_cast<char>(cp);
    if ((kind_ == Kind::kRoom || kind_ == Kind::kAddress) && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');   // (any case)
    if (!Allowed(c) || text_.size() >= Max()) return false;
    if (cursor_ > text_.size()) cursor_ = text_.size();
    text_.insert(text_.begin() + static_cast<std::ptrdiff_t>(cursor_), c);
    ++cursor_;
    return true;
}

std::string TextField::Shown(bool typing, std::size_t room) const {
    std::string shown;
    std::size_t mark = std::string::npos;   // where the cursor's bracket went (a password may hold '[' itself)
    for (std::size_t i = 0; i <= text_.size(); ++i) {
        const bool here = typing && i == cursor_;
        if (here) mark = shown.size();
        if (i == text_.size()) {
            if (here) shown += "[ ]";
            break;
        }
        const char c = kind_ == Kind::kPassword && !here ? '*' : text_[i];
        shown += here ? std::string("[") + c + "]" : std::string(1, c);
    }
    if (shown.size() <= room) return shown;
    // (too long: the part around the cursor, so its letter stays in view)
    if (mark == std::string::npos) mark = shown.size();
    std::size_t from = mark > room / 2 ? mark - room / 2 : 0;
    if (from + room > shown.size()) from = shown.size() - room;
    return shown.substr(from, room);
}

// --- Page ---

// The menu open on System, the strip with its five tabs: R shows our page.
bool Page::CanShow(std::uint64_t now_ms) const {
    return CanStay(now_ms) && pages_.state == 2 && strip_.state == 2;
}

// The menu open, or opening, on System: our page stays, or comes back as the menu opens again. (Opening too: the menu
// opening again shows our page from its first update, and a run showed the pages open, state 2, while the strip still
// opens, state 1.)
bool Page::CanStay(std::uint64_t now_ms) const {
    // The menu's first frames (a run, 2026-11-19): the strip's first update of the opening comes a frame before the
    // pages', whose last update is still the closed one (state 0, old); then the pages are at 0 while the strip
    // opens, System's page already drawn. Both are the menu opening, on the page the pages last had.
    const bool pages_fresh = now_ms - pages_ms_ < kStaleMs;
    const bool pages_open = pages_fresh ? pages_.state == 1 || pages_.state == 2 || (pages_.state == 0 && strip_.state == 1)
                                        : strip_.state == 1;
    return usable_ && pages_.screen != 0 && strip_.screen != 0 && now_ms - strip_ms_ < kStaleMs && pages_open &&
           pages_.page == kSystemPage && pages_.asked == kSystemPage && (strip_.state == 1 || strip_.state == 2) &&
           strip_.mode == kPlusMode && strip_.tabs >= 4 && now_ms >= blocked_until_ms_;
}

bool Page::CanMinus(std::uint64_t now_ms) const {
    return minus_ok_ && strip_.screen != 0 && now_ms - strip_ms_ < kStaleMs && strip_.mode != kPlusMode &&
           (strip_.state == 1 || strip_.state == 2) && now_ms >= blocked_until_ms_;
}

// The pad while the - menu is up and ours can be used: R on Map onto ours, R / L among ours, L off the first back to
// Map. Buttons the module took are kept from the game until let go.
void Page::SetMinusRows(std::uint32_t shown) {
    minus_rows_ = shown & ((1u << kRows) - 1);
    if (minus_rows_ != 0 && (minus_rows_ & (1u << minus_row_)) == 0) MinusStep(1);   // (its row gone: the next shown)
}

void Page::MinusStep(int dir) {
    if (minus_rows_ == 0) return;
    for (int k = 1; k <= kRows; ++k) {
        const int r = ((minus_row_ + dir * k) % kRows + kRows) % kRows;
        if ((minus_rows_ & (1u << r)) == 0) continue;
        // (no wrap, as System's buttons; a row gone from under the cursor finds the next one around)
        if ((minus_rows_ & (1u << minus_row_)) != 0 && (dir > 0 ? r < minus_row_ : r > minus_row_)) return;
        minus_row_ = r;
        return;
    }
}

void Page::SetMinusLines(std::uint32_t pickable, bool raw) {
    minus_lines_ = pickable, minus_raw_ = raw;
    if (raw) return;   // (co-op's menu's own page: its selection is its own)
    if (pickable == 0) {
        minus_list_ = false, minus_line_ = -1;
        return;
    }
    if (minus_line_ < 0 || (pickable & (1u << minus_line_)) == 0) {   // (its line gone: the nearest after, else before)
        const int was = minus_line_;
        minus_line_ = -1;
        for (int k = std::max(was, 0); k < 32 && minus_line_ < 0; ++k) {
            if ((pickable & (1u << k)) != 0) minus_line_ = k;
        }
        for (int k = std::min(was, 31); k >= 0 && minus_line_ < 0; --k) {
            if ((pickable & (1u << k)) != 0) minus_line_ = k;
        }
    }
}

void Page::MinusViewChanged(int row) {
    minus_row_ = row >= 0 && row < kRows ? row : 0;
    minus_list_ = false, minus_line_ = -1, minus_line_decide_ = -1, minus_moves_ = 0, minus_list_back_ = false;
}

void Page::MinusEnterList() {
    if (minus_lines_ == 0 || minus_tab_ < 0) return;
    minus_list_ = true;
    if (!minus_raw_) SetMinusLines(minus_lines_, false);
}

void Page::MinusLineStep(int dir) {
    for (int k = minus_line_ + dir; k >= 0 && k < 32; k += dir) {   // (no wrap, as the rows)
        if ((minus_lines_ & (1u << k)) != 0) {
            minus_line_ = k;
            return;
        }
    }
}

void Page::MinusTabMoved() {
    minus_row_ = 0, minus_rows_ = 0;   // (another page: its first row; the module sets its rows)
    minus_list_ = false, minus_line_ = -1, minus_lines_ = 0, minus_raw_ = false, minus_raw_back_ = false;
    minus_in_view_ = false, minus_back_ = false, minus_list_back_ = false, minus_moves_ = 0, minus_line_decide_ = -1;
    minus_decide_ = -1;
    minus_tab_changed_ = true;
}

std::uint64_t Page::MinusPad(std::uint64_t buttons, std::uint64_t down) {
    if (minus_tab_ < 0) {
        if ((down & kPadR) != 0 && strip_.lit == kMapCategory) {
            minus_tab_ = 0, minus_slide_ = 1;
            MinusTabMoved();
            strip_sound_ = "mc_TabToRight";
            swallow_ |= buttons & ~kPadPassed;
            return buttons & kPadPassed & ~swallow_;
        }
        return buttons & ~swallow_;
    }
    if (typing_ >= 0) {   // (a field of a view typed into: every button is its, none reaches the game)
        TypingPad(down, buttons, pad_ms_);
        if (pages_sound_ != nullptr) strip_sound_ = std::exchange(pages_sound_, nullptr);   // (the strip plays ours)
        swallow_ |= buttons;
        return 0;
    }
    if (minus_rows_ == 0 && minus_lines_ != 0 && !minus_list_) {   // (a tab whose list is its page: the list's)
        minus_list_ = true;
        if (!minus_raw_) SetMinusLines(minus_lines_, false);
    }
    const bool up = (down & (kPadUp | kPadStickUp)) != 0, dn = (down & (kPadDown | kPadStickDown)) != 0;
    const bool left = (down & (kPadLeft | kPadStickLeft)) != 0, right = (down & (kPadRight | kPadStickRight)) != 0;
    if ((down & kPadR) != 0) {
        if (minus_tab_ < kMinusTabs - 1) {
            ++minus_tab_, minus_slide_ = 1;
            strip_sound_ = "mc_TabToRight";
            MinusTabMoved();
        }
        swallow_ |= kPadR;
    } else if ((down & kPadL) != 0) {
        --minus_tab_, minus_slide_ = -1;   // (off the first: Map again)
        strip_sound_ = "mc_TabToLeft";
        MinusTabMoved();
        swallow_ |= kPadL;
    } else if (minus_list_ && minus_raw_) {   // (co-op's menu's own page: the moves are its events)
        if ((down & kPadB) != 0 && (minus_rows_ != 0 || minus_raw_back_)) {
            if (minus_rows_ != 0) minus_list_ = false;   // (back to the rows; a tab with none keeps the list)
            minus_list_back_ = true;
            strip_sound_ = "button_cancel";
            swallow_ |= kPadB;
        } else if ((down & kPadB) == 0) {
            const std::uint32_t moves = (up ? kMinusMoveUp : 0u) | (dn ? kMinusMoveDown : 0u) |
                                        (left ? kMinusMoveLeft : 0u) | (right ? kMinusMoveRight : 0u) |
                                        ((down & kPadA) != 0 ? kMinusMoveA : 0u);
            minus_moves_ |= moves;
            if ((moves & kMinusMoveA) != 0) strip_sound_ = "button_decide";
            else if (moves != 0) strip_sound_ = "button_on";
        }
    } else if (minus_list_) {
        if (((down & kPadB) != 0 || left) && minus_rows_ != 0) {
            minus_list_ = false;
            strip_sound_ = "button_cancel";
            if ((down & kPadB) != 0) swallow_ |= kPadB;
        } else if (up || dn) {
            const int was = minus_line_;
            MinusLineStep(up ? -1 : 1);
            if (minus_line_ != was) strip_sound_ = "button_on";
        } else if ((down & kPadA) != 0 && minus_line_ >= 0) {
            minus_line_decide_ = minus_line_;
            strip_sound_ = "button_decide";
        }
    } else if (right && minus_lines_ != 0) {   // (into the list on the right)
        minus_list_ = true;
        if (!minus_raw_) SetMinusLines(minus_lines_, false);   // (a line picked)
        strip_sound_ = "button_on";
    } else if ((up || dn) && minus_rows_ != 0) {
        const int was = minus_row_;
        MinusStep(up ? -1 : 1);
        if (minus_row_ != was) strip_sound_ = "button_on";
    } else if ((down & kPadA) != 0 && (minus_rows_ & (1u << minus_row_)) != 0) {
        minus_decide_ = minus_row_;
        strip_sound_ = "button_decide";
    } else if ((down & kPadB) != 0 && minus_in_view_) {   // (a view: back a view, not out of the menu)
        minus_back_ = true;
        strip_sound_ = "button_cancel";
        swallow_ |= kPadB;
    }
    // (ours shows: its page has the pad; B (on the tab's own rows), + and - stay the game's, which close or change
    // the menu)
    const std::uint64_t game = kPadB | kPadPlus | kPadMinus;
    swallow_ |= buttons & ~game;
    return buttons & game & ~swallow_;
}

void Page::Hide(const char* why, bool keep) {
    if (!shown_) {
        return;
    }
    shown_ = false;
    sub_ = false, back_ = false;
    typing_ = -1;
    slide_ = 0;   // (gone at once; L sets its slide after)
    again_ = keep;
    why_ = why;
    // Every button held now stays the module's until let go, so none reaches System as a fresh press (an A held
    // as the page goes would otherwise be the System page's A).
    swallow_ |= pad_last_ & ~kPadPassed;
}

std::uint64_t Page::Track(const Facts& f) {
    if (f.kind != Kind::kPages && f.kind != Kind::kStrip) return 0;
    Seen& s = f.kind == Kind::kPages ? pages_seen_ : strip_seen_;
    // (a new opening: the screen opening or open after it was closed or closing. The pages went from 0 straight to
    // 2 on the menu's second opening in a run (2026-11-20), never 1: the first opening's state was kept, our page did
    // not show, and the game took the A on our Multiplayer button and stopped taking B, + and A)
    const bool reopened = (f.state == 1 || f.state == 2) && (s.state == 0 || s.state == 3);
    if (s.gen == 0 || f.screen != s.screen || f.layout != s.layout || reopened) {
        s.screen = f.screen, s.layout = f.layout, s.gen = ++next_gen_;
        if (f.kind == Kind::kPages) {
            page_set_ = -1, texts_set_ = false;
        }
    }
    s.state = f.state;
    return s.gen;
}

void Page::SetUsable(bool usable) {
    usable_ = usable;
    if (!usable_) Hide("its panes or the game's calls are not there");
}

// The whole game stalled: no pad call and no screen update came for a while (a run: a game frozen for 40 s;
// a shader build or a save can take a few hundred ms). The screens are as fresh as they were before it, so their
// times move on by the gap; the first call after it would otherwise find them stale and give System back with
// the menu still open. That first call may be the pad's or any screen's update (the stall can end between the pad
// poll and the UI's updates of a frame), so both note it here first. Screens that stop while the other calls go on
// are still stale after kStaleMs.
void Page::AfterStall(std::uint64_t now_ms) {
    if (call_ms_ != 0 && now_ms > call_ms_ + kStaleMs) {
        const std::uint64_t gap = now_ms - call_ms_;
        if (pages_ms_ != 0) pages_ms_ = std::min(now_ms, pages_ms_ + gap);
        if (strip_ms_ != 0) strip_ms_ = std::min(now_ms, strip_ms_ + gap);
    }
    if (now_ms > call_ms_) call_ms_ = now_ms;
}

Page::Do Page::OnScreen(const Facts& f, std::uint64_t now_ms) {
    AfterStall(now_ms);
    Do d;
    Track(f);
    switch (f.kind) {
    case Kind::kBlocker:
        if (f.state >= 1 && f.state <= 3) {
            blocked_until_ms_ = now_ms + kBlockMs;
            Hide("a window opened over it");
        }
        return d;
    case Kind::kPages: {
        pages_ = f, pages_ms_ = now_ms;
        if (shown_ && !CanStay(now_ms)) {
            const bool closing = f.state == 3 || f.state == 0;
            Hide(closing ? "the menu is closing" : "the menu left the System page", closing);
        }
        ShowAgain(now_ms);
        const int want = shown_ ? 1 + which_ : 0;
        if (page_set_ != want) {
            d.set_page = true, d.page_shown = shown_, d.page_from = std::max(page_set_, 0), d.page_to = want;
            d.slide = std::exchange(slide_, 0), page_set_ = want;
        }
        if (!texts_set_ && f.state == 2) {
            d.set_texts = true, texts_set_ = true;
        }
        return d;
    }
    case Kind::kStrip: {
        strip_ = f, strip_ms_ = now_ms;
        if (shown_ && !CanStay(now_ms)) {
            Hide(f.mode != kPlusMode ? "the Minus menu" : "the tab strip is closing", f.mode == kPlusMode);
        }
        ShowAgain(now_ms);
        // (back at the strip's first update of the opening: the pages' own comes a frame later, so the module shows
        // our page through the pages screen it knows, in this update)
        if (shown_ && page_set_ != 1 + which_ && pages_.screen != 0) {
            d.set_page = true, d.page_shown = true, d.page_from = std::max(page_set_, 0), d.page_to = 1 + which_;
            d.slide = std::exchange(slide_, 0), page_set_ = 1 + which_;
        }
        // (the - menu's tabs of ours: shown while its strip is open; gone with it, and ours with them)
        if (minus_ok_) {
            const bool minus_open = f.mode != kPlusMode && (f.state == 1 || f.state == 2);
            if (!minus_open && minus_tab_ >= 0) minus_tab_ = -1, MinusTabMoved();   // (closed: the view with it)
            if (!minus_open && !shown_ && typing_ >= 0) EndTyping(false);
            const int state = !minus_open ? -2 : minus_tab_;
            const std::uint64_t gen = strip_seen_.gen;
            if (state != minus_set_ || (minus_open && minus_gen_ != gen)) {
                d.set_minus = true, d.minus_shown = minus_open, d.minus_lit = minus_open ? minus_tab_ : -1;
                d.fill_minus = minus_open && minus_gen_ != gen;
                if (d.fill_minus) minus_gen_ = gen;
                d.minus_slide = std::exchange(minus_slide_, 0);
                minus_set_ = state;
            }
        }
        return d;
    }
    case Kind::kOther:
        break;
    }
    return d;
}

std::uint64_t Page::OnPad(std::uint64_t buttons, std::uint64_t now_ms) {
    AfterStall(now_ms);
    pad_ms_ = now_ms;
    const std::uint64_t down = buttons & ~pad_last_;
    pad_last_ = buttons;
    swallow_ &= buttons;   // (let go: the game gets it again)
    if (shown_ && !CanStay(now_ms)) {
        Hide("the menu's state changed", pages_.state == 3 || pages_.state == 0 || strip_.state == 3 || strip_.state == 0);
    }
    if (!shown_ && CanMinus(now_ms)) return MinusPad(buttons, down);   // (the - menu: its tabs of ours)
    if (!shown_) {
        if ((down & kPadA) != 0 && system_button_ >= 0 && (system_button_ != 1 || settings_ok_) && CanShow(now_ms)) {
            // (A on one of System's buttons of ours: its page in System's place, at once; the game's own A never
            // reaches the button, whose name its selectors do not know. Customize Link's is the Multiplayer page,
            // opened on Settings and look's Customize page)
            shown_ = true, decide_ = false, why_.clear(), which_ = system_button_ == 1 ? 1 : 0, sub_ = false, back_ = false;
            opened_from_ = system_button_;
            customize_open_ = system_button_ == 2;
            slide_ = 0;
            opened_ = true;
            pages_sound_ = "button_decide";
            swallow_ |= buttons & ~kPadPassed;
            return buttons & kPadPassed & ~swallow_;
        }
        if ((down & kPadA) != 0 && system_button_ >= 0 && now_ms >= blocked_until_ms_) {   // (a window's own A: its)
            // (A on one of System's buttons of ours, the page not to show now: never the game's. It does not know the
            // button, and after an A on it its System page took no B, + or A any more: the menu stuck open)
            swallow_ |= kPadA;
            refused_a_ = true;
        }
        return buttons & ~swallow_;
    }
    if (typing_ >= 0) {   // (a field typed into: every button is its, none reaches System)
        TypingPad(down, buttons, now_ms);
        swallow_ |= buttons;
        return 0;
    }
    if ((down & (kPadL | kPadZL | kPadR | kPadZR)) != 0) {
        // (our pages are in System's place: System back at once, and the L or R is the game's, on to its next page)
        Hide(which_ == 1 ? "L or R on Longshot Settings" : "L or R on the Multiplayer page");
        swallow_ &= ~(kPadL | kPadZL | kPadR | kPadZR);   // (Hide keeps every held button from the game: these are its)
        return buttons & ~swallow_;
    }
    if (!sub_ && (down & kPadB) != 0) {
        // (B on a page's own rows: back to System at once, the game's cursor back on the page's button; the game's B
        // would close the menu)
        Hide(which_ == 1 ? "B on Longshot Settings" : "B on the Multiplayer page");
        left_ = true;
        swallow_ |= kPadB;
        return buttons & ~swallow_;
    }
    if ((down & kPadA) != 0) {
        decide_ = true;
        pages_sound_ = "button_decide";
    }
    if (sub_ && (down & kPadB) != 0) {   // (a view: B goes back to the five rows, kept until let go)
        back_ = true;
        swallow_ |= kPadB;
    }
    return buttons & kPadPassed & ~swallow_;
}

bool Page::TakeBack() {
    const bool b = back_;
    back_ = false;
    return b;
}

void Page::StartTyping(int which) {
    if (!shown_ && minus_tab_ < 0) return;   // (our page, or a view on the - menu's)
    typing_ = which >= 0 && which < kFields ? which : 0;
    typing_was_ = Field(typing_).text();
    key_ = 0;
    letter_ = 0;
    repeat_since_ = repeat_last_ = 0;
    swallow_ |= pad_last_;   // (the A that started it is not the first key's press)
    typing_from_ms_ = pad_ms_;   // (nor the same A coming again: a click's A, in two pulses, typed the first key)
}

void Page::EndTyping(bool ok) {
    if (typing_ < 0) return;
    if (!ok) Field(typing_).Set(typing_was_);
    typed_ = ok ? typing_ : -1;
    typing_ = -1;
    pages_sound_ = "button_decide";
}

char Page::TypingLetter() const {
    if (typing_ < 0) return 0;
    const std::string& letters = Field(typing_).Letters();
    return letters[letter_ % letters.size()];
}

void Page::PressKey(int key) {
    if (typing_ < 0 || key < 0 || key >= kTypeKeys) return;
    key_ = key;
    TextField& f = Field(typing_);
    switch (static_cast<TypeKey>(key)) {
    case TypeKey::kLetter: f.Type(static_cast<unsigned char>(TypingLetter())); break;
    case TypeKey::kDelete: f.Backspace(); break;
    case TypeKey::kCancel: EndTyping(false); return;
    case TypeKey::kOk: EndTyping(true); return;
    }
    pages_sound_ = "button_decide";
}

const char* TypingTitle(int which) {
    switch (which) {
    case 1: return "Join password";
    case 2: return "New join password";
    case 3: return "Port";
    case 4: return "Relay";
    case 5: return "Search";
    case 6: return "Your room name";
    default: return "Room name";
    }
}

std::vector<ListLine> TypingLines(int which, const TextField& f, char letter) {
    std::vector<ListLine> out(kListRows);
    out[0] = {TypingTitle(which), f.Shown(true, kListValueChars)};
    out[1] = {"Letter", std::string(1, letter)};
    out[1].arrows = true;
    out[2] = {"Delete", ""};
    out[3] = {"OK", ""};
    out[4] = {"Cancel", ""};
    return out;
}

void Page::TypingPad(std::uint64_t down, std::uint64_t held, std::uint64_t now_ms) {
    TextField& f = Field(typing_);
    if ((down & kPadPlus) != 0) {   // (done: the row shows what was typed)
        EndTyping(true);
        return;
    }
    if ((down & kPadA) != 0 && now_ms - typing_from_ms_ >= kTypingStartMs) {
        PressKey(key_);
        if (typing_ < 0) return;
    }
    // B is Cancel, the field as it was (a tester: B should back out of typing, not take letters off); Y takes
    // the letter before the cursor off, as the Delete key does.
    if ((down & kPadB) != 0) {
        EndTyping(false);
        return;
    }
    if ((down & kPadY) != 0) {
        f.Backspace();
        pages_sound_ = "button_on";
    }
    if ((down & (kPadL | kPadZL)) != 0) f.Move(-1);
    if ((down & (kPadR | kPadZR)) != 0) f.Move(+1);
    const std::uint64_t up = kPadUp | kPadStickUp, dn = kPadDown | kPadStickDown;
    const std::uint64_t left = kPadLeft | kPadStickLeft, right = kPadRight | kPadStickRight;
    const std::uint64_t dirs = up | dn | left | right;
    const auto step = [&](std::uint64_t bits) {
        // (up and down: the key, round the four; Left and Right: Letter's letter, round the field's letters)
        const int dy = (bits & dn) != 0 ? 1 : (bits & up) != 0 ? -1 : 0;
        key_ = (key_ + dy + kTypeKeys) % kTypeKeys;
        const int dx = (bits & right) != 0 ? 1 : (bits & left) != 0 ? -1 : 0;
        if (dx != 0 && key_ == static_cast<int>(TypeKey::kLetter)) {
            const std::size_t n = f.Letters().size();
            letter_ = (letter_ % n + n + static_cast<std::size_t>(dx + static_cast<int>(n))) % n;
        }
    };
    if ((down & dirs) != 0) {
        step(down & dirs);
        repeat_since_ = repeat_last_ = now_ms;
        pages_sound_ = "button_on";
    } else if ((held & dirs) != 0 && repeat_since_ != 0 && now_ms - repeat_since_ > 400 && now_ms - repeat_last_ > 90) {
        step(held & dirs);   // (held: again and again, as the game's own lists scroll)
        repeat_last_ = now_ms;
    }
    if ((held & dirs) == 0) repeat_since_ = 0;
}

bool Page::Key(std::uint32_t vk, bool repeat) {
    if (typing_ < 0) return false;
    TextField& f = Field(typing_);
    switch (vk) {
    case 0x25: f.Move(-1); return true;      // VK_LEFT
    case 0x27: f.Move(+1); return true;      // VK_RIGHT
    case 0x26: key_ = (key_ + kTypeKeys - 1) % kTypeKeys; return true;   // VK_UP, VK_DOWN: the key picked
    case 0x28: key_ = (key_ + 1) % kTypeKeys; return true;
    case 0x08: f.Backspace(); return true;   // VK_BACK
    case 0x2e: f.Delete(); return true;      // VK_DELETE
    case 0x0d:                               // VK_RETURN: done
        if (!repeat) EndTyping(true);
        return true;
    case 0x1b:                               // VK_ESCAPE: as it was
        if (!repeat) EndTyping(false);
        return true;
    default:
        return false;   // (a letter comes as its character, Char)
    }
}

bool Page::Char(std::uint32_t cp) {
    if (typing_ < 0) return false;
    Field(typing_).Type(cp);
    return true;   // (every character is the field's while it is typed into, taken or not)
}

void Page::ShowAgain(std::uint64_t now_ms) {
    if (!again_ || shown_) {
        return;
    }
    if (pages_.state == 2 && pages_.page != kSystemPage && pages_.page >= 0) {
        again_ = false;   // (the menu open on another page: not ours)
        return;
    }
    if (CanStay(now_ms)) {
        shown_ = true, again_ = false, decide_ = false, why_.clear();
        slide_ = 0;   // (the menu opening on our page: at once, as the game opens on its own last page)
    }
}

const char* Page::TakePagesSound() {
    return std::exchange(pages_sound_, nullptr);
}

bool Page::TakeDecide() {
    const bool d = decide_;
    decide_ = false;
    return d;
}

std::string Page::TakeWhy() {
    std::string w;
    w.swap(why_);
    return w;
}

// --- the rows ---

int RowOfButton(const std::string& name) {
    if (name.size() != 12 || name.compare(0, 11, "Pa_Button_0") != 0) return -1;
    const int row = name[11] - '0';
    return row >= 0 && row < kButtonRows ? row : -1;
}

float SlideProgress(std::uint64_t ms) {
    // (the measured progress at each 1/60 s update after the press: Items to System, 2026-11-19)
    static constexpr float kCurve[] = {0.0f,   0.179f, 0.319f, 0.426f, 0.508f, 0.570f, 0.619f, 0.662f, 0.706f,
                                       0.749f, 0.788f, 0.823f, 0.854f, 0.881f, 0.904f, 0.924f, 0.941f, 0.956f,
                                       0.967f, 0.977f, 0.984f, 0.990f, 0.994f, 0.997f, 0.9987f, 0.9996f, 1.0f};
    constexpr std::size_t kLast = sizeof kCurve / sizeof kCurve[0] - 1;
    const float at = static_cast<float>(ms) * 60.0f / 1000.0f;
    if (at >= static_cast<float>(kLast)) return 1.0f;
    const std::size_t i = static_cast<std::size_t>(at);
    const float frac = at - static_cast<float>(i);
    return kCurve[i] + (kCurve[i + 1] - kCurve[i]) * frac;
}

std::pair<float, float> SlideOffsets(int dir, float p) {
    p = std::clamp(p, 0.0f, 1.0f);
    if (dir > 0) return {kPageWidth * (1.0f - p), -kPageWidth * p};
    if (dir < 0) return {kPageWidth * p, -kPageWidth * (1.0f - p)};
    return {0.0f, 0.0f};
}

bool MenuUpFrom(int strip_state, std::uint64_t seen_ms, std::uint64_t now_ms) {
    return (strip_state == 1 || strip_state == 2) && seen_ms != 0 && now_ms >= seen_ms && now_ms - seen_ms < kMenuUpMs;
}

SnailEdge SnailEdgeFrom(bool open, std::uint64_t edge_ms, int strip_state, std::uint64_t strip_seen_ms, std::uint64_t now_ms) {
    const bool up = MenuUpFrom(strip_state, strip_seen_ms, now_ms);
    if (open) return up ? SnailEdge::kOpen : SnailEdge::kClose;
    return up && strip_seen_ms > edge_ms ? SnailEdge::kOpen : SnailEdge::kWait;
}

float Ahead(float prev, float now, float limit) {
    const float step = now - prev, next = now + step;
    if ((step > 0 && now <= limit && next > limit) || (step < 0 && now >= limit && next < limit)) {
        return limit;
    }
    return next;
}

int BrowseListed(const Session& s) {
    return s.online ? s.online_listed : s.lan_listed;
}

int CellPages(int listed) {
    return listed <= kCells ? 1 : (listed + kCells - 1) / kCells;
}

int CellIndex(const Session& s, int cell) {
    const int listed = BrowseListed(s), pages = CellPages(listed);
    if (cell < 0 || cell >= kCells) return -1;
    const int i = ((s.cell_page % pages + pages) % pages) * kCells + cell;
    return i < listed ? i : -1;
}

int CellAt(const std::array<RowBox, kCells>& boxes, float x, float y) {
    for (int i = 0; i < kCells; ++i) {
        const RowBox& b = boxes[static_cast<std::size_t>(i)];
        if (b.shown && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return i;
    }
    return -1;
}

int CellMove(int cell, int dx, int dy, int shown) {
    if (shown <= 0 || cell < 0) return kRowsCell;
    const int col = cell % kCellColumns, row = cell / kCellColumns;
    if (dx < 0 && col == 0) return kRowsCell;   // (Left from the left column: back to the rows)
    const int to_col = std::clamp(col + dx, 0, kCellColumns - 1), to_row = std::max(row + dy, 0);
    const int to = to_row * kCellColumns + to_col;
    return to < shown && to < kCells ? to : cell;
}

std::pair<float, float> LayoutPoint(int px, int py, int w, int h) {
    if (w <= 0 || h <= 0) return {0.0f, 0.0f};
    const float sw = 3840.0f / static_cast<float>(w), sh = 2160.0f / static_cast<float>(h);
    const float scale = sw > sh ? sw : sh;
    return {static_cast<float>(px - w / 2) * scale, static_cast<float>(h / 2 - py) * scale};
}

int RowAt(const RowBoxes& boxes, float x, float y) {
    for (int i = 0; i < kButtonRows; ++i) {
        const RowBox& r = boxes[static_cast<std::size_t>(i)];
        if (!r.shown) continue;
        const float cx = (r.x0 + r.x1) / 2;
        float above = -1, below = -1;   // (the gaps to the nearest box above and below in the column)
        for (int j = 0; j < kButtonRows; ++j) {
            const RowBox& o = boxes[static_cast<std::size_t>(j)];
            const float ox = (o.x0 + o.x1) / 2;
            if (j == i || !o.shown || ox - cx > 60 || cx - ox > 60) continue;
            const float up = o.y0 - r.y1, down = r.y0 - o.y1;
            if (up >= 0 && up < 240 && (above < 0 || up < above)) above = up;
            if (down >= 0 && down < 240 && (below < 0 || down < below)) below = down;
        }
        float y0 = r.y0, y1 = r.y1;
        if (above >= 0) y1 += above / 2;
        if (below >= 0) y0 -= below / 2;
        if (above >= 0 && below < 0) y0 -= above / 2;   // (an end of the column)
        if (below >= 0 && above < 0) y1 += below / 2;
        if (x >= r.x0 && x <= r.x1 && y >= y0 && y <= y1) return i;
    }
    return -1;
}

std::uint64_t MouseRows::Step(const In& in) {
    const int hover = in.inside ? RowAt(in.boxes, in.x, in.y) : -1;
    const bool moved = in.inside && (!seen_ || std::abs(in.x - x_) > 2 || std::abs(in.y - y_) > 2);
    if (in.inside) seen_ = true, x_ = in.x, y_ = in.y;
    if (in.pad) target_ = -1;   // (the player's own steps: the mouse's stop)
    if (moved && hover >= 0 && hover != target_ && hover != in.focused) target_ = hover, since_ = in.now_ms, step_from_ = -1;
    // A click is its press: Longshot's mouse presses A 50 ms after the left button goes down on a button (oot_menu.c,
    // mouse_tick_locked: "0.05:A/80"), mostly before it is let go, so the row is taken where the button went down.
    if (in.left && !left_ && in.inside) {
        const bool any = std::any_of(in.boxes.begin(), in.boxes.end(), [](const RowBox& b) { return b.shown; });
        if (hover >= 0) {
            clicked_ = hover, clicked_ms_ = in.now_ms;
            if (hover != in.focused) target_ = hover, since_ = in.now_ms, step_from_ = -1;
        } else if (any) {
            clicked_ = kOffRows, clicked_ms_ = in.now_ms;   // (on the page, off our rows: its A is no row's)
        }
    }
    left_ = in.left;
    if (target_ >= 0 && (in.focused == target_ || in.focused < 0 || in.now_ms - since_ > kGiveUpMs)) {
        target_ = -1, step_from_ = -1;
    }
    if (held_) {
        held_ = false;   // (let go a read between steps: each is a press of its own)
        return 0;
    }
    if (target_ < 0) return 0;
    if (step_from_ >= 0 && in.focused == step_from_ && in.now_ms - step_ms_ < kStepWaitMs) return 0;   // (not moved yet)
    step_from_ = in.focused, step_ms_ = in.now_ms, held_ = true;
    return in.focused < target_ ? kPadDown : kPadUp;
}

int MouseRows::Clicked(std::uint64_t now_ms) const {
    return clicked_ != -1 && now_ms >= clicked_ms_ && now_ms - clicked_ms_ <= kClickMs ? clicked_ : -1;
}

int MouseRows::TakeClicked(std::uint64_t now_ms) {
    const int row = Clicked(now_ms);
    clicked_ = -1;
    return row;
}

void MouseRows::Reset() {
    *this = MouseRows{};
}

std::uint64_t RowSkip::Step(const In& in) {
    if (in.up || in.mouse_up) dir_ = -1;
    if (in.down || in.mouse_down) dir_ = 1;
    if (in.focused < 0) {   // (no button focused for a read or two while the cursor steps: what was, kept)
        held_ = false;
        return 0;
    }
    skipping_ = in.hidden || in.focused >= in.rows;
    if (!skipping_) {
        from_ = -1, held_ = false;
        return 0;
    }
    if (held_) {
        held_ = false;   // (let go a read between steps: each is a press of its own)
        return 0;
    }
    if (in.up || in.down) return 0;   // (the player's own step moves it this read)
    if (from_ == in.focused && in.now_ms - step_ms_ < kStepWaitMs) return 0;   // (not moved yet)
    if (in.focused >= in.rows) {
        dir_ = -1;   // (past our rows: back up to them)
    } else if (from_ == in.focused) {
        dir_ = -dir_;   // (it did not move: an end of the rows, the other way)
    }
    from_ = in.focused, step_ms_ = in.now_ms, held_ = true;
    return dir_ < 0 ? kPadUp : kPadDown;
}

void RowSkip::Reset() {
    *this = RowSkip{};
}

RowAction ActionFor(int row, const Session& s) {
    // (0.11.9: five fixed entries, each opening its own view; what co-op does now is on the list, DetailLines)
    switch (row) {
    case kRowFind:
        return RowAction::kViewLan;   // (in a session the view says so)
    case kRowHost:
        return s.failed && !s.in_one ? RowAction::kViewSession : RowAction::kViewHost;
    case kRowJoin:
        if (s.wait_host) return RowAction::kCopyAnswer;   // (the host must take this game's answer code first)
        return RowAction::kViewJoin;   // (in a friend's game: its details and Leave there)
    case kRowPlayers:
        return RowAction::kViewPlayers;   // (just us, or nobody yet: the view says so; every entry opens)
    case kRowSettings:
        return RowAction::kViewMenu;
    default:
        return RowAction::kNone;
    }
}

View BackOf(View view) {
    switch (view) {
    case View::kPlayerHost: return View::kPlayer;
    case View::kPlayer: return View::kPlayers;
    case View::kLanGame: return View::kLan;
    case View::kPassword: return View::kSession;
    default: return View::kMain;
    }
}

int PlayerPages(int listed) {
    return listed <= kRows - 1 ? 1 : (listed + kRows - 3) / (kRows - 2);
}

int PlayerIndex(int listed, int page, int row) {
    if (row < 0 || row >= kRows - 1 || listed <= 0) return -1;
    if (listed <= kRows - 1) return row < listed ? row : -1;
    if (row == kRows - 2) return -1;   // (the page's turn)
    const int pages = PlayerPages(listed);
    const int i = ((page % pages + pages) % pages) * (kRows - 2) + row;
    return i < listed ? i : -1;
}

RowAction ActionFor(View view, int row, const Session& s) {
    if (row < 0 || row >= kRows) return RowAction::kNone;
    if (view == View::kMain) return ActionFor(row, s);
    if (row == kRows - 1) return RowAction::kBack;
    switch (view) {
    case View::kMain: break;
    case View::kPlayers:
        if (s.listed > kRows - 1 && row == kRows - 2) return RowAction::kMorePlayers;
        if (const int i = PlayerIndex(s.listed, s.page, row); i >= 0 && i != s.self) return RowAction::kViewPlayer;
        return RowAction::kNone;   // (our own row: nothing to do to ourselves)
    case View::kPlayer:
        if (s.player != 2) return RowAction::kNone;   // (us, or they left: nothing to do to them)
        if (row == 0) return RowAction::kTeleport;
        if (row == 1) return RowAction::kOwnVoice;
        if (row == 2) return RowAction::kOwnChat;
        return s.hosting ? RowAction::kHostControls : RowAction::kNone;
    case View::kPlayerHost:
        if (s.player != 2 || !s.hosting) return RowAction::kNone;
        if (row == 0) return RowAction::kHostVoice;
        if (row == 1) return RowAction::kHostChat;
        return row == 2 ? RowAction::kKick : RowAction::kBan;
    case View::kLan:
        // (0 the list, online or this network's, 1 the search, 2 the filter, 3 the order; the games themselves are in
        // the cells: Right goes into them. The list's heading pages it and asks it again)
        if (s.in_one || s.by_launcher) return RowAction::kNone;
        if (row == 0) return RowAction::kBrowseList;
        if (row == 1) return RowAction::kTypeSearch;
        if (row == 2) return RowAction::kBrowseFilter;
        if (row == 3) return RowAction::kBrowseSort;
        return RowAction::kNone;
    case View::kLanGame:
        // (0 the game, 1 its password typed on the row if its host asks one, 2 Join this game; the last row is Back.
        // A host on this network lets its players in without the password unless it said otherwise: docs/protocol.md
        // "Same network")
        if (s.in_one || s.by_launcher) return RowAction::kNone;
        if (row == 1) return RowAction::kTypePassword;
        if (row == 2) return RowAction::kJoinLanGame;
        return RowAction::kNone;
    case View::kMenu:
        return row < kMenuTabs ? RowAction::kMenuTab : RowAction::kNone;
    case View::kHost:
        // (hosting: 0 the room or code, 1 Let in, 2 the session's details, 3 Stop hosting; in a friend's game: 0 its
        // details, 1 the Join view, which leaves it; else 0 Start hosting, 1 the host settings)
        if (s.hosting) {
            if (row == 0) return s.code ? RowAction::kCopyCode : RowAction::kViewSession;   // (the code still coming)
            if (row == 1) return RowAction::kViewLetIn;   // (nobody asks: the view says so)
            if (row == 2) return RowAction::kViewSession;
            return row == kHostRowStop ? RowAction::kConfirmLeave : RowAction::kNone;
        }
        if (s.in_one || s.by_launcher) {
            if (row == 0) return RowAction::kViewSession;
            return row == 1 ? RowAction::kViewJoin : RowAction::kNone;
        }
        if (row == 0) return RowAction::kHost;
        return row == 1 ? RowAction::kViewMenu : RowAction::kNone;
    case View::kLetIn:
        if (row == 0 && s.hosting && s.asking) return RowAction::kAllow;
        if (row == 1 && s.hosting && s.asking) return RowAction::kDeny;
        // (nobody asks: Paste answer code first, nothing between it and Back; a tester saw a row there that took no A
        // and a gap under it)
        if (row == 0 && s.hosting && !s.asking && s.invite) return RowAction::kPasteAnswer;
        if (row == 3 && s.hosting && s.asking && s.invite) return RowAction::kPasteAnswer;
        return RowAction::kNone;
    case View::kJoin: {
        const bool free = !s.in_one && !s.by_launcher;   // (as co-op's menu: nobody joins from inside a session)
        // (0 the clipboard's code, 1 and 2 the room name and its password typed on their rows, 3 that room once it
        // has a name, else the last game again; the last row is Back)
        if (s.hosting) return row == 0 ? RowAction::kViewHost : RowAction::kNone;   // (Stop hosting is there)
        if (!free) {
            // (in a friend's game, or co-op that came with the game: 0 its details, 1 Leave after a second A)
            if (row == 0) return s.wait_host ? RowAction::kCopyAnswer : RowAction::kViewSession;
            return row == kJoinRowLeave ? RowAction::kConfirmLeave : RowAction::kNone;
        }
        if (row == 0) return RowAction::kJoinPaste;
        if (row == 1) return RowAction::kTypeRoom;
        if (row == 2) return RowAction::kTypePassword;
        if (row == 3) return s.room_ready ? RowAction::kJoinRoom : s.rejoin ? RowAction::kRejoin : RowAction::kNone;
        return RowAction::kNone;
    }
    case View::kPassword:
        // (the host's join password: shown on the details list, or a new one typed with the typing keys)
        if (!s.hosting || !s.has_password) return RowAction::kNone;
        if (row == 0) return RowAction::kShowPassword;
        return row == 1 ? RowAction::kChangePassword : RowAction::kNone;
    case View::kSession:
        if (s.hosting) {
            if (row == 0) return s.has_password ? RowAction::kShowPassword : RowAction::kNone;   // (shown on the list)
            if (row == 1) return s.code ? RowAction::kCopyCode : RowAction::kNone;
            return row == 2 ? RowAction::kCopyLan : row == 3 ? RowAction::kCopyVpn : RowAction::kNone;
        }
        if (s.failed && !s.in_one) return row == 3 && !s.by_launcher ? RowAction::kHost : RowAction::kNone;
        if (row == 1 && s.wait_host) return RowAction::kCopyAnswer;
        if (row == 3 && s.in_one && s.can_resync) return RowAction::kResync;
        return RowAction::kNone;
    }
    return RowAction::kNone;
}

namespace {

// At most kRowChars bytes, cut before a UTF-8 sequence's start, with "..." when cut.
std::string Fit(std::string t) {
    if (t.size() <= kRowChars) return t;
    std::size_t n = kRowChars - 3;
    while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xc0) == 0x80) --n;
    return t.substr(0, n) + "...";
}

// At most `n` bytes, cut before a UTF-8 sequence's start, with "..." when cut.
std::string FitWidth(std::string t, std::size_t n) {
    if (t.size() <= n) return t;
    std::size_t k = n - 3;
    while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xc0) == 0x80) --k;
    return t.substr(0, k) + "...";
}

// A line over up to `rows` rows of `width` bytes, broken at spaces (a word longer than a row is cut); the last row ends
// in "..." when more is left.
std::vector<std::string> Wrap(const std::string& text, std::size_t rows, std::size_t width = kRowChars) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (out.size() < rows) {
        while (at < text.size() && text[at] == ' ') ++at;
        if (at >= text.size()) break;
        if (out.size() + 1 == rows) {   // (the last row: the rest, cut to fit)
            out.push_back(FitWidth(text.substr(at), width));
            break;
        }
        std::size_t end = std::min(text.size(), at + width);
        if (end < text.size() && text[end] != ' ') {
            const std::size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > at) end = space;
        }
        out.push_back(text.substr(at, end - at));
        at = end;
    }
    return out;
}

std::string Players(int n) {
    return n == 1 ? "just you" : std::to_string(n) + " players";
}

}  // namespace

std::vector<std::string> RowTexts(MinusPage page, const SessionView& v) {
    std::vector<std::string> rows(kRows);
    const auto main = RowTexts(v);   // (the Multiplayer page's entries, their words for what co-op does now)
    switch (page) {
    case MinusPage::kLobby:
        rows[0] = main[kRowFind];
        rows[1] = main[kRowHost];
        rows[2] = main[kRowJoin];
        break;
    case MinusPage::kMultiplayer:
        rows[0] = main[kRowPlayers];
        rows[1] = main[kRowSettings];
        rows[2] = "Local co-op";
        if (v.s.hosting) {
            rows[3] = main[kRowHost];   // ("Your game", "Your game: Saria asks")
        } else if (v.s.in_one || v.s.by_launcher) {
            rows[3] = main[kRowJoin];   // ("Your session", "Turned away", "Copy answer code")
        }
        break;
    case MinusPage::kMods: break;
    }
    return rows;
}

RowAction ActionFor(MinusPage page, int row, const Session& s) {
    switch (page) {
    case MinusPage::kLobby:
        if (row == 0) return ActionFor(kRowFind, s);
        if (row == 1) return ActionFor(kRowHost, s);
        if (row == 2) return ActionFor(kRowJoin, s);
        return RowAction::kNone;
    case MinusPage::kMultiplayer:
        if (row == 0) return ActionFor(kRowPlayers, s);
        if (row == 1) return ActionFor(kRowSettings, s);
        if (row == 2) return RowAction::kViewLocal;
        if (row == 3 && s.hosting) return ActionFor(kRowHost, s);
        if (row == 3 && (s.in_one || s.by_launcher)) return ActionFor(kRowJoin, s);
        return RowAction::kNone;
    case MinusPage::kMods: return RowAction::kNone;
    }
    return RowAction::kNone;
}

std::vector<std::string> RowTexts(const SessionView& v) {
    std::vector<std::string> rows(kRows);
    // (0.11.9: five fixed entries; a word of what co-op does now only where it changes what the entry opens)
    rows[kRowFind] = "Find a game";
    if (v.s.failed && !v.s.in_one) {
        rows[kRowHost] = "Co-op failed: why";
    } else if (v.connecting && v.s.hosting) {
        rows[kRowHost] = "Starting to host...";
    } else if (v.s.hosting) {
        rows[kRowHost] = v.asking.empty()      ? std::string("Your game")
                         : v.asking.size() == 1 ? "Your game: " + v.asking.front() + " asks"
                                                : "Your game: " + std::to_string(v.asking.size()) + " ask";
    } else {
        rows[kRowHost] = "Host a game";
    }
    if (!v.turned_away.empty()) {
        rows[kRowJoin] = "Turned away";   // (co-op is still on: the Join view leaves it)
    } else if (v.s.wait_host) {
        rows[kRowJoin] = !v.note.empty() ? v.note : "Copy answer code";   // (what the copy on this row did)
    } else if (v.connecting && !v.s.hosting) {
        rows[kRowJoin] = "Connecting...";
    } else if (v.s.by_launcher) {
        rows[kRowJoin] = "Your session";
    } else if (v.s.in_one && !v.s.hosting) {
        rows[kRowJoin] = "Your session";
    } else {
        rows[kRowJoin] = "Join a game";
    }
    rows[kRowPlayers] = v.players <= 0 ? "Players"
                        : v.players == 1 ? "Players: just you"
                                         : "Players: " + std::to_string(v.players);
    rows[kRowSettings] = "Co-op settings";
    for (std::string& r : rows) r = Fit(std::move(r));
    return rows;
}

std::vector<std::string> RowTexts(View view, const SessionView& v) {
    if (view == View::kMain) return RowTexts(v);
    std::vector<std::string> rows(kRows);
    rows[kRows - 1] = "Back";
    switch (view) {
    case View::kMain: break;
    case View::kPlayers: {
        const int listed = static_cast<int>(v.names.size());
        if (listed == 0) rows[0] = v.s.in_one ? "Players: coming..." : "Not in a session";
        for (int r = 0; r < kRows - 1; ++r) {
            const int i = PlayerIndex(listed, v.s.page, r);
            if (i >= 0) rows[static_cast<std::size_t>(r)] = v.names[static_cast<std::size_t>(i)];
        }
        if (listed > kRows - 1) {
            const int pages = PlayerPages(listed);
            rows[kRows - 2] = "More players (" + std::to_string((v.s.page % pages + pages) % pages + 1) + "/" +
                              std::to_string(pages) + ")";
        }
        break;
    }
    case View::kLan: {
        if (v.s.in_one || v.s.by_launcher) {
            rows[0] = v.s.by_launcher ? "Co-op came with the game" : "In a session already";
            break;
        }
        // (the list shown and how many it keeps; A flips to the other one)
        rows[0] = std::string(v.s.online ? "Public games" : "This network") + " (" + std::to_string(BrowseListed(v.s)) + ")";
        rows[1] = v.search.empty() && v.typing != 5 ? std::string("Search: A to type") : "Search: " + v.search;
        rows[2] = std::string("Show: ") + CoopBrowse::FilterWords(v.s.browse_filter);
        rows[3] = std::string("Sort: ") + CoopBrowse::SortWords(v.s.browse_sort);
        break;
    }
    case View::kLanGame:
        rows[0] = v.lan_game;
        if (v.s.in_one || v.s.by_launcher) {
            rows[1] = v.s.by_launcher            ? "Co-op came with the game"
                      : !v.turned_away.empty() ? "Turned away: Leave first"
                                               : "In a session already";
            break;
        }
        rows[1] = v.password_field.empty() && v.typing != 1 ? std::string("Password: if it asks")
                                                              : "Password: " + v.password_field;
        rows[2] = "Join this game";
        break;
    case View::kPlayer:
        if (v.s.player == 0) {
            rows[0] = "They left the session";
        } else if (v.s.player == 1) {
            rows[0] = v.player_name + " (you)";
            rows[1] = "This is you";
        } else {
            rows[0] = "Teleport to " + v.player_name;
            rows[1] = (v.own_mutes & kMuteVoice) != 0 ? "Hear their voice again" : "Mute their voice (you)";
            rows[2] = (v.own_mutes & kMuteChat) != 0 ? "Show their chat again" : "Mute their chat (you)";
            if (v.s.hosting) rows[3] = "Host controls";
        }
        break;
    case View::kPlayerHost:
        if (v.s.player != 2 || !v.s.hosting) {
            rows[0] = "They left the session";
        } else {
            rows[0] = (v.host_mutes & kMuteVoice) != 0 ? "Unmute voice for all" : "Mute voice for all";
            rows[1] = (v.host_mutes & kMuteChat) != 0 ? "Unmute chat for all" : "Mute chat for all";
            const std::string again = v.confirm_click ? "Again: " : "A again: ";   // (a name after it: short)
            rows[2] = v.confirm_kick ? again + "kick " + v.player_name : "Kick " + v.player_name;
            rows[3] = v.confirm_ban ? again + "ban " + v.player_name : "Ban for this session";
        }
        break;
    case View::kLetIn:
        if (v.asking.empty()) {
            rows[0] = v.s.hosting && v.s.invite ? "Paste answer code" : "Nobody asks to join";
            break;
        } else {
            rows[0] = "Let in " + v.asking.front();
            rows[1] = "Refuse " + v.asking.front();
            if (v.asking.size() > 1) rows[2] = "+" + std::to_string(v.asking.size() - 1) + " more ask";
        }
        // (a friend whose way in was blocked sends an answer code: rarely needed, so here, not on the five rows)
        if (v.s.hosting && v.s.invite) rows[3] = "Paste answer code";
        break;
    case View::kJoin:
        if (v.s.hosting) {
            rows[0] = "Hosting: open Your game";   // (to join a friend, stop hosting there first)
            break;
        }
        if (v.s.in_one || v.s.by_launcher) {
            rows[0] = v.s.wait_host               ? "Copy answer code"
                      : !v.turned_away.empty()   ? "Turned away: why"
                      : v.s.by_launcher          ? "Co-op came with the game"
                                                 : "Session details";
            rows[kJoinRowLeave] = v.confirm_leave ? (v.confirm_click ? "Click again to leave" : "A again: leave")
                                                  : "Leave the game";
            break;
        }
        rows[0] = v.s.by_launcher            ? "Co-op came with the game"
                  : !v.turned_away.empty() ? "Turned away: Leave first"
                  : v.s.in_one             ? "In a session already"
                                           : "Paste code and join";
        if (!v.s.in_one && !v.s.by_launcher) {
            // (the fields: the row's label, then what was typed, the letter at the cursor in brackets while typing)
            // (short labels: a tester saw "Room: A to type its name" squeezed to fit its button. The first field takes
            // a room name, or an address with its port after a colon, as the title's Join: "Room or address")
            // (typed: a short label, so an address and its port fit whole)
            rows[1] = v.room_field.empty() && v.typing != 0 ? std::string("Room or IP: A to type")
                      : (v.typing == 0 ? "Room or IP: " : "To: ") + v.room_field;
            rows[2] = "Password: " + (v.password_field.empty() && v.typing != 1 ? std::string("A to type") : v.password_field);
            if (v.room_ready) {
                // (an address: the port it joins on, so a tester sees where it goes; another is typed after a colon)
                rows[3] = v.join_address ? "Join (port " + std::to_string(v.join_port > 0 ? v.join_port : 27510) + ")"
                          : v.relay_set  ? "Join this room"
                                         : "Join room: no relay set";
            } else if (v.s.rejoin) {
                rows[3] = v.rejoin_host.empty() ? "Join the last game again" : "Join " + v.rejoin_host + " again";
            }
        }
        break;
    case View::kHost:
        if (v.s.hosting) {
            if (v.connecting) {
                rows[0] = "Starting to host...";
            } else if (!v.note.empty()) {
                rows[0] = v.note;   // (what the copy on this row did)
            } else {
                rows[0] = !v.room.empty() ? "Room: " + v.room : v.s.code ? "Copy invite code" : "Room: coming...";
            }
            if (v.asking.empty()) {
                rows[1] = v.s.invite ? "Let in: answer code" : "Nobody asks to join";
            } else if (v.asking.size() == 1) {
                rows[1] = "Let in " + v.asking.front();
            } else {
                rows[1] = "Let in " + v.asking.front() + " (+" + std::to_string(v.asking.size() - 1) + ")";
            }
            rows[2] = "Session details";
            rows[kHostRowStop] = v.confirm_leave ? (v.confirm_click ? "Click again to stop" : "A again: stop hosting")
                                                 : "Stop hosting";
        } else if (v.s.in_one || v.s.by_launcher) {
            rows[0] = v.s.by_launcher ? "Co-op came with the game" : "In a friend's game";
            rows[1] = "Leave it first: Join";
        } else {
            rows[0] = "Start hosting";
            rows[1] = "Host settings";
        }
        break;
    case View::kMenu:
        for (int i = 0; i < kMenuTabs; ++i) rows[static_cast<std::size_t>(i)] = kMenuTabNames[static_cast<std::size_t>(i)];
        break;
    case View::kPassword:
        // (the host's join password: on the details list while shown; a new one with the typing keys)
        if (v.s.hosting && v.s.has_password) {
            rows[0] = v.s.password_shown && !v.host_password.empty() ? "Hide join password" : "Show join password";
            rows[1] = "Change join password";
        } else {
            rows[0] = "Not hosting";
        }
        break;
    case View::kSession: {
        // (what can be done; the session's details, co-op's problem or hint and the release are on the list on the
        // right, DetailLines)
        if (!v.says.empty()) {   // (a Host or Join of the page's that did not start: why, on the list)
            rows[0] = "Could not start";
            break;
        }
        if (v.s.hosting) {
            if (!v.note.empty()) {
                rows[0] = v.note;
            } else if (v.s.has_password) {
                // (shown or hidden on the list here; a new one is typed from the list's Join password line: a
                // tester found it in two places)
                rows[0] = v.s.password_shown ? "Hide join password" : "Show join password";
            } else {
                rows[0] = "Hosting: " + Players(std::max(v.players, 1));
            }
            rows[1] = !v.room.empty() ? "Copy room: " + v.room : v.s.code ? "Copy invite code" : "Room: coming...";
            rows[2] = "Copy same-network line";
            rows[3] = "Copy VPN line";
            break;
        }
        if (v.s.failed && !v.s.in_one) {
            rows[0] = "Co-op failed";
            if (!v.s.by_launcher) rows[3] = "Host again";
        } else if (!v.turned_away.empty()) {
            rows[0] = "Turned away";   // (the host's game: kicked, banned, full...; as co-op's menu says it)
        } else if (!v.note.empty()) {
            rows[0] = v.note;
        } else if (v.s.wait_host) {
            rows[0] = "Waiting for the host";
        } else if (v.connecting) {
            rows[0] = "Connecting...";
        } else if (v.s.in_one) {
            rows[0] = v.host_name.empty() ? "In a session" : "Joined " + v.host_name;
        } else {
            rows[0] = v.s.by_launcher ? "Co-op came with the game" : "Not in a session";
        }
        if (v.s.wait_host) rows[1] = "Copy answer code";
        if (v.s.in_one && v.s.can_resync) rows[3] = "Resync from host";
        break;
    }
    }
    for (std::string& r : rows) r = Fit(std::move(r));   // ("": the row not shown)
    return rows;
}

const char* ActionWords(RowAction a) {
    switch (a) {
    case RowAction::kOpenMain: return "co-op's menu, its main page";
    case RowAction::kCopyCode: return "the room name, join code or invite code to the clipboard";
    case RowAction::kAllow: return "the oldest player who asks to join let in (/allow)";
    case RowAction::kHost: return "host now, with the kit's settings";
    case RowAction::kConfirmLeave: return "leave (or stop hosting) after a second A";
    case RowAction::kPasteAnswer: return "a friend's answer code from the clipboard (Paste answer), then co-op's main page";
    case RowAction::kCopyAnswer: return "this game's answer code to the clipboard (Copy answer)";
    case RowAction::kViewPlayers: return "the Players view";
    case RowAction::kViewLetIn: return "the Let in view";
    case RowAction::kViewJoin: return "the Join view";
    case RowAction::kBack: return "back";
    case RowAction::kDeny: return "the oldest player who asks to join refused (/deny)";
    case RowAction::kJoinPaste: return "join with the code on the clipboard";
    case RowAction::kRejoin: return "join the last game again";
    case RowAction::kViewSession: return "the Session view";
    case RowAction::kViewHost: return "the Host a game view";
    case RowAction::kViewLocal: return "Settings and look on Local co-op";
    case RowAction::kCopyLan: return "the same-network join line to the clipboard";
    case RowAction::kCopyVpn: return "the VPN join line to the clipboard";
    case RowAction::kResync: return "the host's save again (Resync from host)";
    case RowAction::kViewPlayer: return "a player's view";
    case RowAction::kMorePlayers: return "the Players view's next page";
    case RowAction::kHostControls: return "the host's controls for that player";
    case RowAction::kTeleport: return "teleport to that player (/teleport)";
    case RowAction::kOwnVoice: return "their voice muted or heard again (only here)";
    case RowAction::kOwnChat: return "their chat muted or shown again (only here)";
    case RowAction::kHostVoice: return "their voice muted or unmuted for everyone";
    case RowAction::kHostChat: return "their chat muted or unmuted for everyone";
    case RowAction::kKick: return "kick that player after a second A";
    case RowAction::kTypeRoom: return "the room name typed on its row";
    case RowAction::kViewLan: return "the games on this network";
    case RowAction::kJoinLan: return "that game's view: its password, then Join this game";
    case RowAction::kJoinLanGame: return "join that game on this network, with the password typed";
    case RowAction::kMoreLan: return "the next page of games on this network";
    case RowAction::kAutoRejoin: return "auto-rejoin on or off";
    case RowAction::kShowPassword: return "the join password shown or hidden";
    case RowAction::kViewPassword: return "the Join password view";
    case RowAction::kChangePassword: return "a new join password typed with the typing keys";
    case RowAction::kChangePort: return "the port when hosting typed with the typing keys";
    case RowAction::kTypeRelay: return "the relay's address typed with the typing keys";
    case RowAction::kTypeSearch: return "the browser's search typed with the typing keys";
    case RowAction::kBrowseList: return "the browser's other list";
    case RowAction::kBrowseFilter: return "the browser's next filter";
    case RowAction::kBrowseSort: return "the browser's next order";
    case RowAction::kTypeRoomName: return "this game's room name typed with the typing keys (saved)";
    case RowAction::kBrowseLan: return "the browser lists the games on this network";
    case RowAction::kBrowseOnline: return "the browser lists the relay's public rooms";
    case RowAction::kBrowsePage: return "the browser's next page of games";
    case RowAction::kBrowseRefresh: return "the browser's list asked again";
    case RowAction::kListRoom: return "this game's room listed online when it hosts, on or off (saved)";
    case RowAction::kTypePassword: return "the join password typed on its row";
    case RowAction::kJoinRoom: return "join that room through the kit's relay";
    case RowAction::kBan: return "ban that player for this session after a second A";
    case RowAction::kViewMenu: return "Settings and look (co-op's menu's Customize, Settings, Commands, Local co-op)";
    case RowAction::kMenuTab: return "that page's rows on the list, the list has the pad";
    case RowAction::kNone: break;
    }
    return "nothing";
}

// --- ScreenGuard ---

namespace {
thread_local bool t_in_screen = false;
}  // namespace

ScreenGuard::ScreenGuard() : nested_(t_in_screen) {
    t_in_screen = true;
}

ScreenGuard::~ScreenGuard() {
    if (!nested_) t_in_screen = false;
}

// --- Probe ---

std::vector<std::string> Census::Seen(std::uint64_t vtable_offset, int state, const Menu& menu, std::uint64_t now_ms) {
    std::vector<std::string> out;
    char line[160];
    const auto menu_words = [&](const Menu& m) {
        std::snprintf(line, sizeof line, "census: menu plus=%d minus=%d minus_on=%d", m.plus, m.minus, m.minus_on);
        return std::string(line);
    };
    if (!menu_ || !(*menu_ == menu)) {
        menu_ = menu;
        out.push_back(menu_words(menu));
    }
    for (auto it = screens_.begin(); it != screens_.end();) {
        if (it->vtable != vtable_offset && now_ms > it->last_ms + kCensusGoneMs) {
            std::snprintf(line, sizeof line, "census: screen 0x%llx gone", static_cast<unsigned long long>(it->vtable));
            out.emplace_back(line);
            it = screens_.erase(it);
        } else {
            ++it;
        }
    }
    auto it = std::find_if(screens_.begin(), screens_.end(), [&](const Screen& s) { return s.vtable == vtable_offset; });
    if (it == screens_.end()) {
        screens_.push_back({vtable_offset, state, now_ms});
        std::snprintf(line, sizeof line, "census: screen 0x%llx seen, state %d", static_cast<unsigned long long>(vtable_offset),
                      state);
        out.emplace_back(line);
        return out;
    }
    if (it->state != state) {
        std::snprintf(line, sizeof line, "census: screen 0x%llx state %d -> %d", static_cast<unsigned long long>(vtable_offset),
                      it->state, state);
        out.emplace_back(line);
        it->state = state;
    }
    it->last_ms = now_ms;
    return out;
}

std::vector<std::string> Probe::Seen(const Facts& f, std::uint64_t now_ms) {
    std::vector<std::string> out;
    if ((f.kind != Kind::kPages && f.kind != Kind::kStrip) || f.screen == 0) {
        return out;
    }
    Last& last = f.kind == Kind::kPages ? pages_ : strip_;
    const char* what = f.kind == Kind::kPages ? "the pause menu's pages (PlusMenu_00)" : "the tab strip (MenuOverlay_00)";
    if (last.screen != f.screen) {
        last = Last{};
        last.screen = f.screen;
        last.first_ms = now_ms;
        out.push_back("coop: pause tab: " + std::string(what) + ", screen " + Hex(f.screen) +
                      ": its updates reach the module (patch 0030); " + Shows(f));
        last.facts = f;
    } else if (Shows(f) != Shows(last.facts)) {
        out.push_back("coop: pause tab: " + std::string(what) + ": " + Shows(f) + " (was " + Shows(last.facts) + ")");
        last.facts = f;
    }
    ++last.updates;
    if (!last.counted && now_ms - last.first_ms >= 1000) {
        last.counted = true;
        out.push_back("coop: pause tab: " + std::string(what) + ": " + std::to_string(last.updates) +
                      " updates in its first " + std::to_string(now_ms - last.first_ms) + " ms");
    }
    return out;
}

namespace {
// At most `n` bytes, cut before a UTF-8 sequence's start, with "..." when cut (the list's columns are wider than a row).
std::string FitTo(std::string t, std::size_t n) {
    if (t.size() <= n) return t;
    std::size_t k = n - 3;
    while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xc0) == 0x80) --k;
    return t.substr(0, k) + "...";
}

}  // namespace

std::string ListValue(int players, int max, bool same_version) {
    if (!same_version) return "Other version";
    if (max > 0 && players >= max) return "Full";
    return max > 0 ? std::to_string(players) + "/" + std::to_string(max) : std::to_string(players);
}

std::string ListName(const std::string& shown, const std::string& room) {
    // (room names are the same name in any case on the relay: room_key)
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    const bool same = shown.size() == room.size() &&
                      std::equal(shown.begin(), shown.end(), room.begin(),
                                 [&](char a, char b) { return lower(a) == lower(b); });
    if (shown.empty()) return room;
    return same ? shown : shown + " (" + room + ")";
}

std::vector<SettingRow> SettingsOf(const std::vector<SettingRow>& all, int category) {
    std::vector<SettingRow> out;
    if (category < 0 || category >= kSettingRows) return out;
    // (Longshot names its tabs in lower case, "graphics": the match ignores case)
    const std::string want = kSettingTabs[static_cast<std::size_t>(category)];
    const auto same = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    // (Longshot's NFC tab has one setting, the tag to use: it goes with Controls, as the game's reader is a pad's)
    for (const SettingRow& r : all) {
        if (same(r.tab, want) || (category == 3 && same(r.tab, "nfc"))) out.push_back(r);
    }
    return out;
}

int SettingPages(int count, int per_page) {
    per_page = std::clamp(per_page, 1, kSettingsPerPageMost);
    return count <= per_page ? 1 : (count + per_page - 1) / per_page;
}

std::vector<ListLine> SettingLines(const std::vector<SettingRow>& rows, int page, const std::string& title, int per_page) {
    per_page = std::clamp(per_page, 1, kSettingsPerPageMost);
    std::vector<ListLine> lines(kSettingListRows);
    const int pages = SettingPages(static_cast<int>(rows.size()), per_page);
    const int at_page = (page % pages + pages) % pages;
    // (the header has no value: the module hides its value box and arrows, so it never looks like a setting)
    // (the page with arrows a click on either half of the line turns: the module's mouse)
    lines[0] = ListLine{FitTo(pages > 1 ? title + "  < " + std::to_string(at_page + 1) + " of " + std::to_string(pages) + " >"
                                        : title,
                              kListNameChars),
                        ""};
    for (int i = 0; i < per_page; ++i) {
        const std::size_t at = static_cast<std::size_t>(at_page * per_page + i);
        if (at >= rows.size()) break;
        const SettingRow& r = rows[at];
        lines[static_cast<std::size_t>(i) + 1] =
            ListLine{FitTo(r.restart ? r.name + " *" : r.name, kListNameChars), FitTo(r.text, kListValueChars)};
    }
    return lines;
}

int SettingNext(const SettingRow& r, int dir) {
    if (r.fixed || r.hi < r.lo) return r.value;
    const int step = r.step > 0 ? r.step : 1;
    if (dir == 0) {
        if ((r.hi - r.lo) / step + 1 > kCycleChoices) return r.value;   // (a slider: Left and Right only)
        return r.value + step > r.hi ? r.lo : r.value + step;
    }
    return std::clamp(r.value + (dir < 0 ? -step : step), r.lo, r.hi);
}

void SettingMove(int count, int dy, int* page, int* row, int per_page) {
    per_page = std::clamp(per_page, 1, kSettingsPerPageMost);
    if (count <= 0) {
        *page = 0, *row = 0;
        return;
    }
    const int at = std::clamp(*page * per_page + *row + dy, 0, count - 1);
    *page = at / per_page, *row = at % per_page;
}

std::vector<ListLine> ListLines(const SessionView& v) {
    std::vector<ListLine> lines(kListRows);
    if (v.s.in_one || v.s.by_launcher) return lines;
    // (a heading of text: "Game | Players" with its value box read as a button that did nothing, a tester)
    // (the heading: the list and its page, "< 1 of 3 >" when there are more (a click on its left half goes back a
    // page, on its right half on); its box asks the list again (a click, or Y))
    const int pages = CellPages(BrowseListed(v.s));
    std::string head = v.s.online ? "Public games" : "This network";
    if (pages > 1) head += "  < " + std::to_string((v.s.cell_page % pages + pages) % pages + 1) + " of " +
                           std::to_string(pages) + " >";
    lines[0] = ListLine{head, "Refresh", RowAction::kBrowseRefresh};
    const std::vector<ListLine>& list = v.s.online ? v.online_list : v.lan_list;
    if (list.empty()) {
        lines[1].name = FitTo(v.browse_all_out ? std::string("None match the search or Show")
                              : v.s.online     ? (v.online_state.empty() ? std::string("Asking the relay...") : v.online_state)
                                               : std::string("Looking for games..."),
                              kListNameChars);
        if (v.s.online && !v.relay_set) {   // (no relay: typed here, saved as Co-op Settings' Relay row)
            lines[1].name = "No relay set: A to type one";
            lines[1].act = RowAction::kTypeRelay;
        }
        return lines;
    }
    for (int c = 0; c < kCells; ++c) {
        if (const int i = CellIndex(v.s, c); i >= 0 && static_cast<std::size_t>(i) < list.size()) {
            const ListLine& l = list[static_cast<std::size_t>(i)];
            lines[static_cast<std::size_t>(c) + 1] = ListLine{FitTo(l.name, kListNameChars), FitTo(l.value, kListValueChars)};
        }
    }
    return lines;
}

bool RoomMatches(const std::string& room, const std::string& host, const std::string& search) {
    if (search.empty()) return true;
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(search);
    return lower(room).find(want) != std::string::npos || lower(host).find(want) != std::string::npos;
}

std::vector<ListLine> DetailLines(View view, const SessionView& v) {
    std::vector<ListLine> lines(kListRows);
    if (view != View::kMain && view != View::kSession && view != View::kPassword) return lines;
    std::size_t at = 1;
    const auto add = [&](const std::string& name, const std::string& value) {
        if (at < lines.size()) lines[at++] = ListLine{FitTo(name, kListNameChars), FitTo(value, kListValueChars)};
    };
    // (the release a short value: "Squadala! v0.11.6" is "v0.11.6")
    std::string release = v.version;
    if (const std::string brand = "Squadala! "; release.rfind(brand, 0) == 0) release.erase(0, brand.size());
    std::string header, why = v.why;
    if (!v.says.empty()) {
        header = "Could not start", why = v.says;
    } else if (v.s.failed && !v.s.in_one) {
        header = "Co-op failed";
    } else if (!v.turned_away.empty()) {
        header = "Turned away";
        why = v.turned_away + "." + (v.banned ? " Leave." : " Leave, then join again.");
    } else if (v.s.hosting) {
        header = "Hosting";
    } else if (v.s.wait_host) {
        header = "Waiting for the host";
    } else if (v.connecting) {
        header = "Connecting...";
    } else if (v.s.in_one) {
        header = "In a session";
    } else {
        header = v.s.by_launcher ? "Co-op came with the game" : "Not in a session";
    }
    // (the header's value: hosting, who can reach this game. The header had none: the game's cursor frame, which
    // Longshot's mouse puts on the line under it, framed an empty box there, a tester's screenshot 2026-11-19)
    std::string reach;
    if (v.s.hosting && v.says.empty()) {
        if (v.port_open || !v.room.empty() || v.s.invite) {
            reach = "Online";
        } else if (v.problem == HOS_COOP_PROBLEM_NO_INVITE_CODE) {
            reach = "This network";
        } else {
            reach = "Starting...";
        }
    }
    lines[0] = ListLine{FitTo(header, kListNameChars), FitTo(reach, kListValueChars)};
    // The host's known problems and hints in words of ours, one line each (the Router line above says the port's
    // state): the server's message ("the router did not open the port (UPnP, PCP and NAT-PMP are off on it, or it said
    // no)") cut over three lines read as pieces of a sentence (a tester's screenshot, 2026-11-19). Other messages as
    // they are, with a capital letter.
    if (v.s.hosting && v.says.empty()) {
        if (v.problem == HOS_COOP_PROBLEM_UPNP_REFUSED) {
            // (what to forward by hand; the invite code or a room still gets friends in meanwhile)
            why = v.port > 0 ? "Forward UDP " + std::to_string(v.port) + " to this PC" : "Forward the port to this PC";
        } else if (v.problem == HOS_COOP_PROBLEM_NO_INVITE_CODE) {
            why = "Only this network can join";
        } else if (v.problem == HOS_COOP_PROBLEM_SYMMETRIC_NAT) {
            why = "Strict router: try a relay";
        } else if (!why.empty() && why[0] >= 'a' && why[0] <= 'z') {
            why[0] = static_cast<char>(why[0] - 'a' + 'A');
        }
    }
    if (v.s.hosting && v.says.empty()) {
        // (each line that can do something does it on an A or a click, as a tester expected of rows that look like
        // the game's Options: the room's name or code copied, the players listed, a new join password, another port)
        add("Room", !v.room.empty() ? v.room : v.s.invite ? "Invite code" : "Coming...");
        if (v.s.code) lines[at - 1].act = RowAction::kCopyCode;
        add("Players in", std::to_string(std::max(v.players, 1)));
        if (v.s.listed > 1) lines[at - 1].act = RowAction::kViewPlayers;
        if (!v.s.has_password) {
            add("Join password", "None");
        } else if (!v.s.password_shown || v.host_password.empty()) {
            add("Join password", "Hidden");
            lines[at - 1].act = RowAction::kChangePassword;
        } else {
            // (in fives, ten letters a line: a made one is 20, two lines; a longer one cut on its third)
            for (std::size_t r = 0; r < 3 && r * 10 < v.host_password.size(); ++r) {
                std::string line;
                for (std::size_t i = r * 10; i < v.host_password.size() && i < r * 10 + 10; ++i) {
                    if (i > r * 10 && (i - r * 10) % 5 == 0) line += ' ';
                    line += v.host_password[i];
                }
                if (r == 2 && v.host_password.size() > 30) line = FitTo(line + "...", kListValueChars);
                add(r == 0 ? "Join password" : " ", line);
                lines[at - 1].act = RowAction::kChangePassword;
            }
        }
        if (!v.asking.empty()) add("Asking to join", std::to_string(v.asking.size()));   // (nobody: the Let in row says so)
        // (the port itself: a shut one is forwarded by hand; an A types another, and hosting starts again on it)
        const std::string port = v.port > 0 ? std::to_string(v.port) + " " : std::string();
        const std::size_t before = at;
        if (v.port_open) {
            add("Port", port + "open");
        } else if (v.problem == HOS_COOP_PROBLEM_UPNP_REFUSED || v.problem == HOS_COOP_PROBLEM_NO_INVITE_CODE) {
            add("Port", port + "shut");
        } else if (v.port > 0) {
            add("Port", std::to_string(v.port));
        }
        if (at > before) lines[at - 1].act = RowAction::kChangePort;
    } else if (v.s.in_one && !v.connecting && v.says.empty() && v.turned_away.empty()) {
        if (!v.host_name.empty()) add("Host", v.host_name);
        if (v.players > 0) add("Players", std::to_string(v.players));
    } else if (!v.s.in_one && !v.s.by_launcher && !v.s.failed && v.says.empty() && v.turned_away.empty()) {
        // (the room this game hosts under, and whether the relay lists it: set here as on the title's Co-op Settings,
        // a tester asked why it could not be set in the game)
        add("Room name", v.room_name.empty() ? "None" : v.room_name);
        lines[at - 1].act = RowAction::kTypeRoomName;
        add("List my room", v.s.list_room ? "On" : "Off");
        lines[at - 1].act = RowAction::kListRoom;
        add("Auto-rejoin", !v.s.auto_rejoin ? "Off" : v.s.rejoining ? "Trying..." : "On");
        // (it looks like a setting of the game's Options, so an A on it turns it on or off, as the row of the same
        // name: a tester pressed it there and nothing happened)
        lines[at - 1].act = RowAction::kAutoRejoin;
    }
    // (co-op's problem or hint as lines of text on the lines left, the release on the last when it is free: the hint
    // takes it when it needs it)
    const std::size_t left = lines.size() - at;
    if (!why.empty() && left > 0) {
        const bool all = release.empty() || Wrap(why, left, kListNameChars).size() >= left;
        for (const std::string& l : Wrap(why, all ? left : left - 1, kListNameChars)) lines[at++] = ListLine{l, ""};
    }
    if (!release.empty()) add("Release", release);
    return lines;
}

CoopPauseMenu::Page MenuTabPage(int tab) {
    switch (tab) {
    case 0: return CoopPauseMenu::Page::kCustomize;
    case 1: return CoopPauseMenu::Page::kSettings;
    case 2: return CoopPauseMenu::Page::kCommands;
    default: return CoopPauseMenu::Page::kLocal;
    }
}

std::string MenuValue(const CoopPauseMenu::Row& r, bool typing) {
    using CoopPauseMenu::RowKind;
    if (typing) return !r.value.empty() && r.value.back() == '_' ? r.value : r.value + "_";   // (some end in it already)
    switch (r.kind) {
    case RowKind::kToggle: return !r.value.empty() ? r.value : r.on ? "On" : "Off";
    case RowKind::kChoice: {
        std::string v = std::string(r.left ? "< " : "") + r.value + (r.right ? " >" : "");
        // (with its arrows it would be cut: "< By player n..."; the value alone when that fits)
        return v.size() > kListValueChars && r.value.size() <= kListValueChars ? r.value : v;
    }
    case RowKind::kHeading:
    case RowKind::kNote: return "";
    case RowKind::kAction: break;
    }
    return r.value;
}

std::string MenuHeadingOf(const CoopPauseMenu::View& menu, int row) {
    for (int i = std::min(row, static_cast<int>(menu.rows.size()) - 1); i >= 0; --i) {
        if (menu.rows[static_cast<std::size_t>(i)].kind == CoopPauseMenu::RowKind::kHeading) {
            return menu.rows[static_cast<std::size_t>(i)].name;
        }
    }
    return "";
}

MenuList MenuLines(const CoopPauseMenu::View& menu, int tab, int top, bool list, int per_page) {
    using CoopPauseMenu::RowKind;
    const int cells = std::clamp(per_page, 1, kMenuMost);
    MenuList out;
    out.lines.assign(kMenuMost + 1, ListLine{});
    out.rows.fill(-1);
    out.selectable.fill(false);
    const int n = static_cast<int>(menu.rows.size());
    const int most = std::max(0, n - cells);
    top = std::clamp(top, 0, most);
    const int sel = menu.selected;
    if (list && sel >= 0 && sel < n) {   // (the selected row in view: the list scrolls with it)
        if (sel < top) top = sel;
        if (sel >= top + cells) top = sel - cells + 1;
        // (a heading just above it shown too, when there is room: its part's name)
        if (top > 0 && top == sel && menu.rows[static_cast<std::size_t>(top - 1)].kind == RowKind::kHeading) --top;
    }
    out.top = top;
    std::string head = tab >= 0 && tab < kMenuTabs ? kMenuTabNames[static_cast<std::size_t>(tab)] : "";
    if (n > cells) {
        const int pages = (n + cells - 1) / cells;
        const int page = top + cells >= n ? pages : std::min(pages, (top + cells / 2) / cells + 1);
        head += "  < " + std::to_string(page) + " of " + std::to_string(pages) + " >";
    }
    out.lines[0] = ListLine{FitTo(head, kListNameChars), ""};
    for (int c = 0; c < cells; ++c) {
        const int i = top + c;
        if (i >= n) break;
        const CoopPauseMenu::Row& r = menu.rows[static_cast<std::size_t>(i)];
        const std::size_t k = static_cast<std::size_t>(c) + 1;
        const bool typing = menu.typing && i == sel;
        std::string name = r.name;
        if (r.kind == RowKind::kNote && name.empty()) name = r.value;   // (a note's words, whichever holds them)
        out.lines[k] = ListLine{FitTo(name.empty() ? std::string(" ") : name, kListNameChars),
                                FitTo(r.kind == RowKind::kNote ? std::string() : MenuValue(r, typing), kListValueChars)};
        out.rows[k] = i;
        out.selectable[k] = r.kind != RowKind::kHeading && r.kind != RowKind::kNote;
        if (list && i == sel) out.picked = c;
    }
    return out;
}

}  // namespace CoopPauseTab
