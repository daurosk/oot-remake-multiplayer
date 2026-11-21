// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_lan.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <random>

namespace CoopLan {
namespace {

constexpr std::string_view kHead = "SQUADALA LAN 1";

// At most `most` bytes, cut before a UTF-8 sequence's start; control characters and newlines out.
std::string Clean(std::string_view s, std::size_t most) {
    std::string out;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) continue;
        out.push_back(c);
    }
    if (out.size() <= most) return out;
    std::size_t n = most;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xc0) == 0x80) --n;
    out.resize(n);
    return out;
}

std::optional<int> Number(std::string_view v, int lo, int hi) {
    int n = 0;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), n);
    if (r.ec != std::errc() || r.ptr != v.data() + v.size() || n < lo || n > hi) return std::nullopt;
    return n;
}

bool IsId(std::string_view v) {
    return v.size() == kIdLength &&
           std::all_of(v.begin(), v.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

std::string Encode(const Game& g) {
    std::string out(kHead);
    out += "\nname=" + Clean(g.name, kMaxName);
    out += "\nport=" + std::to_string(g.port);
    out += "\nplayers=" + std::to_string(std::clamp(g.players, 0, 99));
    out += "\nmax=" + std::to_string(std::clamp(g.max, 0, 99));
    if (!g.room.empty()) out += "\nroom=" + Clean(g.room, 24);
    out += "\nprotocol=" + std::to_string(g.protocol);
    if (IsId(g.id)) out += "\nid=" + g.id;
    out += "\n";
    if (out.size() > kMaxDatagram) out.resize(kMaxDatagram);
    return out;
}

std::optional<Game> Decode(std::string_view d) {
    if (d.size() > kMaxDatagram || d.substr(0, kHead.size()) != kHead) return std::nullopt;
    if (d.size() > kHead.size() && d[kHead.size()] != '\n') return std::nullopt;   // ("SQUADALA LAN 10": another)
    Game g;
    bool port = false, protocol = false;
    std::size_t at = kHead.size();
    while (at < d.size()) {
        if (d[at] == '\n') {
            ++at;
            continue;
        }
        const std::size_t end = std::min(d.find('\n', at), d.size());
        const std::string_view line = d.substr(at, end - at);
        at = end;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "name") {
            g.name = Clean(value, kMaxName);
        } else if (key == "port") {
            const auto n = Number(value, 1, 65535);
            if (!n) return std::nullopt;
            g.port = static_cast<std::uint16_t>(*n), port = true;
        } else if (key == "players") {
            g.players = Number(value, 0, 99).value_or(0);
        } else if (key == "max") {
            g.max = Number(value, 0, 99).value_or(0);
        } else if (key == "room") {
            g.room = Clean(value, 24);
        } else if (key == "protocol") {
            const auto n = Number(value, 0, 65535);
            if (!n) return std::nullopt;
            g.protocol = *n, protocol = true;
        } else if (key == "id") {
            if (IsId(value)) g.id = std::string(value);   // (anything else: no id, rows by address and port)
        }   // (a key a newer game adds: left alone)
    }
    if (!port || !protocol) return std::nullopt;
    if (g.name.empty()) g.name = "A game";
    return g;
}

std::string NewId() {
    std::random_device rd;
    std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id(kIdLength, '0');
    for (char& c : id) c = kHex[v & 15], v >>= 4;
    return id;
}

void List::Add(const std::string& address, const Game& g, std::uint64_t now_ms) {
    for (Heard& h : heard_) {
        const bool same = !g.id.empty() ? h.game.id == g.id
                                        : h.game.id.empty() && h.address == address && h.game.port == g.port;
        if (same) {
            if (h.address == "127.0.0.1" && address != "127.0.0.1") h.address = address;
            h.game = g, h.at_ms = now_ms;
            return;
        }
    }
    if (heard_.size() >= kMaxGames) {   // (full: the one heard longest ago makes room)
        heard_.erase(std::min_element(heard_.begin(), heard_.end(),
                                      [](const Heard& a, const Heard& b) { return a.at_ms < b.at_ms; }));
    }
    heard_.push_back(Heard{address, g, now_ms});
}

std::vector<Heard> List::Now(std::uint64_t now_ms) const {
    std::vector<Heard> out;
    for (const Heard& h : heard_) {
        if (now_ms - h.at_ms < kGoneMs) out.push_back(h);
    }
    // (a game on this PC is heard at 127.0.0.1 too: one row for it, at its network address)
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&out](const Heard& h) {
                                 return h.address == "127.0.0.1" &&
                                        std::any_of(out.begin(), out.end(), [&h](const Heard& o) {
                                            return o.address != "127.0.0.1" && o.game.name == h.game.name &&
                                                   o.game.port == h.game.port;
                                        });
                             }),
              out.end());
    std::sort(out.begin(), out.end(), [](const Heard& a, const Heard& b) {
        if (a.game.name != b.game.name) return a.game.name < b.game.name;
        if (a.address != b.address) return a.address < b.address;
        return a.game.port < b.game.port;
    });
    return out;
}

std::string RowText(const Heard& h, int my_protocol) {
    std::string t = h.game.name + "'s game";
    if (h.game.protocol != my_protocol) return t + " (other version)";
    if (h.game.max > 0 && h.game.players >= h.game.max) return t + " (full)";
    if (h.game.max > 0) return t + " (" + std::to_string(h.game.players) + "/" + std::to_string(h.game.max) + ")";
    return t;
}

bool Joinable(const Heard& h, int my_protocol) {
    return h.game.protocol == my_protocol && (h.game.max <= 0 || h.game.players < h.game.max);
}

std::optional<Heard> PickToJoin(const std::vector<Heard>& heard, const std::string& want, int my_protocol) {
    const auto lower = [](std::string t) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    const std::string w = lower(want);
    for (const Heard& h : heard) {
        if (!Joinable(h, my_protocol)) continue;
        if (w.empty() || w == "first" || lower(h.game.name) == w) return h;
    }
    return std::nullopt;
}

}  // namespace CoopLan
