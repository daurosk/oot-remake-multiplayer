// SPDX-License-Identifier: GPL-3.0-or-later
// The module's side of coop_hallway.h: our game's event request asks RefuseAsk; the frame posts the lines.
#include "coop_hallway.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include "coop_hook.h"

namespace CoopHallway {

namespace {

std::atomic<std::uint32_t> g_cannot_move{0};
std::mutex g_mutex;
Lines g_lines;  // guarded

std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

bool RefuseAsk(std::string_view event) {
    if (!IsTurn(event)) {
        return false;  // (every other event: no lock, no copy)
    }
    CoopHook::MapView view;
    if (!CoopHook::CurrentMapView(view)) {
        return false;
    }
    const std::uint32_t cannot = g_cannot_move.load(std::memory_order_acquire);
    std::vector<Other> others;
    others.reserve(view.others.size());
    for (const auto& p : view.others) {
        Other o;
        o.number = p.number;
        o.name = p.name;
        o.position = p.position;
        o.scene = p.scene;
        o.can_move = p.number >= 32 || (cannot & (1u << p.number)) == 0;
        others.push_back(std::move(o));
    }
    const Verdict v = Decide(event, view.scene, view.self, others);
    if (v.refuse) {
        std::lock_guard lock(g_mutex);
        g_lines.Refused(v, NowUs());
    }
    return v.refuse;
}

void SetCannotMove(std::uint32_t mask) {
    g_cannot_move.store(mask, std::memory_order_release);
}

std::vector<std::string> TakeOurLines() {
    std::lock_guard lock(g_mutex);
    return g_lines.TakeOurs();
}

std::vector<std::string> TakeChatLines() {
    std::lock_guard lock(g_mutex);
    return g_lines.TakeChats();
}

} // namespace CoopHallway
