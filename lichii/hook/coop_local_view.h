// SPDX-License-Identifier: GPL-3.0-or-later
//
// Split screen, step 3a: the world layer's viewport (docs/local-coop.md "Views").
//
// The game sets a layer's viewport itself with its AI node OneShotLayerViewport
// (main+0x28e70e0, the write at main+0x28e7248): on the layer it writes
// min x, min y at +0x30 and max x, max y at +0x38 (in the layer's pixels,
// 0 0 1280 720 for the whole picture), and on the layer's projection
// (layer +0x80) the ratio w / h at +0xb0 and 1 at +0x8 (made again). This
// file does the same write, for the world layer Main_3D_0 (layer id 0 in the
// renderer at main+0x4e81cf0: count +0x20, array +0x28), so player 1's view
// is its own rect of the split. The engine then culls and draws only that
// rect, at that ratio.
//
// Each frame (the calc thread) Step reads the layer's rect and writes only
// when it is not the one wanted, so a scene that sets its own viewport is
// put back at once, and nothing is written while local co-op is off except
// once to give the whole picture back. Plain logic behind two functions (the
// game's memory), tested without a game.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "coop_local_layout.h"

namespace CoopLocalView {

inline constexpr std::uint64_t kRenderer = 0x4e81cf0;   // main+: the renderer's pointer
inline constexpr std::uint64_t kLayerCount = 0x20;      // u32
inline constexpr std::uint64_t kLayerArray = 0x28;      // layer pointers, by id
inline constexpr std::uint64_t kLayerRect = 0x30;       // f32 min x, min y, max x, max y
inline constexpr std::uint64_t kLayerProjection = 0x80;
inline constexpr std::uint64_t kProjectionDirty = 0x8;  // u8: 1 = made again
inline constexpr std::uint64_t kProjectionRatio = 0xb0; // f32 w / h

struct Rect {
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    bool operator==(const Rect&) const = default;
    float W() const { return max_x - min_x; }
    float H() const { return max_y - min_y; }
};

// A layout rect (in a picture of `whole`'s size, from 0) as the layer's rect.
inline Rect Of(const CoopLocalLayout::Rect& r, const Rect& whole) {
    return {whole.min_x + static_cast<float>(r.x), whole.min_y + static_cast<float>(r.y),
            whole.min_x + static_cast<float>(r.x + r.w), whole.min_y + static_cast<float>(r.y + r.h)};
}

inline bool Usable(const Rect& r) {
    return std::isfinite(r.min_x) && std::isfinite(r.min_y) && std::isfinite(r.max_x) && std::isfinite(r.max_y) &&
           r.W() >= 4.0f && r.H() >= 4.0f && r.W() <= 16384.0f && r.H() <= 16384.0f;
}

// What the views should be this frame.
struct Want {
    int players = 0;   // local players in play (0 or 1: the whole picture)
    CoopLocalLayout::Split split = CoopLocalLayout::Split::Side;
};

struct Memory {
    std::function<std::uint64_t()> main_base;
    std::function<bool(std::uint64_t, void*, std::size_t)> read;
    std::function<bool(std::uint64_t, const void*, std::size_t)> write;
    std::function<void(const std::string&)> log;
};

class Views {
public:
    explicit Views(Memory memory) : m_(std::move(memory)) {}

    // The calc thread, each frame.
    void Step(const Want& want) {
        const std::uint64_t layer = WorldLayer();
        if (layer == 0) return;
        if (layer != layer_) {   // (a new world layer: its rect while nobody split it is the whole picture)
            layer_ = layer;
            whole_.reset();
            written_ = false;
        }
        Rect now{};
        if (!m_.read(layer + kLayerRect, &now, sizeof now) || !Usable(now)) return;
        const bool split = want.players >= 2;
        if (!whole_) {
            if (split && written_) return;   // (never take our own split rect for the whole picture)
            whole_ = now;
        }
        if (!split) {
            if (written_) {
                if (now != *whole_) Put(layer, *whole_, "the whole picture again");
                written_ = false;
            } else {
                whole_ = now;   // (follows the game's own changes while nobody splits)
            }
            return;
        }
        const CoopLocalLayout::Views views = CoopLocalLayout::Place(
            want.players, want.split, static_cast<int>(whole_->W()), static_cast<int>(whole_->H()));
        if (views.count < 1) return;
        const Rect mine = Of(views.rect[0], *whole_);
        if (now != mine) Put(layer, mine, "player 1's view");
        written_ = true;
    }

    std::optional<Rect> Whole() const { return whole_; }

private:
    std::uint64_t WorldLayer() const {
        const std::uint64_t base = m_.main_base ? m_.main_base() : 0;
        std::uint64_t renderer = 0, array = 0, layer = 0;
        std::uint32_t count = 0;
        if (base == 0 || !m_.read(base + kRenderer, &renderer, 8) || renderer == 0) return 0;
        if (!m_.read(renderer + kLayerCount, &count, 4) || count == 0) return 0;
        if (!m_.read(renderer + kLayerArray, &array, 8) || array == 0) return 0;
        if (!m_.read(array, &layer, 8)) return 0;   // (id 0: Main_3D_0)
        return layer;
    }

    void Put(std::uint64_t layer, const Rect& r, const char* what) {
        std::uint64_t projection = 0;
        const float ratio = r.W() / r.H();
        const std::uint8_t again = 1;
        bool ok = m_.write(layer + kLayerRect, &r, sizeof r);
        if (ok && m_.read(layer + kLayerProjection, &projection, 8) && projection != 0)
            ok = m_.write(projection + kProjectionRatio, &ratio, 4) && m_.write(projection + kProjectionDirty, &again, 1);
        if (m_.log && (++puts_ <= 8 || puts_ % 600 == 0))
            m_.log("local co-op: views: " + std::string(what) + " (" + std::to_string(static_cast<int>(r.min_x)) + " " +
                   std::to_string(static_cast<int>(r.min_y)) + " " + std::to_string(static_cast<int>(r.max_x)) + " " +
                   std::to_string(static_cast<int>(r.max_y)) + ")" + (ok ? "" : ": not written") +
                   (puts_ > 8 ? " (" + std::to_string(puts_) + " times: the game sets it back)" : ""));
    }

    Memory m_;
    std::uint64_t layer_ = 0;
    std::optional<Rect> whole_;
    bool written_ = false;
    std::uint64_t puts_ = 0;
};

}  // namespace CoopLocalView
