// SPDX-License-Identifier: GPL-3.0-or-later
// The item cheats on Hookshot (docs/hookshot.md): the emulator's debug
// cheats window (lichii/hook/coop_cheats_window.cpp, Qt), as a panel over the
// picture worked from the keyboard, only with LICHII_COOP_CHEATS=1. F8 opens
// and closes it; typing finds an item flag (any part of its name), Up and
// Down choose, Enter gives it (CoopCheats::Give), Esc closes. With nothing
// typed it lists the window's quick gifts. While it is open the game reads
// no keys (hos_coop_keys_taken). Not thread-safe: the module guards it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CoopCheatsPanel {

inline constexpr std::uint32_t kKeyToggle = 0x77;  // F8: free in Hookshot's keys and menu
inline constexpr std::uint32_t kKeyUp = 0x26;
inline constexpr std::uint32_t kKeyDown = 0x28;
inline constexpr std::size_t kMaxRows = 12;

struct Row {
    std::string label;
    std::string flag;  // what CoopCheats::Give gets
};

// The co-op window's quick gifts.
const std::vector<Row>& QuickRows();

// The rows for `filter`: the quick gifts when it is empty, else the item
// flags whose name has it (any case), at most kMaxRows.
std::vector<Row> Rows(const std::string& filter, const std::vector<std::string>& items);

class Panel {
public:
    bool open() const { return open_; }
    const std::string& filter() const { return filter_; }
    std::size_t selected() const { return selected_; }

    // A key press: true when taken (F8 to open; every key while open). Enter
    // leaves the chosen row's flag in TakeGive. `rows` is how many rows show now.
    bool Key(std::uint32_t vk, bool repeat, std::size_t rows);
    // A character (Unicode, as UTF-8 already): true while open; printable
    // ones go into the filter.
    bool Char(std::uint32_t cp, const std::string& utf8);
    // The flag Enter chose, once.
    std::optional<std::size_t> TakeGive();
    // Open, or the Esc, Enter or F8 that closed it still held.
    bool KeysTaken(bool closing_key_down);

private:
    bool open_ = false;
    bool wait_release_ = false;
    std::string filter_;
    std::size_t selected_ = 0;
    std::optional<std::size_t> give_;
};

}  // namespace CoopCheatsPanel
