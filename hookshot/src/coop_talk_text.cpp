// SPDX-License-Identifier: GPL-3.0-or-later
// The texts the game puts in its text panes (coop_talk_text.h).
#include "coop_talk_text.h"

#include <algorithm>
#include <cstdio>

namespace CoopTalkText {

namespace {

bool ReadU64(const ReadFn& read, std::uint64_t addr, std::uint64_t& out) {
    out = 0;
    return addr != 0 && read(addr, &out, sizeof out);
}

// The length of a valid UTF-8 sequence starting at s[i] (2 to 4), or 0 when it is not one.
std::size_t Utf8Run(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (c >= 0xc2 && c <= 0xdf) n = 2, cp = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef) n = 3, cp = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4) n = 4, cp = c & 0x07;
    else return 0;
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xc0) != 0x80) return 0;
        cp = cp << 6 | (b(i + k) & 0x3f);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff))) {
        return 0;
    }
    return n;
}

}  // namespace

std::string ReadName(std::uint64_t addr, const ReadFn& read) {
    std::string out;
    if (addr == 0) return out;
    // In small pieces: a name near the end of a mapped block must not fail because the read went past it.
    char piece[16];
    while (out.size() < kMaxName) {
        if (!read(addr + out.size(), piece, sizeof piece)) {
            // The last piece may cross into memory that cannot be read: byte by byte.
            char c = 0;
            while (out.size() < kMaxName && read(addr + out.size(), &c, 1) && c != 0) out.push_back(c);
            return out.size() < kMaxName ? out : std::string();
        }
        for (const char c : piece) {
            if (c == 0) return out;
            out.push_back(c);
            if (out.size() == kMaxName) break;
        }
    }
    return {};   // (no end within kMaxName: not a name)
}

void PaneNames(std::uint64_t pane, const ReadFn& read, std::string& layout, std::string& name) {
    layout.clear();
    name.clear();
    std::uint64_t lay = 0, res = 0, lname = 0, names = 0, base = 0, pname = 0;
    if (!ReadU64(read, pane + kPaneLayout, lay) || !ReadU64(read, lay + kLayoutRes, res) || res == 0) return;
    if (ReadU64(read, res + kResName, lname)) layout = ReadName(lname, read);
    if (!ReadU64(read, res + kResPaneNames, names) || names == 0) return;
    if (!ReadU64(read, lay + kLayoutPanes, base) || base == 0 || pane < base || (pane - base) % kPaneSize != 0) return;
    if (ReadU64(read, names + (pane - base) / kPaneSize * 8, pname)) name = ReadName(pname, read);
}

void Log::Note(std::uint64_t pane, std::uint64_t ref, std::uint64_t frame, const ReadFn& read, std::uint32_t wrap) {
    if (pane == 0 || ref == 0) return;
    std::uint64_t text = 0;
    std::uint32_t length = 0;
    if (!read(ref + kRefText, &text, sizeof text) || !read(ref + kRefLength, &length, sizeof length)) return;
    NoteText(pane, text, length, frame, read, wrap);
}

void Log::NoteText(std::uint64_t pane, std::uint64_t text, std::uint64_t length64, std::uint64_t frame,
                   const ReadFn& read, std::uint32_t wrap) {
    if (pane == 0) return;
    // (the letter-animated setter takes a 16-bit length, the others a 32-bit one: the game's own masks)
    const auto length = static_cast<std::uint32_t>(wrap == HOS_COOP_WRAP_PANE_BODY ? (length64 & 0xffff)
                                                                                   : (length64 & 0xffffffffu));
    Entry e;
    e.frame = frame;
    e.length = length;
    e.wrap = wrap;
    if (text != 0 && length != 0) {
        const std::size_t n = std::min<std::size_t>(length, kMaxBytes);
        e.text.resize(n);
        if (!read(text, e.text.data(), n)) return;
        e.cut = length > kMaxBytes;
        while (!e.text.empty() && e.text.back() == 0) e.text.pop_back();
    }
    bool again = false;
    {
        std::lock_guard lock(mu_);
        ++seen_;
        // A pane the game left alone for a while that gets its same text again: a later talk (the same speaker, the
        // same button), kept again. While a window is open it sets its text every frame, so no gap comes then.
        const auto at = set_at_.find(pane);
        const bool gap = at != set_at_.end() && frame > at->second + kSameAgain;
        if (at != set_at_.end()) {
            at->second = std::max(at->second, frame);   // (threads note out of order)
        } else {
            // Panes only ever set to "" never reach shown_, so this map has its own bound (a long run: forget them;
            // a pane forgotten has no gap on its next set).
            if (set_at_.size() >= kMaxPanes) set_at_.clear();
            set_at_[pane] = frame;
        }
        const auto it = shown_.find(pane);
        if (it == shown_.end() ? e.text.empty() : it->second == e.text && (!gap || e.text.empty()))
            return;   // (shown already, or a first clear)
        again = it != shown_.end() && it->second == e.text;
    }
    PaneNames(pane, read, e.layout, e.pane);   // (outside the lock: guest reads)
    Keep(pane, std::move(e), again);
}

