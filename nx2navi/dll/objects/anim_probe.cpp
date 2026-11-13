// SPDX-License-Identifier: GPL-3.0-or-later
// A research tool (anim_probe.hpp), off unless OOT_COOP_PROBE_ANIM is set.
#include "anim_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_api_v2.h"

namespace CoopNx2navi::Objects::AnimProbe {
namespace {

struct Local {
    std::uint32_t flags;
    std::uint32_t unused[3];
    float scale[4];
    float rows[12];
    float translation[4];
};
static_assert(sizeof(Local) == 0x60);

struct Track {
    std::string name;
    std::vector<Local> last;      // as the last game frame's step saw them
    std::vector<Local> frozen;    // the freeze test's pose
    std::uint64_t first_ms = 0;
    unsigned frames = 0, changed = 0, kept = 0, overwritten = 0;
    std::uint64_t logged_ms = 0;
    bool wrote = false;
};

std::map<std::uint64_t, Track> g_tracks;
int g_mode = -1;
int g_freeze_after_s = 25;

template <typename T>
bool Rd(GuestPort& port, std::uint64_t at, T& out) {
    return at >= 0x1000 && port.Read(at, &out, sizeof(T));
}

bool Skeleton(GuestPort& port, std::uint64_t actor, std::uint64_t& local, std::uint16_t& count) {
    std::uint64_t at = actor;
    for (const std::uint64_t o : {0x270ull, 0x8ull, 0x18ull, 0x18ull, 0x140ull}) {
        if (!Rd(port, at + o, at) || at < 0x1000) {
            return false;
        }
    }
    return Rd(port, at + 0x48, count) && Rd(port, at + 0x18, local) && local >= 0x1000 && count > 0 &&
           count <= 512;
}

std::string Hex(GuestPort& port, std::uint64_t at, std::size_t words) {
    std::string out;
    for (std::size_t i = 0; i < words; ++i) {
        std::uint32_t w = 0;
        if (!Rd(port, at + 4 * i, w)) {
            out += " ?";
            continue;
        }
        float f = 0;
        std::memcpy(&f, &w, 4);
        char text[40];
        if (w == 0) {
            std::snprintf(text, sizeof(text), " 0");
        } else if (std::isfinite(f) && std::fabs(f) > 1e-4f && std::fabs(f) < 1e5f) {
            std::snprintf(text, sizeof(text), " %.2f", static_cast<double>(f));
        } else {
            std::snprintf(text, sizeof(text), " x%x", w);
        }
        out += text;
    }
    return out;
}

}  // namespace

namespace {
const Nx2PluginApi* g_hook_api = nullptr;
void (*g_hook_log)(const std::string&) = nullptr;
std::uint64_t g_hook_main = 0;

// Every AS play (main+0x2022a0, x0 = the request) of a Stalchild, Guay or
// Tektite: its name, slot, rate, the two floats after it, the skip flag and
// who called.
void OnAsPlay(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_hook_api;
    std::uint64_t actor = 0, name_at = 0, actor_name_at = 0;
    std::uint8_t request[0x30] = {};
    char name[40] = {}, actor_name[40] = {};
    if (api == nullptr || api->read(call->x[0], request, sizeof(request)) != 1) {
        return;
    }
    std::memcpy(&actor, request, 8);
    std::memcpy(&name_at, request + 8, 8);
    if (actor < 0x1000 || api->read(actor + 0x260, &actor_name_at, 8) != 1 || actor_name_at < 0x1000 ||
        api->read(actor_name_at, actor_name, sizeof(actor_name) - 1) != 1) {
        return;
    }
    const std::string_view who(actor_name);
    if (who.find("Stalchild") == who.npos && who.find("Guay_Fixed") == who.npos &&
        who.find("Tektite") == who.npos) {
        return;
    }
    if (name_at >= 0x1000) {
        api->read(name_at, name, sizeof(name) - 1);
    }
    std::int32_t slot = 0;
    float rate = 0, a = 0, b = 0;
    std::uint16_t skip = 0;
    std::memcpy(&slot, request + 0x10, 4);
    std::memcpy(&rate, request + 0x14, 4);
    std::memcpy(&a, request + 0x18, 4);
    std::memcpy(&b, request + 0x1c, 4);
    std::memcpy(&skip, request + 0x20, 2);
    char line[240];
    std::snprintf(line, sizeof(line),
                  "objects: probe: asplay %s %#llx '%s' slot %d rate %.2f %.2f %.2f skip %u byte22 %u from main+%#llx",
                  actor_name, static_cast<unsigned long long>(actor), name, slot, static_cast<double>(rate),
                  static_cast<double>(a), static_cast<double>(b), static_cast<unsigned>(skip),
                  static_cast<unsigned>(request[0x22]),
                  static_cast<unsigned long long>(call->lr - g_hook_main));
    if (g_hook_log != nullptr) {
        g_hook_log(line);
    }
}
}  // namespace

namespace {
// Mode 4: which function empties an AS slot's command name? Each candidate is
// hooked; around its run the slot names of the Stalchildren near us are read,
// and a name that went from something to nothing is logged with the function.
constexpr std::uint64_t kClearCandidates[] = {
    0x37b5584, 0x37b5690, 0x37b57c0, 0x37b5840, 0x37b58c0, 0x37b59b8, 0x37b5a98, 0x37b5c10, 0x37b5c90,
    0x37b5e70, 0x37b5ea0, 0x37b5f74, 0x37b6140, 0x37b6170, 0x37b61a0, 0x37b61d0, 0x37b6250, 0x37b62e0,
    0x37b6360, 0x37b63b0, 0x37b6420, 0x37b6464, 0x37b65d0, 0x37b6610, 0x37b6680, 0x37b6700, 0x37b6750,
    0x37b67a0, 0x37b6850, 0x37b68b0, 0x37b68f8, 0x37b6948, 0x37b69a4, 0x37b6a40, 0x37b6a9c, 0x37b6bf0,
    0x37b6c40, 0x37b6ca0, 0x37b6cd0, 0x37b6d40, 0x37b6d80, 0x37b6e20, 0x37b7040, 0x37b7250,
    0x201f00, 0x2020c0, 0x2021d4, 0x2025c8, 0x2026e0, 0x20287c, 0x202ad0, 0x3abec10};
std::mutex g_watch_mutex;
std::uint64_t g_watch[8] = {};  // AS controllers of the Stalchildren near us
std::atomic<int> g_clear_lines{0};

void SlotNames(const Nx2PluginApi* api, std::uint64_t controller, char out[3][24]) {
    std::uint64_t array = 0;
    std::int32_t count = 0;
    for (int i = 0; i < 3; ++i) {
        out[i][0] = 0;
    }
    if (api->read(controller + 0x18, &count, 4) != 1 || api->read(controller + 0x20, &array, 8) != 1) {
        return;
    }
    for (int i = 0; i < 3 && i < count; ++i) {
        std::uint64_t slot = 0, name_at = 0;
        if (api->read(array + 0x48ull * static_cast<std::uint64_t>(i), &slot, 8) == 1 && slot >= 0x1000 &&
            api->read(slot + 0x10, &name_at, 8) == 1 && name_at >= 0x1000) {
            api->read(name_at, out[i], 23);
            out[i][23] = 0;
        }
    }
}

void OnClearCandidate(Nx2HookCall* call, void* user) {
    const Nx2PluginApi* api = g_hook_api;
    std::uint64_t watch[8];
    {
        std::lock_guard lock(g_watch_mutex);
        std::memcpy(watch, g_watch, sizeof(watch));
    }
    if (api == nullptr || watch[0] == 0 || g_clear_lines.load() > 400) {
        return;
    }
    char before[8][3][24];
    for (int w = 0; w < 8 && watch[w] != 0; ++w) {
        SlotNames(api, watch[w], before[w]);
    }
    const std::uint64_t x0 = call->x[0], x1 = call->x[1], x2 = call->x[2], lr = call->lr;
    api->call_original(call);
    for (int w = 0; w < 8 && watch[w] != 0; ++w) {
        char after[3][24];
        SlotNames(api, watch[w], after);
        for (int i = 0; i < 3; ++i) {
            if (std::strcmp(before[w][i], after[i]) != 0) {
                g_clear_lines.fetch_add(1);
                char line[260];
                std::snprintf(line, sizeof(line),
                              "objects: probe: slotchange by main+%#llx (x0 %#llx x1 %#llx x2 %#llx, from main+%#llx): "
                              "controller %#llx slot %d '%s' -> '%s'",
                              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(user)),
                              static_cast<unsigned long long>(x0), static_cast<unsigned long long>(x1),
                              static_cast<unsigned long long>(x2), static_cast<unsigned long long>(lr - g_hook_main),
                              static_cast<unsigned long long>(watch[w]), i, before[w][i], after[i]);
                if (g_hook_log != nullptr) {
                    g_hook_log(line);
                }
            }
        }
    }
}
}  // namespace

