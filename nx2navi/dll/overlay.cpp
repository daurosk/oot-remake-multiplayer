// SPDX-License-Identifier: GPL-3.0-or-later
#include "overlay.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "third_party/plugin_api.h"

namespace CoopNx2navi::Overlay {
namespace {

// The longest chat text, in code points (coop/protocol messages.hpp,
// kMaxChatChars; coop::net::Client::send_chat refuses more).
constexpr std::size_t kMaxChat = 200;
// The key that hides and shows the panel. F9 is not one of the runtime's own
// keys (F1 menu, F8 flight, F10, F11 full screen, Insert, Esc) and has no
// game button by default (NX2navi runtime/input_map.c, DEFAULT_KEYS).
constexpr std::uint32_t kHideKey = NX2_KEY_F1 + 8;  // F9

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::atomic<void (*)(const char*)> g_log{nullptr};

void Log(const char* format, ...) {
    auto* log = g_log.load();
    if (log == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log(line);
}

// --- The interface and the callbacks' registration ---

std::mutex g_reg;  // guards the four below
const Nx2PluginApi* g_api = nullptr;
bool g_want = false;
bool g_registered = false;
int g_handles[3] = {0, 0, 0};
std::atomic<bool> g_active{false};
std::atomic<int> g_captured{0};  // our capture_input(1) calls not yet undone

// --- What the callbacks share with the DLL's threads ---

struct Shared {
    std::mutex mutex;
    bool have_snapshot = false;
    Ui::Snapshot snapshot;
    std::deque<Ui::Notice> notices;
    Ui::ChatView chat;
    std::vector<std::string> outbox;
    bool visible = true;
    bool menu_open = false;
};
Shared g;

std::string Trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// --- Callbacks ---

// The window's thread.
int OnKey(const Nx2KeyEvent* event, void*) {
    if (!g_active || event == nullptr) {
        return 0;
    }
    int capture = 0;  // +1 take the input, -1 give it back
    int keep = 0;
    {
        std::lock_guard lock(g.mutex);
        if (g.chat.open) {
            keep = 1;  // nothing typed into the box opens the game's menu
            if (event->down && event->key == NX2_KEY_ENTER && !event->repeat) {
                std::string text = Trimmed(g.chat.input);
                if (!text.empty()) {
                    g.outbox.push_back(std::move(text));
                }
                g.chat.input.clear();
                g.chat.open = false;
                capture = -1;
            } else if (event->down && event->key == NX2_KEY_ESCAPE && !event->repeat) {
                g.chat.input.clear();
                g.chat.open = false;
                capture = -1;
            } else if (event->down && event->key == NX2_KEY_BACKSPACE) {
                Ui::Utf8PopBack(g.chat.input);
            }
        } else if (event->down && !event->repeat) {
            if (event->key == kHideKey) {
                g.visible = !g.visible;
            } else if (event->key == NX2_KEY_ENTER && g.visible && !g.menu_open &&
                       g.have_snapshot) {
                g.chat.open = true;
                g.chat.input.clear();
                capture = 1;
                keep = 1;
            }
        }
    }
    // Outside our lock: the runtime takes its own.
    const Nx2PluginApi* api = g_api;
    if (capture > 0) {
        ++g_captured;
        api->capture_input(1);
    } else if (capture < 0) {
        int held = g_captured.load();
        while (held > 0 && !g_captured.compare_exchange_weak(held, held - 1)) {
        }
        if (held > 0) {
            api->capture_input(0);
        }
    }
    return keep;
}

// The window's thread, only while the input is captured.
void OnText(std::uint32_t codepoint, void*) {
    if (!g_active) {
        return;
    }
    std::lock_guard lock(g.mutex);
    if (g.chat.open) {
        Ui::Utf8Append(g.chat.input, codepoint, kMaxChat);  // control characters are dropped
    }
}

// The other players' colours (Ui::PeerColour): green, blue, yellow.
constexpr int kPeerRgb[3][3] = {{120, 235, 120}, {120, 180, 255}, {255, 228, 90}};

// --- The other players on the game's minimap ---
//
// Read from the game's main (build e0f3ccfaf50065ae), nothing of it traced in
// a running game before this was written: the map screen (UIMapMenuScreen,
// which is the minimap and the full map) has a per-frame update main+0x33e12c0
// with the screen in x0. The screen keeps the icon controller at +0x390, whose
// floats at +0x160, +0x168 and +0x16c are the world x and z at the picture's
// centre and the world length the picture shows (set by main+0x33e34f0 from
// the scene's map capture), the field minimap's picture pane at +0x1498, and a
// byte at +0x2350 that is not 0 while the small map is the one shown. A pane
// has its scale at +0x28 / +0x38, its place in the layout's canvas at +0x4c /
// +0x50 and its size at +0x58 / +0x5c. A marker is drawn only while every one
// of those reads and is sane (Ui::MapPoint).
constexpr std::uint64_t kMapScreenUpdate = 0x33e12c0;
constexpr std::uint64_t kMapScreenUpdate2 = 0x583fe0;
constexpr std::uint64_t kScreenIcons = 0x390;
constexpr std::uint64_t kScreenFieldMiniPane = 0x1498;
constexpr std::uint64_t kScreenMini = 0x2350;
constexpr std::uint64_t kScreenDungeon = 0x2357;
std::atomic<bool> g_minimap{true};
std::atomic<bool> g_minimap_debug{false};
std::atomic<std::uint64_t> g_map_screen{0};  // the game's map screen, from the hook
std::atomic<std::uint64_t> g_map_seen_ms{0}; // when its update last ran
int g_map_hook = 0;                          // under g_reg
int g_map_hook2 = 0;                         // under g_reg
std::atomic<std::uint64_t> g_map_logged_ms{0};
std::atomic<std::uint64_t> g_map_drawn_logged_ms{0};

template <typename T>
bool ReadGuest(const Nx2PluginApi* api, std::uint64_t address, T& out) {
    return api->read(address, &out, sizeof(out)) == 1;
}

bool ReadMapView(const Nx2PluginApi* api, Ui::MapView& view, bool log);

// The map's view as the game's own thread last read it (in the map screen's
// update, below), for the presenting thread: the game's memory is read on the
// game's thread only (README, "Frame rate").
std::mutex g_map_mutex;
Ui::MapView g_map_view;       // under g_map_mutex
bool g_map_view_ok = false;   // under g_map_mutex
const Nx2PluginApi* g_map_api = nullptr;  // set before the hooks exist

void OnMapScreenUpdate(Nx2HookCall* call, void*) {
    const std::uint64_t now = NowMs();
    g_map_screen.store(call->x[0], std::memory_order_relaxed);
    g_map_seen_ms.store(now, std::memory_order_relaxed);
    if (g_map_api == nullptr) {
        return;
    }
    bool log = false;
    if (g_minimap_debug.load(std::memory_order_relaxed) && now - g_map_logged_ms.load() >= 1000) {
        g_map_logged_ms = now;
        log = true;
    }
    Ui::MapView view;
    const bool ok = ReadMapView(g_map_api, view, log);
    std::lock_guard lock(g_map_mutex);
    g_map_view = view;
    g_map_view_ok = ok;
}

// The field minimap's view now, or nothing: no screen yet, its update did not
// run lately (the map is not on screen: a menu, a load), the small map is not
// the one shown, a dungeon's map (another layout, not handled), or a read failed.
bool ReadMapView(const Nx2PluginApi* api, Ui::MapView& view, bool log) {
    const std::uint64_t screen = g_map_screen.load(std::memory_order_relaxed);
    if (screen == 0 || NowMs() - g_map_seen_ms.load(std::memory_order_relaxed) > 500) {
        if (log) {
            Log("minimap: no map screen update lately (screen %#llx, last %llu ms ago)",
                static_cast<unsigned long long>(screen),
                static_cast<unsigned long long>(NowMs() - g_map_seen_ms.load()));
        }
        return false;
    }
    std::uint64_t icons = 0;
    std::uint64_t pane = 0;
    std::uint8_t mini = 0;
    std::uint8_t dungeon = 0;
    float centre[4] = {};  // x, y, z, the world length
    float scale_x = 0.0f, scale_y = 0.0f;
    float place[2] = {};
    float size[2] = {};
    if (!ReadGuest(api, screen + kScreenIcons, icons) || icons == 0 ||
        !ReadGuest(api, screen + kScreenFieldMiniPane, pane) || pane == 0 ||
        !ReadGuest(api, screen + kScreenMini, mini) ||
        !ReadGuest(api, screen + kScreenDungeon, dungeon) ||
        !ReadGuest(api, icons + 0x160, centre) || !ReadGuest(api, pane + 0x28, scale_x) ||
        !ReadGuest(api, pane + 0x38, scale_y) || !ReadGuest(api, pane + 0x4c, place) ||
        !ReadGuest(api, pane + 0x58, size)) {
        if (log) {
            Log("minimap: a read failed (screen %#llx, icons %#llx, pane %#llx)",
                static_cast<unsigned long long>(screen), static_cast<unsigned long long>(icons),
                static_cast<unsigned long long>(pane));
        }
        return false;
    }
    if (log) {
        float icon_more[3] = {};
        float rect[8] = {};
        std::uint8_t ready = 0;
        ReadGuest(api, icons + 0x170, icon_more);
        ReadGuest(api, screen + 0x2388, rect);
        ReadGuest(api, screen + 0x23a8, ready);
        Log("minimap: screen %#llx mini %u dungeon %u ready %u; centre %.1f %.1f %.1f span %.1f, "
            "icons +0x170 %.2f %.3f %.2f; pane at %.1f %.1f size %.1f x %.1f scale %.3f %.3f; "
            "rect %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f",
            static_cast<unsigned long long>(screen), mini, dungeon, ready,
            static_cast<double>(centre[0]), static_cast<double>(centre[1]),
            static_cast<double>(centre[2]), static_cast<double>(centre[3]),
            static_cast<double>(icon_more[0]), static_cast<double>(icon_more[1]),
            static_cast<double>(icon_more[2]), static_cast<double>(place[0]),
            static_cast<double>(place[1]), static_cast<double>(size[0]),
            static_cast<double>(size[1]), static_cast<double>(scale_x),
            static_cast<double>(scale_y), static_cast<double>(rect[0]), static_cast<double>(rect[1]),
            static_cast<double>(rect[2]), static_cast<double>(rect[3]), static_cast<double>(rect[4]),
            static_cast<double>(rect[5]), static_cast<double>(rect[6]), static_cast<double>(rect[7]));
    }
    if (mini == 0 || dungeon == 1) {
        return false;
    }
    view.centre_x = centre[0];
    view.centre_z = centre[2];
    view.world_span = centre[3];
    view.pane_x = place[0];
    view.pane_y = place[1];
    view.pane_w = size[0] * scale_x;
    view.pane_h = size[1] * scale_y;
    return true;
}

// The presenting thread: a dot for each other player in our scene.
void DrawMinimap(const Nx2Draw* d, const Ui::Snapshot& snapshot) {
    const Nx2PluginApi* api = g_api;  // set once, before the callbacks exist
    if (api == nullptr || d->menu_open != 0 || !snapshot.local.in_game || snapshot.local.event ||
        !snapshot.local.scene) {
        return;
    }
    const bool debug = g_minimap_debug.load(std::memory_order_relaxed);
    bool log = false;
    if (debug) {
        const std::uint64_t now = NowMs();
        if (now - g_map_drawn_logged_ms.load() >= 1000) {
            g_map_drawn_logged_ms = now;
            log = true;
        }
    }
    // What the game's thread read in the map screen's last update; nothing when
    // that update did not run lately (the map is not on screen: a menu, a load).
    Ui::MapView view;
    {
        std::lock_guard lock(g_map_mutex);
        if (!g_map_view_ok || NowMs() - g_map_seen_ms.load(std::memory_order_relaxed) > 500) {
            return;
        }
        view = g_map_view;
    }
    const float scale = d->scale > 0.0f ? d->scale : 1.0f;
    const float r = 5.0f * scale;
    // The game's picture in the window (interface version 4): with black bars
    // beside or above it, the map is placed in the picture, not in the window.
    float px = 0.0f, py = 0.0f, pw = d->width, ph = d->height;
    if (NX2_HAS(d, Nx2Draw, picture_height) && d->picture_width > 0.0f && d->picture_height > 0.0f) {
        px = d->picture_x, py = d->picture_y, pw = d->picture_width, ph = d->picture_height;
    }
    const auto dot = [&](const Ui::Vec3& at, const int rgb[3], const char* what) {
        auto p = Ui::MapPoint(view, at[0], at[2], pw, ph);
        if (p) {
            (*p)[0] += px;
            (*p)[1] += py;
        }
        if (log) {
            Log("minimap: %s at world %.1f %.1f -> %s %.0f %.0f of %.0f x %.0f", what,
                static_cast<double>(at[0]), static_cast<double>(at[2]), p ? "window" : "outside",
                p ? static_cast<double>((*p)[0]) : 0.0, p ? static_cast<double>((*p)[1]) : 0.0,
                static_cast<double>(d->width), static_cast<double>(d->height));
        }
        if (!p) {
            return;
        }
        d->rect_filled((*p)[0] - r - scale, (*p)[1] - r - scale, (*p)[0] + r + scale,
                       (*p)[1] + r + scale, NX2_RGBA(0, 0, 0, 230), r + scale);
        d->rect_filled((*p)[0] - r, (*p)[1] - r, (*p)[0] + r, (*p)[1] + r,
                       NX2_RGBA(rgb[0], rgb[1], rgb[2], 255), r);
    };
    if (debug) {
        static constexpr int kWhite[3] = {255, 255, 255};
        dot(snapshot.local.position, kWhite, "our own Link");
    }
    for (const Ui::Peer& peer : snapshot.peers) {
        const int colour = Ui::PeerColour(snapshot.self, peer.number);
        if (colour < 0 || !peer.fresh || !peer.position || !peer.scene ||
            *peer.scene != *snapshot.local.scene) {
            continue;
        }
        dot(*peer.position, kPeerRgb[colour], peer.name.c_str());
    }
}

// --- Name tags over the other players ---

using WorldToScreen = int (*)(const float world[3], float out_xy[2]);
std::atomic<bool> g_tags{true};
std::atomic<bool> g_tags_debug{false};       // OOT_COOP_DEBUG_TAGS=1: a line a second
std::atomic<std::uint64_t> g_tag_logged_ms{0};
std::atomic<WorldToScreen> g_world_to_screen{nullptr};  // the interface's, once known

// The presenting thread: each other player in our scene, their name over
// their head in their colour, on a dark plate. Only with the game's camera
// (g_world_to_screen); never while a cutscene or talk runs here or the game's
// menu is open.
void DrawNameTags(const Nx2Draw* d, const Ui::Snapshot& snapshot) {
    const WorldToScreen project = g_world_to_screen.load(std::memory_order_relaxed);
    if (project == nullptr || d->menu_open != 0 || !snapshot.local.in_game || snapshot.local.event ||
        !snapshot.local.scene) {
        return;
    }
    const float scale = d->scale > 0.0f ? d->scale : 1.0f;
    const float size = (d->text_height > 0.0f ? d->text_height : 24.0f * scale) * 0.9f;
    const float pad = 4.0f * scale;
    float px = 0.0f, py = 0.0f, pw = d->width, ph = d->height;
    if (NX2_HAS(d, Nx2Draw, picture_height) && d->picture_width > 0.0f && d->picture_height > 0.0f) {
        px = d->picture_x, py = d->picture_y, pw = d->picture_width, ph = d->picture_height;
    }
    for (const Ui::Peer& peer : snapshot.peers) {
        const int colour = Ui::PeerColour(snapshot.self, peer.number);
        if (colour < 0 || !peer.fresh || !peer.position || !peer.scene ||
            *peer.scene != *snapshot.local.scene || peer.name.empty()) {
            continue;
        }
        const float head[3] = {(*peer.position)[0],
                               (*peer.position)[1] +
                                   (snapshot.local.child ? Ui::kTagHeightChild : Ui::kTagHeightAdult),
                               (*peer.position)[2]};
        float at[2] = {};
        if (project(head, at) != 1) {
            continue;  // behind the camera
        }
        float wh[2] = {0, 0};
        d->text_size(size, peer.name.c_str(), wh);
        const auto box = Ui::NameTagBox(at[0], at[1], wh[0], std::max(wh[1], size), pad, px, py, pw, ph);
        if (!box) {
            continue;
        }
        // Fainter with the distance from our own Link.
        const float dx = (*peer.position)[0] - snapshot.local.position[0];
        const float dy = (*peer.position)[1] - snapshot.local.position[1];
        const float dz = (*peer.position)[2] - snapshot.local.position[2];
        const float alpha = Ui::TagAlpha(std::sqrt(dx * dx + dy * dy + dz * dz));
        if (alpha <= 0.0f) {
            continue;
        }
        if (g_tags_debug.load(std::memory_order_relaxed)) {
            const std::uint64_t now = NowMs();
            if (now - g_tag_logged_ms.load() >= 1000) {
                g_tag_logged_ms = now;
                Log("name tag: %s at world %.1f %.1f %.1f, %.1f units away -> window %.0f %.0f of "
                    "%.0f x %.0f (picture %.0f,%.0f %.0f x %.0f), alpha %.2f",
                    peer.name.c_str(), static_cast<double>(head[0]), static_cast<double>(head[1]),
                    static_cast<double>(head[2]),
                    static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)),
                    static_cast<double>(at[0]), static_cast<double>(at[1]),
                    static_cast<double>(d->width), static_cast<double>(d->height),
                    static_cast<double>(px), static_cast<double>(py), static_cast<double>(pw),
                    static_cast<double>(ph), static_cast<double>(alpha));
            }
        }
        d->rect_filled(box->x0, box->y0, box->x1, box->y1,
                       NX2_RGBA(0, 0, 0, static_cast<int>(150.0f * alpha + 0.5f)), 4.0f * scale);
        d->text(box->x, box->y, size,
                NX2_RGBA(kPeerRgb[colour][0], kPeerRgb[colour][1], kPeerRgb[colour][2],
                         static_cast<int>(255.0f * alpha + 0.5f)),
                peer.name.c_str());
    }
}

std::uint32_t Colour(const Ui::Line& line) {
    const float alpha = line.alpha;
    if (line.colour >= 0 && line.colour < 3) {
        const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        return NX2_RGBA(kPeerRgb[line.colour][0], kPeerRgb[line.colour][1], kPeerRgb[line.colour][2], a);
    }
    const Ui::Role role = line.role;
    int r = 255, gr = 255, b = 255;
    switch (role) {
    case Ui::Role::title: r = 236, gr = 196, b = 104; break;
    case Ui::Role::local: r = 200, gr = 200, b = 200; break;
    case Ui::Role::notice: r = 150, gr = 220, b = 255; break;
    case Ui::Role::input: r = 255, gr = 255, b = 170; break;
    case Ui::Role::hint: r = 150, gr = 150, b = 150; break;
    default: break;
    }
    const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return NX2_RGBA(r, gr, b, a);
}

// The presenting thread. Copies the lines under the lock, draws outside it.
void OnOverlay(const Nx2Draw* d, void*) {
    if (!g_active || d == nullptr || !NX2_HAS(d, Nx2Draw, line)) {
        return;
    }
    std::vector<Ui::Line> lines;
    Ui::Snapshot for_map;
    bool panel = true;
    {
        std::lock_guard lock(g.mutex);
        g.menu_open = d->menu_open != 0;
        if (!g.have_snapshot) {
            return;
        }
        if (g_minimap.load(std::memory_order_relaxed) || g_tags.load(std::memory_order_relaxed)) {
            for_map = g.snapshot;
        }
        panel = g.visible || g.chat.open;
        const std::uint64_t now = NowMs();
        Ui::PruneNotices(g.notices, now);
        if (panel) {
            lines = Ui::Panel(g.snapshot, g.notices, g.chat, now);
        }
    }
    // The map markers show with the panel hidden too (F9 hides only the panel).
    if (g_minimap.load(std::memory_order_relaxed)) {
        DrawMinimap(d, for_map);
    }
    if (g_tags.load(std::memory_order_relaxed)) {
        DrawNameTags(d, for_map);
    }
    if (!panel) {
        return;
    }
    const float scale = d->scale > 0.0f ? d->scale : 1.0f;
    const float size = d->text_height > 0.0f ? d->text_height : 24.0f * scale;
    const float hint_size = size * 0.8f;
    const float pad = 8.0f * scale;
    const float gap = 2.0f * scale;
    const float x0 = 16.0f * scale;
    const float max_width = std::max(d->width * 0.6f, 200.0f * scale);

    // The chat box keeps its end in view: the text is cut from the front.
    for (Ui::Line& line : lines) {
        if (line.role != Ui::Role::input) {
            continue;
        }
        float wh[2] = {0, 0};
        d->text_size(size, line.text.c_str(), wh);
        for (int guard = 0; wh[0] > max_width - 2 * pad && line.text.size() > 4 && guard < 400;
             ++guard) {
            std::size_t cut = 3;  // after "> " and one character...
            while (cut < line.text.size() &&
                   (static_cast<unsigned char>(line.text[cut]) & 0xc0) == 0x80) {
                ++cut;  // ...whole
            }
            line.text = "> " + line.text.substr(cut);
            d->text_size(size, line.text.c_str(), wh);
        }
    }

    float width = 0.0f;
    float height = 0.0f;
    std::vector<float> heights;
    heights.reserve(lines.size());
    for (const Ui::Line& line : lines) {
        float wh[2] = {0, 0};
        const float s = line.role == Ui::Role::hint ? hint_size : size;
        d->text_size(s, line.text.c_str(), wh);
        width = std::max(width, std::min(wh[0], max_width - 2 * pad));
        const float h = std::max(wh[1], s);
        heights.push_back(h);
        height += h + gap;
    }
    // Bottom left, growing upwards: the game's HUD keeps the top left (hearts), the top right
    // (item buttons) and the bottom right (map).
    const float box = height - gap + 2 * pad;
    const float y0 = std::max(16.0f * scale, d->height - 16.0f * scale - box);
    d->rect_filled(x0, y0, x0 + width + 2 * pad, y0 + box,
                   NX2_RGBA(0, 0, 0, 150), 6.0f * scale);
    float y = y0 + pad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Ui::Line& line = lines[i];
        const float s = line.role == Ui::Role::hint ? hint_size : size;
        if (line.role == Ui::Role::input) {
            d->rect(x0 + pad * 0.5f, y - gap, x0 + width + pad * 1.5f, y + heights[i] + gap,
                    NX2_RGBA(255, 255, 170, 200), 1.0f * scale);
        }
        d->text(x0 + pad, y, s, Colour(line), line.text.c_str());
        y += heights[i] + gap;
    }
}

