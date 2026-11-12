// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_counters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace CoopCounters {
namespace {

constexpr std::uint64_t kSlotLimit = 0x100000;  // node - graph larger than this: not a slot

std::uint64_t Mix(std::uint64_t h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ull;
    }
    return h;
}

bool Close(std::uint64_t a, std::uint64_t b, float slack) {
    const double hi = static_cast<double>(std::max(a, b));
    const double lo = static_cast<double>(std::min(a, b));
    return hi - lo <= slack * hi;
}

}  // namespace

std::vector<std::uint64_t> RingNodes(const std::array<std::uint64_t, kRingNodes>& entries) {
    std::vector<std::uint64_t> out;
    for (const std::uint64_t e : entries) {
        if (e != 0 && std::find(out.begin(), out.end(), e) == out.end()) {
            out.push_back(e);
        }
    }
    return out;
}

std::uint64_t KeyOf(std::uint64_t placement_hash, float duration_frames, std::uint64_t node,
                    std::uint64_t graph) {
    const auto tenths = static_cast<std::int64_t>(std::lround(static_cast<double>(duration_frames) * 10.0));
    const std::uint64_t slot =
        graph != 0 && node > graph && node - graph < kSlotLimit ? node - graph : 0;
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = Mix(h, placement_hash);
    h = Mix(h, static_cast<std::uint64_t>(tenths));
    h = Mix(h, slot);
    return h == 0 ? 1 : h;
}

bool IsPlatformOwner(std::string_view name) {
    return name.starts_with("Obj_");  // not enemies, NPCs, items or Link (Hot Onion)
}

void OtherNodes::Add(std::uint64_t node, std::uint64_t actor) {
    if (nodes_.size() >= kMax) {
        nodes_.clear();
    }
    nodes_[node] = actor;
}

bool OtherNodes::Skip(std::uint64_t node, std::uint64_t actor_now) {
    const auto it = nodes_.find(node);
    if (it == nodes_.end()) {
        return false;
    }
    if (it->second != actor_now) {
        nodes_.erase(it);  // reused: someone else's node now
        return false;
    }
    return true;
}

void OtherNodes::Keep(const std::function<bool(std::uint64_t actor)>& alive) {
    std::erase_if(nodes_, [&](const auto& p) { return !alive(p.second); });
}

float PhaseGap(std::uint64_t ours_us, std::uint64_t theirs_us, std::uint64_t period_us) {
    if (period_us == 0) {
        return 0.0f;
    }
    const auto p = static_cast<std::int64_t>(period_us);
    std::int64_t d = static_cast<std::int64_t>(ours_us) - static_cast<std::int64_t>(theirs_us);
    d %= p;
    if (d > p / 2) {
        d -= p;
    } else if (d <= -p / 2) {
        d += p;
    }
    return static_cast<float>(static_cast<double>(d) / 1e6);
}

void CounterSync::Receive(const coop::protocol::PlatformCounter& message) {
    if (message.scene != scene_ || message.player_number == 0) {
        return;
    }
    for (const auto& t : message.ticks) {
        Theirs& th = theirs_[t.key][message.player_number];
        if (th.got_us != 0 && message.server_time_us <= th.got_us) {
            continue;  // older than one we have
        }
        th.got_us = message.server_time_us;
        th.start_us = message.server_time_us > t.since_us ? message.server_time_us - t.since_us : 0;
        th.period_us = t.period_us;
        th.starts = t.starts;
    }
}

