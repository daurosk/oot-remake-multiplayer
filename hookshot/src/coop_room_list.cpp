// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_room_list.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <enet/enet.h>

#include "common/logging.h"
#include "coop/protocol/messages.hpp"
#include "coop_pause_tab.h"
#include "coop_title_menu.h"
#include "relay_link.hpp"

namespace CoopRoomList {

std::string CellText(const Room& r, int my_protocol) {
    std::string tail;
    if (r.protocol != my_protocol) {
        tail = " (other version)";
    } else if (r.max > 0 && r.players >= r.max) {
        tail = " (full)";
    } else if (r.max > 0) {
        tail = " (" + std::to_string(r.players) + "/" + std::to_string(r.max) + ")";
    }
    // (the browser's list shows the name the room is listed under on its own line, CoopPauseTab::ListName)
    return r.room + tail;
}

bool Joinable(const Room& r, int my_protocol) {
    return r.protocol == my_protocol && (r.max <= 0 || r.players < r.max);
}

Room FromListing(std::string room, std::string host, int players, int max, int version) {
    Room r{std::move(room), std::move(host), players, max, version & ~int{coop::protocol::kPublicListingBit}, false};
    r.is_public = (version & coop::protocol::kPublicListingBit) != 0;
    return r;
}

std::string DetailText(const Room& r, int my_protocol) {
    if (r.protocol > my_protocol) return "Needs an update: this game is newer than yours";
    if (r.protocol != my_protocol) return "Another version: the host needs to update";
    std::string out = r.is_public ? "Public game" : "Needs the host's password";
    if (r.max > 0) {
        out += r.players >= r.max ? std::string(", Full")
                                  : ", Players " + std::to_string(r.players) + "/" + std::to_string(r.max);
    }
    return out;
}

namespace {

std::mutex g_mutex;
Snapshot g_now;
std::uint64_t g_asked_ms = 0, g_wanted_ms = 0;
std::uint64_t g_seen_ms[kViewers] = {};      // each viewer's last frame showing the list (0: not showing)
constexpr std::uint64_t kViewerGoneMs = 500;  // a viewer silent this long has stopped showing it
std::string g_relay_asked;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_stop{false};   // the list left (or the module going): the asks end within 0.1 s
std::atomic<bool> g_done{true};    // the thread has nothing more of ours to touch
std::thread g_thread;              // under g_mutex

void Ask(std::string relay) {
    namespace relay_ns = coop::server::relay;
    Snapshot s;
    std::string host;
    std::uint16_t port = 0;
    if (!relay_ns::parse_relay_address(relay, host, port)) {
        s.state = kBadServer;
    } else if (enet_initialize() != 0) {
        s.state = kNoNetwork;
    } else {
        ENetAddress at{};
        if (enet_address_set_host(&at, host.c_str()) != 0) {
            s.state = kCantReach;
        } else {
            at.port = port;
            if (const auto list = relay_ns::list_rooms_on_relay(at, [] { return g_stop.load(); })) {
                for (const auto& e : list->rooms) {
                    s.rooms.push_back(FromListing(e.room, e.host, e.players, e.max_players, e.version));
                }
                // (public games first, as the Lobby and Find a game show them; the relay's order kept otherwise)
                std::stable_partition(s.rooms.begin(), s.rooms.end(), [](const Room& r) { return r.is_public; });
                if (s.rooms.empty()) s.state = kNoGames;
            } else {
                s.state = kCantReach;   // (no answer: no internet, the server down, or one from before the list)
            }
        }
        enet_deinitialize();
    }
    {
        std::lock_guard lock(g_mutex);
        if (!g_stop.load()) {   // (stopped: the list was left, what came is not wanted)
            LOG_INFO(Core, "coop: room list: {} room(s) from the relay{}", s.rooms.size(),
                     s.state.empty() ? std::string() : " (" + s.state + ")");
            g_now = std::move(s);
        } else {
            g_asked_ms = 0;   // (back on the list: asked again at once)
        }
        g_busy = false;
    }
    g_done = true;
}

// At the module's end (its statics going): the asks stopped and the thread waited for. Windows: a DLL's statics go
// under the loader lock, which a thread's own end needs, so it is waited for only until it has touched our state
// (g_done, at most ~0.1 s after the stop), then let go; elsewhere it is joined.
struct Ender {
    ~Ender() {
        g_stop = true;
        if (!g_thread.joinable()) return;
#ifdef _WIN32
        for (int i = 0; i < 50 && !g_done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        g_thread.detach();
#else
        g_thread.join();
#endif
    }
} g_ender;   // (after the state above: it goes first)

}  // namespace

void Look(Viewer who, bool wanted, std::uint64_t now_ms, const std::string& relay_typed) {
    std::lock_guard lock(g_mutex);
    // (each viewer's last frame that showed the list; another viewer still showing it keeps the asks going)
    const int me = static_cast<int>(who);
    g_seen_ms[me] = wanted ? now_ms : 0;
    for (int v = 0; v < kViewers && !wanted; ++v) {
        if (v != me && g_seen_ms[v] != 0 && now_ms - g_seen_ms[v] < kViewerGoneMs) return;   // (theirs goes on)
    }
    const std::string relay = relay_typed.empty() ? std::string(CoopTitleMenu::kDefaultRelay) : relay_typed;
    if (!wanted) {
        if (g_busy) g_stop = true;   // (the list left: its asks end)
        if (g_wanted_ms != 0 && now_ms - g_wanted_ms > kKeepMs) g_now = Snapshot{}, g_wanted_ms = 0, g_asked_ms = 0;
        return;
    }
    g_wanted_ms = now_ms;
    if (relay != g_relay_asked) g_asked_ms = 0, g_now = Snapshot{};   // (another relay: its own list)
    if (g_busy || (g_asked_ms != 0 && now_ms - g_asked_ms < kEveryMs)) return;
    if (g_thread.joinable()) g_thread.join();   // (done: g_busy is off only at its end)
    g_asked_ms = now_ms, g_relay_asked = relay, g_busy = true, g_stop = false, g_done = false;
    g_thread = std::thread(Ask, relay);   // (a few seconds at most: list_rooms_on_relay's own tries, or the stop)
}

void AskNow() {
    std::lock_guard lock(g_mutex);
    if (!g_busy) g_asked_ms = 0, g_now = Snapshot{};
}

Snapshot Now() {
    std::lock_guard lock(g_mutex);
    return g_now;
}

}  // namespace CoopRoomList