void TryRegister() {
    // Under g_reg.
    if (g_api == nullptr || !g_want || g_registered) {
        return;
    }
    g_handles[0] = g_api->on_overlay(OnOverlay, nullptr);
    g_handles[1] = g_api->on_key(OnKey, nullptr);
    g_handles[2] = g_api->on_text(OnText, nullptr);
    if (g_handles[0] <= 0 || g_handles[1] <= 0 || g_handles[2] <= 0) {
        for (int& handle : g_handles) {
            if (handle > 0) {
                g_api->remove(handle);
            }
            handle = 0;
        }
        Log("overlay: the game had no free callback slot; no overlay");
        return;
    }
    g_registered = true;
    g_active = true;
    Log("overlay: on (bottom left; F9 hides and shows it, Enter opens the chat box)");
    // The minimap markers need the game's map screen: a hook of its update
    // (interface version 2). Without it there are no markers.
    const Nx2GameInfo* info = g_api->game_info();
    if (!g_minimap) {
        Log("minimap: off (minimap = 0 in coop.ini)");
    } else if (info != nullptr && info->main_base != 0 && NX2_HAS(g_api, Nx2PluginApi, hook_function) &&
               g_api->hook_function != nullptr) {
        g_map_api = g_api;
        g_map_hook = g_api->hook_function(info->main_base + kMapScreenUpdate, OnMapScreenUpdate, nullptr);
        // The screen's other per-frame function (its vtable's slot 96), in case only one runs.
        g_map_hook2 = g_api->hook_function(info->main_base + kMapScreenUpdate2, OnMapScreenUpdate, nullptr);
        Log(g_map_hook > 0 ? "minimap: on (the other players in your scene as green, blue and yellow dots)"
                           : "minimap: off (the game refused the hook of its map screen)");
    } else {
        Log("minimap: off (it needs the plugin interface version 2)");
    }
    if (NX2_HAS(g_api, Nx2PluginApi, world_to_screen) && g_api->world_to_screen != nullptr) {
        g_world_to_screen = g_api->world_to_screen;
    }
    if (!g_tags) {
        Log("name tags: off (name_tags = 0 in coop.ini)");
    } else if (g_world_to_screen.load() != nullptr) {
        Log("name tags: on (each other player's name over their head, in their colour)");
    } else {
        Log("name tags: off (they need the game's camera: plugin interface version 4)");
    }
    g_api->log("co-op overlay on: F9 hides it, Enter chats");
}

}  // namespace

