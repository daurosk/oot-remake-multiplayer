// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_keyboard.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace CoopKeyboard {

namespace {

// (as the Save Profiles name keyboard: capitals, small letters, digits and a few signs; a field takes only the
// characters it allows, TextField::Allowed. ":" for an address's port, as 192.168.1.20:27510, in "_"'s place, which a
// room name may still have from the PC's keyboard: a test run found no way to type a port with the mouse)
constexpr const char* kChars[kCharRows] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz",
                                              "0123456789-.:"};
constexpr const char* kBottom[kBottomKeys] = {"Space", "Delete", "Cancel", "OK"};

// The bottom key a character column is under, and the character column in the middle of a bottom key.
int BottomOf(int col) { return std::clamp(col * kBottomKeys / kCols, 0, kBottomKeys - 1); }
int MiddleOf(int key) { return std::clamp((2 * key + 1) * kCols / (2 * kBottomKeys), 0, kCols - 1); }

}  // namespace

KeyAt At(int row, int col) {
    if (row >= 0 && row < kCharRows && col >= 0 && col < kCols) return KeyAt{Key::kChar, kChars[row][col]};
    if (row == kCharRows && col >= 0 && col < kBottomKeys) {
        constexpr Key keys[kBottomKeys] = {Key::kSpace, Key::kDelete, Key::kCancel, Key::kOk};
        return KeyAt{keys[col], 0};
    }
    return KeyAt{};
}

const char* CharRow(int row) { return row >= 0 && row < kCharRows ? kChars[row] : ""; }
const char* BottomWord(int col) { return col >= 0 && col < kBottomKeys ? kBottom[col] : ""; }

void Cursor::Move(int drow, int dcol) {
    if (dcol != 0) {
        const int n = row_ == kCharRows ? kBottomKeys : kCols;
        col_ = ((col_ + dcol) % n + n) % n;
    }
    if (drow != 0) {
        const int to = ((row_ + drow) % kRows + kRows) % kRows;
        if (to == kCharRows && row_ != kCharRows) col_ = BottomOf(col_);
        else if (row_ == kCharRows && to != kCharRows) col_ = MiddleOf(col_);
        row_ = to;
    }
}

void Cursor::Set(int row, int col) {
    row_ = std::clamp(row, 0, kRows - 1);
    col_ = std::clamp(col, 0, (row_ == kCharRows ? kBottomKeys : kCols) - 1);
}

// --- the drawing ---

namespace {

std::mutex g_mutex;
Shown g_shown;   // under g_mutex

// Where the keys are, in the drawn picture's pixels (the last Draw's), for the mouse.
struct Layout {
    float width = 0, height = 0;
    float grid_x = 0, grid_y = 0, key_w = 0, key_h = 0, gap = 0, bottom_y = 0, bottom_w = 0;
};
Layout g_layout;   // under g_mutex

constexpr std::uint32_t Rgba(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b) << 16 |
           static_cast<std::uint32_t>(a) << 24;
}

Layout LayoutFor(float w, float h) {
    Layout l;
    l.width = w, l.height = h;
    const float s = h / 1080.0f;
    l.key_w = 70 * s, l.key_h = 60 * s, l.gap = 9 * s;
    const float grid_w = kCols * l.key_w + (kCols - 1) * l.gap;
    l.grid_x = (w - grid_w) / 2;
    l.grid_y = h / 2 - 150 * s;
    l.bottom_y = l.grid_y + kCharRows * (l.key_h + l.gap) + 6 * s;
    l.bottom_w = (grid_w - (kBottomKeys - 1) * l.gap) / kBottomKeys;
    return l;
}

void Centred(const HosCoopDraw& d, float x0, float x1, float y, float px, std::uint32_t rgba, const char* t) {
    const float tw = d.text_width(d.user, px, t);
    d.text(d.user, (x0 + x1 - tw) / 2, y, px, rgba, t);
}

}  // namespace

void Publish(const Shown& s) {
    std::lock_guard lock(g_mutex);
    g_shown = s;
}

bool Wanted() {
    std::lock_guard lock(g_mutex);
    return g_shown.open;
}

