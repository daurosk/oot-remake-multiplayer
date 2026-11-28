// Row 299: the PauseMgr's holds read (coop_pause_world.h). Pure: reads through the given ReadFn only.
#include "coop_pause_world.h"

#include <cstdio>

namespace CoopPauseWorld {
namespace {

template <class T>
bool Get(const ReadFn& read, std::uint64_t a, T& out) {
    out = T{};
    return a != 0 && read(a, &out, sizeof out);
}

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// The manager and its holds' array, or why not.
bool Manager(const ReadFn& read, std::uint64_t main_base, Snapshot& s, std::uint64_t& array) {
    std::uint64_t global = 0;
    if (main_base == 0 || !Get(read, main_base + kPauseMgrSlot, global) || global == 0) {
        s.why = "no GOT slot";
        return false;
    }
    if (!Get(read, global, s.manager) || s.manager == 0) {
        s.why = "no manager yet";
        return false;
    }
    if (!Get(read, s.manager, s.holds) || s.holds > kMostHolds) {
        s.why = "hold count unreadable or above " + std::to_string(kMostHolds);
        return false;
    }
    if (s.holds != 0 && (!Get(read, s.manager + kHolds, array) || array == 0)) {
        s.why = "no holds array";
        return false;
    }
    for (std::size_t i = 0; i < s.words.size(); ++i) {
        Get(read, s.manager + kWords + 4 * i, s.words[i]);
    }
    return true;
}

Hold ReadHold(const ReadFn& read, std::uint64_t array, std::uint32_t i) {
    Hold h;
    h.index = i;
    const std::uint64_t at = array + kHoldSize * i;
    for (std::size_t w = 0; w < h.raw.size(); ++w) Get(read, at + 8 * w, h.raw[w]);
    h.key = static_cast<std::uint32_t>(h.raw[kHoldKey / 8] & 0xffffffffu);
    h.count = static_cast<std::uint32_t>(h.raw[kHoldCount / 8] >> 32);
    return h;
}

}  // namespace

Snapshot Take(const ReadFn& read, std::uint64_t main_base) {
    Snapshot s;
    std::uint64_t array = 0;
    if (!Manager(read, main_base, s, array)) return s;
    s.ok = true;
    for (std::uint32_t i = 0; i < s.holds; ++i) {
        const Hold h = ReadHold(read, array, i);
        if (h.count != 0) s.held.push_back(h);
    }
    return s;
}

const Hold* MenuHeld(const Snapshot& s) {
    for (const Hold& h : s.held) {
        if (IsMenuHold(h.key)) return &h;
    }
    return nullptr;
}

std::string Json(const Snapshot& s) {
    if (!s.ok) return "{\"ok\":false,\"why\":\"" + s.why + "\"}";
    std::string out = "{\"ok\":true,\"manager\":\"" + Hex(s.manager) + "\",\"holds\":" + std::to_string(s.holds) +
                      ",\"words\":[";
    for (std::size_t i = 0; i < s.words.size(); ++i) out += (i ? "," : "") + std::to_string(s.words[i]);
    out += "],\"held\":[";
    for (std::size_t i = 0; i < s.held.size(); ++i) {
        const Hold& h = s.held[i];
        out += std::string(i ? "," : "") + "{\"index\":" + std::to_string(h.index) + ",\"key\":\"" + Hex(h.key) +
               "\",\"count\":" + std::to_string(h.count) + "}";
    }
    return out + "]}";
}

std::vector<std::string> AllHoldLines(const ReadFn& read, std::uint64_t main_base) {
    std::vector<std::string> lines;
    Snapshot s;
    std::uint64_t array = 0;
    if (!Manager(read, main_base, s, array)) {
        lines.push_back("coop: pause world: probe: " + s.why);
        return lines;
    }
    lines.push_back("coop: pause world: probe: manager " + Hex(s.manager) + ", " + std::to_string(s.holds) +
                    " holds, words " + std::to_string(s.words[0]) + " " + std::to_string(s.words[1]) + " " +
                    std::to_string(s.words[2]) + " " + std::to_string(s.words[3]));
    for (std::uint32_t i = 0; i < s.holds; ++i) {
        const Hold h = ReadHold(read, array, i);
        lines.push_back("coop: pause world: probe: hold " + std::to_string(i) + " key " + Hex(h.key) + " count " +
                        std::to_string(h.count) + " raw " + Hex(h.raw[0]) + " " + Hex(h.raw[1]) + " " +
                        Hex(h.raw[2]) + " " + Hex(h.raw[3]));
    }
    return lines;
}

}  // namespace CoopPauseWorld