void SetLog(void (*log)(const char* line)) { g_log = log; }

void SetMinimap(bool on, bool debug) {
    g_minimap = on;
    g_minimap_debug = debug;
}

void SetNameTags(bool on, bool debug) {
    g_tags = on;
    g_tags_debug = debug;
}

bool Offer(const Nx2PluginApi* api, const char* how) {
    if (api == nullptr) {
        return false;
    }
    std::lock_guard lock(g_reg);
    if (g_api != nullptr) {
        return false;
    }
    if (api->version < 1 || !NX2_HAS(api, Nx2PluginApi, capture_input)) {
        Log("plugin interface from %s is too old (version %u, %u bytes); no overlay", how,
            api->version, api->size);
        return false;
    }
    g_api = api;
    const Nx2GameInfo* info = api->game_info();
    Log("plugin interface version %u from %s (game %u, NX2navi %s)", api->version, how,
        info ? info->game_version : 0u, info && info->runtime_version ? info->runtime_version : "?");
    TryRegister();
    return true;
}

int AskGame() {
    if (HaveApi()) {
        return 1;
    }
    const auto get = reinterpret_cast<Nx2PluginApiFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(nullptr), NX2_PLUGIN_API_EXPORT)));
    if (get == nullptr) {
        return 0;
    }
    // Version 2 (function hooks, fast memory) when the game serves it, else 1:
    // everything of 2 is used only where NX2_HAS finds it.
    const Nx2PluginApi* api = get(NX2_PLUGIN_API_VERSION);
    if (api == nullptr) {
        api = get(1);
    }
    if (api == nullptr) {
        return -1;
    }
    Offer(api, "the game's export");
    return 1;
}

