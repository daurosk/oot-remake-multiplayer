// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_chatbox.h"

namespace CoopChatBox {
namespace {

std::size_t Chars(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }
    return n;
}

std::string Trimmed(const std::string& s) {
    const auto a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
        return {};
    }
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

}  // namespace

std::string Utf8(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return {};
        }
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

bool Box::Key(std::uint32_t vk, bool repeat) {
    if (!open_) {
        if (vk == kKeyOpen && !repeat) {
            open_ = true;
            failed_ = false;
            skip_open_char_ = true;
            wait_release_ = false;
            text_.clear();
            return true;
        }
        return false;
    }
    skip_open_char_ = false;
    if (vk == kKeyEscape) {
        Close();
    } else if (vk == kKeyReturn && !repeat) {
        const std::string t = Trimmed(text_);
        if (t.empty()) {
            Close();
        } else {
            pending_ = t;
        }
    } else if (vk == kKeyBack) {
        while (!text_.empty() && (static_cast<unsigned char>(text_.back()) & 0xC0) == 0x80) {
            text_.pop_back();
        }
        if (!text_.empty()) {
            text_.pop_back();
        }
        failed_ = false;
    }
    return true;  // every key while the box is open
}

bool Box::Char(std::uint32_t cp) {
    if (!open_) {
        return false;
    }
    if (skip_open_char_) {
        skip_open_char_ = false;
        if (cp == 't' || cp == 'T') {
            return true;
        }
    }
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
        return true;  // Enter, Backspace, Esc and other control characters: keys, not text
    }
    if (Chars(text_) >= kMaxChars) {
        return true;
    }
    text_ += Utf8(cp);
    failed_ = false;
    return true;
}

std::optional<std::string> Box::TakeSend() {
    auto out = std::move(pending_);
    pending_.reset();
    return out;
}

void Box::Sent(bool ok) {
    if (ok) {
        Close();
    } else {
        failed_ = true;  // not connected, or text the protocol refuses: stays open
    }
}

bool Box::KeysTaken(bool return_or_escape_down) {
    if (open_) {
        return true;
    }
    if (wait_release_ && !return_or_escape_down) {
        wait_release_ = false;
    }
    return wait_release_;
}

void Box::Close() {
    open_ = false;
    failed_ = false;
    skip_open_char_ = false;
    wait_release_ = true;
    pending_.reset();
    text_.clear();
}

}  // namespace CoopChatBox
