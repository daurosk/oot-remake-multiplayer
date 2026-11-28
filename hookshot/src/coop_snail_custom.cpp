// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_snail_custom.h"

#include "coop_i18n.h"

namespace CoopSnailCustom {

namespace {
constexpr std::size_t kTitleBytes = 61;   // (the title pane's room: the placeholder's)
}  // namespace

int Neighbour(int f, Dir dir) {
    if (f < 0 || f >= kOptions) return -1;
    const int side = f / kRows, i = f % kRows;
    int to = -1;
    switch (dir) {
    case Dir::kUp: to = i > 0 ? f - 1 : -1; break;
    case Dir::kDown: to = i < kRows - 1 ? f + 1 : -1; break;
    case Dir::kLeft: to = side > 0 ? f - kRows : -1; break;
    case Dir::kRight: to = f + kRows; break;
    }
    return to >= 0 && to < kOptions ? to : -1;
}

std::uint64_t GameKeys(std::uint64_t in, std::uint64_t down, std::uint64_t his, bool mixer, std::uint64_t* kept) {
    constexpr std::uint64_t kDirs = kUp | kDown | kLeft | kRight | kStickUp | kStickDown | kStickLeft | kStickRight;
    constexpr std::uint64_t kOurs = kB | kMinus;   // (the keys co-op may take: B leaves its mixer, - stays in it)
    *kept = (*kept | (down & kOurs & ~his)) & in;   // (taken at its press, co-op's until let go)
    std::uint64_t pass = kDirs | kOurs | kPlus;
    if (mixer) pass &= ~kOurs;   // (B leaves co-op's mixer, not his screen; - does not close his screen from it)
    return in & pass & ~*kept;
}

void Driver::Open(const CoopLook::Look& look, std::uint16_t character) {
    page_.SetPaged(true);
    page_.SetCharacter(character, false);
    page_.Open(look);
    focus_ = 0;   // (his screen opens on its first option)
    active_ = true;
}

std::uint64_t Driver::Pad(std::uint64_t down) {
    if (!active_) return down;
    std::uint64_t his = 0;
    // (the d-pad and the stick: his graph moves his marker; we follow his rule)
    const struct {
        std::uint64_t bits;
        Dir dir;
    } moves[] = {{kUp | kStickUp, Dir::kUp}, {kDown | kStickDown, Dir::kDown},
                 {kLeft | kStickLeft, Dir::kLeft}, {kRight | kStickRight, Dir::kRight}};
    for (const auto& m : moves) {
        if ((down & m.bits) == 0) continue;
        his |= down & m.bits;
        if (const int to = Neighbour(focus_, m.dir); to >= 0) focus_ = to;
    }
    const std::size_t row = static_cast<std::size_t>(focus_ + 1);   // (the page's rows: its heading, then the six)
    const bool has = row < page_.view().rows.size();   // (a page of fewer: his option past them does nothing)
    if (has && (down & (kZL | kZR)) != 0) {
        page_.Select(row);
        page_.Press((down & kZR) != 0 ? CoopLookMenu::Input::kRight : CoopLookMenu::Input::kLeft);
    }
    if (has && (down & kA) != 0) {
        page_.Select(row);
        page_.Press(CoopLookMenu::Input::kDecide);
    }
    if ((down & kY) != 0) page_.Press(CoopLookMenu::Input::kPageNext);
    if ((down & kB) != 0) {
        if (page_.editing()) {
            page_.Press(CoopLookMenu::Input::kBack);   // (the mixer: back to the parts, his screen stays)
        } else {
            his |= kB;   // (his: back to his picker)
        }
    }
    if ((down & kMinus) != 0 && !page_.editing()) his |= kMinus;
    return his;
}

// A value as his screen writes its values ("Default", "Same as tunic"): its first letter a capital, after the
// translation (the colour names are lower case for chat and the files, "red"; capitalised before it, "Red" would be
// another line of the table, zh's "红" of a colour's red part, not the colour "红色"). A text not starting with an
// ASCII letter (#RRGGBB, Chinese) comes back unchanged.
std::string Capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

Driver::Keys Driver::KeysNow() const {
    Keys k;
    if (!active_ || page_.editing()) return k;
    k.page = page_.GroupsOffered() > 1;
    k.mix = page_.GroupsOffered() >= 1;
    return k;
}

Texts Driver::Shown() const { return Shown(CoopI18n::Current()); }

// In the player's language, as co-op's own pages put their rows' names and values (CoopI18n::Tr at the write, the
// pause tab's list): a name or value already in it, or a colour of their own (#RRGGBB), comes back unchanged.
Texts Driver::Shown(const CoopI18n::Lang* lang) const {
    const CoopLookMenu::View v = page_.view();
    Texts t;
    if (v.rows.empty()) return t;
    t.title = CoopI18n::Tr(lang, v.rows[0].name);
    // (the title's pane takes 61 bytes at most: a longer text is refused by the game and Character Select's own "Customize"
    // stays, seen in zh, 62 bytes, and counted for en page 4 (66) and zh page 4 (71). The page's name comes first; the Y
    // hint (more than one page), then the A hint (any set with colours: it opens the mixer) follow only while the
    // whole stays within it, the A hint dropped before the Y hint.)
    const auto add = [&](const char* hint) {
        if (keys_shown_) return;   // (the keys show as the game's guide keys: KeysNow)
        const std::string more = "  " + CoopI18n::Tr(lang, hint);
        if (t.title.size() + more.size() <= kTitleBytes) t.title += more;
    };
    if (!page_.editing() && page_.GroupsOffered() > 1) add("Y: page");
    if (!page_.editing() && page_.GroupsOffered() >= 1) add("A: Fine-tune");
    for (int f = 0; f < kOptions; ++f) {
        const std::size_t r = static_cast<std::size_t>(f + 1);
        if (r >= v.rows.size()) break;
        t.labels[static_cast<std::size_t>(f)] = CoopI18n::Tr(lang, v.rows[r].name);
        // (an action row, Default colour or Done, has no value of its own: its key)
        t.values[static_cast<std::size_t>(f)] = v.rows[r].kind == 0 ? std::string("A") : Capitalised(CoopI18n::Tr(lang, v.rows[r].value));
    }
    return t;
}

}  // namespace CoopSnailCustom
