// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_cheats_panel.h"

#include <algorithm>
#include <cctype>

namespace CoopCheatsPanel {
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

const std::vector<Row>& QuickRows() {
    static const std::vector<Row> rows{
        {"Gold Skulltula token (+1)", "PouchData.Item_Token_Gold.StockNum"},
        {"Heart piece (+1, count only, no heart shown)", "PouchData.Obj_Heart_Piece.StockNum"},
        {"Heart container (+1 heart)", "PlayerStatus.MaxLife"},
        {"Small key, Forest Temple (+1)", "DungeonInfo.ForestTemple.SmallKeyNum"},
        {"Hookshot", "PouchData.Item_Hookshot.StockNum"},
        {"Song of Storms", "IsGet.Item_Melody_StormSong"},
    };
    return rows;
}

std::vector<Row> Rows(const std::string& filter, const std::vector<std::string>& items) {
    if (filter.empty()) {
        return QuickRows();
    }
    const std::string want = Lower(filter);
    std::vector<Row> out;
    for (const auto& name : items) {
        if (Lower(name).find(want) != std::string::npos) {
            out.push_back({name, name});
            if (out.size() == kMaxRows) {
                break;
            }
        }
    }
    return out;
}

bool Panel::Key(std::uint32_t vk, bool repeat, std::size_t rows) {
    if (!open_) {
        if (vk == kKeyToggle && !repeat) {
            open_ = true;
            wait_release_ = false;
            filter_.clear();
            selected_ = 0;
            give_.reset();
            return true;
        }
        return false;
    }
    if (vk == kKeyToggle || vk == 0x1B) {
        if (!repeat) {
            open_ = false;
            wait_release_ = true;
        }
    } else if (vk == kKeyUp) {
        selected_ = selected_ > 0 ? selected_ - 1 : 0;
    } else if (vk == kKeyDown) {
        selected_ = rows == 0 ? 0 : std::min(selected_ + 1, rows - 1);
    } else if (vk == 0x0D && !repeat) {
        if (selected_ < rows) {
            give_ = selected_;
        }
    } else if (vk == 0x08) {
        while (!filter_.empty() && (static_cast<unsigned char>(filter_.back()) & 0xC0) == 0x80) {
            filter_.pop_back();
        }
        if (!filter_.empty()) {
            filter_.pop_back();
        }
        selected_ = 0;
    }
    return true;
}

bool Panel::Char(std::uint32_t cp, const std::string& utf8) {
    if (!open_) {
        return false;
    }
    if (cp >= 0x20 && cp != 0x7F && filter_.size() < 64) {
        filter_ += utf8;
        selected_ = 0;
    }
    return true;
}

std::optional<std::size_t> Panel::TakeGive() {
    auto out = give_;
    give_.reset();
    return out;
}

bool Panel::Pointer(Mouse what, std::optional<std::size_t> row, std::size_t rows) {
    if (!open_) {
        return false;
    }
    if (row && *row >= rows) {
        row.reset();
    }
    switch (what) {
        case Mouse::kMove:
            if (row) {
                selected_ = *row;
            }
            return false;
        case Mouse::kLeft:
            if (row) {
                selected_ = *row;
                give_ = *row;
            }
            return true;
        case Mouse::kWheelUp:
            selected_ = selected_ > 0 ? selected_ - 1 : 0;
            return true;
        case Mouse::kWheelDown:
            selected_ = rows == 0 ? 0 : std::min(selected_ + 1, rows - 1);
            return true;
    }
    return false;
}

bool Panel::KeysTaken(bool closing_key_down) {
    if (open_) {
        return true;
    }
    if (wait_release_ && !closing_key_down) {
        wait_release_ = false;
    }
    return wait_release_;
}

}  // namespace CoopCheatsPanel