void Draw(const HosCoopDraw& d, int font) {
    Shown s;
    {
        std::lock_guard lock(g_mutex);
        if (!g_shown.open) return;
        s = g_shown;
        g_layout = LayoutFor(d.width, d.height);
    }
    const Layout l = LayoutFor(d.width, d.height);
    const float sc = d.height / 1080.0f;
    if (font != 0 && d.use_font != nullptr) d.use_font(d.user, font);
    // (the menu dimmed behind; the night-blue panel of the file select, a lighter edge)
    d.rect(d.user, 0, 0, d.width, d.height, Rgba(0, 0, 10, 120), 0);
    const float px0 = l.grid_x - 60 * sc, px1 = d.width - l.grid_x + 60 * sc;
    const float py0 = l.grid_y - 230 * sc, py1 = l.bottom_y + l.key_h + 130 * sc;
    d.rect(d.user, px0 - 4 * sc, py0 - 4 * sc, px1 + 4 * sc, py1 + 4 * sc, Rgba(120, 150, 255, 210), 22 * sc);
    d.rect(d.user, px0, py0, px1, py1, Rgba(18, 26, 74, 245), 20 * sc);
    d.text(d.user, px0 + 44 * sc, py0 + 26 * sc, 42 * sc, Rgba(255, 255, 255, 255), s.title.c_str());
    // (what is typed, on a light plate)
    const float tx0 = l.grid_x, tx1 = d.width - l.grid_x, ty0 = py0 + 92 * sc, ty1 = ty0 + 74 * sc;
    d.rect(d.user, tx0, ty0, tx1, ty1, Rgba(232, 238, 255, 255), 14 * sc);
    d.text(d.user, tx0 + 22 * sc, ty0 + 14 * sc, 40 * sc, Rgba(14, 18, 40, 255), s.text.empty() ? " " : s.text.c_str());
    // (the keys: the cursor's bright with a cyan edge, the others the file select's blue)
    const auto key = [&](float x0, float y0, float x1, float y1, bool at, const char* word, float px) {
        if (at) d.rect(d.user, x0 - 4 * sc, y0 - 4 * sc, x1 + 4 * sc, y1 + 4 * sc, Rgba(120, 248, 255, 255), 12 * sc);
        d.rect(d.user, x0, y0, x1, y1, at ? Rgba(172, 202, 255, 255) : Rgba(84, 112, 214, 255), 10 * sc);
        Centred(d, x0, x1, y0 + (y1 - y0 - px) / 2 - 2 * sc, px, at ? Rgba(10, 16, 50, 255) : Rgba(255, 255, 255, 255),
                word);
    };
    for (int r = 0; r < kCharRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const float x0 = l.grid_x + c * (l.key_w + l.gap), y0 = l.grid_y + r * (l.key_h + l.gap);
            const char ch[2] = {kChars[r][c], 0};
            key(x0, y0, x0 + l.key_w, y0 + l.key_h, s.row == r && s.col == c, ch, 36 * sc);
        }
    }
    for (int k = 0; k < kBottomKeys; ++k) {
        const float x0 = l.grid_x + k * (l.bottom_w + l.gap);
        key(x0, l.bottom_y, x0 + l.bottom_w, l.bottom_y + l.key_h, s.row == kCharRows && s.col == k, kBottom[k],
            32 * sc);
    }
    // (what the buttons do, as the file select's footer)
    Centred(d, px0, px1, l.bottom_y + l.key_h + 30 * sc, 32 * sc, Rgba(120, 248, 255, 255),
            "A - Enter   B - Cancel   Y - Delete   + - Done");
    Centred(d, px0, px1, l.bottom_y + l.key_h + 74 * sc, 24 * sc, Rgba(200, 210, 240, 255),
            "Keyboard: type, Enter when done, Esc to cancel   L / R: move in the text");
    if (font != 0 && d.use_font != nullptr) d.use_font(d.user, 0);
}

bool KeyAtPoint(float fx, float fy, int* row, int* col) {
    Layout l;
    {
        std::lock_guard lock(g_mutex);
        if (!g_shown.open || g_layout.width <= 0) return false;
        l = g_layout;
    }
    const float x = fx * l.width, y = fy * l.height;
    for (int r = 0; r < kCharRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const float x0 = l.grid_x + c * (l.key_w + l.gap), y0 = l.grid_y + r * (l.key_h + l.gap);
            if (x >= x0 && x <= x0 + l.key_w && y >= y0 && y <= y0 + l.key_h) return *row = r, *col = c, true;
        }
    }
    for (int k = 0; k < kBottomKeys; ++k) {
        const float x0 = l.grid_x + k * (l.bottom_w + l.gap);
        if (x >= x0 && x <= x0 + l.bottom_w && y >= l.bottom_y && y <= l.bottom_y + l.key_h) {
            return *row = kCharRows, *col = k, true;
        }
    }
    return false;
}

}  // namespace CoopKeyboard