void InstallHooks(const Nx2PluginApi* api, std::uint64_t main, void (*log)(const std::string&)) {
    if (!On() || api == nullptr || g_hook_api != nullptr) {
        return;
    }
    g_hook_api = api;
    g_hook_log = log;
    g_hook_main = main;
    api->hook_function(main + 0x2022a0, OnAsPlay, nullptr);
    if (g_mode == 4) {
        int hooked = 0;
        for (const std::uint64_t at : kClearCandidates) {
            hooked += api->hook_function(main + at, OnClearCandidate, reinterpret_cast<void*>(at)) > 0 ? 1 : 0;
        }
        log("objects: probe: " + std::to_string(hooked) + " slot-clear candidates hooked");
    }
}

bool On() {
    if (g_mode < 0) {
        const char* e = std::getenv("OOT_COOP_PROBE_ANIM");
        g_mode = e != nullptr ? std::atoi(e) : 0;
        if (const char* f = std::getenv("OOT_COOP_PROBE_FREEZE_AFTER"); f != nullptr) {
            g_freeze_after_s = std::atoi(f);
        }
    }
    return g_mode > 0;
}

void Step(GuestPort& port, std::uint64_t main, std::uint64_t now_ms,
          const std::function<void(const std::string&)>& log) {
    if (!On() || main == 0) {
        return;
    }
    std::uint64_t manager = 0, lists = 0;
    std::uint32_t count = 0;
    if (!Rd(port, main + 0x4e91888, manager) || !Rd(port, manager + 0x118, lists) || lists < 0x1000) {
        return;
    }
    const std::uint64_t head = lists + 0x670;
    if (!Rd(port, head + 0x10, count) || count > 8192) {
        return;
    }
    std::vector<std::pair<std::uint64_t, std::string>> enemies;
    std::array<float, 3> link{};
    bool have_link = false;
    std::uint64_t node = 0;
    Rd(port, head, node);
    for (std::uint32_t i = 0; i < count && node >= 0x1000 && node != head; ++i) {
        std::uint64_t owner = 0, name_at = 0;
        char name[48] = {};
        if (Rd(port, node + 0x10, owner) && owner + 0x538 == node && Rd(port, owner + 0x260, name_at) &&
            name_at >= 0x1000 && port.Read(name_at, name, sizeof(name) - 1)) {
            const std::string_view n(name);
            if (n == "Player_Adult" || n == "Player_Child") {
                have_link = port.Read(owner + 0x31c, link.data(), 12);
            } else if (n.rfind("Enemy_", 0) == 0) {
                enemies.emplace_back(owner, std::string(n));
            }
        }
        if (!Rd(port, node, node)) {
            break;
        }
    }
    if (!have_link) {
        return;
    }
    for (const auto& [actor, name] : enemies) {
        std::array<float, 3> at{};
        std::uint32_t state = 0;
        if (!port.Read(actor + 0x31c, at.data(), 12) || !Rd(port, actor + 0x2c, state) || state != 4) {
            continue;
        }
        const float dx = at[0] - link[0], dy = at[1] - link[1], dz = at[2] - link[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > 30.0f) {
            continue;
        }
        std::uint64_t local = 0;
        std::uint16_t bones = 0;
        if (!Skeleton(port, actor, local, bones)) {
            continue;
        }
        Track& t = g_tracks[actor];
        if (t.name != name) {
            t = Track{};
            t.name = name;
            t.first_ms = now_ms;
        }
        std::vector<Local> now(bones);
        if (!port.Read(local, now.data(), now.size() * sizeof(Local))) {
            continue;
        }
        ++t.frames;
        if (t.last.size() == now.size()) {
            bool changed = false;
            for (std::size_t i = 0; i < now.size() && !changed; ++i) {
                for (std::size_t k = 0; k < 12; ++k) {
                    if (std::fabs(now[i].rows[k] - t.last[i].rows[k]) > 1e-4f) {
                        changed = true;
                        break;
                    }
                }
            }
            t.changed += changed ? 1 : 0;
        }
        if (t.wrote && t.frozen.size() == now.size()) {
            // The freeze test wrote the frozen pose last frame: is it still there?
            bool same = true;
            for (std::size_t i = 0; i < now.size() && same; ++i) {
                same = std::memcmp(now[i].rows, t.frozen[i].rows, sizeof(now[i].rows)) == 0;
            }
            (same ? t.kept : t.overwritten) += 1;
        }
        t.wrote = false;
        // Mode 2: after a while every enemy near us is written back to the
        // pose it had then, every game frame, here (after the actors' calc,
        // before the draw): does the picture show it frozen?
        if (g_mode == 2 && now_ms - t.first_ms > static_cast<std::uint64_t>(g_freeze_after_s) * 1000) {
            if (t.frozen.size() != now.size()) {
                t.frozen = now;
                log("objects: probe: " + name + " frozen from now on");
            }
            std::vector<Local> write = now;
            for (std::size_t i = 0; i < write.size(); ++i) {
                std::memcpy(write[i].rows, t.frozen[i].rows, sizeof(write[i].rows));
                std::memcpy(write[i].translation, t.frozen[i].translation, sizeof(write[i].translation));
            }
            t.wrote = port.Write(local, write.data(), write.size() * sizeof(Local));
            t.last = write;
        } else {
            t.last = now;
        }
        if (now_ms - t.logged_ms < 500) {
            continue;
        }
        t.logged_ms = now_ms;
        // The AS controller and its slots.
        std::uint64_t table = 0, as = 0, inner = 0, controller = 0, array = 0;
        std::int32_t slots = 0;
        std::string anim;
        if (Rd(port, actor + 0x270, table) && Rd(port, table + 0x40, as) && Rd(port, as + 0x18, inner) &&
            Rd(port, inner + 0x18, controller) && Rd(port, controller + 0x18, slots) &&
            Rd(port, controller + 0x20, array)) {
            if (g_mode == 4 && name.find("Stalchild") != name.npos) {
                std::lock_guard lock(g_watch_mutex);
                bool known = false;
                int free_at = -1;
                for (int w = 0; w < 8; ++w) {
                    known = known || g_watch[w] == controller;
                    if (g_watch[w] == 0 && free_at < 0) {
                        free_at = w;
                    }
                }
                if (!known && free_at >= 0) {
                    g_watch[free_at] = controller;
                }
            }
            // Slot i is [array + 0x48 * i] (main+0x37b5d70); its command name at +0x10.
            for (std::int32_t s = 0; s < std::min(slots, 4); ++s) {
                std::uint64_t slot = 0, name_at = 0;
                char text[40] = {};
                if (Rd(port, array + 0x48ull * static_cast<std::uint64_t>(s), slot) && slot >= 0x1000 &&
                    Rd(port, slot + 0x10, name_at) && name_at >= 0x1000) {
                    port.Read(name_at, text, sizeof(text) - 1);
                }
                anim += s == 0 ? " '" : "|";
                anim += text;
            }
            anim += "'";
        }
        char line[260];
        std::array<float, 9> rows{};
        port.Read(actor + 0x328, rows.data(), sizeof(rows));
        std::snprintf(line, sizeof(line),
                      "objects: probe: %s %#llx at %.1f %.1f %.1f yaw %.0f, %.1f m from us, bones changed in %u of "
                      "%u frames, slots",
                      name.c_str(), static_cast<unsigned long long>(actor), static_cast<double>(at[0]),
                      static_cast<double>(at[1]), static_cast<double>(at[2]),
                      static_cast<double>(std::atan2(rows[2], rows[8]) * 57.29578f),
                      static_cast<double>(distance), t.changed, t.frames);
        log(std::string(line) + anim);
        t.frames = t.changed = t.kept = t.overwritten = 0;
    }
}

}  // namespace CoopNx2navi::Objects::AnimProbe