bool HaveApi() {
    std::lock_guard lock(g_reg);
    return g_api != nullptr;
}

const Nx2PluginApi* Api() {
    std::lock_guard lock(g_reg);
    return g_api;
}

void Want() {
    std::lock_guard lock(g_reg);
    g_want = true;
    TryRegister();
}

bool Active() { return g_active; }

void Publish(Ui::Snapshot snapshot) {
    std::lock_guard lock(g.mutex);
    g.snapshot = std::move(snapshot);
    g.have_snapshot = true;
}

std::vector<std::string> TakeOutbox() {
    std::lock_guard lock(g.mutex);
    std::vector<std::string> out;
    out.swap(g.outbox);
    return out;
}

void Notice(std::string text, bool chat) {
    std::lock_guard lock(g.mutex);
    if (chat) {
        Ui::AddHistory(g.chat.history, text);
    }
    Ui::AddNotice(g.notices, std::move(text), NowMs());
}

void Shutdown() {
    const Nx2PluginApi* api = nullptr;
    std::vector<int> gone;
    {
        std::lock_guard lock(g_reg);
        if (!g_registered) {
            g_want = false;
            return;
        }
        api = g_api;
        g_active = false;
        for (int& handle : g_handles) {
            gone.push_back(handle);
            handle = 0;
        }
        gone.push_back(std::exchange(g_map_hook, 0));
        gone.push_back(std::exchange(g_map_hook2, 0));
        g_registered = false;
        g_want = false;
    }
    // Outside the lock: a callback that is running may be asking for it (the
    // session's frame, on the game's thread, asks for the interface), and a
    // game whose remove waits for a running callback would then wait for ever.
    for (const int handle : gone) {
        if (handle > 0) {
            api->remove(handle);
        }
    }
    for (int held = g_captured.exchange(0); held > 0; --held) {
        api->capture_input(0);
    }
    std::lock_guard lock(g.mutex);
    g.chat.open = false;
    g.chat.input.clear();
}

}  // namespace CoopNx2navi::Overlay