void Log::Keep(std::uint64_t pane, Entry e, bool again) {
    std::lock_guard lock(mu_);
    auto& shown = shown_[pane];
    if (shown == e.text && !again) return;   // (another thread kept the same text meanwhile)
    // A text that only grew replaces its shorter self, when that one is still the newest kept text of all (no other
    // text came between: a box typed out letter by letter).
    if (!shown.empty() && e.text.size() > shown.size() && e.text.compare(0, shown.size(), shown) == 0) {
        const auto last = last_of_.find(pane);
        if (last != last_of_.end() && !kept_.empty() && kept_.back().seq == last->second) kept_.pop_back();
    }
    shown = e.text;
    e.seq = ++seq_;
    last_of_[pane] = e.seq;
    kept_.push_back(std::move(e));
    while (kept_.size() > kKeep) {
        dropped_ = kept_.front().seq;
        kept_.pop_front();
    }
    if (shown_.size() > kMaxPanes) {   // (a long run: forget the panes; the next set of each is kept again)
        shown_.clear();
        last_of_.clear();
        set_at_.clear();
    }
}

std::size_t Log::PanesTimed() const {
    std::lock_guard lock(mu_);
    return set_at_.size();
}

std::vector<Entry> Log::Since(std::uint64_t since, std::string_view layout, bool* lost) const {
    std::lock_guard lock(mu_);
    std::vector<Entry> out;
    for (const auto& e : kept_) {
        if (e.seq > since && (layout.empty() || e.layout.find(layout) != std::string::npos)) out.push_back(e);
    }
    // Lost: a text after `since` was dropped for room. (One that grew and replaced its shorter self is not lost: the
    // grown text is there with a newer seq.)
    if (lost) *lost = dropped_ > since;
    return out;
}

std::uint64_t Log::Last() const {
    std::lock_guard lock(mu_);
    return seq_;
}

std::uint64_t Log::Seen() const {
    std::lock_guard lock(mu_);
    return seen_;
}

void Log::Clear() {
    std::lock_guard lock(mu_);
    kept_.clear();
    shown_.clear();
    last_of_.clear();
    set_at_.clear();
    dropped_ = seq_;
}

std::string JsonText(std::string_view s) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7f) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(static_cast<char>(c));
            ++i;
        } else if (c >= 0x80) {
            if (const std::size_t n = Utf8Run(s, i)) {
                out.append(s.substr(i, n));
                i += n;
            } else {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\u%04x", c);
                out += esc;
                ++i;
            }
        } else {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
            ++i;
        }
    }
    return out + "\"";
}

bool HasMarkup(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7f) {
            ++i;
        } else if (c >= 0x80 && Utf8Run(s, i)) {
            i += Utf8Run(s, i);
        } else {
            return true;
        }
    }
    return false;
}

std::string Members(bool hooked, bool body, std::uint64_t last, bool lost, const std::vector<Entry>& texts) {
    std::string out = std::string("\"hooked\":") + (hooked ? "true" : "false") + ",\"body\":" +
                      (body ? "true" : "false") + ",\"last\":" + std::to_string(last) +
                      ",\"lost\":" + (lost ? "true" : "false") + ",\"texts\":[";
    for (std::size_t i = 0; i < texts.size(); ++i) {
        const Entry& e = texts[i];
        out += (i ? ",{" : "{");
        out += "\"seq\":" + std::to_string(e.seq) + ",\"frame\":" + std::to_string(e.frame) +
               ",\"layout\":" + JsonText(e.layout) + ",\"pane\":" + JsonText(e.pane) + ",\"text\":" + JsonText(e.text) +
               ",\"length\":" + std::to_string(e.length) + ",\"cut\":" + (e.cut ? "true" : "false") +
               ",\"markup\":" + (HasMarkup(e.text) ? "true" : "false") + ",\"wrap\":" + std::to_string(e.wrap) + "}";
    }
    return out + "]";
}

Log& TheLog() {
    static Log log;
    return log;
}

}  // namespace CoopTalkText
