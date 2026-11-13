// SPDX-License-Identifier: GPL-3.0-or-later
// world_probe.hpp: the actors near our Link, once a second, for tests.
#include "world_probe.hpp"

#include <algorithm>
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
bool g_param = false;
std::vector<std::string> g_param_done;

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

// OOT_COOP_PROBE_PARAM=1: where a Stalchild's parameters (StalchildParam: ReturnDistance 24,
// OutsideUserPercepReturnDistance 18, ReturnByPathFailedTime 600) are in memory: pointers are
// followed from the actor, three deep, and every block holding those three numbers within 0x80
// bytes is logged with the way there. Once per actor kind.
void SearchParam(GuestPort& port, std::uint64_t actor, const std::string& name,
                 const std::function<void(const std::string&)>& log) {
    struct Node {
        std::uint64_t at;
        int depth;
        std::string path;
    };
    std::vector<Node> queue{{actor, 0, "actor"}};
    std::vector<std::uint64_t> seen{actor};
    std::size_t hits = 0;
    for (std::size_t q = 0; q < queue.size() && q < 30000 && hits < 12; ++q) {
        const Node n = queue[q];
        std::array<std::uint32_t, 0x800 / 4> words{};
        if (!port.Read(n.at, words.data(), sizeof(words))) {
            if (!port.Read(n.at, words.data(), 0x100)) {
                continue;
            }
        }
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i] != 0x41c00000u) {  // 24.0
                continue;
            }
            bool r18 = false, r600 = false;
            for (std::size_t j = i >= 32 ? i - 32 : 0; j < words.size() && j < i + 32; ++j) {
                r18 |= words[j] == 0x41900000u;   // 18.0
                r600 |= words[j] == 0x44160000u;  // 600.0
            }
            if (r18 && r600) {
                ++hits;
                char line[1400];
                int k = std::snprintf(line, sizeof(line), "objects: param probe: %s %#llx: 24.0 at %#llx (%s +%#zx):",
                                      name.c_str(), static_cast<unsigned long long>(actor),
                                      static_cast<unsigned long long>(n.at + i * 4), n.path.c_str(), i * 4);
                for (std::size_t j = i >= 24 ? i - 24 : 0; j < words.size() && j < i + 24 && k < 1300; ++j) {
                    k += std::snprintf(line + k, sizeof(line) - k, " %08x", words[j]);
                }
                log(line);
            }
        }
        if (n.depth >= 3) {
            continue;
        }
        for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
            const std::uint64_t p = words[i] | (static_cast<std::uint64_t>(words[i + 1]) << 32);
            if (p < 0x100000000ull || p >= 0x8000000000ull || (p & 7) != 0) {
                continue;
            }
            if (std::find(seen.begin(), seen.end(), p) != seen.end() || seen.size() > 40000) {
                continue;
            }
            seen.push_back(p);
            char step[24];
            std::snprintf(step, sizeof(step), "+%#zx>", i * 4);
            queue.push_back({p, n.depth + 1, n.path + step});
        }
    }
    char line[200];
    std::snprintf(line, sizeof(line), "objects: param probe: %s: %zu blocks read, %zu hits", name.c_str(), queue.size(), hits);
    log(line);
}

// OOT_COOP_PROBE_PARAM=1, second way: every readable megabyte from 4 to 32 GB of the game's
// address space, for the same three numbers within 0x80 bytes.
void ScanParam(GuestPort& port, const std::function<void(const std::string&)>& log) {
    std::vector<std::uint32_t> words(0x100000 / 4);
    std::size_t hits = 0, read_mb = 0;
    for (std::uint64_t base = 0x80000000ull; base < 0x800000000ull && hits < 40; base += 0x100000) {
        std::uint32_t probe = 0;
        if (!port.Read(base, &probe, 4) && !port.Read(base + 0x80000, &probe, 4)) {
            continue;
        }
        if (!port.Read(base, words.data(), words.size() * 4)) {
            continue;
        }
        ++read_mb;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i] != 0x41c00000u) {
                continue;
            }
            bool r18 = false, r600 = false, r300 = false;
            for (std::size_t j = i >= 32 ? i - 32 : 0; j < words.size() && j < i + 32; ++j) {
                r18 |= words[j] == 0x41900000u;
                r600 |= words[j] == 0x44160000u;
                r300 |= words[j] == 0x43960000u;
            }
            if (r18 && r600 && r300) {
                ++hits;
                char line[1400];
                int k = std::snprintf(line, sizeof(line), "objects: param scan: 24.0 at %#llx:",
                                      static_cast<unsigned long long>(base + i * 4));
                for (std::size_t j = i >= 24 ? i - 24 : 0; j < words.size() && j < i + 24 && k < 1300; ++j) {
                    k += std::snprintf(line + k, sizeof(line) - k, " %08x", words[j]);
                }
                log(line);
            }
        }
    }
    char line[160];
    std::snprintf(line, sizeof(line), "objects: param scan: %zu MB read, %zu hits", read_mb, hits);
    log(line);
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
        if (const char* pp = std::getenv("OOT_COOP_PROBE_PARAM"); pp != nullptr && *pp == '1') {
            g_param = true;
            if (g_words.empty()) {
                g_words.emplace_back("Enemy_Stalchild");
            }
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
        if (g_param && std::find(g_param_done.begin(), g_param_done.end(), f.name) == g_param_done.end()) {
            g_param_done.push_back(f.name);
            SearchParam(port, f.actor, f.name, log);
            if (g_param_done.size() == 1) {
                ScanParam(port, log);
            }
        }
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
