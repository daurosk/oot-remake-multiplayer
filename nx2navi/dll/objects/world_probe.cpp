// SPDX-License-Identifier: GPL-3.0-or-later
// world_probe.hpp: the actors near our Link, once a second, for tests.
#include "world_probe.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace CoopNx2navi::Objects::WorldProbe {
namespace {

int g_on = -1;
std::vector<std::string> g_words;
float g_range = 60.0f;
std::uint64_t g_next_ms = 0;

template <typename T>
bool Rd(GuestPort& port, std::uint64_t at, T& out) {
    return at >= 0x1000 && port.Read(at, &out, sizeof(T));
}

bool Wanted(std::string_view name) {
    for (const std::string& word : g_words) {
        if (word == "*" || name.rfind(word, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool On() {
    if (g_on < 0) {
        const char* e = std::getenv("OOT_COOP_DEBUG_ACTORS");
        std::string_view text = e != nullptr ? e : "";
        while (!text.empty()) {
            const auto end = text.find(',');
            if (end != 0) {
                g_words.emplace_back(text.substr(0, end));
            }
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        }
        if (const char* r = std::getenv("OOT_COOP_DEBUG_ACTORS_RANGE"); r != nullptr && std::atof(r) > 0) {
            g_range = static_cast<float>(std::atof(r));
        }
        g_on = g_words.empty() ? 0 : 1;
    }
    return g_on == 1;
}

void Step(GuestPort& port, std::uint64_t main, std::uint64_t now_ms,
          const std::function<void(const std::string&)>& log) {
    if (!On() || main == 0 || now_ms < g_next_ms) {
        return;
    }
    g_next_ms = now_ms + 1000;
    std::uint64_t manager = 0, lists = 0;
    std::uint32_t count = 0;
    if (!Rd(port, main + 0x4e91888, manager) || !Rd(port, manager + 0x118, lists) || lists < 0x1000) {
        return;
    }
    const std::uint64_t head = lists + 0x670;
    if (!Rd(port, head + 0x10, count) || count > 8192) {
        return;
    }
    struct Found {
        std::uint64_t actor;
        std::string name;
    };
    std::vector<Found> found;
    std::array<float, 3> link{};
    bool have_link = false;
    std::uint64_t node = 0;
    Rd(port, head, node);
    for (std::uint32_t i = 0; i < count && node >= 0x1000 && node != head; ++i) {
        std::uint64_t owner = 0, name_at = 0;
        char name[64] = {};
        if (Rd(port, node + 0x10, owner) && owner + 0x538 == node && Rd(port, owner + 0x260, name_at) &&
            name_at >= 0x1000 && port.Read(name_at, name, sizeof(name) - 1)) {
            const std::string_view n(name);
            if (n == "Player_Adult" || n == "Player_Child") {
                have_link = port.Read(owner + 0x31c, link.data(), 12);
            } else if (Wanted(n)) {
                found.push_back({owner, std::string(n)});
            }
        }
        if (!Rd(port, node, node)) {
            break;
        }
    }
    if (!have_link) {
        return;
    }
    for (const Found& f : found) {
        std::array<float, 3> at{};
        std::array<float, 9> rows{};
        std::uint32_t state = 0;
        if (!port.Read(f.actor + 0x31c, at.data(), 12) || !Rd(port, f.actor + 0x2c, state)) {
            continue;
        }
        const float dx = at[0] - link[0], dy = at[1] - link[1], dz = at[2] - link[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > g_range) {
            continue;
        }
        port.Read(f.actor + 0x328, rows.data(), sizeof(rows));
        std::uint64_t placement = 0, hash = 0;
        if (Rd(port, f.actor + 0x3d8, placement)) {
            Rd(port, placement + 0x88, hash);
        }
        // Life: [[actor + 0x270] + 0x2b8], the value at [life + 0x1c10] + 8.
        std::uint64_t table = 0, life = 0, value_at = 0;
        std::int32_t life_now = -1;
        if (Rd(port, f.actor + 0x270, table) && Rd(port, table + 0x2b8, life) && life >= 0x1000 &&
            Rd(port, life + 0x1c10, value_at) && value_at >= 0x1000) {
            Rd(port, value_at + 8, life_now);
        }
        std::string anim;
        std::uint64_t as = 0, inner = 0, controller = 0, array = 0;
        std::int32_t slots = 0;
        if (table >= 0x1000 && Rd(port, table + 0x40, as) && Rd(port, as + 0x18, inner) &&
            Rd(port, inner + 0x18, controller) && Rd(port, controller + 0x18, slots) &&
            Rd(port, controller + 0x20, array)) {
            for (std::int32_t s = 0; s < slots && s < 4; ++s) {
                std::uint64_t slot = 0, name_at = 0;
                char text[40] = {};
                if (Rd(port, array + 0x48ull * static_cast<std::uint64_t>(s), slot) && slot >= 0x1000 &&
                    Rd(port, slot + 0x10, name_at) && name_at >= 0x1000) {
                    port.Read(name_at, text, sizeof(text) - 1);
                }
                anim += s == 0 ? "" : "|";
                anim += text;
            }
        }
        char line[320];
        std::snprintf(line, sizeof(line),
                      "objects: actor: %s %#llx hash %#llx state %u at %.2f %.2f %.2f yaw %.0f (front %.2f %.2f), "
                      "%.1f m from us, life %d, slots '%s'",
                      f.name.c_str(), static_cast<unsigned long long>(f.actor),
                      static_cast<unsigned long long>(hash), state, static_cast<double>(at[0]),
                      static_cast<double>(at[1]), static_cast<double>(at[2]),
                      static_cast<double>(std::atan2(rows[2], rows[8]) * 57.29578f),
                      static_cast<double>(rows[2]), static_cast<double>(rows[8]),
                      static_cast<double>(distance), life_now, anim.c_str());
        log(line);
    }
}

}  // namespace CoopNx2navi::Objects::WorldProbe
