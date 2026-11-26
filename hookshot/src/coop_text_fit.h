// SPDX-License-Identifier: GPL-3.0-or-later
// One place that decides how much text a native row, button or caption holds (the game's text panes have a fixed
// width and a proportional font): the rows used to be cut by BYTES (coop_pause_tab.cpp Fit/FitWidth/FitTo,
// coop_title_native.cpp Fit) or by code points (the microphone's name), which let a run of wide letters ("WWWWWWWW"),
// wide scripts counted as one letter (a 28-character Korean microphone name) and, in Wrap, a cut inside a
// multi-byte letter (a Chinese line has no spaces) run past the pane or show a broken letter.
//
// A text's width is counted in half-letters: an ordinary letter is 2, the widest letters of the game's font (W, @, %)
// are 3, a wide script's character (CJK, kana, hangul, fullwidth forms) is 4. A
// budget of `n` letters is 2 * n half-letters, so every text that fitted before still fits and the byte limit is kept
// as well (the pane's text buffer): a cut text is never longer than before. Logic only: tests/hookshot/text_fit_test.cpp.
#pragma once

#include <algorithm>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace CoopTextFit {

// The code point that starts at s[at] and how many bytes it takes (a broken byte counts as one letter of 1 byte).
inline std::uint32_t Decode(std::string_view s, std::size_t at, std::size_t* bytes) {
    const unsigned char c = static_cast<unsigned char>(s[at]);
    std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (at + n > s.size()) n = 1;
    std::uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (std::size_t i = 1; i < n; ++i) {
        const unsigned char d = static_cast<unsigned char>(s[at + i]);
        if ((d & 0xC0) != 0x80) {
            n = 1, cp = c;
            break;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    *bytes = n;
    return cp;
}

// Half-letters one code point takes.
inline int Half(std::uint32_t cp) {
    if (cp == 'W' || cp == '@' || cp == '%') return 3;
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
    return wide ? 4 : 2;
}

// The width of `s`, in half-letters.
inline std::size_t Halves(std::string_view s) {
    std::size_t w = 0;
    for (std::size_t at = 0, n = 1; at < s.size(); at += n) w += static_cast<std::size_t>(Half(Decode(s, at, &n)));
    return w;
}

// The longest start of `s` (a whole number of letters) that holds at most `bytes` bytes and `halves` half-letters.
inline std::size_t PrefixEnd(std::string_view s, std::size_t bytes, std::size_t halves) {
    std::size_t end = 0, w = 0;
    for (std::size_t at = 0, n = 1; at < s.size(); at += n) {
        const int h = Half(Decode(s, at, &n));
        if (at + n > bytes || w + static_cast<std::size_t>(h) > halves) break;
        w += static_cast<std::size_t>(h);
        end = at + n;
    }
    return end;
}

// `t` as it is when it holds at most `n` bytes and `n` letters of width, else its start and "..." (also within both).
inline std::string Fit(const std::string& t, std::size_t n) {
    if (t.size() <= n && Halves(t) <= 2 * n) return t;
    if (n <= 3) return std::string();
    return t.substr(0, PrefixEnd(t, n - 3, 2 * n - 6)) + "...";
}

// The letter budget (for Fit) that makes `shown`, drawn `drawn` wide by the game, fit its box `box` wide
// (menu-polish-01113: a count of letters alone cannot know a proportional font: "Co-op came with the game" is 24
// letters and ran past its button). The drawn width scaled down to the box, always at least a letter shorter than now,
// never below 4 (one letter and "..."); 0 when it fits already, or there is nothing to cut.
inline std::size_t LettersForWidth(const std::string& shown, float drawn, float box) {
    if (!(box > 0.0f) || !(drawn > box)) return 0;
    const std::size_t now = (Halves(shown) + 1) / 2;
    if (now <= 4) return 0;
    std::size_t n = static_cast<std::size_t>(static_cast<float>(now) * box / drawn);
    if (n >= now) n = now - 1;
    return n < 4 ? 4 : n;
}

// The end of the first line of `t` from `at`, for a line of `n` letters: the last space that fits, else the letter
// before the one that does not (never inside a letter; at least one letter, so a loop always moves on).
inline std::size_t LineEnd(const std::string& t, std::size_t at, std::size_t n) {
    const std::size_t end = at + PrefixEnd(std::string_view(t).substr(at), n, 2 * n);
    if (end >= t.size() || t[end] == ' ') return end > at ? end : std::min(t.size(), at + 1);
    const std::size_t space = t.rfind(' ', end);
    if (space != std::string::npos && space > at) return space;
    if (end > at) return end;
    std::size_t k = 1;
    Decode(t, at, &k);
    return at + k;
}

}  // namespace CoopTextFit