void CounterSync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t server_us,
                       const std::vector<Seen>& seen, bool probe_only, Out& out) {
    if (scene != scene_) {
        Reset();
        scene_ = scene;
    }
    for (auto it = theirs_.begin(); it != theirs_.end();) {
        std::erase_if(it->second, [&](const auto& p) { return server_us > p.second.got_us + kStaleUs; });
        it = it->second.empty() ? theirs_.erase(it) : std::next(it);
    }
    std::set<std::uint64_t> here;
    std::map<std::uint64_t, int> per_key;  // our nodes of each key: two is no key
    for (const Seen& s : seen) {
        if (IsPlatformOwner(s.owner_name)) {
            ++per_key[s.key];
        }
    }
    for (const Seen& s : seen) {
        if (!IsPlatformOwner(s.owner_name) || s.node == 0 || !std::isfinite(s.remaining) ||
            !std::isfinite(s.duration) || s.duration <= 0.0f || s.remaining > s.duration + 1.0f) {
            continue;
        }
        here.insert(s.node);
        Node& n = nodes_[s.node];
        if (n.key != s.key) {
            n = Node{};  // a new node at that address
            n.key = s.key;
        }
        n.owner_hash = s.owner_hash;
        n.owner_name = s.owner_name;
        const float elapsed_frames = std::max(0.0f, s.duration - std::max(s.remaining, 0.0f));
        const auto began = [&] {
            const auto back = static_cast<std::uint64_t>(elapsed_frames / kFramesPerSecond * 1e6f);
            return server_us > back ? server_us - back : 0;
        };
        if (n.last_remaining < 0.0f) {
            if (s.remaining > 0.0f) {
                n.start_us = began();  // seen in the middle of a pause
                n.starts = 1;
            }
        } else if (s.remaining > n.last_remaining + 0.5f) {
            const std::uint64_t start = began();
            if (n.start_us != 0 && start > n.start_us) {
                const std::uint64_t period = start - n.start_us;
                if (period >= kMinPeriodUs && period <= kMaxPeriodUs) {
                    n.periods.push_back(period);
                    while (n.periods.size() > 2) {
                        n.periods.pop_front();
                    }
                } else {
                    n.periods.clear();
                }
            }
            n.start_us = start;
            ++n.starts;
            const bool was = n.looping;
            n.looping = n.periods.size() == 2 && Close(n.periods[0], n.periods[1], kPeriodSlack);
            if (n.looping && !was && !n.logged) {
                n.logged = true;
                char text[200];
                std::snprintf(text, sizeof text, "%016llx %s: pause counter %016llx loops every %.2f s",
                              static_cast<unsigned long long>(n.owner_hash), n.owner_name.c_str(),
                              static_cast<unsigned long long>(n.key),
                              static_cast<double>(n.periods.back()) / 1e6);
                out.notes.emplace_back(text);
            }
        }
        n.last_remaining = s.remaining;
        if (!n.looping || per_key[n.key] != 1 || s.remaining <= 0.0f || n.written_start_us == n.start_us) {
            continue;
        }
        // The one to follow: the game that has seen the most starts (then
        // the lowest number), with the same period.
        const auto tk = theirs_.find(n.key);
        if (tk == theirs_.end()) {
            continue;
        }
        const std::uint64_t period = n.periods.back();
        const Theirs* best = nullptr;
        std::uint8_t best_player = 0;
        for (const auto& [player, th] : tk->second) {
            if (player == self || th.start_us == 0 || !Close(th.period_us, period, kPeriodSlack)) {
                continue;
            }
            if (best == nullptr || th.starts > best->starts) {
                best = &th;
                best_player = player;
            }
        }
        if (best == nullptr) {
            continue;
        }
        const float gap = PhaseGap(n.start_us, best->start_us, period);
        if (std::fabs(gap) <= kOffS) {
            continue;  // in phase: nothing to write
        }
        const bool we_move = n.starts < best->starts || (n.starts == best->starts && self > best_player);
        if (!we_move) {
            continue;
        }
        // Began later (gap > 0): end the pause sooner, if it has that long
        // left; else, and when it began earlier, make it longer (by the
        // rest of the period when it began later). One write either way.
        float target = s.remaining - gap * kFramesPerSecond;
        if (target < 0.0f) {
            target = s.remaining + (static_cast<float>(period) / 1e6f - gap) * kFramesPerSecond;
        }
        if (probe_only) {
            n.written_start_us = n.start_us;  // said once a pause; nothing changes
        } else {
            out.writes.push_back({s.node, n.key, target, s.remaining});
            ++writes_;
            // The pause now ends as if it began this much earlier (negative: later).
            const auto shift = static_cast<std::int64_t>(static_cast<double>(s.remaining - target) /
                                                         kFramesPerSecond * 1e6);
            n.start_us = static_cast<std::uint64_t>(static_cast<std::int64_t>(n.start_us) - shift);
            n.written_start_us = n.start_us;
            n.last_remaining = target;
        }
        char text[220];
        std::snprintf(text, sizeof text,
                      "%016llx %s: pause counter %016llx %.2f s off player %u's: frames left %.1f -> %.1f%s",
                      static_cast<unsigned long long>(n.owner_hash), n.owner_name.c_str(),
                      static_cast<unsigned long long>(n.key), static_cast<double>(gap),
                      static_cast<unsigned>(best_player), static_cast<double>(s.remaining),
                      static_cast<double>(target), probe_only ? " (probe: not written)" : "");
        out.notes.emplace_back(text);
    }
    std::erase_if(nodes_, [&](const auto& p) { return !here.contains(p.first); });
    if (last_send_us_ != 0 && server_us < last_send_us_ + kSendEveryUs) {
        return;
    }
    coop::protocol::PlatformCounter m;
    m.scene = scene;
    m.server_time_us = server_us;
    std::set<std::uint64_t> sent_keys;
    for (const auto& [address, n] : nodes_) {
        if (!n.looping || per_key[n.key] != 1 || n.start_us == 0 || n.start_us > server_us ||
            !sent_keys.insert(n.key).second || m.ticks.size() >= coop::protocol::kMaxPlatformCounters) {
            continue;
        }
        const std::uint64_t since = server_us - n.start_us;
        if (since > 0xffffffffull || n.periods.back() > 0xffffffffull) {
            continue;
        }
        m.ticks.push_back({n.key, static_cast<std::uint32_t>(n.periods.back()),
                           static_cast<std::uint32_t>(since), n.starts});
    }
    last_send_us_ = server_us;
    if (!m.ticks.empty()) {
        out.send.emplace_back(std::move(m));
    }
}

void CounterSync::Reset() {
    scene_.clear();
    nodes_.clear();
    theirs_.clear();
    last_send_us_ = 0;
}

std::set<std::uint64_t> CounterSync::LoopingOwners() const {
    std::set<std::uint64_t> out;
    for (const auto& [address, n] : nodes_) {
        if (n.looping && n.owner_hash != 0) {
            out.insert(n.owner_hash);
        }
    }
    return out;
}

bool CounterSync::Looping(std::uint64_t key) const {
    return std::any_of(nodes_.begin(), nodes_.end(),
                       [key](const auto& p) { return p.second.key == key && p.second.looping; });
}

}  // namespace CoopCounters
