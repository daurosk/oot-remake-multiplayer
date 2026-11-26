// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sync digest (sync-architect D6, step S0): each game writes, in its own log, what it holds of every shared object
// of its scene, on the server's clock, so a desync is found by comparing two players' logs with a tool
// (tools/autotest/compare_digest.py) instead of by a player noticing it. Module and tool only: nothing is sent.
// Tested in tests/lichii/digest_test.cpp.
//
// Every 2 s of server time (kPeriodUs) Take returns:
//   - one line per object whose compared fields changed since its last line, or whose last line is kRefreshUs old:
//       coop: digest: t=<server ms> scene=<s> self=<n> obj=<id> name=<actor> owner=<n> epoch=<e> life=<l> ...
//         ... h=<8 hex>
//   - one line per object that was in the last tick and is not in this one: ... obj=<id> gone
//   - one room line: ... room objects=<count> h=<8 hex over the objects' hashes, by id>
// <id> is p<placement hash hex> for a placed object, m<maker>.<counter hex> for a made one. Fields are key=value with
// no spaces (a space in a value becomes '_'). t, scene, self, obj and h are context; the object's fields are what
// every game must agree on (later steps add their own: brain keys, room plugs) and h hashes those only.
// A field whose key starts with "local." is this game's own view (a copy's last owner sample) and is not compared
// either: it says whether a difference came over the wire or arose here.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopDigest {

inline constexpr std::uint64_t kPeriodUs = 2'000'000;
inline constexpr std::uint64_t kRefreshUs = 10'000'000;  // an unchanged object is written again this often
inline constexpr std::string_view kPrefix = "coop: digest: ";

// What one game holds of one shared object (or, later, of its room): compared fields in a fixed order.
struct Entry {
    coop::protocol::ObjectId id;
    std::vector<std::pair<std::string, std::string>> fields;  // key, value
};

inline std::string IdText(const coop::protocol::ObjectId& id) {
    char buf[40];
    if (id.kind == coop::protocol::ObjectKind::placed) {
        std::snprintf(buf, sizeof buf, "p%llx", static_cast<unsigned long long>(id.value));
    } else {
        std::snprintf(buf, sizeof buf, "m%u.%llx", static_cast<unsigned>(id.maker),
                      static_cast<unsigned long long>(id.value));
    }
    return buf;
}

// A value with no spaces (the line is split on spaces), never empty.
inline std::string Clean(std::string_view v) {
    std::string out(v.empty() ? std::string_view("-") : v);
    for (char& c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '=') c = '_';
    }
    return out;
}

inline bool IsLocal(std::string_view key) { return key.starts_with("local."); }

// FNV-1a 32 over the compared fields, "key=value;" each, in order.
inline std::uint32_t Hash(const Entry& e) {
    std::uint32_t h = 2166136261u;
    const auto add = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 16777619u;
        }
    };
    for (const auto& [k, v] : e.fields) {
        if (IsLocal(k)) continue;
        add(k);
        add("=");
        add(Clean(v));
        add(";");
    }
    return h;
}

inline std::string Hex8(std::uint32_t h) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "%08x", h);
    return buf;
}

// The lines of one game. One per hook (CoopObjects' share step); not thread-safe.
class Log {
public:
    // The lines due at server time `now_us` (none between ticks). `entries` is everything this game shares now in
    // `scene`; an id given twice keeps its first entry.
    std::vector<std::string> Take(std::uint64_t now_us, std::string_view scene, std::uint8_t self,
                                  const std::vector<Entry>& entries) {
        std::vector<std::string> out;
        if (!Due(now_us)) {
            return out;
        }
        // On time or a little late: keep the beat; far behind (a load, a pause): start over.
        next_us_ = next_us_ != 0 && now_us - next_us_ < kPeriodUs ? next_us_ + kPeriodUs : now_us + kPeriodUs;
        const std::string head = std::string(kPrefix) + "t=" + std::to_string(now_us / 1000) + " scene=" +
                                 Clean(scene) + " self=" + std::to_string(self) + " ";
        if (scene != scene_) {
            last_.clear();  // a new scene: every object is written afresh (the tool keeps each scene apart)
            scene_ = std::string(scene);
        }
        std::map<coop::protocol::ObjectId, Last> now;
        std::uint32_t room = 2166136261u;
        for (const Entry& e : entries) {
            if (now.contains(e.id)) continue;
            const std::uint32_t h = Hash(e);
            std::string body;
            for (const auto& [k, v] : e.fields) {
                body += " " + Clean(k) + "=" + Clean(v);
            }
            Last l{h, body, now_us};
            const auto was = last_.find(e.id);
            if (was != last_.end() && was->second.hash == h && was->second.body == body &&
                now_us - was->second.written_us < kRefreshUs) {
                l.written_us = was->second.written_us;
            } else {
                out.push_back(head + "obj=" + IdText(e.id) + body + " h=" + Hex8(h));
            }
            now.emplace(e.id, std::move(l));
        }
        for (const auto& [id, l] : now) {  // by id: the same order in every game
            for (int shift = 0; shift < 32; shift += 8) {
                room ^= (l.hash >> shift) & 0xffu;
                room *= 16777619u;
            }
        }
        for (const auto& [id, l] : last_) {
            if (!now.contains(id)) {
                out.push_back(head + "obj=" + IdText(id) + " gone");
            }
        }
        out.push_back(head + "room objects=" + std::to_string(now.size()) + " h=" + Hex8(room));
        last_ = std::move(now);
        return out;
    }

    // Whether Take(now_us, ...) writes lines: the caller builds the entries only then (they cost a walk of
    // every shared object, so not every frame).
    bool Due(std::uint64_t now_us) const { return now_us != 0 && (next_us_ == 0 || now_us >= next_us_); }

    void Reset() {
        last_.clear();
        next_us_ = 0;
        scene_.clear();
    }

private:
    struct Last {
        std::uint32_t hash = 0;
        std::string body;
        std::uint64_t written_us = 0;
    };
    std::map<coop::protocol::ObjectId, Last> last_;
    std::uint64_t next_us_ = 0;
    std::string scene_;
};

} // namespace CoopDigest
