// SPDX-License-Identifier: GPL-3.0-or-later
// coop_map_drawn.h: what co-op drew on the maps last present, as the play-test bridge's `map_icons` state member.
#include "coop_map_drawn.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>

#include "coop_playtest_core.h"

namespace CoopMapDrawn {
namespace {

std::mutex g_lock;
Frame g_last;
std::uint64_t g_presents = 0;

std::string Num(float v) {
    if (!std::isfinite(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(v));
    return buf;
}

}  // namespace

void Set(Frame f) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_last = std::move(f);
    ++g_presents;
}

Frame Last(std::uint64_t* presents) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (presents) *presents = g_presents;
    return g_last;
}

std::string Json(const Frame& f, std::uint64_t presents) {
    using CoopPlaytest::Quote;
    std::string out = "{\"view\":" + Quote(f.view.empty() ? "none" : f.view) + ",\"presents\":" +
                      std::to_string(presents) + ",\"width\":" + std::to_string(f.width) + ",\"height\":" +
                      std::to_string(f.height) + ",\"icons\":[";
    for (std::size_t i = 0; i < f.icons.size(); ++i) {
        const Icon& c = f.icons[i];
        out += std::string(i ? "," : "") + "{\"number\":" + std::to_string(c.number) + ",\"x\":" + Num(c.x) +
               ",\"y\":" + Num(c.y) + ",\"turn\":" + (c.turn ? Num(*c.turn) : std::string("null")) +
               ",\"arrow\":" + (c.arrow ? "true" : "false") + ",\"clamped\":" + (c.clamped ? "true" : "false") +
               ",\"alpha\":" + Num(c.alpha) + ",\"floor_delta\":" + std::to_string(c.floor_delta) +
               ",\"label\":" + Quote(c.label) + "}";
    }
    return out + "]}";
}

}  // namespace CoopMapDrawn
