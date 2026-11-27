// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's customization in Character Select's own Customize screen (0.11.9; the owner: "our customization in place of
// his, keep his UI"). Snail's screen (OoT Character Select, MIT; mods/customize.py: layout CharacterCustomize_00)
// stays as it is: its two columns of three options, each a label (T_Label_ff) over a value (one text pane per value,
// T_Val_ff_vv, shown by its "Val_ff" animation frame), its focus marker moved by his graph (up and down in a column,
// left and right across, no wrap; the first option at the start). Co-op drives what it shows and what its keys do:
// the six options are a page of co-op's look (CoopLookMenu::Page in pages of six: Clothes, Shields, Swords), every
// value pane of an option shows co-op's value, ZL / ZR (his slider keys) step the focused part's colour, A opens
// co-op's any-colour mixer in the same six places (Hue, Saturation, Brightness | Preset, Default colour, Done), Y
// turns the page, B leaves the mixer (else his: back). The d-pad stays his graph's, so his marker moves, and this
// mirrors his rule to know the row.
//
// Logic only (no game, no files): the module (coop_pause_tab_module.cpp) finds his screen, writes the texts and
// hands the presses; tests/hookshot/snail_custom_test.cpp drives it.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "coop_look.h"
#include "coop_look_menu.h"

namespace CoopI18n {
struct Lang;
}

namespace CoopSnailCustom {

inline constexpr int kOptions = 6, kRows = 3;   // his layout: two columns of three
enum class Dir { kUp, kDown, kLeft, kRight };
// His focus rule (customize.py neighbour): the option the d-pad goes to from `f`, or -1 (none: no wrap).
int Neighbour(int f, Dir dir);

// What his screen shows: the title line, and each option's label and value.
struct Texts {
    std::string title;
    std::array<std::string, kOptions> labels{}, values{};
    bool operator==(const Texts&) const = default;
};
// What a pane of his screen is given for a text: a text pane's text is never empty (the game's set-message call
// takes no empty one, as the pause tab's rows), so a place with nothing (an option past a character's last row, a
// character with no options) shows a space.
inline std::string PaneText(const std::string& s) { return s.empty() ? std::string(" ") : s; }
// A value with its first letter a capital, as his screen's own values ("red" -> "Red"); see the .cpp.
std::string Capitalised(std::string s);

// Switch pad bits (nn::hid's), as the pause tab has them.
inline constexpr std::uint64_t kA = 1ull << 0, kB = 1ull << 1, kY = 1ull << 3, kZL = 1ull << 8, kZR = 1ull << 9,
                               kMinus = 1ull << 11, kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14,
                               kDown = 1ull << 15, kStickLeft = 1ull << 16, kStickUp = 1ull << 17,
                               kStickRight = 1ull << 18, kStickDown = 1ull << 19;

// The two key parts the kit puts in his layout (tools/puppet_pack/merge_tables.py SNAIL_KEYS, Y then A) and each one's
// text panes (the game's PaGuideKey_00: the button letter, then the words), whose labels are "<part>-<pane>" in his
// message file. chartab-0121: the game does not give a copied part's panes those labels when it makes the layout, so
// the module sets each pane's text itself (coop_pause_tab_module.cpp SnailKeysLookUp).
inline constexpr const char* kKeyParts[] = {"Pa_CoopKeyY_00", "Pa_CoopKeyA_00"};
inline constexpr const char* kKeyPanes[] = {"T_Icon_00", "T_Guide_00"};
// Whether his screen shows the keys as the game's guide keys (the title then drops its worded hints): only with both
// parts in his layout AND every one of their panes given its text. chartab-0121 (owner, v0.12.0): the parts were there,
// their texts were not, so the keys drew nothing and the title had no hints either: neither "page" nor "exact hue"
// could be seen. Anything less keeps the hints in the title's words.
constexpr bool KeysShownFrom(bool page_part, bool mix_part, bool texts_set) {
    return page_part && mix_part && texts_set;
}

class Driver {
public:
    // His screen opened on co-op's look (the one waiting, else worn), for the character shown (0 Link).
    void Open(const CoopLook::Look& look, std::uint16_t character);
    void Close() { active_ = false; }
    // npc-customize: the rows your character's model has (CoopCharColours::Rows; 0 not known yet: every row).
    void SetRows(std::uint8_t rows) { page_.SetRows(rows); }
    bool Active() const { return active_; }
    // One pad read's new presses while his screen shows: the presses his screen still gets (the d-pad and the stick;
    // B and - unless the mixer is open); the rest are co-op's.
    std::uint64_t Pad(std::uint64_t down);
    // His screen's texts in the game's language (CoopI18n::Current); Shown(lang) in that one (nullptr: English).
    Texts Shown() const;
    Texts Shown(const CoopI18n::Lang* lang) const;
    // menu-customize-01113: the keys his screen shows as the game's guide keys (the pack's Pa_CoopKeyY_00 / _A_00 in
    // his layout, merge_tables.py add_snail_keys): Y turns the page (more than one page, the mixer closed), A opens the
    // mixer (a page with parts, the mixer closed). With them shown (SetKeysShown(true)) the title is the page's name
    // alone; without them (a kit not merged again) it keeps the hints in words.
    struct Keys {
        bool page = false, mix = false;
        bool operator==(const Keys&) const = default;
    };
    Keys KeysNow() const;
    void SetKeysShown(bool on) { keys_shown_ = on; }
    bool KeysShown() const { return keys_shown_; }
    int Focus() const { return focus_; }
    bool Editing() const { return page_.editing(); }
    // The page of six shown (1 first) and how many there are (0: none to show, a character with no options).
    int PageNo() const { return page_.GroupsOffered() == 0 ? 0 : static_cast<int>(page_.PageIndex()) + 1; }
    int Pages() const { return static_cast<int>(page_.GroupsOffered()); }
    // The look after a change, once.
    std::optional<CoopLook::Look> TakeChange() { return page_.TakeChange(); }

private:
    CoopLookMenu::Page page_;
    int focus_ = 0;
    bool active_ = false;
    bool keys_shown_ = false;
};

}  // namespace CoopSnailCustom
