// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_room_list.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <enet/enet.h>

#include "common/logging.h"
#include "coop/net/bind_guard.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_i18n.h"
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

static_assert(kPublicListingBit == coop::protocol::kPublicListingBit);

Room FromListing(std::string room, std::string host, int players, int max, int version) {
    Room r{std::move(room), std::move(host), players, max, version & ~int{coop::protocol::kPublicListingBit}, false, false};
    r.is_public = (version & coop::protocol::kPublicListingBit) != 0;
    return r;
}

std::string DetailText(const Room& r, int my_protocol) {
    // (each piece in the player's language: coop_i18n's table has the pieces, not the built line)
    using CoopI18n::Tr;
    if (r.protocol > my_protocol) return Tr("Needs an update: this game is newer than yours");
    if (r.protocol != my_protocol) return Tr("Another version: the host needs to update");
    std::string out = r.official    ? Tr("Official public game")
                      : r.is_public ? Tr("Public game")
                                    : Tr("Needs the host's password");
    if (r.max > 0) {
        out += Tr(", ") + (r.players >= r.max ? Tr("Full")
                                              : Tr("Players") + " " + std::to_string(r.players) + "/" +
                                                    std::to_string(r.max));
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

// The kit's own relay as the proofs name it, and until when it is not asked for proofs again after it did not answer
// (a relay before 0.11.8 never does: asked again every kProofsRetryMs, not with every refresh).
constexpr std::uint64_t kProofsRetryMs = 10 * 60 * 1000;
std::atomic<std::uint64_t> g_proofs_silent_until_ms{0};

std::string KitsRelayName() {
    std::string host;
    std::uint16_t port = 0;
    if (!coop::server::relay::parse_relay_address(CoopTitleMenu::kDefaultRelay, host, port)) return {};
    return coop::protocol::official_relay_name(host, port);
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

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
            const std::string asked = coop::protocol::official_relay_name(host, port);
            const std::string kits = KitsRelayName();
            // (proofs only from the kit's own relay: the official game is listed nowhere else)
            const bool ask_proofs = asked == kits && SteadyMs() >= g_proofs_silent_until_ms.load();
            if (const auto list = relay_ns::list_rooms_on_relay(at, [] { return g_stop.load(); }, ask_proofs, coop::net::test_bind())) {
                if (ask_proofs && !list->rooms.empty() && !list->proofs_answered && !g_stop.load()) {
                    g_proofs_silent_until_ms = SteadyMs() + kProofsRetryMs;
                }
                for (const auto& e : list->rooms) {
                    s.rooms.push_back(FromListing(e.room, e.host, e.players, e.max_players, e.version));
                }
                // (the official game, then public games, as every viewer shows them; the relay's order kept)
                std::vector<Proof> proofs;
                for (const auto& p : list->proofs) {
                    proofs.push_back(Proof{p.room, coop::protocol::OfficialProof{p.time_s, p.signature}});
                }
                Order(s.rooms, proofs, coop::protocol::official_now_s(), asked, kits);
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
