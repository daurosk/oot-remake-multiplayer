// SPDX-License-Identifier: GPL-3.0-or-later
// The chat box on Hookshot (docs/hookshot.md): T opens it, Enter sends, Esc
// closes, Backspace removes the last character. Hookshot's window offers its
// key presses and characters to the module first (hos_coop_api.h, typing);
// while the box is open, and until Enter or Esc is let go after it closes,
// the game reads no keys (hos_coop_keys_taken). The emulator's co-op window
// opens its box with Enter, but on Hookshot Enter (and Tab) is the game's
// Plus. Not thread-safe: the module guards it with a mutex.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace CoopChatBox {

// coop::protocol::kMaxChatChars, as the co-op window's box.
inline constexpr std::size_t kMaxChars = 200;

// Windows virtual-key codes the box uses.
inline constexpr std::uint32_t kKeyOpen = 'T';
inline constexpr std::uint32_t kKeyReturn = 0x0D;
inline constexpr std::uint32_t kKeyEscape = 0x1B;
inline constexpr std::uint32_t kKeyBack = 0x08;

// `cp` as UTF-8 ("" for a surrogate or a value past U+10FFFF).
std::string Utf8(std::uint32_t cp);

class Box {
public:
    bool open() const { return open_; }
    bool failed() const { return failed_; }
    const std::string& text() const { return text_; }

    // A key press: true when the box takes it (T to open it; every key while
    // it is open). Enter with text leaves it in TakeSend.
    bool Key(std::uint32_t vk, bool repeat);
    // A character (Unicode): true while the box is open. Control characters
    // are taken and dropped; the T that opened the box is not typed.
    bool Char(std::uint32_t cp);
    // The text Enter sent, once: the caller sends it, then calls Sent.
    std::optional<std::string> TakeSend();
    // The send went (the box closes) or was refused (it stays open, marked,
    // until the text changes).
    void Sent(bool ok);
    // The game must read no keys: the box is open, or Enter or Esc that
    // closed it is still held (else the game would see Plus, or Hookshot's
    // menu Esc, as the box closes).
    bool KeysTaken(bool return_or_escape_down);

private:
    void Close();

    bool open_ = false;
    bool failed_ = false;
    bool skip_open_char_ = false;  // the T's own WM_CHAR, right after it opened the box
    bool wait_release_ = false;
    std::optional<std::string> pending_;
    std::string text_;
};

}  // namespace CoopChatBox
