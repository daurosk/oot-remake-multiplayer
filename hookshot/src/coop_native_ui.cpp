// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's chat as the game's own UI (coop_native_ui.h).
#include "coop_native_ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace CoopNativeUi {

namespace {

// Bytes in the UTF-8 sequence `lead` starts; 0 for a byte that cannot start one.
std::size_t SequenceBytes(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

// Whether s[i..i+n) is a whole, shortest-form, non-surrogate UTF-8 character.
bool WholeCharacter(std::string_view s, std::size_t i, std::size_t n) {
    if (i + n > s.size()) return false;
    for (std::size_t k = 1; k < n; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = n > 1 ? static_cast<unsigned char>(s[i + 1]) : 0;
    if (n == 3 && ((b0 == 0xE0 && b1 < 0xA0) || (b0 == 0xED && b1 >= 0xA0))) return false;  // overlong, surrogate
    if (n == 4 && ((b0 == 0xF0 && b1 < 0x90) || (b0 == 0xF4 && b1 >= 0x90))) return false;  // overlong, > U+10FFFF
    return true;
}

// The code point at s[i] of clean text (whole characters only), and its bytes.
std::pair<std::uint32_t, std::size_t> CodePoint(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    std::size_t n = SequenceBytes(b0);
    if (n == 0 || !WholeCharacter(s, i, n)) return {'?', 1};
    if (n == 1) return {b0, 1};
    std::uint32_t c = b0 & (0xFF >> (n + 1));
    for (std::size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return {c, n};
}

std::size_t Wide(std::uint32_t c) {
    const bool wide = (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
                      (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
                      (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x3FFFD);
    return wide ? 2 : 1;
}

// The text's characters, each with its columns.
std::vector<std::pair<std::string_view, std::size_t>> Characters(std::string_view s) {
    std::vector<std::pair<std::string_view, std::size_t>> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto [c, n] = CodePoint(s, i);
        out.emplace_back(s.substr(i, n), Wide(c));
        i += n;
    }
    return out;
}

// The advances SetFontWidths read (hundredths of an em by code point), none before it.
using WidthTable = std::unordered_map<std::uint32_t, std::uint16_t>;
std::mutex g_widths_mutex;
std::shared_ptr<const WidthTable> g_widths;

std::shared_ptr<const WidthTable> Widths() {
    std::lock_guard lock(g_widths_mutex);
    return g_widths;
}

// The guess for a character no font told us (Width): at least its advance in each of the game's Latin fonts.
std::size_t Guess(std::uint32_t c) {
    if (c == ' ') return kGuessSpace;
    if (c >= 0x80) return kGuessWidest;   // accented, Cyrillic, Greek...: as wide as the widest Latin letter
    const auto in = [c](std::string_view set) { return set.find(static_cast<char>(c)) != std::string_view::npos; };
    if (in("ilj',.I!:;f")) return kGuessNarrow;
    if (in("tr()[]{}|-/\\J`")) return kGuessSlim;
    if (in("W@")) return kGuessWidest;
    if (in("wm%M")) return kGuessBroad;
    if ((c >= 'A' && c <= 'Z' && !in("LFSET")) || in("#&")) return kGuessUpper;
    return kGuessLower;   // the other lower-case letters, the digits, L F S E T and the rest of the punctuation
}

// One character's width (Width): a wide one 1 em; else the font's advance, else the guess.
std::size_t Units(std::uint32_t c, const WidthTable* table) {
    if (Wide(c) == 2) return kWideWidth;
    if (table != nullptr) {
        if (const auto it = table->find(c); it != table->end()) return it->second;
    }
    return Guess(c);
}

// The text cut to `width` (Width) at a character.
std::string CutWidth(std::string_view s, std::size_t width) {
    const auto table = Widths();
    std::string out;
    std::size_t used = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto [c, n] = CodePoint(s, i);
        const std::size_t w = Units(c, table.get());
        if (used + w > width) break;
        out.append(s.substr(i, n));
        used += w;
        i += n;
    }
    return out;
}

// Big-endian reads of a font's bytes; 0 past its end.
std::uint32_t Be(const std::vector<std::uint8_t>& b, std::size_t at, int n) {
    if (at > b.size() || static_cast<std::size_t>(n) > b.size() - at) return 0;
    std::uint32_t v = 0;
    for (int k = 0; k < n; ++k) v = (v << 8) | b[at + static_cast<std::size_t>(k)];
    return v;
}

template <typename T>
bool ReadValue(Game& game, std::uint64_t address, T& out) {
    return game.Read(address, &out, sizeof out);
}

template <typename T>
bool WriteValue(Game& game, std::uint64_t address, const T& in) {
    return game.Write(address, &in, sizeof in);
}

// The screen's open call (vtable slot kSlotOpen) with `mode`.
bool Open(Game& game, std::uint64_t screen, std::uint64_t mode) {
    std::uint64_t vtable = 0, open = 0;
    if (!ReadValue(game, screen, vtable) || vtable == 0 || !ReadValue(game, vtable + addr::kSlotOpen * 8, open) ||
        open == 0) {
        return false;
    }
    return game.Call(open, {screen, mode, 0, 0, 0}).has_value();
}

// Sets the pane named by the game's string at main+`pane_name` in the screen's layout.
bool SetPane(Game& game, std::uint64_t screen, std::uint64_t pane_name, std::uint64_t ref) {
    const std::uint64_t layout = ScreenLayout(game, screen);
    if (layout == 0) return false;
    return game.Call(game.At(addr::kSetPaneText), {layout, game.At(pane_name), ref, 1, 0}).has_value();
}

}  // namespace

std::string Clean(std::string_view utf8, std::size_t max_bytes, bool keep_breaks) {
    std::string out;
    out.reserve(std::min(utf8.size(), max_bytes));
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t n = SequenceBytes(lead);
        std::string_view piece;
        if (n == 0 || !WholeCharacter(utf8, i, n)) {
            piece = "?";
            n = 1;  // and start again at the next byte
        } else if (n == 1 && lead == '\n' && keep_breaks) {
            piece = "\n";
        } else if (n == 1 && (lead < 0x20 || lead == 0x7F)) {
            piece = lead == '\t' || lead == '\n' || lead == '\r' ? " " : "";  // tags (0x0E) and controls go
        } else {
            piece = utf8.substr(i, n);
        }
        if (out.size() + piece.size() > max_bytes) break;
        out.append(piece);
        i += n;
    }
    return out;
}

Said SplitSpeaker(std::string_view line) {
    const auto at = line.find(": ");
    if (at == std::string_view::npos || at == 0) return {"", std::string(line)};
    return {std::string(line.substr(0, at)), std::string(line.substr(at + 2))};
}

std::size_t Columns(std::string_view utf8) {
    std::size_t n = 0;
    for (const auto& c : Characters(utf8)) n += c.second;
    return n;
}

std::size_t Width(std::string_view utf8) {
    const auto table = Widths();
    std::size_t n = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto [c, bytes] = CodePoint(utf8, i);
        n += Units(c, table.get());
        i += bytes;
    }
    return n;
}

std::vector<std::pair<std::uint32_t, std::uint16_t>> FontWidths(const std::vector<std::uint8_t>& font) {
    // The table directory: 12 bytes, then 16 a table (tag, checksum, offset, length).
    std::size_t head = 0, hhea = 0, hmtx = 0, cmap = 0, hmtx_len = 0;
    const std::size_t tables = Be(font, 4, 2);
    for (std::size_t i = 0; i < tables && 12 + 16 * (i + 1) <= font.size(); ++i) {
        const std::size_t at = 12 + 16 * i;
        const std::uint32_t tag = Be(font, at, 4), off = Be(font, at + 8, 4), len = Be(font, at + 12, 4);
        if (off >= font.size() || len > font.size() - off) continue;
        if (tag == 0x68656164u && len >= 54) {          // 'head'
            head = off;
        } else if (tag == 0x68686561u && len >= 36) {   // 'hhea'
            hhea = off;
        } else if (tag == 0x686d7478u) {                // 'hmtx'
            hmtx = off;
            hmtx_len = len;
        } else if (tag == 0x636d6170u && len >= 4) {    // 'cmap'
            cmap = off;
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint16_t>> out;
    const std::uint32_t upm = head != 0 ? Be(font, head + 18, 2) : 0;
    const std::size_t metrics = hhea != 0 ? Be(font, hhea + 34, 2) : 0;
    if (upm < 16 || metrics == 0 || hmtx == 0 || cmap == 0 || hmtx_len < 4 * metrics) return out;
    const auto advance = [&](std::uint32_t glyph) -> std::uint16_t {
        const std::size_t g = std::min<std::size_t>(glyph, metrics - 1);   // (past the last: the last's advance)
        const std::uint32_t a = Be(font, hmtx + 4 * g, 2);
        return static_cast<std::uint16_t>(std::min<std::uint32_t>((a * 100 + upm - 1) / upm, 1000));   // rounded up
    };
    // The cmap: a Unicode subtable (platform 0, or 3 with encoding 1 or 10), format 12 (all planes) before 4 (BMP).
    std::size_t f4 = 0, f12 = 0;
    const std::size_t subtables = Be(font, cmap + 2, 2);
    for (std::size_t i = 0; i < subtables; ++i) {
        const std::uint32_t platform = Be(font, cmap + 4 + 8 * i, 2), encoding = Be(font, cmap + 6 + 8 * i, 2);
        const std::size_t at = cmap + Be(font, cmap + 8 + 8 * i, 4);
        if (at >= font.size() || !(platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10)))) continue;
        const std::uint32_t format = Be(font, at, 2);
        if (format == 12 && f12 == 0) f12 = at;
        if (format == 4 && f4 == 0) f4 = at;
    }
    constexpr std::uint32_t kLast = 0x2FFFF;   // (past every plane a chat line uses)
    constexpr std::size_t kMost = 70000;       // a damaged table never takes the memory
    if (f12 != 0) {
        const std::size_t groups = Be(font, f12 + 12, 4);
        for (std::size_t g = 0; g < groups && 16 + 12 * (g + 1) <= font.size() - f12; ++g) {
            const std::size_t at = f12 + 16 + 12 * g;
            const std::uint32_t first = Be(font, at, 4), last = std::min(Be(font, at + 4, 4), kLast);
            const std::uint32_t glyph = Be(font, at + 8, 4);
            for (std::uint32_t c = first; c <= last && out.size() < kMost; ++c) {
                if (glyph + (c - first) != 0) out.emplace_back(c, advance(glyph + (c - first)));
            }
        }
    } else if (f4 != 0) {
        const std::size_t segs = Be(font, f4 + 6, 2) / 2;
        const std::size_t ends = f4 + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs,
                          ranges = deltas + 2 * segs;
        if (ranges + 2 * segs > font.size()) return {};
        for (std::size_t k = 0; k < segs; ++k) {
            const std::uint32_t first = Be(font, starts + 2 * k, 2), last = Be(font, ends + 2 * k, 2);
            const std::uint32_t delta = Be(font, deltas + 2 * k, 2), range = Be(font, ranges + 2 * k, 2);
            for (std::uint32_t c = first; c <= last && c != 0xFFFF && out.size() < kMost; ++c) {
                std::uint32_t glyph = 0;
                if (range == 0) {
                    glyph = (c + delta) & 0xFFFF;
                } else {
                    glyph = Be(font, ranges + 2 * k + range + 2 * (c - first), 2);
                    if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
                }
                if (glyph != 0) out.emplace_back(c, advance(glyph));
            }
        }
    }
    return out;
}

bool SetFontWidths(const std::vector<std::uint8_t>& font) {
    const auto widths = FontWidths(font);
    if (widths.empty()) return false;
    auto table = std::make_shared<const WidthTable>(widths.begin(), widths.end());
    std::lock_guard lock(g_widths_mutex);
    g_widths = std::move(table);
    return true;
}

void ResetFontWidths() {
    std::lock_guard lock(g_widths_mutex);
    g_widths.reset();
}

std::vector<std::string> Wrap(std::string_view clean, std::size_t width, std::size_t lines) {
    const auto table = Widths();
    width = std::max<std::size_t>(width, kWideWidth + kGuessWidest);   // (room for any one character)
    lines = std::max<std::size_t>(lines, 1);
    const std::size_t space = Units(' ', table.get());
    // The lines, broken at spaces; a word wider than a line is broken inside, at a character. A wide (CJK) character
    // may break anywhere, as the game's own Chinese and Japanese texts do.
    std::vector<std::string> rows;
    std::string row;
    std::size_t used = 0;
    const auto flush = [&] {
        while (!row.empty() && row.back() == ' ') row.pop_back();
        if (!row.empty()) rows.push_back(row);
        row.clear();
        used = 0;
    };
    std::size_t i = 0;
    while (i < clean.size()) {
        if (clean[i] == ' ') {
            if (used > 0 && used + space <= width) {
                row.push_back(' ');
                used += space;
            }
            ++i;
            continue;
        }
        // The next word: up to a space, or one wide character.
        std::size_t end = i;
        std::size_t word = 0;
        while (end < clean.size() && clean[end] != ' ') {
            const auto [c, n] = CodePoint(clean, end);
            const bool wide = Wide(c) == 2;
            if (wide && end > i) break;  // a wide character is a word of its own
            end += n;
            word += Units(c, table.get());
            if (wide) break;
        }
        const std::string_view piece = clean.substr(i, end - i);
        if (used + word <= width) {
            row.append(piece);
            used += word;
        } else if (word <= width) {
            flush();
            row.append(piece);
            used = word;
        } else {
            for (std::size_t k = 0; k < piece.size();) {  // too long for any line: broken at a character
                const auto [c, n] = CodePoint(piece, k);
                const std::size_t w = Units(c, table.get());
                if (used + w > width) flush();
                row.append(piece.substr(k, n));
                used += w;
                k += n;
            }
        }
        i = end;
    }
    flush();
    std::vector<std::string> pages;
    for (std::size_t r = 0; r < rows.size(); r += lines) {
        std::string page;
        for (std::size_t k = r; k < std::min(rows.size(), r + lines); ++k) {
            if (!page.empty()) page.push_back('\n');
            page += rows[k];
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

std::vector<std::string> BannerPages(std::string_view clean, std::size_t width) {
    std::vector<std::string> pages = Wrap(clean, width, 1);
    if (pages.size() <= 1) return pages;
    const std::size_t mark = Width(kMoreMark) + Width(" ");   // a mark and its space, on each side of a page
    const std::size_t room = width > 2 * mark + kWideWidth + kGuessWidest ? width - 2 * mark : width;
    const std::size_t n = Wrap(clean, room, 1).size();
    // The narrowest width that still makes n pages (a greedy wrap needs no fewer pages on a narrower line).
    std::size_t lo = 1, hi = room;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (Wrap(clean, mid, 1).size() <= n) hi = mid;
        else lo = mid + 1;
    }
    pages = Wrap(clean, hi, 1);
    for (std::size_t k = 0; k < pages.size(); ++k) {
        if (k > 0) pages[k] = std::string(kMoreMark) + " " + pages[k];
        if (k + 1 < pages.size()) pages[k] += std::string(" ") + kMoreMark;
    }
    return pages;
}

float ChatFeed::Seconds(std::size_t columns) {
    return std::min(6.0f, 3.0f + 0.06f * static_cast<float>(columns));
}

std::string BannerText(const Shown& page) {
    return page.speaker.empty() ? page.text : page.speaker + ": " + page.text;
}

namespace {
// The width left on the banner's line after "speaker: " (a speaker is at most kNameWidth).
std::size_t TextWidth(std::string_view speaker) {
    return speaker.empty() ? kBannerWidth : kBannerWidth - std::min(Width(std::string(speaker) + ": "), kBannerWidth);
}
}  // namespace

void ChatFeed::Update(const std::vector<ChatIn>& lines, std::uint64_t now_ms) {
    for (const auto& line : lines) {
        const std::uint64_t came = now_ms >= line.age_ms ? now_ms - line.age_ms : 0;
        // Two looks at one line can differ by a frame in age: the step absorbs it, and the step either side is
        // checked too.
        const std::uint64_t step = came / kSameMs;
        if (seen_.count({line.text, step}) != 0 || seen_.count({line.text, step + 1}) != 0 ||
            (step > 0 && seen_.count({line.text, step - 1}) != 0)) {
            continue;
        }
        seen_.insert({line.text, step});
        if (!primed_) continue;  // there before co-op's UI was
        std::string speaker, text;
        if (line.notice) {
            text = Clean(line.text, kTextBytes);
        } else {
            const Said said = SplitSpeaker(line.text);
            speaker = CutWidth(Clean(said.speaker, kSpeakerBytes), kNameWidth);
            text = Clean(said.text, kTextBytes);
        }
        const auto pages = BannerPages(text, TextWidth(speaker));   // (kBannerLines: one)
        for (std::size_t k = 0; k < pages.size(); ++k) {
            Shown shown;
            shown.notice = line.notice;
            shown.first = k == 0;
            shown.speaker = speaker;
            shown.text = pages[k];
            shown.colour = line.notice ? 0 : line.colour;
            shown.seconds = Seconds(Columns(BannerText(shown)));
            shown.came_ms = came;
            waiting_.push_back(std::move(shown));
        }
        // Too many waiting: the oldest line goes, all of it (never half a line).
        while (waiting_.size() > kWaiting) {
            waiting_.pop_front();
            while (!waiting_.empty() && !waiting_.front().first) waiting_.pop_front();
        }
    }
    primed_ = true;
    // Forget what can no longer come back (the hook keeps a few lines only).
    const std::uint64_t keep_from = now_ms > 10 * kStaleMs ? (now_ms - 10 * kStaleMs) / kSameMs : 0;
    for (auto it = seen_.begin(); it != seen_.end();) {
        it = it->second < keep_from ? seen_.erase(it) : std::next(it);
    }
}

void ChatFeed::PutBack(Shown line) {
    waiting_.push_front(std::move(line));
    busy_until_ = 0;
}

std::optional<Shown> ChatFeed::Next(std::uint64_t now_ms) {
    if (Busy(now_ms)) return std::nullopt;
    while (!waiting_.empty()) {
        Shown w = std::move(waiting_.front());
        waiting_.pop_front();
        if (w.first && now_ms > w.came_ms + kStaleMs) {
            // Stale before it began: the whole line goes.
            while (!waiting_.empty() && !waiting_.front().first) waiting_.pop_front();
            continue;
        }
        busy_until_ = now_ms + static_cast<std::uint64_t>(w.seconds * 1000.0f);
        return w;
    }
    return std::nullopt;
}

Shown TypingLine(std::string_view speaker, std::string_view typed, bool failed, std::string_view not_sent) {
    Shown s;
    s.speaker = CutWidth(Clean(speaker, kSpeakerBytes), kNameWidth);
    const std::string tail = failed ? "_ " + Clean(not_sent, 64) : "_";   // the caret: the game font's own underscore
    const std::size_t line = TextWidth(s.speaker);
    const std::size_t room = line - std::min(Width(tail), line);
    const std::string text = Clean(typed, kTextBytes);
    std::string shown = text;
    if (Width(text) > room) {
        // The end of what is typed, where the caret is: "..." and as much as fits before it.
        const auto chars = Characters(text);
        std::size_t used = Width("..."), from = chars.size();
        while (from > 0 && used + Width(chars[from - 1].first) <= room) used += Width(chars[--from].first);
        shown = "...";
        for (std::size_t k = from; k < chars.size(); ++k) shown.append(chars[k].first);
    }
    s.text = CutWidth(shown + tail, line);   // (one line: a "(not sent)" longer than the room is cut)
    s.colour = failed ? kFailedColour : 0;
    s.seconds = kTypingSeconds;
    return s;
}

std::uint64_t TextRing::Put(Game& game, std::string_view text) {
    if (base_ == 0) return 0;
    const std::string cut = Clean(text, kSlotText, true);  // already clean text stays as it is
    const std::uint64_t slot = base_ + (next_ % kSlots) * kSlotBytes;
    next_ = (next_ + 1) % kSlots;
    const std::uint64_t at = slot + sizeof(Ref);
    std::string bytes = cut;
    bytes.push_back('\0');
    Ref ref;
    ref.text = at;
    ref.bytes = static_cast<std::uint32_t>(cut.size());
    if (!game.Write(at, bytes.data(), bytes.size()) || !WriteValue(game, slot, ref)) return 0;
    return slot;
}

std::uint64_t TextRing::PutFixed(Game& game, std::size_t index, std::string_view text) {
    if (base_ == 0 || index >= kFixed) return 0;
    const std::string cut = Clean(text, kSlotText, true);
    const std::uint64_t slot = base_ + (kSlots + index) * kSlotBytes;
    std::string bytes = cut;
    bytes.push_back('\0');
    Ref ref;
    ref.text = slot + sizeof(Ref);
    ref.bytes = static_cast<std::uint32_t>(cut.size());
    if (!game.Write(ref.text, bytes.data(), bytes.size()) || !WriteValue(game, slot, ref)) return 0;
    return slot;
}

std::uint64_t PaneTextBox(Game& game, std::uint64_t screen, std::uint64_t pane_name) {
    std::uint64_t inner = 0, box = 0;
    std::uint32_t flags = 0;
    const std::uint64_t layout = ScreenLayout(game, screen);
    if (layout == 0) return 0;
    const auto found = game.Call(game.At(addr::kPaneByName), {layout, game.At(pane_name), 0, 0, 0});
    const std::uint64_t pane = found ? *found : 0;
    // Only a text pane's text box, as the docs have it: *(*(pane+8)+8), checked by the pane's kind.
    if (pane == 0 || !ReadValue(game, pane + addr::kPaneFlags, flags) || ((flags >> 22) & 0xF) != 2 ||
        !ReadValue(game, pane + 8, inner) || inner == 0 || !ReadValue(game, inner + 8, box)) {
        return 0;
    }
    return box;
}

namespace {
std::atomic<bool> g_scene_loading{false};
std::atomic<bool> g_title_up{false};   // (party-hud-crash-c126-6: the title's menu is up)
std::atomic<bool> g_ui_up{false};        // patch 0018's HUD screen ran since the last scene load ended
std::mutex g_running_mutex;
std::vector<std::uint64_t> g_running;   // patch 0018's running screens (empty: none, or not said)
}  // namespace

void SetSceneLoading(bool on) {
    g_scene_loading.store(on);
    if (on) g_ui_up.store(false);   // (a load: the play UI is up again only once the HUD screen has run after it)
}
bool SceneLoading() { return g_scene_loading.load(); }

void SetTitleUp(bool up) {
    g_title_up.store(up);
    if (up) g_ui_up.store(false);   // (the title: the play UI is up again only once the HUD screen has run after it)
}
bool PlayUiDown() { return g_scene_loading.load() || g_title_up.load(); }

void SetRunningScreens(std::optional<std::vector<std::uint64_t>> screens, std::optional<bool> hud_runs) {
    {
        std::lock_guard lock(g_running_mutex);
        g_running = screens ? std::move(*screens) : std::vector<std::uint64_t>{};
    }
    if (hud_runs.value_or(false) && !PlayUiDown()) g_ui_up.store(true);
}

bool GameUiUp() { return !PlayUiDown() && g_ui_up.load(); }

bool ScreenLive(std::uint64_t screen) {
    if (screen == 0 || PlayUiDown()) return false;
    if (g_ui_up.load()) return true;
    std::lock_guard lock(g_running_mutex);
    return std::find(g_running.begin(), g_running.end(), screen) != g_running.end();
}

std::uint64_t ScreenLayout(Game& game, std::uint64_t screen) {
    std::uint64_t layout = 0;
    if (!ScreenLive(screen) || !ReadValue(game, screen + addr::kScreenLayout, layout)) return 0;
    return layout;
}

std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable_offset) {
    if (PlayUiDown()) return 0;   // (nothing is live: ScreenLive)
    const std::uint64_t vtable = game.At(vtable_offset);
    std::uint64_t got = 0, manager = 0, screens = 0;
    std::uint32_t count = 0;
    if (!ReadValue(game, game.At(addr::kUiManagerGot), got) || got == 0 || !ReadValue(game, got, manager) ||
        manager == 0 || !ReadValue(game, manager + addr::kUiCount, count) ||
        !ReadValue(game, manager + addr::kUiScreens, screens) || screens == 0 || count > 512) {
        return 0;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t screen = 0, vt = 0;
        if (ReadValue(game, screens + i * 8ull, screen) && screen != 0 && ReadValue(game, screen, vt) && vt == vtable &&
            ScreenLive(screen)) {
            return screen;
        }
    }
    return 0;
}

bool TextTint::Colours(Game& game, const TextRing& ring, const float (&rgba)[8]) {
    const std::uint64_t scratch = ring.Scratch();
    if (scratch == 0 || box_ == 0 || !game.Write(scratch, rgba, sizeof rgba)) return false;
    return game.Call(game.At(addr::kSetColour0), {box_, scratch, 0, 0, 0}).has_value() &&
           game.Call(game.At(addr::kSetColour1), {box_, scratch + 16, 0, 0, 0}).has_value();
}

bool TextTint::Set(Game& game, const TextRing& ring, std::uint64_t screen, std::uint64_t pane_name,
                   std::uint32_t rgb) {
    const std::uint64_t box = PaneTextBox(game, screen, pane_name);
    if (box == 0) return false;
    float now[8] = {};
    if (!game.Read(box + addr::kBoxColour0, now, sizeof now)) return false;
    for (const float v : now) {
        if (!std::isfinite(v) || v < 0.0f || v > 1.0f) return false;  // not colours: not the text box we think
    }
    if (box != box_) {
        box_ = box;
        saved_ = tinted_ = false;
    }
    if (!saved_) {
        std::memcpy(game_, now, sizeof now);
        saved_ = true;
    }
    if (tinted_ && now_ == rgb) return true;
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                b = static_cast<float>(rgb & 0xFF) / 255.0f;
    // Both colours (the text's top and bottom), each with the game's own alpha.
    const float want[8] = {r, g, b, game_[3], r, g, b, game_[7]};
    if (!Colours(game, ring, want)) return false;
    tinted_ = true;
    now_ = rgb;
    return true;
}

bool TextTint::Restore(Game& game, const TextRing& ring) {
    if (!tinted_ || !saved_) return true;
    if (!Colours(game, ring, game_)) return false;
    tinted_ = false;
    return true;
}

Holds Banner::Holding(Game& game, const TextRing& ring) {
    const std::uint64_t screen = ScreenByVtable(game, addr::kInfoVtable);
    if (screen == 0) {
        ours_ = 0;   // (gone: whatever comes next is not ours)
        return Holds::kGames;  // (none: nothing to show on)
    }
    std::int8_t request = 0;
    std::uint8_t state = 0;
    if (!ReadValue(game, screen + addr::kScreenRequest, request) || !ReadValue(game, screen + addr::kScreenState, state)) {
        return Holds::kGames;  // (unreadable: never cover it)
    }
    if (state == 0 && request < 1) {
        // Shut, no open asked: free. Our last text is done: a game that keeps its text in one buffer of its own shows
        // its next banner from the same pointer, and that one is the game's, never ours.
        ours_ = 0;
        return Holds::kNothing;
    }
    // Up, or opening: whose text? Unreadable counts as the game's.
    const std::uint64_t box = PaneTextBox(game, screen, addr::kNameT_Text_00);
    std::uint64_t text = 0;
    if (box == 0 || !ReadValue(game, box + addr::kBoxText, text)) return Holds::kGames;
    return (text != 0 && (ring.Owns(text) || text == ours_)) ? Holds::kOurs : Holds::kGames;
}

namespace {
std::mutex g_stats_mutex;
BannerStats g_stats;                  // (under g_stats_mutex)
std::vector<std::string> g_lines;     // (under g_stats_mutex) TakeBannerLines
std::uint64_t g_line_count = 0;

const char* HoldsWord(Holds h) {
    switch (h) {
    case Holds::kNothing: return "shut";
    case Holds::kOurs: return "ours";
    case Holds::kGames: return "the game's";
    }
    return "?";
}

void AddLine(std::string line) {   // (under g_stats_mutex)
    ++g_line_count;
    if (g_line_count > 300 && g_line_count % 100 != 0) return;
    if (g_lines.size() < 64) g_lines.push_back("coop: native ui: banner: " + std::move(line));
}

// The screen's state and request now (-1, 0 when unreadable).
std::pair<int, int> Look(Game& game, std::uint64_t screen) {
    std::int8_t request = 0;
    std::uint8_t state = 0;
    if (screen == 0 || !ReadValue(game, screen + addr::kScreenRequest, request) ||
        !ReadValue(game, screen + addr::kScreenState, state)) {
        return {-1, 0};
    }
    return {state, request};
}
}  // namespace

BannerStats Stats() {
    std::lock_guard lock(g_stats_mutex);
    return g_stats;
}

std::string StatsJson(const BannerStats& s) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    return std::string("{\"opens\":") + std::to_string(s.opens) + ",\"reopens\":" + std::to_string(s.reopens) +
           ",\"shut_by_game\":" + std::to_string(s.shut_by_game) + ",\"taken_by_game\":" +
           std::to_string(s.taken_by_game) + ",\"sets\":" + std::to_string(s.sets) + ",\"type_resets\":" + std::to_string(s.type_resets) + ",\"state\":" +
           std::to_string(s.state) + ",\"request\":" + std::to_string(s.request) + ",\"holds\":\"" +
           (s.holds == Holds::kOurs ? "ours" : s.holds == Holds::kGames ? "game" : "shut") + "\",\"page\":" +
           b(s.page) + ",\"held\":" + b(s.held) + "}";
}

std::vector<std::string> TakeBannerLines() {
    std::lock_guard lock(g_stats_mutex);
    return std::exchange(g_lines, {});
}

bool Banner::KeptUp(Game& game, std::uint64_t screen) {
    const auto [state, request] = Look(game, screen);
    // Up (1 opening, 2 open) or an open already asked, and no close of ours pending (request below 0): it shows, or
    // is about to. The game's open would play its "open" again, and the Type reset would replay too (BannerStats):
    // it stays as it is, the text set is all.
    const bool up = ((state == 1 || state == addr::kStateOpen) && request >= 0) || (state == 0 && request >= 1);
    if (!up) return false;
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.reopens;
    AddLine("open asked while up (state " + std::to_string(state) + ", request " + std::to_string(request) + ", " +
            (up_ ? "held line" : page_ ? "page" : "new") + "): kept up, not opened again");
    return true;
}

bool Banner::Raise(Game& game, std::uint64_t screen) {
    {
        std::lock_guard lock(g_stats_mutex);
        ++g_stats.opens;
    }
    closed_ = false;
    return Open(game, screen, addr::kInfoOpenMode);
}

void Banner::Note(Game& game, std::uint64_t screen, Holds holds) {
    const auto [state, request] = Look(game, screen);
    std::lock_guard lock(g_stats_mutex);
    const bool ours_up = page_ || up_;
    if (holds != noted_) {
        if (ours_up && noted_ == Holds::kOurs && holds == Holds::kNothing && !closed_) ++g_stats.shut_by_game;
        if (ours_up && noted_ == Holds::kOurs && holds == Holds::kGames) ++g_stats.taken_by_game;
        AddLine(std::string(HoldsWord(noted_)) + " -> " + HoldsWord(holds) + " (state " + std::to_string(state) +
                ", request " + std::to_string(request) + (up_ ? ", held line" : "") + (page_ ? ", page" : "") +
                (closed_ ? ", our close asked" : "") + ")");
        noted_ = holds;
    }
    g_stats.state = state;
    g_stats.request = request;
    g_stats.holds = holds;
    g_stats.page = page_;
    g_stats.held = up_;
}

bool Banner::Set(Game& game, std::uint64_t screen, std::uint64_t ref, bool opening) {
    {
        std::lock_guard lock(g_stats_mutex);
        ++g_stats.sets;
    }
    // As the game's f_83419b10 puts up a banner of an ordinary kind: its Type animation at its first frame (only
    // kind 0x1d plays it to its end), then T_Text_00. A banner the game showed last as that kind looks ordinary again.
    // Only when it is put up: on a banner of ours that is up the Type is ordinary already, and putting it back at its
    // first frame again made the banner blink at each new text (revive-flicker-0121).
    std::uint64_t type = 0;
    if (opening && ReadValue(game, screen + addr::kInfoType, type) && type != 0) {
        game.Call(game.At(addr::kAnimFirstFrame), {type, 0, 0, 0, 0});
        std::lock_guard lock(g_stats_mutex);
        ++g_stats.type_resets;
    }
    if (!SetPane(game, screen, addr::kNameT_Text_00, ref)) return false;
    // What the text box holds after our set: ours from now on, even when the game keeps a copy of its own.
    const std::uint64_t box = PaneTextBox(game, screen, addr::kNameT_Text_00);
    std::uint64_t text = 0;
    ours_ = box != 0 && ReadValue(game, box + addr::kBoxText, text) ? text : 0;
    return true;
}

void Banner::Close(Game& game, std::uint64_t screen) {
    std::uint64_t vtable = 0, close = 0;
    if (ReadValue(game, screen, vtable) && vtable != 0 && ReadValue(game, vtable + addr::kSlotClose * 8, close) &&
        close != 0) {
        game.Call(close, {screen, static_cast<std::uint64_t>(-1), 0, 0, 0});
    }
    closed_ = true;
}

bool Banner::Show(Game& game, TextRing& ring, const Shown& page, std::uint64_t now_ms) {
    if (up_) return false;  // the held line has the banner
    const Holds holds = Holding(game, ring);
    if (holds == Holds::kGames) return false;  // the game's own banner (or none): the page waits
    const std::uint64_t screen = ScreenByVtable(game, addr::kInfoVtable);
    const std::uint64_t ref = ring.Put(game, BannerText(page));
    // (a page of ours up already, or a banner of ours that shows: only the text changes, typing-blink-0121)
    const bool reopen = !(page_ && holds == Holds::kOurs) && !(screen != 0 && KeptUp(game, screen));
    if (screen == 0 || ref == 0 || !Set(game, screen, ref, reopen)) return false;
    // The speaker's colour on the line; a notice (or a text box not found) the game's own.
    if (page.notice || page.colour == 0 || !tint_.Set(game, ring, screen, addr::kNameT_Text_00, page.colour)) {
        tint_.Restore(game, ring);
    }
    // Up with ours already (open, or opening): only the text changed. Shut: opened, with the game's in animation.
    if (reopen && !Raise(game, screen)) return false;
    page_ = true;
    until_ = now_ms + static_cast<std::uint64_t>(page.seconds * 1000.0f);
    return true;
}

void Banner::End(Game& game, const TextRing& ring) {
    if (!page_) return;
    page_ = false;
    const std::uint64_t screen = ScreenByVtable(game, addr::kInfoVtable);
    if (screen != 0 && Holding(game, ring) == Holds::kOurs) Close(game, screen);  // (never the game's own)
    ours_ = 0;
    tint_.Restore(game, ring);
}

namespace {
std::mutex g_held_mutex;
std::optional<std::string> g_held;
}  // namespace

void HoldBanner(std::string_view text) {
    std::lock_guard lock(g_held_mutex);
    g_held = std::string(text);
}

void EndBanner() {
    std::lock_guard lock(g_held_mutex);
    g_held.reset();
}

std::optional<std::string> HeldBanner() {
    std::lock_guard lock(g_held_mutex);
    return g_held;
}

bool Banner::Step(Game& game, TextRing& ring, const std::optional<std::string>& held, std::uint64_t now_ms) {
    const std::uint64_t screen = ScreenByVtable(game, addr::kInfoVtable);
    if (screen == 0) {
        {
            std::lock_guard lock(g_stats_mutex);
            g_stats.state = -1;
            g_stats.page = g_stats.held = false;
        }
        noted_ = Holds::kNothing;
        up_ = page_ = false;   // (no banner now: the title, a load; shown again once it is back)
        shown_.clear();
        return held.has_value();
    }
    const Holds holds = Holding(game, ring);
    Note(game, screen, holds);
    if (holds != Holds::kOurs && tint_.tinted()) {
        tint_.Restore(game, ring);   // (the game's own banner, or shut: its own colour)
    }
    if (holds == Holds::kNothing) page_ = false;   // (our page closed by itself)
    if (page_ && !held && now_ms >= until_) End(game, ring);   // the page's time is over
    if (!held) {
        if (up_ && holds == Holds::kOurs) Close(game, screen);   // (closes only a banner of ours: never the game's own)
        if (up_) ours_ = 0;   // (let go: what the banner shows from now on is not the held line's)
        up_ = false;   // (let go: pages may have the banner again)
        shown_.clear();
        return false;
    }
    page_ = false;   // (the held line takes the banner over from a page of ours)
    // One line: what fits the banner; a longer held line ends " ..." on its first page (as a page of BannerPages), never
    // a silent cut.
    const auto pages = BannerPages(Clean(*held, kTextBytes), kBannerWidth);
    const std::string text = pages.empty() ? std::string() : pages.front();
    if (holds == Holds::kGames) {
        up_ = false;   // the game's own banner: the held line waits for it, set again once it is gone
        shown_.clear();
        return true;
    }
    const bool shut = holds == Holds::kNothing;  // closed (by itself, or never opened) and no open asked
    if (text != shown_ || !up_ || shut) {
        // Up with the held line already: only its text changes (the revive's share, the clock), nothing replayed.
        const bool opening = (!up_ || shut) && !KeptUp(game, screen);   // (shows already: typing-blink-0121)
        const std::uint64_t ref = ring.PutFixed(game, 0, text);
        if (ref == 0 || !Set(game, screen, ref, opening)) return true;
        tint_.Restore(game, ring);   // (the held line in the game's own colour)
        if (opening && !Raise(game, screen)) return true;
        shown_ = text;
        up_ = true;
    }
    return true;
}

}  // namespace CoopNativeUi
