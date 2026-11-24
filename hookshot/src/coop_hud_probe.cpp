// SPDX-License-Identifier: GPL-3.0-or-later
// The HUD research probe (coop_hud_probe.h).
#include "coop_hud_probe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace CoopHudProbe {

namespace {

constexpr const char* kSay = "coop: hud probe: ";

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

std::uint64_t Word(Game& game, std::uint64_t at) {
    std::uint64_t v = 0;
    return game.Read(at, &v, sizeof v) ? v : 0;
}

bool IsGauge(const std::string& path) {
    return path.size() >= 10 && path.compare(path.size() - 10, 10, "HeartGauge") == 0;
}

}  // namespace

std::vector<Hold> HoldsFor(const std::string& path, std::uint64_t anim, std::int32_t last) {
    std::vector<Hold> out;
    if (anim == 0 || last < 0) return out;
    if (IsGauge(path)) {
        for (int k = 0; k <= 4; ++k) out.push_back({path, anim, static_cast<float>(last) * static_cast<float>(k) / 4.0f});
        return out;
    }
    for (std::int32_t f = 0; f <= std::min<std::int32_t>(last, 7); ++f) out.push_back({path, anim, static_cast<float>(f)});
    return out;
}

void Probe::Look(Game& game, std::uint64_t screen, std::uint64_t layout, std::vector<std::string>& out) {
    // 1. The animations.
    bool have_gauge = false, have_state = false;
    for (const char* path : kPaths) {
        const std::uint64_t name = game.String(path);
        const auto anim = name == 0 ? std::nullopt : game.Call(game.At(addr::kAnimFind), {layout, name, 0, 0, 0});
        if (!anim || *anim == 0) {
            out.push_back(std::string(kSay) + "anim " + path + ": not found");
            continue;
        }
        const std::uint64_t player = Word(game, *anim + addr::kAnimPlayer);
        const auto last = player == 0 ? std::nullopt : game.Call(game.At(addr::kAnimLastFrame), {player, 0, 0, 0, 0});
        const auto frames = last ? static_cast<std::int32_t>(static_cast<std::uint32_t>(*last)) : -1;
        out.push_back(std::string(kSay) + "anim " + path + ": found " + Hex(*anim) + " last frame " +
                      std::to_string(frames));
        const std::string p = path;
        const bool gauge = IsGauge(p), state = p.find("HeartState") != std::string::npos;
        if ((gauge && !have_gauge) || (state && !have_state)) {
            auto h = HoldsFor(p, *anim, frames);
            holds_.insert(holds_.end(), h.begin(), h.end());
            (gauge ? have_gauge : have_state) = !h.empty();
        }
    }
    // 2. The heart's pane and its kind in the screen object, one level deep.
    std::size_t lines = 0;
    for (const char* pane_name : kPanes) {
        const std::uint64_t name = game.String(pane_name);
        const auto pane = name == 0 ? std::nullopt : game.Call(game.At(addr::kPaneByName), {layout, name, 0, 0, 0});
        if (!pane || *pane == 0) {
            out.push_back(std::string(kSay) + "pane " + pane_name + ": not found");
            continue;
        }
        const std::uint64_t kind = Word(game, *pane);
        out.push_back(std::string(kSay) + "pane " + pane_name + ": " + Hex(*pane) + " vtable " + Hex(kind));
        if (kind == 0) continue;
        std::size_t like = 0;
        for (std::uint64_t off = 0; off < addr::kScanBytes; off += 8) {
            const std::uint64_t w = Word(game, screen + off);
            if (w == 0) continue;
            const bool same = w == *pane;
            const bool kin = !same && Word(game, w) == kind;
            if ((same || kin) && lines < kMaxScanLines) {
                out.push_back(std::string(kSay) + "screen+" + Hex(off) + (same ? " = " : " -> a pane like ") + pane_name);
                ++lines;
            }
            like += kin ? 1 : 0;
            std::size_t deep = 0, deep_same = 0;
            for (std::size_t k = 0; k < addr::kDeepWords; ++k) {
                const std::uint64_t v = Word(game, w + 8 * k);
                if (v == 0) continue;
                deep_same += v == *pane ? 1 : 0;
                deep += v != *pane && Word(game, v) == kind ? 1 : 0;
            }
            if ((deep > 0 || deep_same > 0) && lines < kMaxScanLines) {
                out.push_back(std::string(kSay) + "screen+" + Hex(off) + " -> " + Hex(w) + ": " + std::to_string(deep) +
                              " panes like " + pane_name + (deep_same > 0 ? " and the pane itself" : ""));
                ++lines;
            }
        }
        out.push_back(std::string(kSay) + "pane " + pane_name + ": " + std::to_string(like) +
                      " panes of its kind straight in the screen");
    }
    out.push_back(std::string(kSay) + std::to_string(holds_.size()) + " holds of " + std::to_string(kHoldMs) +
                  " ms from " + std::to_string(kStartMs) + " ms after the HUD opened");
}

std::vector<std::string> Probe::Step(Game& game, std::uint64_t now_ms) {
    std::vector<std::string> out;
    if (phase_ == Phase::kDone) return out;
    if (phase_ == Phase::kFind) {
        const std::uint64_t screen = CoopNativeUi::ScreenByVtable(game, addr::kStatusVtable);
        std::uint8_t state = 0;
        if (screen == 0 || !game.Read(screen + CoopNativeUi::addr::kScreenState, &state, 1) ||
            state != CoopNativeUi::addr::kStateOpen) {
            if (!std::exchange(said_wait_, true)) out.push_back(std::string(kSay) + "waiting for PlayerStatus_00 open");
            return out;
        }
        const std::uint64_t layout = Word(game, screen + CoopNativeUi::addr::kScreenLayout);
        out.push_back(std::string(kSay) + "PlayerStatus_00 " + Hex(screen) + " layout " + Hex(layout));
        if (layout != 0) Look(game, screen, layout, out);
        found_ms_ = now_ms;
        phase_ = holds_.empty() ? Phase::kDone : Phase::kHold;
        if (holds_.empty()) out.push_back(std::string(kSay) + "done (nothing to hold)");
        return out;
    }
    if (now_ms < found_ms_ + kStartMs) return out;
    const std::size_t at = static_cast<std::size_t>((now_ms - found_ms_ - kStartMs) / kHoldMs);
    if (at >= holds_.size()) {
        phase_ = Phase::kDone;
        out.push_back(std::string(kSay) + "done");
        return out;
    }
    const Hold& h = holds_[at];
    const bool set = game.CallFloat(game.At(addr::kAnimStopAt), h.anim, h.frame).has_value();
    if (at != shown_) {
        shown_ = at;
        const long hundredths = std::lround(static_cast<double>(h.frame) * 100.0);   // (frames are small, never negative)
        const std::string cents = std::to_string(hundredths % 100);
        const std::string frame = std::to_string(hundredths / 100) + "." + (cents.size() < 2 ? "0" : "") + cents;
        out.push_back(std::string(kSay) + "hold " + std::to_string(at + 1) + "/" + std::to_string(holds_.size()) +
                      " " + h.path + " frame " + frame + (set ? "" : " (the call failed)"));
    }
    return out;
}

}  // namespace CoopHudProbe
