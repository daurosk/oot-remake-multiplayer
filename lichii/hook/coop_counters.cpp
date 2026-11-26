// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_counters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

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

std::uint64_t WaitKey(std::uint64_t key, int wait) {
    const std::uint64_t h = Mix(Mix(0x84222325cbf29ce4ull, key), static_cast<std::uint64_t>(wait + 1));
    return h == 0 ? 1 : h;
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

std::uint64_t LoopOf(const std::deque<std::uint64_t>& periods, float slack) {
    const std::size_t n = periods.size();
    if (n >= 2 && Close(periods[n - 1], periods[n - 2], slack)) {
        return periods[n - 1];
    }
    if (n >= 4 && Close(periods[n - 1], periods[n - 3], slack) &&
        Close(periods[n - 2], periods[n - 4], slack)) {
        return periods[n - 1] + periods[n - 2];
    }
    return 0;
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
    if (!message.ticks.empty() && !away_.contains(message.player_number)) {
        present_since_.try_emplace(message.player_number, message.server_time_us);  // row 164: here since then
    }
}

void CounterSync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t server_us,
                       const std::vector<Seen>& seen, bool probe_only, Out& out,
                       const std::set<std::uint8_t>& away) {
    if (scene != scene_) {
        Reset();
        scene_ = scene;
    }
    // Row 164: an away game (a pause menu, a load, a cutscene) leads nobody, and settles again once it is back.
    away_ = away;
    for (const std::uint8_t p : away_) {
        present_since_.erase(p);
    }
    const bool self_away = away_.contains(self);
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
        // More frames left than the pause's length is a misread, unless we wrote it: a pause we made longer (the
        // period rule, AlignPause) reads above its length until it runs down. Dropped, the node started over at
        // zero starts and its next pauses were never lined up (report 138's review).
        const auto known = nodes_.find(s.node);
        const float most = std::max(s.duration, known != nodes_.end() && known->second.key == s.key
                                                    ? known->second.last_remaining : 0.0f);
        if (!IsPlatformOwner(s.owner_name) || s.node == 0 || !std::isfinite(s.remaining) ||
            !std::isfinite(s.duration) || s.duration <= 0.0f || s.remaining > most + 1.0f) {
            continue;
        }
        here.insert(s.node);
        if (s.owner_hash != 0) {
            counter_owners_.insert(s.owner_hash);  // the clock-word path leaves it alone
        }
        if (s.driven) {
            nodes_.erase(s.node);  // its own logic moves it (a switch, a sensor, a rider): never sent or written
            continue;
        }
        Node& n = nodes_[s.node];
        if (n.key != s.key) {
            n = Node{};  // a new node at that address
            n.key = s.key;
        }
        n.owner_hash = s.owner_hash;
        n.owner_name = s.owner_name;
        n.switched = s.switched;
        const float elapsed_frames = std::max(0.0f, s.duration - std::max(s.remaining, 0.0f));
        const auto began = [&] {
            const auto back = static_cast<std::uint64_t>(elapsed_frames / kFramesPerSecond * 1e6f);
            return server_us > back ? server_us - back : 0;
        };
        if (!n.looked) {
            n.looked = true;
            if (s.remaining > 0.0f) {
                n.start_us = began();  // seen in the middle of a pause
                n.starts = 1;
            }
        } else if (s.remaining > n.last_remaining + 0.5f) {
            const std::uint64_t start = began();
            n.started_now = true;
            n.wait_known = s.wait >= 0;
            n.wait = s.wait > 0 ? 1 : 0;
            if (n.wait_known && n.home_said < 2) {
                ++n.home_said;
                char text[200];
                std::snprintf(text, sizeof text, "%016llx %s: a pause %.2f m from its placement (%s wait)",
                              static_cast<unsigned long long>(n.owner_hash), n.owner_name.c_str(),
                              static_cast<double>(s.home_m), n.wait == 0 ? "home" : "far");
                out.notes.emplace_back(text);
            }
            auto& ws = n.wait_start[static_cast<std::size_t>(n.wait)];
            if (ws != 0 && start > ws && start - ws >= kMinPeriodUs && start - ws <= kMaxPeriodUs) {
                n.wait_period[static_cast<std::size_t>(n.wait)] = start - ws;
            }
            ws = start;
            if (n.start_us != 0 && start > n.start_us) {
                const std::uint64_t period = start - n.start_us;
                if (period >= kMinPeriodUs && period <= kMaxPeriodUs) {
                    n.periods.push_back(period);
                    while (n.periods.size() > 4) {
                        n.periods.pop_front();
                    }
                } else {
                    n.periods.clear();
                }
            }
            n.start_us = start;
            ++n.starts;
            const bool was = n.looping;
            const std::uint64_t loop = LoopOf(n.periods, kPeriodSlack);
            if (loop != 0) {
                n.cycle_us = loop;
                n.looping = true;
                n.misses = 0;
                const std::size_t np = n.periods.size();
                const bool two = loop != n.periods.back();
                if (!two || n.periods[np - 1] >= n.periods[np - 2]) {
                    n.ref_us = start;  // ends the longer period (or the only one)
                }
            } else if (n.looping && ++n.misses >= kLoopMisses) {
                n.looping = false;  // it stopped looping (a switch, a broken lift)
                n.cycle_us = 0;
                n.ref_us = 0;
            }
            if (n.looping && !was && !n.logged) {
                n.logged = true;
                char text[200];
                std::snprintf(text, sizeof text, "%016llx %s: pause counter %016llx loops every %.2f s%s",
                              static_cast<unsigned long long>(n.owner_hash), n.owner_name.c_str(),
                              static_cast<unsigned long long>(n.key),
                              static_cast<double>(n.cycle_us) / 1e6,
                              n.cycle_us != n.periods.back() ? " (two waits)" : "");
                out.notes.emplace_back(text);
            }
        }
        n.last_remaining = s.remaining;
        // Row 164: our game stands still (a pause menu, a load, a cutscene) while the server clock runs: a pause
        // written now would end at the wrong time. A switched mover is lined up only while it loops on its own.
        if (per_key[n.key] != 1 || s.remaining <= 0.0f || self_away || (n.switched && !n.looping)) {
            continue;
        }
        // Pause by pause, when our wait is known and a leader sends its waits: the lowest settled number, the same
        // in every game (review: counts compared at different times could leave every game thinking it leads).
        // Row 164: a follower lines up from its first pause here (the leader shares only a counter that repeats in
        // its game), not only after kPauseStarts: a game that came back into the room is in step at once.
        if (n.wait_known) {
            const std::uint8_t leader = Leader(n, self, server_us);
            if (leader != 0) {
                if (leader != self) {
                    AlignPause(n, s, leader, server_us, probe_only, out);
                }
                continue;
            }
        }
        if (!Shared(n) || !n.looping || n.ref_us == 0 || n.written_start_us == n.start_us) {
            continue;
        }
        // The period rule (no wait known, or a game that sends no waits: an older module): the game that has
        // seen the most starts (then the lowest number), with the same period.
        const auto tk = theirs_.find(n.key);
        if (tk == theirs_.end()) {
            continue;
        }
        const std::uint64_t period = n.cycle_us;
        const Theirs* best = nullptr;
        std::uint8_t best_player = 0;
        for (const auto& [player, th] : tk->second) {
            if (player == self || th.start_us == 0 || !Close(th.period_us, period, kPeriodSlack) ||
                away_.contains(player)) {  // row 164: a game standing still is followed by nobody
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
        const float gap = PhaseGap(n.ref_us, best->start_us, period);
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
        WritePause(n, s, target, gap, best_player, "", probe_only, out);
    }
    std::erase_if(nodes_, [&](const auto& p) { return !here.contains(p.first); });
    struct Tick {
        std::uint64_t key, ref, cycle;
        std::uint32_t starts;
    };
    std::vector<Tick> ticks;
    std::set<std::uint64_t> sent_keys;
    bool fresh = false;  // a start that goes into the message: sent at once, at most every kMinSendGapUs
    const auto add = [&](std::uint64_t key, std::uint64_t ref, std::uint64_t cycle, std::uint32_t starts) {
        if (ref == 0 || ref > server_us || cycle == 0 || cycle > 0xffffffffull || server_us - ref > 0xffffffffull ||
            !sent_keys.insert(key).second) {
            return false;
        }
        ticks.push_back({key, ref, cycle, starts});
        return true;
    };
    for (auto& [address, n] : nodes_) {
        const bool started = std::exchange(n.started_now, false);
        if (!Shared(n) || per_key[n.key] != 1) {
            continue;
        }
        bool in = false;
        if (n.looping) {
            // The loop and the start every game compares, under the node's key (the period rule).
            in = add(n.key, n.ref_us, n.cycle_us, n.starts) || in;
        }
        if (n.wait_known) {
            // Each wait under its own key: its last start and the time between two starts of it.
            for (std::size_t w = 0; w < 2; ++w) {
                in = add(WaitKey(n.key, static_cast<int>(w)), n.wait_start[w], n.wait_period[w], n.starts) || in;
            }
        }
        fresh = fresh || (started && in);
    }
    const bool due = last_send_us_ == 0 || server_us >= last_send_us_ + kSendEveryUs;
    const bool soon = fresh && (last_send_us_ == 0 || server_us >= last_send_us_ + kMinSendGapUs);
    if (!due && !soon) {
        return;
    }
    last_send_us_ = server_us;
    if (!ticks.empty() && !self_away) {
        present_since_.try_emplace(self, server_us);  // row 164: we are here since our first send
    }
    // As many messages as it takes, kMaxPlatformCounters entries each (review: at 3 per node the cap filled at
    // about 10 nodes, and the same nodes always missed out).
    for (std::size_t from = 0; from < ticks.size(); from += coop::protocol::kMaxPlatformCounters) {
        coop::protocol::PlatformCounter m;
        m.scene = scene;
        m.server_time_us = server_us;
        for (std::size_t k = from; k < ticks.size() && k < from + coop::protocol::kMaxPlatformCounters; ++k) {
            const Tick& t = ticks[k];
            m.ticks.push_back({t.key, static_cast<std::uint32_t>(t.cycle),
                               static_cast<std::uint32_t>(server_us - t.ref), t.starts});
        }
        out.send.emplace_back(std::move(m));
    }
}

bool CounterSync::Shared(const Node& n) {
    if (n.switched) {
        return n.looping;  // row 164: a switch's moves never loop; the shortcut lift's own loop does
    }
    return n.looping || n.starts >= kPauseStarts;
}

bool CounterSync::Settled(std::uint8_t player, std::uint64_t server_us) const {
    const auto it = present_since_.find(player);
    return it != present_since_.end() && server_us >= it->second + kSettleUs;
}

std::uint8_t CounterSync::Leader(const Node& n, std::uint8_t self, std::uint64_t server_us) const {
    // Each other game's newest start over both waits, and the longest period it gave.
    std::map<std::uint8_t, std::pair<std::uint64_t, std::uint64_t>> newest;  // player -> (start, period)
    for (const std::uint64_t key : {WaitKey(n.key, 0), WaitKey(n.key, 1)}) {
        if (const auto tk = theirs_.find(key); tk != theirs_.end()) {
            for (const auto& [player, th] : tk->second) {
                if (player == self || th.start_us == 0) {
                    continue;
                }
                auto& [start, period] = newest[player];
                start = std::max(start, th.start_us);
                period = std::max<std::uint64_t>(period, th.period_us);
            }
        }
    }
    // The games that may lead: the others that send this counter's waits, and we once we send them (a game that
    // has not yet shared this counter leads nobody: the others would not see what to follow).
    std::vector<std::uint8_t> candidates;
    for (const auto& [player, sp] : newest) {
        const auto [start, period] = sp;
        if (period != 0 && server_us > start + kLeaderStalePeriods * period) {
            continue;  // M1: its lift stopped starting; its resent start leads nobody
        }
        if (away_.contains(player)) {
            continue;  // row 164: its game stands still (a pause menu, a load, a cutscene)
        }
        candidates.push_back(player);
    }
    if (candidates.empty()) {
        return 0;  // nobody else sends this counter's waits
    }
    if (Shared(n) && !away_.contains(self)) {
        candidates.push_back(self);
    }
    // Row 164: the lowest settled number; none settled (all new here): the lowest number, as before.
    std::uint8_t lowest = 0;
    std::uint8_t settled = 0;
    for (const std::uint8_t p : candidates) {
        lowest = lowest == 0 ? p : std::min(lowest, p);
        if (Settled(p, server_us)) {
            settled = settled == 0 ? p : std::min(settled, p);
        }
    }
    return settled != 0 ? settled : lowest;
}

bool CounterSync::AlignPause(Node& n, const Seen& s, std::uint8_t leader, std::uint64_t server_us,
                             bool probe_only, Out& out) {
    if (!n.wait_known || n.start_us == 0 || n.aligned_start_us == n.start_us) {
        return false;
    }
    const auto tk = theirs_.find(WaitKey(n.key, n.wait));
    const auto it = tk == theirs_.end() ? std::map<std::uint8_t, Theirs>::const_iterator{}
                                        : tk->second.find(leader);
    const Theirs* th = tk == theirs_.end() || it == tk->second.end() ? nullptr : &it->second;
    const auto pause_us = static_cast<std::uint64_t>(static_cast<double>(s.duration) / kFramesPerSecond * 1e6);
    // The leader's start of this same pause: no older than one pause before ours. Then ours ends when the
    // leader's does, from that actual start (review: a guess from its last period is off by seconds).
    if (th != nullptr && th->start_us != 0 && th->start_us + pause_us >= n.start_us) {
        n.aligned_start_us = n.start_us;
        const auto end_us = static_cast<std::int64_t>(th->start_us + pause_us);
        const float gap = static_cast<float>(
            static_cast<double>(static_cast<std::int64_t>(n.start_us) - static_cast<std::int64_t>(th->start_us)) / 1e6);
        const float target = std::max(0.0f, static_cast<float>(static_cast<double>(end_us - static_cast<std::int64_t>(server_us)) /
                                                                1e6 * kFramesPerSecond));
        if (std::fabs(target - s.remaining) <= kAlignOffS * kFramesPerSecond) {
            return false;  // in step: nothing to write
        }
        WritePause(n, s, target, gap, leader, n.wait == 0 ? " (this pause, home wait)" : " (this pause, far wait)",
                   probe_only, out);
        return true;
    }
    // Not come yet: we began first, or the leader is more than a pause ahead (its next start of this wait is
    // most of a period away). Held from running out until it comes: only when the leader gave this wait's period,
    // and only until its next start is due (review M1: a flat 60 s held every follower's lift at each wait when
    // the leader stopped, or never sent this wait), never past kMaxHoldUs.
    std::uint64_t due_us = 0;
    if (th != nullptr && th->start_us != 0 && th->period_us != 0) {
        due_us = th->start_us + static_cast<std::uint64_t>(kHoldDuePeriods * th->period_us) + pause_us;
    }
    if (due_us == 0 || server_us > due_us || server_us > n.start_us + kMaxHoldUs) {
        n.aligned_start_us = n.start_us;  // not held / given up: it runs out as the game has it
        return false;
    }
    if (n.held_start_us != n.start_us) {
        n.held_start_us = n.start_us;
        char text[200];
        std::snprintf(text, sizeof text, "%016llx %s: pause counter %016llx held for player %u's start (%s wait)",
                      static_cast<unsigned long long>(n.owner_hash), n.owner_name.c_str(),
                      static_cast<unsigned long long>(n.key), static_cast<unsigned>(leader),
                      n.wait == 0 ? "home" : "far");
        out.notes.emplace_back(text);
    }
    if (s.remaining < kKeepFrames && !probe_only) {
        out.writes.push_back({s.node, n.key, kKeepFrames * 2.0f, s.remaining});  // kept: not counted, not said
        n.last_remaining = kKeepFrames * 2.0f;
    }
    return false;
}

void CounterSync::WritePause(Node& n, const Seen& s, float target, float gap, std::uint8_t leader, const char* how,
                             bool probe_only, Out& out) {
    if (probe_only) {
        n.written_start_us = n.start_us;  // said once a pause; nothing changes
    } else {
        out.writes.push_back({s.node, n.key, target, s.remaining});
        ++writes_;
        // The pause now ends as if it began this much earlier (negative: later): every start we send moves.
        const auto shift =
            static_cast<std::int64_t>(static_cast<double>(s.remaining - target) / kFramesPerSecond * 1e6);
        const auto move = [shift](std::uint64_t& t) {
            if (t != 0) {
                t = static_cast<std::uint64_t>(static_cast<std::int64_t>(t) - shift);
            }
        };
        move(n.start_us);
        move(n.ref_us);  // the start the period rule compares moves with it (review: it kept the old one)
        move(n.wait_start[static_cast<std::size_t>(n.wait)]);  // this pause is this wait's last start
        n.written_start_us = n.start_us;
        n.last_remaining = target;
    }
    char text[240];
    std::snprintf(text, sizeof text, "%016llx %s: pause counter %016llx %.2f s off player %u's%s: frames left %.1f -> %.1f%s",
                  static_cast<unsigned long long>(n.owner_hash), n.owner_name.c_str(),
                  static_cast<unsigned long long>(n.key), static_cast<double>(gap), static_cast<unsigned>(leader),
                  how, static_cast<double>(s.remaining), static_cast<double>(target),
                  probe_only ? " (probe: not written)" : "");
    out.notes.emplace_back(text);
}

void CounterSync::Reset() {
    scene_.clear();
    nodes_.clear();
    theirs_.clear();
    last_send_us_ = 0;
    counter_owners_.clear();
    away_.clear();
    present_since_.clear();
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

std::uint64_t CounterSync::CycleOf(std::uint64_t key) const {
    for (const auto& [address, n] : nodes_) {
        if (n.key == key && n.looping) {
            return n.cycle_us;
        }
    }
    return 0;
}

bool CounterSync::Looping(std::uint64_t key) const {
    return std::any_of(nodes_.begin(), nodes_.end(),
                       [key](const auto& p) { return p.second.key == key && p.second.looping; });
}

}  // namespace CoopCounters
