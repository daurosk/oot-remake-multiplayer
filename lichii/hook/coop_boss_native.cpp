// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_native.h"
#include "coop/protocol/animation_words.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <variant>

namespace CoopBossNative {

namespace {

template <typename T>
T Get(const Request& r, std::size_t at) {
    T v{};
    std::memcpy(&v, r.data() + at, sizeof v);
    return v;
}

template <typename T>
void Put(Request& r, std::size_t at, T v) {
    std::memcpy(r.data() + at, &v, sizeof v);
}

std::string Hex(std::uint64_t v) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), digits[v & 15]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

} // namespace

std::array<std::optional<coop::protocol::Vec3>, 3> PlacesOf(const Request& r) {
    using namespace addr;
    std::array<std::optional<coop::protocol::Vec3>, 3> places{};
    for (std::size_t i = 0; i < places.size(); ++i) {
        const coop::protocol::Vec3 p{Get<float>(r, kReqPlaces + 12 * i), Get<float>(r, kReqPlaces + 12 * i + 4),
                                     Get<float>(r, kReqPlaces + 12 * i + 8)};
        if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
            places[i] = p;  // (a place whose first float is NaN is none: the game's rule)
        }
    }
    return places;
}

std::optional<BossAction> ActionFromRequest(const Request& r, bool* plug) {
    using namespace addr;
    if (Get<std::uint32_t>(r, kReqKind) != kKind || Get<std::uint32_t>(r, kReqKind2) != kKind2) {
        return std::nullopt;
    }
    BossAction a;
    a.slot = Get<std::int32_t>(r, kReqSlot);
    a.verb = Get<std::int32_t>(r, kReqVerb);
    a.flags = Get<std::uint16_t>(r, kReqFlags);
    const bool has_plug = r[kReqHasPlug] == 1;
    if (plug != nullptr) {
        *plug = has_plug;
    }
    if (!has_plug) {
        a.places = PlacesOf(r);
    }
    const std::array<float, 2> extra{Get<float>(r, kReqExtra), Get<float>(r, kReqExtra + 4)};
    if (std::isfinite(extra[0]) && std::isfinite(extra[1])) {
        a.extra = extra;
    }
    return a;
}

Request RequestFromAction(const BossAction& a, std::uint64_t node) {
    using namespace addr;
    Request r{};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t i = 0; i < a.places.size(); ++i) {
        const coop::protocol::Vec3 p = a.places[i].value_or(coop::protocol::Vec3{nan, 0.0f, 0.0f});
        for (std::size_t k = 0; k < 3; ++k) {
            Put<float>(r, kReqPlaces + 12 * i + 4 * k, p[k]);
        }
    }
    Put<std::uint32_t>(r, kReqKind, kKind);
    Put<std::uint32_t>(r, kReqKind2, kKind2);
    const std::array<float, 2> extra = a.extra.value_or(std::array<float, 2>{nan, nan});
    Put<float>(r, kReqExtra, extra[0]);
    Put<float>(r, kReqExtra + 4, extra[1]);
    Put<std::uint32_t>(r, kReqKind3, kKind3);
    Put<std::uint64_t>(r, kReqPlug, 0);
    r[kReqHasPlug] = 0;
    Put<std::int32_t>(r, kReqSlot, a.slot);
    Put<std::int32_t>(r, kReqVerb, a.verb);
    Put<std::uint16_t>(r, kReqFlags, a.flags);
    Put<std::uint64_t>(r, kReqNode, node);
    return r;
}

Request RequestFromAction(const BossAction& a, const OwnAsk& ask, std::uint32_t kind) {
    using namespace addr;
    Request r = RequestFromAction(a, ask.node);
    Put<std::uint32_t>(r, kReqKind3, kind);
    const bool placed = std::any_of(a.places.begin(), a.places.end(), [](const auto& p) { return p.has_value(); });
    if (ask.has_plug && ask.plug != 0 && (ask.named || !placed)) {
        Put<std::uint64_t>(r, kReqPlug, ask.plug);
        r[kReqHasPlug] = 1;
    }
    return r;
}

std::uint32_t KindFor(const std::optional<std::uint32_t>& owner,
                      const std::map<std::pair<std::int32_t, std::int32_t>, OwnAsk>& by_move, const BossAction& a) {
    if (owner) {
        return *owner;
    }
    const auto it = by_move.find({a.slot, a.verb});
    return it != by_move.end() ? it->second.kind : addr::kKind3;
}

std::optional<OwnAsk> AskFor(const std::map<std::pair<std::int32_t, std::int32_t>, OwnAsk>& by_move,
                             const std::map<std::int32_t, OwnAsk>& by_slot, std::uint64_t last, const BossAction& a) {
    if (const auto it = by_move.find({a.slot, a.verb}); it != by_move.end()) {
        return it->second;
    }
    if (const auto it = by_slot.find(a.slot); it != by_slot.end()) {
        return OwnAsk{it->second.node};  // (another move's plug is not this one's)
    }
    if (last != 0) {
        return OwnAsk{last};
    }
    return std::nullopt;
}

std::string_view AskRuleName(AskRule rule) {
    switch (rule) {
    case AskRule::own:
        return "its node of this move";
    case AskRule::slot:
        return "its slot's last node";
    case AskRule::last:
        return "its last node";
    case AskRule::other:
        return "another node of its own";
    case AskRule::all_conflict:
        return "its node of this move, though it chose another variation (every node did)";
    }
    return "?";
}

std::optional<OwnAsk> AskFor(const std::map<std::pair<std::int32_t, std::int32_t>, OwnAsk>& by_move,
                             const std::map<std::int32_t, OwnAsk>& by_slot, std::uint64_t last, const BossAction& a,
                             const OwnVariants& own, const std::optional<std::string>& variant, AskRule* rule) {
    const auto say = [&](AskRule r) {
        if (rule != nullptr) {
            *rule = r;
        }
    };
    const std::optional<OwnAsk> plain = AskFor(by_move, by_slot, last, a);
    const AskRule plain_rule = by_move.contains({a.slot, a.verb}) ? AskRule::own
                               : by_slot.contains(a.slot)         ? AskRule::slot
                                                                  : AskRule::last;
    say(plain_rule);
    if (!variant || !plain) {
        return plain;
    }
    const auto conflicts = [&](std::uint64_t node) {
        const auto it = own.choices.find({node, a.slot, a.verb});
        return it != own.choices.end() && it->second != *variant;
    };
    if (!conflicts(plain->node)) {
        return plain;
    }
    if (const auto it = by_slot.find(a.slot);
        it != by_slot.end() && it->second.node != 0 && !conflicts(it->second.node)) {
        say(AskRule::slot);
        return OwnAsk{it->second.node};
    }
    if (last != 0 && !conflicts(last)) {
        say(AskRule::last);
        return OwnAsk{last};
    }
    for (const auto& [key, chose] : own.choices) {
        const std::uint64_t node = std::get<0>(key);
        if (node != 0 && !conflicts(node)) {
            say(AskRule::other);
            return OwnAsk{node};
        }
    }
    say(AskRule::all_conflict);
    return plain;  // (every node it has chose another variation of this move: the move's own, as before)
}

std::string OwnAskVariant(const Request& r, const std::optional<std::string>& plug_name) {
    if (r[addr::kReqHasPlug] != 1) {
        return {};
    }
    return plug_name && !plug_name->empty() ? *plug_name : std::string("?");
}

std::optional<std::string> VariantOf(const std::optional<BossVariants>& v, std::uint32_t seq) {
    if (!v || seq == 0) {
        return std::nullopt;
    }
    for (const BossVariant& x : v->recent) {
        if (x.seq == seq) {
            return x.name;
        }
    }
    return std::nullopt;
}

namespace {

// ActionVariantOf's entry (nothing: not known).
const BossVariant* ActionVariantEntry(const BossBsa& state, const std::optional<BossVariants>& v) {
    if (!state.action || !v) {
        return nullptr;
    }
    for (auto it = state.recent.rbegin(); it != state.recent.rend(); ++it) {
        if (std::holds_alternative<BossAction>(it->what)) {
            // Its newest action change: the action now.
            const auto x = std::find_if(v->recent.begin(), v->recent.end(),
                                        [&](const BossVariant& b) { return b.seq == it->seq; });
            return x == v->recent.end() ? nullptr : &*x;
        }
    }
    // All of recent are brain changes: the owner sends its action now's variation older than recent (only that one).
    const std::uint32_t front = state.recent.empty() ? state.seq + 1 : state.recent.front().seq;
    for (auto it = v->recent.rbegin(); it != v->recent.rend(); ++it) {
        if (it->seq < front) {
            return &*it;
        }
    }
    return nullptr;
}

} // namespace

std::optional<std::string> ActionVariantOf(const BossBsa& state, const std::optional<BossVariants>& v) {
    if (const BossVariant* x = ActionVariantEntry(state, v)) {
        return x->name;
    }
    return std::nullopt;
}

void Table::SetOn(bool brain, bool actions) {
    std::lock_guard lock(mutex_);
    brain_on_ = brain;
    actions_on_ = brain && actions;
}

bool Table::BrainOn() const {
    std::lock_guard lock(mutex_);
    return brain_on_;
}

bool Table::ActionsOn() const {
    std::lock_guard lock(mutex_);
    return actions_on_;
}

Table::Entry* Table::Find(std::uint64_t bsa) {
    const auto it = by_bsa_.find(bsa);
    return it == by_bsa_.end() ? nullptr : &it->second;
}

Table::Entry* Table::FindActor(std::uint64_t actor) {
    for (auto& [bsa, e] : by_bsa_) {
        if (e.actor == actor) {
            return &e;
        }
    }
    return nullptr;
}

const Table::Entry* Table::FindActor(std::uint64_t actor) const {
    for (const auto& [bsa, e] : by_bsa_) {
        if (e.actor == actor) {
            return &e;
        }
    }
    return nullptr;
}

void Table::ForgetOwnAsks(Entry& e) {
    e.own_asks.clear();
    e.own_slot_asks.clear();
    e.own_variants.choices.clear();
}

void Table::BrainRuns(Entry& e, std::int32_t verb) {
    if (!e.asks_brain || *e.asks_brain != verb) {
        ForgetOwnAsks(e);  // (not known which they were learnt in: forgotten too)
    }
    e.asks_brain = verb;
}

void Table::LearnOwnAsk(Entry& e, const Request& request, const std::optional<std::string>& plug_name) {
    // The node (and plug) its own AI names for this move, for the owner's same move (AskFor). The game's callers
    // always name the asking node.
    std::uint64_t node = 0;
    std::memcpy(&node, request.data() + addr::kReqNode, sizeof node);
    if (node == 0) {
        return;
    }
    e.own_node = node;
    if (!e.asks_brain) {
        // The brain verb it is in, as far as we know (BrainRuns): ours as the owner, the owner's as a copy.
        if (e.role == Role::owner && e.have_mine) {
            e.asks_brain = e.mine.brain_verb;
        } else if (e.role == Role::copy && e.wanted) {
            e.asks_brain = e.wanted->brain_verb;
        }
    }
    const std::int32_t slot = Get<std::int32_t>(request, addr::kReqSlot);
    const std::int32_t verb = Get<std::int32_t>(request, addr::kReqVerb);
    const OwnAsk ask{node, Get<std::uint64_t>(request, addr::kReqPlug), request[addr::kReqHasPlug] == 1, false,
                     Get<std::uint32_t>(request, addr::kReqKind3)};
    if (e.own_asks.size() < kMaxOwnAsks || e.own_asks.contains({slot, verb})) {
        e.own_asks[{slot, verb}] = ask;
    }
    if (e.own_slot_asks.size() < kMaxOwnAsks || e.own_slot_asks.contains(slot)) {
        e.own_slot_asks[slot] = ask;
    }
    if (e.own_variants.choices.size() < kMaxOwnAsks || e.own_variants.choices.contains({node, slot, verb})) {
        e.own_variants.choices[{node, slot, verb}] = OwnAskVariant(request, plug_name);
    }
}

bool ContinuesChain(const BossBsa& ours, const BossBsa& theirs) {
    if (theirs.seq < ours.seq || ours.recent.empty() || ours.recent.back().seq != ours.seq) {
        return false;
    }
    bool newest = false;
    for (const BossChange& c : theirs.recent) {
        if (c.seq > ours.seq) {
            continue;
        }
        const auto it = std::find_if(ours.recent.begin(), ours.recent.end(),
                                     [&](const BossChange& x) { return x.seq == c.seq; });
        if (it != ours.recent.end()) {
            if (!(*it == c)) {
                return false;
            }
            newest = newest || c.seq == ours.seq;
        }
    }
    return newest;  // (their change at our newest seq is ours: nothing between is missing)
}

void Table::Note(std::string line) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(line));
    }
}

void Table::Stream(Entry& e, std::string kind) {
    if (kind == e.stream_last) {
        ++e.stream_run;
        return;
    }
    if (stream_notes_ < kMaxStreamNotes) {
        ++stream_notes_;
        Note(e.name + ": action stream: " + kind +
             (e.stream_last.empty() ? std::string()
                                    : " after " + std::to_string(e.stream_run) + " x " + e.stream_last));
    }
    e.stream_last = std::move(kind);
    e.stream_run = 1;
}

void Table::Publish(const std::vector<Boss>& bosses) {
    std::lock_guard lock(mutex_);
    std::map<std::uint64_t, Entry> next;
    for (const Boss& b : bosses) {
        if (b.actor == 0 || b.bsa == 0) {
            continue;
        }
        Entry e;
        if (const auto it = by_bsa_.find(b.bsa); it != by_bsa_.end() && it->second.actor == b.actor) {
            e = std::move(it->second);
            if (e.role != b.role || (b.role == Role::copy && e.owner != b.owner)) {
                Note(b.name + ": now " + (b.role == Role::owner ? "ours: our game runs it (sends its verbs)"
                                          : e.role == Role::copy ? "a copy of a new owner: it waits for that owner's state"
                                                                 : "a copy: it takes the owner's verbs"));
                // Row 149: a copy made ours whose body ran the owner's verbs while its own AI's were skipped (each
                // skipped request answered as taken): its AI may wait for a move its body never ran and never ask
                // the held one's end. Its brain verb is set again once (Step): it chooses from what its body does.
                // A copy in its own time (its stun of our hit, our event, its start) ran its own AI: left as it is.
                if (e.role == Role::owner && b.role == Role::copy && RunsOwnStart(b.name) &&
                    (steps_ < e.seen_since + kStartSteps || PlaysOwnStart(b.animation))) {
                    // #238: ours was handed over during its own start: the start runs on here, then the owner's
                    // state (taking its verbs now skipped the appear and left it unseen).
                    e.react_until = std::max(e.react_until, steps_ + kStartSteps);
                    Note(b.name + ": made a copy during its own start (appear): it runs on, then the owner's state");
                }
                const bool own_ask_same =
                    e.put_action && e.own_asks.contains({e.put_action->slot, e.put_action->verb});
                const MadeOurs plan = PlanMadeOurs(turns_, b.attacking, actions_on_ && e.put_action.has_value(),
                                                   own_ask_same, e.own_node != 0);
                if (e.role == Role::copy && b.role == Role::owner && e.wanted && !b.plain && !OwnTime(e) &&
                    plan == MadeOurs::resume) {
                    // Boss turns, condition (1): handed to us between moves. Its body runs the owner's move (not an
                    // attack) and its own brain asked that same move, in the owner's brain verb: it goes on from here.
                    Note(b.name + ": made ours between moves (verb " + std::to_string(e.put_action->verb) +
                         ", no attack): its own AI goes on, nothing started over (boss turns)");
                } else if (e.role == Role::copy && b.role == Role::owner && e.wanted && !b.plain && !OwnTime(e)) {
                    if (plan == MadeOurs::restart_move) {
                        // Row 178: made ours in the middle of the owner's move (Phantom Ganon handed over mid
                        // RushAttack when his owner went down): that move starts over as ours through the game's
                        // own request, named by the node its own AI names for it (before ForgetOwnAsks below).
                        BossAction again = *e.put_action;
                        again.flags = static_cast<std::uint16_t>(again.flags | addr::kReqStartOver);
                        again.changed = true;
                        e.restart_ask = AskFor(e.own_asks, e.own_slot_asks, e.own_node, again, e.own_variants,
                                               e.put_variant)
                                            .value_or(OwnAsk{e.own_node});
                        e.restart_action = again;
                        e.restart_variant = e.put_variant;  // (row 222 part 2: the variation it was put in with)
                        e.restart_variant_places = e.put_variant_places;  // (row 222: a named ask's places)
                        e.restart_variant_kind = e.put_variant_kind;      // (row 222: and the places' kind)
                        Note(b.name + ": made ours mid-move: its move (slot " + std::to_string(again.slot) +
                             " verb " + std::to_string(again.verb) + ") starts over as ours (row 178)");
                    } else if (turns_ && e.put_action && b.attacking.has_value() && !*b.attacking) {
                        // Row 325: mid-move, but no attack: its own cycle goes on from its body, nothing started over.
                        e.rechoose = true;
                        Note(b.name + ": made ours mid-move (verb " + std::to_string(e.put_action->verb) +
                             ", no attack): its AI chooses again from its body, nothing started over (row 325)");
                    } else {
                        e.rechoose = true;
                        Note(b.name + ": its AI did not run its body as a copy: it chooses again");
                    }
                }
                e.hold_since = steps_;
                e.hold_asks = 0;
                e.hold_rechoices = 0;
                e.stall_rechoices = 0;  // (another game's turn since: judged anew)
                // (own asks kept: the same boss in the same game; a brain change forgets them, BrainRuns)
                if (b.role == Role::copy) {
                    // Twinrova hand-over: the chain we held, so the new owner's changes after it are made in
                    // order (Receive), not collapsed into a resync of its last move.
                    if (e.role == Role::owner) {
                        e.handover = e.have_mine ? std::optional<BossBsa>(e.mine) : std::nullopt;
                    } else {
                        e.handover = e.wanted;
                    }
                    e.restart_action.reset();
                    // (put_action stays: our copy's body still runs that move; a new owner whose state names
                    // the same one, its old owner's, is not asked it again)
                    e.rechoose = false;
                } else {
                    e.put_action.reset();
                    e.put_variant.reset();
                    e.own_action_ran = false;
                }
                // Ours from here: we go on from the old owner's last state
                // and seq (changes still queued here are dropped; Step takes
                // the brain verb from our game's own BSA at once, so we never
                // send a phase our boss is not in). A copy: wait for the
                // owner's state.
                e.have_mine = e.have_mine || e.wanted.has_value();
                e.mine_variants.clear();
                e.mine_action_variant.reset();
                if (e.wanted) {
                    e.mine = *e.wanted;
                    if (b.role == Role::owner && e.wanted_variants) {
                        // Row 222 part 2: the old owner's variations go on with its seqs (a copy's next resync).
                        e.mine_variants = e.wanted_variants->recent;
                        if (const BossVariant* x = ActionVariantEntry(*e.wanted, e.wanted_variants)) {
                            e.mine_action_variant = *x;
                        }
                    }
                }
                e.wanted.reset();
                e.wanted_variants.reset();
                if (b.role == Role::owner || !e.handover) {
                    // (a copy keeps the old owner's changes still queued: the new owner's state joins them, or
                    // its resync drops them)
                    e.due.clear();
                    e.asking = Entry::Asking::none;
                    e.handover.reset();
                }
                e.queued_seq = 0;
                e.drift = 0;
                e.drift_fixes = 0;
            }
        } else {
            e.actor = b.actor;
            e.seen_since = steps_;
            Note(b.name + ": native boss sync on (BSA " + Hex(b.bsa) + ", " +
                 (b.role == Role::owner ? "ours" : "a copy") + ")");
            if (b.role == Role::copy && RunsOwnStart(b.name)) {
                // Issue 62 (seed N2BTEYKM, Dodongo's Cavern): a mid-boss copy
                // made while its owner already fights (the second Lizalfos,
                // back in our walk after the first died) took the owner's
                // verbs at once, its own appear was skipped and it stayed
                // unseen. Its own start runs first; then the owner's state.
                e.react_until = steps_ + kStartSteps;
                Note(b.name + ": a new copy: its own start (appear) runs first, then the owner's state");
            }
        }
        if (b.role == Role::copy && RunsOwnStart(b.name) && PlaysOwnStart(b.animation) && !e.dying &&
            steps_ < e.seen_since + kStartMaxSteps && e.react_until <= steps_ + kLookBackSteps) {
            // #238: its start animation still plays as its own time ends: the appear finishes first (bounded).
            if (e.react_until <= steps_) {
                Note(b.name + ": its start (appear) still plays: its own start runs on");
            }
            e.react_until = std::min(e.seen_since + kStartMaxSteps, steps_ + kStartSteps);
        }
        e.role = b.role;
        e.name = b.name;
        e.owner = b.owner;
        e.life_component = b.plain ? 0 : b.life_component;
        e.plain = b.plain;
        if (b.plain) {
            // A plain enemy's death is its own game's from its first frame: no boss glue names it.
            if (b.own_death && !e.dying && b.role == Role::copy) {
                if (e.wanted && !event_ && steps_ >= e.react_until) {
                    MakeLate(e);  // its death's first changes, skipped a moment ago
                }
                ++counts_.plain_deaths;
            }
            e.dying = b.own_death;
        } else {
            // Probe 3 run 20261123-092928: Twinrova's sisters died here (life 0, their owner's death) mid-fight, no
            // outro: their copies' own death flow (which starts the fusion event in the game that runs it) was
            // skipped as any copy change, so neither game fused. A dead body runs its own death, as a plain enemy.
            const bool was_dying = e.dying;
            e.body_dead = b.own_death;
            e.dying = Dying(b.name) || e.body_dead;
            if (e.dying && b.role == Role::copy) {
                if (Dying(b.name)) {
                    AddDyingLife(b.life_component);  // (the outro's: its next step ends it. A dead body's Life is
                                                     // free already, its owner's death released it: one that comes
                                                     // back, a Bongo Bongo hand, a Stalfos, is held again)
                }
                if (!was_dying && e.body_dead) {
                    if (e.wanted && !event_ && steps_ >= e.react_until) {
                        MakeLate(e);  // its death's first changes, skipped a moment ago
                    }
                    ++counts_.body_deaths;
                    Note(b.name + ": dead here (its life 0 or its owner's death): the copy runs all its own from now on");
                }
            } else if (was_dying && !e.dying && b.role == Role::copy) {
                e.react_until = std::max(e.react_until, steps_ + kAfterEventSteps);  // (a part back from life 0)
            }
            if (b.role == Role::owner) {
                Hold(e, b);
            }
            if (e.dying) {
                e.restart_action.reset();
            }
            Fall(e, b);
        }
        next.emplace(b.bsa, std::move(e));
    }
    by_bsa_ = std::move(next);

    // The copies' Life components for HeldLifeStep: held while the owner's
    // lives, released (let die) once it said it died. A slot already holding
    // the same component keeps its released bit's first sighting (said once).
    std::array<std::uint64_t, kMaxHeld> want{};
    std::size_t n = 0;
    for (const Boss& b : bosses) {
        if (b.plain || b.role != Role::copy || b.life_component == 0 || (b.life_component & kReleased) != 0 ||
            n == kMaxHeld) {
            continue;  // (a plain enemy's Life is never held: the Sync shares its kills)
        }
        want[n++] = b.life_component | (b.owner_dead || Dying(b.name) ? kReleased : 0);
    }
    for (std::size_t i = 0; i < kMaxHeld; ++i) {
        const std::uint64_t was = held_life_[i].load(std::memory_order_relaxed);
        if (want[i] != was) {
            if ((want[i] & kReleased) != 0 && (was & ~kReleased) == (want[i] & ~kReleased) && (was & kReleased) == 0) {
                for (const Boss& b : bosses) {
                    if (b.life_component == (want[i] & ~kReleased)) {
                        Note(b.name + (b.owner_dead ? ": its owner's died" : ": its death has begun") +
                             ": the copy's own Life step lets it die now");
                    }
                }
            }
            held_life_[i].store(want[i], std::memory_order_release);
        }
    }
}

void Table::Hold(Entry& e, const Boss& b) {
    const std::optional<std::int32_t> action =
        e.mine.action ? std::optional<std::int32_t>(e.mine.action->verb) : std::nullopt;
    const bool same = b.animation == e.hold_animation && e.mine.brain_verb == e.hold_brain &&
                      action == e.hold_action && b.life == e.hold_life;
    if (!same) {
        e.hold_animation = b.animation;
        e.hold_brain = e.mine.brain_verb;
        e.hold_action = action;
        e.hold_life = b.life;
        if (e.stall_rechoices != 0 && b.life != e.stall_life) {
            e.stall_rechoices = 0;  // a hit since: its fight goes on, nothing stuck is remembered
        }
        // Back in the move it was re-chosen out of (the re-choice's own short move between): the count goes on.
        const bool stuck_again = e.stall_rechoices != 0 && b.animation == e.stall_animation &&
                                 e.mine.brain_verb == e.stall_brain && action == e.stall_action;
        e.hold_rechoices = stuck_again ? e.stall_rechoices : 0;
    }
    if (b.life) {
        e.hit = e.hit || (e.top_life && *b.life < *e.top_life);
        e.top_life = std::max(e.top_life.value_or(*b.life), *b.life);
    }
    if (!same || event_ || e.dying || (NotBegun(b.name) && !e.hit) || !StallJudged(b.animation, b.life)) {
        // A new move, or none to judge (our event's, its death's, a cutscene's, its wait before the fight, nothing
        // read): counted from now.
        e.hold_since = steps_;
        e.hold_asks = 0;
        return;
    }
    if (steps_ - e.hold_since < kStallSteps || e.hold_asks == 0 || e.hold_rechoices >= kMaxStallRechoices) {
        return;
    }
    ++e.hold_rechoices;
    e.stall_animation = e.hold_animation;
    e.stall_brain = e.hold_brain;
    e.stall_action = e.hold_action;
    e.stall_life = e.hold_life;
    e.stall_rechoices = e.hold_rechoices;
    e.hold_since = steps_;
    e.hold_asks = 0;
    e.rechoose = true;
    ++counts_.owner_stalls;
    Note(b.name + ": ours held '" + b.animation + "' (brain " + std::to_string(e.hold_brain) + ", action " +
         (action ? std::to_string(*action) : std::string("none")) + ", life " + std::to_string(*b.life) + ") " +
         std::to_string(kStallSteps / 60) + " s with no hit: stuck, its AI chooses again (" +
         std::to_string(e.hold_rechoices) + " of " + std::to_string(kMaxStallRechoices) + ")");
}

bool Table::Watches(std::uint64_t component) const {
    if (component == 0) {
        return false;
    }
    for (const auto& slot : dying_life_) {
        if (slot.load(std::memory_order_acquire) == component) {
            return true;
        }
    }
    for (const auto& slot : held_life_) {
        if ((slot.load(std::memory_order_acquire) & ~kReleased) == component) {
            return true;
        }
    }
    return false;
}

bool Table::Finishes(std::uint64_t component) const {
    if (component == 0) {
        return false;
    }
    for (const auto& slot : dying_life_) {
        if (slot.load(std::memory_order_acquire) == component) {
            return true;
        }
    }
    for (const auto& slot : held_life_) {
        const std::uint64_t v = slot.load(std::memory_order_acquire);
        if ((v & ~kReleased) == component) {
            return (v & kReleased) != 0;
        }
    }
    return false;
}

std::int32_t Table::HeldLifeStep(std::uint64_t component, std::int32_t life, std::int32_t sum) {
    if (component == 0 || sum <= 0) {
        return sum;
    }
    for (const auto& slot : dying_life_) {
        if (slot.load(std::memory_order_acquire) != component) {
            continue;
        }
        if (life > 0 && sum < life) {
            life_finished_.fetch_add(1, std::memory_order_relaxed);
            return life;  // its death has begun: this step ends it (the owner's killing hit, here too)
        }
        return sum;
    }
    for (const auto& slot : held_life_) {
        const std::uint64_t v = slot.load(std::memory_order_acquire);
        if ((v & ~kReleased) != component) {
            continue;
        }
        if ((v & kReleased) != 0) {
            if (sum >= life && life > 0) {
                life_killed_.fetch_add(1, std::memory_order_relaxed);
            }
            return sum;  // the owner's died: ours dies with it
        }
        if (sum >= life) {
            life_held_.fetch_add(1, std::memory_order_relaxed);
            return std::max(0, life - 1);  // never dies of its own: the owner judges (its DAMAGE went there)
        }
        return sum;
    }
    return sum;
}

bool Table::OnBrainVerb(std::uint64_t bsa, std::int32_t verb, bool force, bool own) {
    std::lock_guard lock(mutex_);  // (waits: see the header, "The wraps")
    if (!brain_on_) {
        return false;
    }
    Entry* e = Find(bsa);
    if (e == nullptr) {
        return false;  // not a boss we know
    }
    if (e->role == Role::owner) {
        if (event_) {
            return false;  // our event's verb (its cutscene), not the fight's: sent when it is over
        }
        BrainRuns(*e, verb);
        e->mine.brain_verb = verb;
        Record(*e, BossBrain{verb, force});
        ++counts_.owner_brain;
        return false;
    }
    if (own || !e->wanted) {
        BrainRuns(*e, verb);
        return false;  // ours, or no owner's state yet: the copy runs as before
    }
    if (OwnTime(*e)) {
        BrainRuns(*e, verb);
        ++counts_.copy_own_run;  // our event or our own hit: the game's own change runs
        return false;
    }
    Keep(*e, BossBrain{verb, force});
    ++counts_.copy_skipped;
    if (e->plain && verb != e->wanted->brain_verb) {
        ++counts_.plain_wants;
        if (wants_.size() < kMaxWants && std::find(wants_.begin(), wants_.end(), e->actor) == wants_.end()) {
            wants_.push_back(e->actor);
        }
    }
    return true;
}

bool Table::OnActionVerb(std::uint64_t bsa, const Request& request, bool changed, bool own,
                         std::optional<std::string> plug_name, std::optional<Request>* renamed) {
    std::lock_guard lock(mutex_);  // (waits: see the header, "The wraps")
    if (!actions_on_) {
        return false;
    }
    Entry* e = Find(bsa);
    if (e == nullptr) {
        return false;
    }
    if (e->role == Role::owner) {
        if (event_) {
            return false;  // our event's move, not the fight's
        }
        if (!own) {
            LearnOwnAsk(*e, request, plug_name);  // (Twinrova hand-over: known when it is handed over)
        }
        bool plug = false;
        if (auto a = ActionFromRequest(request, &plug)) {
            a->changed = changed;
            Stream(*e, "ours asks slot " + std::to_string(a->slot) + " verb " + std::to_string(a->verb) +
                           " (changed " + std::to_string(changed ? 1 : 0) + (plug ? ", by plug)" : ")"));
            e->mine.action = *a;
            Record(*e, std::move(*a));
            // Row 222 part 2: its variation (plain, or the plug's name), sent with this change (component 24). A
            // plug we could not read as a name: not known (the copies name their own AI's, as before).
            std::optional<std::string> variant;
            if (!plug) {
                variant = std::string{};
            } else if (plug_name && !plug_name->empty() && coop::protocol::ValidVariantName(*plug_name)) {
                variant = std::move(plug_name);
            }
            std::erase_if(e->mine_variants, [&](const BossVariant& v) {
                return e->mine.recent.empty() || v.seq < e->mine.recent.front().seq;  // (gone from recent)
            });
            if (variant) {
                BossVariant entry{e->mine.seq, std::move(*variant)};
                if (!entry.name.empty()) {
                    entry.places = PlacesOf(request);  // row 222: its places (bsa's action has none for a plug ask)
                }
                entry.kind = Get<std::uint32_t>(request, addr::kReqKind3);  // row 222: its places' kind (KindFor)
                e->mine_variants.push_back(entry);
                e->mine_action_variant = std::move(entry);
                ++counts_.owner_variants;
            } else {
                e->mine_action_variant.reset();
            }
            ++counts_.owner_action;
            ++e->hold_asks;  // (its AI is acting: the stall check, Hold)
            if (plug) {
                ++counts_.plug_actions;
            }
        }
        return false;
    }
    if (own) {
        return false;
    }
    // A copy's own ask: its node is the one our calls name (the game's
    // callers always name the asking node).
    std::uint64_t node = 0;
    std::memcpy(&node, request.data() + addr::kReqNode, sizeof node);
    LearnOwnAsk(*e, request, plug_name);  // row 177: its node (and plug) of this move, for the owner's (AskFor)
    if (!e->wanted) {
        return false;
    }
    if (OwnTime(*e)) {
        ++counts_.copy_own_run;
        e->own_action_ran = true;  // its body leaves the owner's move: a resync asks that move again
        return false;
    }
    const std::int32_t ask_slot = Get<std::int32_t>(request, addr::kReqSlot);
    const std::int32_t ask_verb = Get<std::int32_t>(request, addr::kReqVerb);
    if (!changed && node != 0 && e->put_action && e->put_action->slot == ask_slot && e->put_action->verb == ask_verb) {
        // Row 222: its own AI asks the owner's move on: the game runs it (see the header), with the owner's variation.
        // A start of its own (changed 1: the AI asks the move again after it ended) is skipped: only the owner's
        // starts go in, or the owner's next start would fire a second shot.
        bool runs = !e->put_variant || *e->put_variant == OwnAskVariant(request, plug_name);
        if (!runs && renamed != nullptr) {
            Request r = request;
            std::uint64_t plug = 0;
            if (!e->put_variant->empty()) {
                const auto at = std::find(pool_.begin(), pool_.end(), *e->put_variant);
                if (at != pool_.end()) {
                    plug = SlotAt(static_cast<std::size_t>(at - pool_.begin()));
                }
            }
            if (e->put_variant->empty() || plug != 0) {
                std::memcpy(r.data() + addr::kReqPlug, &plug, sizeof plug);
                r[addr::kReqHasPlug] = plug != 0 ? 1 : 0;
                *renamed = r;
                ++counts_.own_move_renamed;
                runs = true;
            }
        }
        if (runs) {
            e->own_moves[ask_slot] = {ask_verb, steps_};
            ++counts_.copy_own_move;
            return false;
        }
    }
    if (auto a = ActionFromRequest(request)) {
        a->changed = changed;
        Keep(*e, std::move(*a));
    }
    ++counts_.copy_skipped;
    return true;
}

bool RunsOwnStart(std::string_view name) {
    return name.starts_with("Enemy_MiddleBoss_");
}

bool PlaysOwnStart(std::string_view animation) {
    return animation.starts_with("Appear");
}

bool OwnHitTookLife(std::optional<std::int32_t> before, std::optional<std::int32_t> after) {
    return before && after && *after < *before;
}

bool PlainDeath(std::optional<std::int32_t> life, bool owner_dead) {
    return owner_dead || (life && *life <= 0);
}

bool StallJudged(std::string_view animation, std::optional<std::int32_t> life) {
    // Its death (an own death move before the Director's outro: the death event refused, late, or waiting; life 0
    // whatever plays, as a Bongo Bongo hand down until it grows back) and a cutscene's animation (an event of
    // another game's, or one ours plays before SetEvent sees it) are never a held move: re-choosing there would
    // break the death or the cut (boss-die-0116).
    // Nor the sleep a boss waits in for its intro (Gohma's 'Sleep' before anyone came in): re-chosen twice there in a
    // four-player run (2026-11-20), as if stuck. (Its ceiling wait, Pre_Battle_Ceiling_Wait, stays judged: row 149.)
    namespace anim = coop::protocol::anim;
    return !animation.empty() && animation != "Sleep" && life && *life > 0 && !anim::IsDeath(animation) &&
           !anim::IsEvent(animation);
}

bool Table::OwnTime(const Entry& e) const {
    return event_ || e.dying || steps_ < e.react_until;
}

bool Table::Dying(std::string_view name) const {
    return std::find(dying_.begin(), dying_.end(), name) != dying_.end();
}

void Table::Release(std::uint64_t component) {
    if (component == 0) {
        return;
    }
    for (auto& slot : held_life_) {
        const std::uint64_t v = slot.load(std::memory_order_relaxed);
        if (v == component) {
            slot.store(v | kReleased, std::memory_order_release);
        }
    }
}

void Table::AddDyingLife(std::uint64_t component) {
    if (component == 0 || (component & kReleased) != 0) {
        return;
    }
    for (const auto& slot : dying_life_) {
        if (slot.load(std::memory_order_relaxed) == component) {
            return;
        }
    }
    for (auto& slot : dying_life_) {
        if (slot.load(std::memory_order_relaxed) == 0) {
            slot.store(component, std::memory_order_release);
            return;
        }
    }
}

bool Table::NotBegun(std::string_view name) const {
    return std::find(not_begun_.begin(), not_begun_.end(), name) != not_begun_.end();
}

void Table::SetNotBegun(std::vector<std::string> bodies) {
    std::lock_guard lock(mutex_);
    not_begun_ = std::move(bodies);
}

void Table::SetDying(std::vector<std::string> bodies) {
    std::lock_guard lock(mutex_);
    if (bodies == dying_) {
        return;
    }
    dying_ = std::move(bodies);
    if (dying_.empty()) {
        for (auto& slot : dying_life_) {
            slot.store(0, std::memory_order_release);
        }
    }
    for (auto& [bsa, e] : by_bsa_) {
        const bool now = Dying(e.name) || e.body_dead;
        if (e.plain || now == e.dying) {
            continue;  // (a plain enemy's death is Publish's own_death)
        }
        e.dying = now;
        if (e.role != Role::copy) {
            continue;
        }
        if (now) {
            if (e.wanted && !event_ && steps_ >= e.react_until) {
                MakeLate(e);  // its own death's first changes, skipped a moment ago
            }
            Release(e.life_component);
            AddDyingLife(e.life_component);
            Note(e.name + ": its death has begun: the copy runs all its own from now on (its Life not held)");
        } else {
            e.react_until = std::max(e.react_until, steps_ + kAfterEventSteps);
        }
    }
}

void Table::Keep(Entry& e, std::variant<BossBrain, BossAction> what) {
    e.skipped.push_back({steps_, std::move(what)});
    while (e.skipped.size() > kMaxSkippedKept) {
        e.skipped.pop_front();
    }
}

void Table::MakeLate(Entry& e) {
    for (auto& s : e.skipped) {
        if (steps_ - s.step <= kLookBackSteps) {
            e.own_late.push_back(std::move(s.what));
        }
    }
    e.skipped.clear();
}

void Table::SetEvent(bool running) {
    std::lock_guard lock(mutex_);
    if (running == event_) {
        return;
    }
    event_ = running;
    for (auto& [bsa, e] : by_bsa_) {
        if (e.role == Role::copy && e.wanted) {
            if (running && steps_ >= e.react_until) {
                MakeLate(e);
            }
            if (!running) {
                e.react_until = std::max(e.react_until, steps_ + kAfterEventSteps);
            }
            Note(e.name + (running ? ": our event: the copy runs its own (the owner's wait)"
                                   : ": our event is over (its own still runs for a moment)"));
        } else if (e.role == Role::owner && !running) {
            e.record_after_event = true;  // its brain verb now goes out as a change (Step)
        }
    }
}

void Table::NoteOwnHit(std::uint64_t actor, bool by_shot) {
    std::lock_guard lock(mutex_);
    Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::copy || !e->wanted) {
        return;
    }
    if (!OwnTime(*e)) {
        MakeLate(*e);
        if (by_shot) {
            ++counts_.copy_shot_hits;
            Note(e->name + ": our own shot ended at it: the copy runs its own reaction");
        } else {
            Note(e->name + ": our own hit: the copy runs its own reaction");
        }
    }
    e->react_until = std::max(e->react_until, steps_ + kReactSteps);
}

void Table::NoteOwnStun(std::uint64_t actor) {
    std::lock_guard lock(mutex_);
    Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::copy || !e->wanted) {
        return;
    }
    if (!OwnTime(*e)) {
        MakeLate(*e);
        ++counts_.copy_own_stuns;
        Note(e->name + ": its own stun in our game (the owner's is not stunned): the copy runs its own");
    }
    e->react_until = std::max(e->react_until, steps_ + kReactSteps);
}

void Table::NoteOwnDamage(std::uint64_t actor) {
    std::lock_guard lock(mutex_);
    Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::copy || !e->wanted) {
        return;
    }
    if (counts_.copy_hits_to_owner++ == 0) {
        Note(e->name + ": our own hit took life: its owner's game reacts (the copy keeps the owner's verbs)");
    }
}

void Table::NoteOwnShotEnd(const coop::protocol::Vec3& at) {
    if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) {
        return;
    }
    std::lock_guard lock(mutex_);
    shot_ends_.push_back({at, steps_});
    while (shot_ends_.size() > kMaxShotEnds ||
           (!shot_ends_.empty() && steps_ - shot_ends_.front().step > kShotEndKeepSteps)) {
        shot_ends_.pop_front();
    }
}

bool Table::OwnShotNear(const coop::protocol::Vec3& at) const {
    std::lock_guard lock(mutex_);
    for (const ShotEnd& end : shot_ends_) {
        if (steps_ - end.step > kShotEndKeepSteps) {
            continue;
        }
        const float dx = end.at[0] - at[0];
        const float dy = end.at[1] - at[1];
        const float dz = end.at[2] - at[2];
        if (dx * dx + dy * dy + dz * dz <= kShotNearBoss * kShotNearBoss) {
            return true;
        }
    }
    return false;
}

void Table::NoteOwnWake(std::uint64_t actor) {
    std::lock_guard lock(mutex_);
    Entry* e = FindActor(actor);
    if (e == nullptr || !e->plain || e->role != Role::copy || !e->wanted) {
        return;
    }
    if (!OwnTime(*e)) {
        MakeLate(*e);
        ++counts_.plain_wakes;
        Note(e->name + ": its own wake for our Link: the copy's own brain runs for a moment");
    }
    e->react_until = std::max(e->react_until, steps_ + kWakeSteps);
}

bool Table::RunsOwnMoveWhenOurs(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    if (!brain_on_) {
        return false;
    }
    const Entry* e = FindActor(actor);
    return e != nullptr && !e->plain;
}

bool Table::KeepsLinkAsAttacker(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const Entry* e = FindActor(actor);
    // Every boss keeps our Link (row 178: a puppet in its memory held its attacks; with boss turns every boss
    // fights only the Player of the game it runs in, and the server's turn aggro counts every player's hits).
    return e != nullptr && !e->plain;
}

void Table::SetTurns(bool on) {
    std::lock_guard lock(mutex_);
    turns_ = on;
}

bool Table::TurnsOn() const {
    std::lock_guard lock(mutex_);
    return turns_;
}

bool Table::OutOfReach(std::uint64_t actor, const HitGuard& guard) const {
    std::lock_guard lock(mutex_);
    const Entry* e = FindActor(actor);
    return e != nullptr && !e->plain && !e->dying && e->role == Role::owner && guard.TakesNoHit();
}

std::optional<std::string> Table::OurBossMove(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const Entry* e = FindActor(actor);
    if (e == nullptr || e->plain || e->role != Role::owner) {
        return std::nullopt;
    }
    return e->hold_animation;
}

bool Table::FightsPlayerOnly(std::string_view name) {
    // Their brains run ExecutePhantomGanonMetaAI / ExecuteDungeonBossGanondorfMetaAI (Phantom Ganon runs both;
    // Ganondorf's fly module and moves are his too). Checked against every boss's AI files: the other bosses
    // pick their moves in their AI files only.
    return name == "Enemy_DungeonBoss_PhantomGanon" || name == "Enemy_DungeonBoss_Ganondorf";
}

std::optional<std::string> Table::PlayerOnlyBoss() const {
    std::lock_guard lock(mutex_);
    for (const auto& [bsa, e] : by_bsa_) {
        // Boss turns (review-charsel-0118 M1): with turns on EVERY boss fights the Player only. A boss whose own
        // perception picked a puppet never attacks (the attack permission is the Player's), so it would never reach
        // an attack end and never take a turn. Puppets unseen, each game's boss fights that game's own Link; the
        // turns hand it to each player in turn.
        if (!e.plain && !e.dying && (turns_ || FightsPlayerOnly(e.name))) {
            return e.name;
        }
    }
    return std::nullopt;
}

void Table::Fall(Entry& e, const Boss& b) {
    if (!b.position) {
        return;
    }
    const float y = (*b.position)[1];
    if (!e.start_y) {
        e.start_y = y;  // (its first sighting: the fight's start)
        e.start = *b.position;
        return;
    }
    const float below = *e.start_y - y;
    if (below <= kFallBelowStart) {
        e.fallen = false;  // (back in its room: a later fall counts again)
        return;
    }
    if (b.role != Role::owner || e.dying || event_ || std::exchange(e.fallen, true)) {
        return;
    }
    ++counts_.owner_fell;
    if (e.fall_rechoices >= kMaxFallRechoices) {
        Note(b.name + ": fell " + std::to_string(static_cast<int>(below)) + " m below its fight start again: left");
        return;
    }
    ++e.fall_rechoices;
    e.rechoose = true;
    if (e.start && returns_.size() < kMaxWants) {
        returns_.emplace_back(e.actor, *e.start);
    }
    Note(b.name + ": fell " + std::to_string(static_cast<int>(below)) +
         " m below its fight start (row 178): put back at its fight start, its AI chooses again");
}

bool Table::TestMove(std::string_view name, std::int32_t verb) {
    std::lock_guard lock(mutex_);
    for (auto& [bsa, e] : by_bsa_) {
        if (e.name != name || e.role != Role::owner || e.plain || e.dying || e.own_node == 0) {
            continue;
        }
        BossAction a;
        a.verb = verb;
        a.flags = static_cast<std::uint16_t>(addr::kReqStartOver);
        e.restart_ask = AskFor(e.own_asks, e.own_slot_asks, e.own_node, a).value_or(OwnAsk{e.own_node});
        e.restart_action = a;
        e.restart_variant = {};
        e.restart_variant_places = {};
        e.restart_variant_kind = {};
        return true;
    }
    return false;
}

bool Table::WakesOwnBrain(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    if (!brain_on_) {
        return false;
    }
    const Entry* e = FindActor(actor);
    return e != nullptr && e->plain && e->role == Role::copy && e->wanted.has_value();
}

std::vector<std::uint64_t> Table::TakeOwnWants() {
    std::lock_guard lock(mutex_);
    return std::exchange(wants_, {});
}

std::vector<std::pair<std::uint64_t, coop::protocol::Vec3>> Table::TakeReturns() {
    std::lock_guard lock(mutex_);
    return std::exchange(returns_, {});
}

void Table::Record(Entry& e, std::variant<BossBrain, BossAction> what) {
    ++e.mine.seq;
    e.mine.recent.push_back(BossChange{e.mine.seq, std::move(what)});
    if (e.mine.recent.size() > coop::protocol::kMaxBossChanges) {
        e.mine.recent.erase(e.mine.recent.begin());
    }
    e.have_mine = true;
}

void Table::Resync(Entry& e, const BossBsa& state) {
    // The state now: the brain verb (skipped when the copy is so already,
    // see Step), then the last action asked. The game holds no "action now"
    // we could compare with, so the action is asked again (a joiner's copy, or
    // one whose own AI ran a move of its own, starts the owner's current move
    // over), except when it is the one we put in last and the copy's own AI
    // ran none since (row 149): its body runs that move already, and asking
    // it again started a long move (King Dodongo's roll) over after every
    // own time with no move of its own (a sword hit with no stun).
    e.due.clear();
    if (e.asking == Entry::Asking::due) {
        e.asking = Entry::Asking::none;  // (its asked change is gone: the call, if made, takes nothing out)
    }
    e.due.push_back({BossChange{state.seq, BossBrain{state.brain_verb, true}}, true});
    if (actions_on_ && state.action) {
        // Row 222 part 2: the same action in another variation (Twinrova's feint after an Ice: both by plug, no
        // places) is another move, asked again.
        std::optional<std::string> variant = ActionVariantOf(state, e.wanted_variants);
        const BossVariant* entry = ActionVariantEntry(state, e.wanted_variants);
        if (e.put_action && *e.put_action == *state.action && e.put_variant == variant && !e.own_action_ran) {
            ++counts_.moves_kept;
        } else {
            Entry::Due d{BossChange{state.seq, *state.action}, true};
            d.variant = std::move(variant);
            if (entry != nullptr && d.variant && *d.variant == entry->name) {
                d.variant_places = entry->places;
            }
            if (entry != nullptr) {
                d.variant_kind = entry->kind;  // (row 222: the owner's places' kind)
            }
            e.due.push_back(std::move(d));
        }
    }
    e.queued_seq = state.seq;
}

bool Table::CopyMakes(std::string_view maker) {
    if (maker.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);  // (waits: see the header, "The wraps")
    if (!brain_on_) {
        return false;
    }
    for (const auto& [bsa, e] : by_bsa_) {
        if (e.name == maker && e.role == Role::copy && e.wanted) {
            ++counts_.minions_stopped;
            return true;
        }
    }
    return false;
}

std::vector<std::uint64_t> Table::CopiesRunningOwn() const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint64_t> out;
    if (!brain_on_) {
        return out;
    }
    for (const auto& [bsa, e] : by_bsa_) {
        if (e.role == Role::copy && OwnTime(e)) {
            out.push_back(e.actor);
        }
    }
    return out;
}

std::optional<BossBsa> Table::OwnerState(std::uint64_t actor) const {
    return OwnerState(actor, nullptr);
}

std::optional<BossBsa> Table::OwnerState(std::uint64_t actor, std::optional<BossVariants>* variants) const {
    std::lock_guard lock(mutex_);
    if (variants != nullptr) {
        variants->reset();
    }
    const Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::owner || !e->have_mine) {
        return std::nullopt;
    }
    if (variants != nullptr) {
        // The names for its action changes still in recent, and (when older than recent) its action now's.
        const std::uint32_t front = e->mine.recent.empty() ? e->mine.seq + 1 : e->mine.recent.front().seq;
        BossVariants v;
        if (e->mine_action_variant && e->mine_action_variant->seq < front) {
            v.recent.push_back(*e->mine_action_variant);
        }
        for (const BossVariant& x : e->mine_variants) {
            if (x.seq >= front && x.seq <= e->mine.seq && (v.recent.empty() || x.seq > v.recent.back().seq)) {
                v.recent.push_back(x);
            }
        }
        if (v.recent.size() > coop::protocol::kMaxBossVariants) {
            v.recent.erase(v.recent.begin(),
                           v.recent.end() - static_cast<std::ptrdiff_t>(coop::protocol::kMaxBossVariants));
        }
        if (!v.recent.empty()) {
            *variants = std::move(v);
        }
    }
    return e->mine;
}

void Table::Receive(std::uint64_t actor, const BossBsa& state, const std::optional<BossVariants>& variants) {
    std::lock_guard lock(mutex_);
    if (!brain_on_) {
        return;
    }
    Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::copy) {
        return;
    }
    bool first = !e->wanted;
    if (!first && state.seq <= e->wanted->seq) {
        // The same state again (every sample carries it), or an older sample
        // after a newer one. One owner's seq only rises: a new owner is seen
        // in Publish, which makes the next state a first one.
        return;
    }
    if (first && e->handover) {
        // Twinrova hand-over: the new owner's first state goes on from the chain we held: its changes after our
        // newest are made in order (the old owner's still queued here stay); else a resync as before.
        if (ContinuesChain(*e->handover, state)) {
            first = false;
            e->queued_seq = e->handover->seq;
            Note(e->name + ": the new owner goes on from seq " + std::to_string(e->handover->seq) +
                 ": its changes after it are made in order");
        }
        e->handover.reset();
    }
    e->wanted = state;
    e->wanted_variants = variants;
    // The changes after the newest one queued here, in the owner's order.
    // All of them are held when the oldest held is at most the one after ours.
    const auto next = std::find_if(state.recent.begin(), state.recent.end(),
                                   [&](const BossChange& c) { return c.seq > e->queued_seq; });
    const bool joined = !state.recent.empty() && state.recent.front().seq <= e->queued_seq + 1;
    if (first || !joined) {
        // First state (of this owner), or more missed than a sample holds:
        // from the state now.
        if (!first) {
            ++counts_.resyncs;
            Note(e->name + ": started again from the owner's state (seq " + std::to_string(state.seq) + ")");
        }
        Resync(*e, state);
    } else {
        for (auto it = next; it != state.recent.end(); ++it) {
            if (!actions_on_ && std::holds_alternative<BossAction>(it->what)) {
                continue;
            }
            Entry::Due d{*it, false};
            if (std::holds_alternative<BossAction>(it->what)) {
                d.variant = VariantOf(variants, it->seq);  // row 222 part 2: the owner's variation of this move
                if (variants) {
                    for (const BossVariant& x : variants->recent) {
                        if (x.seq == it->seq) {
                            d.variant_places = x.places;  // row 222: a named ask's places (it aims as the owner's)
                            d.variant_kind = x.kind;      // row 222: and its places' kind (KindFor)
                        }
                    }
                }
            }
            e->due.push_back(std::move(d));
        }
        e->queued_seq = state.seq;
    }
    e->drift = 0;
    e->drift_fixes = 0;
    if (first) {
        Note(e->name + ": the owner's verbs came (brain " + std::to_string(state.brain_verb) +
             "): the copy's own brain is skipped from now");
    }
}

bool Table::StepOne(CoopPuppet::Guest& guest, std::uint64_t bsa, Entry& e, std::uint64_t main_base,
                    std::optional<CoopPuppet::Call>& call) {
    if (e.role == Role::owner) {
        // What we send as the brain now is what our game's boss is in
        // (also right after we took the boss over from a copy).
        // Not during our event: its verbs are the cutscene's; once it is
        // over the verb it left goes out as a change.
        std::int32_t own = 0;
        if (!event_ && guest.Read(bsa + addr::kBsaBrainVerb, &own, sizeof own)) {
            e.mine.brain_verb = own;
            if (std::exchange(e.record_after_event, false)) {
                Record(e, BossBrain{own, true});
                ++counts_.owner_brain;
            }
            if (e.restart_action && !e.dying && !e.plain) {
                // Row 178: the move it was in as a copy, started over as ours (Publish). Our caller's call: the
                // wrap keeps it as any ask of ours (kReqStartOver with it), so every copy starts it over too.
                if (block_ == 0) {
                    if (allocating_) {
                        call.reset();
                        return true;
                    }
                    CoopPuppet::Call c;
                    c.function = main_base + addr::kMalloc;
                    c.x[0] = addr::kBlockSize;
                    allocating_ = true;
                    pending_ = Pending::malloc;
                    call = std::move(c);
                    return true;
                }
                OwnAsk ask = e.restart_ask;
                if (WithVariant(guest, e.restart_variant, ask)) {  // row 222 part 2: the variation it ran as a copy
                    call = PoolCall(main_base);  // (a new name pool first; asked again after it)
                    return true;
                }
                // Row 222 (review-01112-h): a named ask restarts with its places, as it was put in (StepOne).
                BossAction again = *e.restart_action;
                if (ask.named &&
                    std::none_of(again.places.begin(), again.places.end(), [](const auto& p) { return p.has_value(); })) {
                    again.places = e.restart_variant_places;
                }
                const Request r =
                    RequestFromAction(again, ask, KindFor(e.restart_variant_kind, e.own_asks, again));
                if (guest.Write(block_, r.data(), r.size())) {
                    CoopPuppet::Call c;
                    c.function = main_base + addr::kRequestActionVerb;
                    c.x[0] = bsa;
                    c.x[1] = block_;
                    c.x[2] = 1;
                    pending_ = Pending::restart;
                    pending_bsa_ = bsa;
                    call = std::move(c);
                    return true;
                }
                e.restart_action.reset();  // (a block it cannot write: its AI chooses again instead)
                e.rechoose = true;
            }
            if (e.rechoose && !e.dying && !e.plain) {
                // Row 149: the brain verb it is in, set again (force) through the game's own function. Our
                // caller's call: the wrap keeps it as any change of ours, so every copy does the same.
                CoopPuppet::Call c;
                c.function = main_base + addr::kSetBrainVerb;
                c.x[0] = bsa;
                c.x[1] = static_cast<std::uint32_t>(own);
                c.x[2] = 1;
                c.x[3] = 0;  // (the asking node: not read by the function)
                pending_ = Pending::rechoose;
                pending_bsa_ = bsa;
                call = std::move(c);
                return true;
            }
        }
        return false;
    }
    if (!e.wanted) {
        return false;
    }
    if (OwnTime(e)) {
        // Our event or our hit: the copy's own changes run; the ones it
        // made a moment before we knew are made now, late.
        e.held = true;
        e.drift = 0;
        if (!e.own_late.empty()) {
            bool made = false;
            auto c = Ask(guest, e, bsa, e.own_late.front(), main_base, &made);
            if (made && !c) {
                e.own_late.pop_front();  // (nothing to call: dropped)
            } else if (made) {
                e.asking = Entry::Asking::late;  // out of own_late once the call is made
                pending_ = Pending::late;
                pending_bsa_ = bsa;
            }
            if (c) {
                call = std::move(c);
                return true;
            }
        }
        return false;
    }
    if (std::exchange(e.held, false)) {
        // Its own time is over: from the owner's state now.
        e.own_late.clear();
        if (e.asking == Entry::Asking::late) {
            e.asking = Entry::Asking::none;
        }
        Resync(e, *e.wanted);
        Note(e.name + ": the copy takes the owner's state again");
    }
    std::int32_t now = 0;
    const bool read = guest.Read(bsa + addr::kBsaBrainVerb, &now, sizeof now);
    while (!e.due.empty()) {
        Entry::Due& d = e.due.front();
        if (const auto* brain = std::get_if<BossBrain>(&d.change.what)) {
            if (!std::exchange(d.counted, true)) {
                ++counts_.brain_came;
            }
            if (d.resync && read && now == brain->verb) {
                ++counts_.brain_already;
                e.due.pop_front();  // already so
                continue;
            }
            CoopPuppet::Call c;
            c.function = main_base + addr::kSetBrainVerb;
            c.x[0] = bsa;
            c.x[1] = static_cast<std::uint32_t>(brain->verb);
            c.x[2] = 1;  // force: the change is the owner's, already decided
            c.x[3] = 0;  // (the asking node: not read by the function)
            c.copy_game = true;
            e.asking = Entry::Asking::due;  // out of due once the call is made
            pending_ = Pending::brain;
            pending_bsa_ = bsa;
            call = std::move(c);
            return true;
        }
        const BossAction& action = std::get<BossAction>(d.change.what);
        if (e.busy_step == steps_) {
            break;  // row 211: its slot is busy this frame: asked again next frame
        }
        if (!action.changed) {
            // Row 222: its own AI runs this move now (asked it within kOwnMoveSteps): the owner's repeat is left out.
            const auto own = e.own_moves.find(action.slot);
            if (own != e.own_moves.end() && own->second.first == action.verb &&
                steps_ - own->second.second <= kOwnMoveSteps) {
                ++counts_.owner_move_left;
                e.due.pop_front();
                continue;
            }
        }
        if (e.own_node == 0) {
            ++counts_.action_waiting;  // (asked again next frame; the changes after it wait too)
            break;
        }
        if (block_ == 0) {
            if (allocating_) {
                call.reset();  // (the block is being asked: nothing more this frame)
                return true;
            }
            CoopPuppet::Call c;
            c.function = main_base + addr::kMalloc;
            c.x[0] = addr::kBlockSize;
            allocating_ = true;
            pending_ = Pending::malloc;
            call = std::move(c);
            return true;
        }
        // Row 222: through a node whose own choice is not another variation of this move (OwnVariants).
        AskRule rule = AskRule::last;
        const std::optional<OwnAsk> asked =
            AskFor(e.own_asks, e.own_slot_asks, e.own_node, action, e.own_variants, d.variant, &rule);
        OwnAsk ask = asked.value_or(OwnAsk{e.own_node});
        const bool moved = d.variant && asked && e.own_asks.contains({action.slot, action.verb}) &&
                           rule != AskRule::own && rule != AskRule::all_conflict;
        // Row 222 part 2: the owner's variation of this move (its plug name, or plain), not our own AI's last.
        if (WithVariant(guest, d.variant, ask)) {
            call = PoolCall(main_base);  // (a new name pool first; this change is asked again after it)
            return true;
        }
        // Row 222: a named ask goes in with the owner's places (Twinrova's 'Ice' aims at the Link the owner's did).
        BossAction put = action;
        if (ask.named && std::none_of(put.places.begin(), put.places.end(), [](const auto& p) { return p.has_value(); })) {
            put.places = d.variant_places;
        }
        const Request r = RequestFromAction(put, ask, KindFor(d.variant_kind, e.own_asks, action));
        if (!guest.Write(block_, r.data(), r.size())) {
            break;
        }
        if (action.changed && moved) {
            ++counts_.variant_node_moved;
        }
        if (action.changed && rule == AskRule::all_conflict) {
            ++counts_.variant_node_conflict;
        }
        Stream(e, "owner's put in: slot " + std::to_string(action.slot) + " verb " + std::to_string(action.verb) +
                      " (changed " + std::to_string(action.changed ? 1 : 0) + ", node " + Hex(ask.node) + ": " +
                      std::string(AskRuleName(rule)) +
                      (!d.variant           ? std::string(r[addr::kReqHasPlug] == 1 ? ", our plug)" : ")")
                       : d.variant->empty() ? std::string(", the owner's: plain)")
                                            : ", the owner's: '" + *d.variant + "')"));
        CoopPuppet::Call c;
        c.function = main_base + addr::kRequestActionVerb;
        c.x[0] = bsa;
        c.x[1] = block_;
        c.x[2] = action.changed ? 1 : 0;
        c.copy_game = true;
        e.asking = Entry::Asking::due;  // out of due once the call is made
        pending_ = Pending::action;
        pending_bsa_ = bsa;
        call = std::move(c);
        return true;
    }
    if (!e.due.empty() || !read || now == e.wanted->brain_verb) {
        e.drift = 0;
        return false;
    }
    // Nothing queued and the copy's phase is not the owner's: put back
    // after kDriftSteps (a change of its own that the game made where no
    // wrap sees it, or one of ours it refused).
    if (e.drift_fixes >= kMaxDriftFixes) {
        return false;  // the game keeps refusing it: left alone until the owner's next state
    }
    if (++e.drift >= kDriftSteps) {
        e.drift = 0;
        if (++e.drift_fixes == kMaxDriftFixes) {
            Note(e.name + ": brain verb " + std::to_string(e.wanted->brain_verb) +
                 " put back " + std::to_string(kMaxDriftFixes) + " times, refused: left until the owner's next state");
        }
        ++counts_.drift_fixes;
        e.due.push_back({BossChange{e.wanted->seq, BossBrain{e.wanted->brain_verb, true}}, true});
    }
    return false;
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::unique_lock lock(mutex_);
    if (!after_call) {
        ++steps_;  // once a frame (coop_callers.h)
    }
    if (!after_call && pending_ != Pending::none) {
        // Last frame's call was not made (the runtime could not, or the
        // frame's calls ran out): its change stays queued and is asked again.
        if (pending_ == Pending::malloc || pending_ == Pending::pool) {
            allocating_ = false;
        } else if (pending_ == Pending::rechoose || pending_ == Pending::restart) {
            pending_bsa_ = 0;  // (still asked: the next frame calls it again)
        } else {
            Made(false);
        }
        pending_ = Pending::none;
    }
    if (after_call) {
        switch (std::exchange(pending_, Pending::none)) {
        case Pending::malloc:
            allocating_ = false;
            if (result != 0) {
                block_ = result;
                pool_.clear();  // (row 222 part 2: a new block: no names, its first pool after the request)
                pools_.assign(1, result + addr::kRequestSize);
                Note("native boss sync: request block at " + Hex(result));
            }
            break;
        case Pending::pool:
            allocating_ = false;
            if (result != 0 && block_ != 0) {
                pools_.push_back(result);  // (row 222 part 2: room for kVariantSlots more names)
                Note("native boss sync: variation name pool " + std::to_string(pools_.size()) + " at " + Hex(result));
            }
            break;
        case Pending::brain:
            ++counts_.brain_applied;
            Made(true);
            break;
        case Pending::action: {
            NoteAnswer("the owner's move", result, true);
            Entry* e = Find(pending_bsa_);
            if (e != nullptr && result == addr::kActionSlotBusy && e->taken_step != steps_ &&
                e->busy_asks < kBusyAsks) {
                // Row 211: the copy's own request took the slot this frame, not one of ours: ours next frame.
                ++e->busy_asks;
                e->busy_step = steps_;
                ++counts_.action_busy_again;
                Made(false);
                break;
            }
            if (e != nullptr) {
                e->busy_asks = 0;
                if (result == addr::kActionTaken) {
                    e->taken_step = steps_;
                }
            }
            ++counts_.action_applied;
            Made(true);
            break;
        }
        case Pending::late: {
            ++counts_.copy_own_late;
            const Entry* e = Find(pending_bsa_);
            NoteAnswer("its own move made late", result,
                       e != nullptr && !e->own_late.empty() && std::holds_alternative<BossAction>(e->own_late.front()));
            Made(true);
            break;
        }
        case Pending::rechoose:
            if (Entry* e = Find(std::exchange(pending_bsa_, 0)); e != nullptr && e->role == Role::owner) {
                e->rechoose = false;
                ++counts_.owner_rechosen;
            }
            break;
        case Pending::restart:
            NoteAnswer("its move started over as ours", result, true);
            if (Entry* e = Find(std::exchange(pending_bsa_, 0)); e != nullptr && e->role == Role::owner) {
                e->restart_action.reset();
                ++counts_.owner_restarted;
            }
            break;
        case Pending::none:
            break;
        }
    }
    if (main_base != main_base_) {
        // A new game (or none): our block was its heap's (as the spawn director's).
        main_base_ = main_base;
        block_ = 0;
        pool_.clear();
        pools_.clear();
        allocating_ = false;
    }
    if (!brain_on_ || main_base == 0) {
        return std::nullopt;
    }
    // From the copy after the one served last, round the table once.
    std::optional<CoopPuppet::Call> call;
    const auto first = by_bsa_.upper_bound(served_);
    for (auto it = first; it != by_bsa_.end(); ++it) {
        if (StepOne(guest, it->first, it->second, main_base, call)) {
            if (call && pending_ != Pending::malloc && pending_ != Pending::pool) {
                served_ = it->first;  // (the block's or a pool's allocation is nobody's turn)
            }
            return call;
        }
    }
    for (auto it = by_bsa_.begin(); it != first; ++it) {
        if (StepOne(guest, it->first, it->second, main_base, call)) {
            if (call && pending_ != Pending::malloc && pending_ != Pending::pool) {
                served_ = it->first;  // (the block's or a pool's allocation is nobody's turn)
            }
            return call;
        }
    }
    return std::nullopt;
}

void Table::NoteAnswer(const char* what, std::uint64_t result, bool is_action) {
    // (the lock is held; pending_bsa_ still names the boss) Only the action request has an answer (the brain
    // verb's x0 is not one).
    if (!is_action || result == addr::kActionTaken) {
        return;
    }
    const bool busy = result == addr::kActionSlotBusy;
    std::uint64_t& n = busy ? counts_.action_slot_busy : counts_.action_refused;
    if (++n <= 8) {
        const Entry* e = Find(pending_bsa_);
        Note((e != nullptr ? e->name : std::string("a boss")) + ": " + what + " was not taken by the game (" +
             std::to_string(result) + (busy ? ": its slot had taken a request this frame)" : ")"));
    }
}

void Table::Made(bool made) {
    Entry* e = Find(std::exchange(pending_bsa_, 0));
    if (e == nullptr) {
        return;  // (gone from the walk meanwhile)
    }
    const Entry::Asking asking = std::exchange(e->asking, Entry::Asking::none);
    if (!made) {
        return;  // the change is still at the front of its queue
    }
    const bool brain = (asking == Entry::Asking::due && !e->due.empty() &&
                        std::holds_alternative<BossBrain>(e->due.front().change.what)) ||
                       (asking == Entry::Asking::late && !e->own_late.empty() &&
                        std::holds_alternative<BossBrain>(e->own_late.front()));
    if (brain) {
        // Row 177 review: a new brain verb may change the copy's action sequence, whose AI nodes go with it; the
        // nodes kept for its moves are learnt again from its next own asks (the game reads a named node's name).
        ForgetOwnAsks(*e);
        if (asking == Entry::Asking::due) {
            e->asks_brain = std::get<BossBrain>(e->due.front().change.what).verb;  // (BrainRuns: learnt in it)
        } else {
            e->asks_brain.reset();
        }
    }
    if (asking == Entry::Asking::due && !e->due.empty()) {
        if (const auto* action = std::get_if<BossAction>(&e->due.front().change.what)) {
            e->put_action = *action;  // (the move our copy's body runs now: Resync)
            e->put_variant = e->due.front().variant;  // (row 222 part 2: with its variation)
            e->put_variant_places = e->due.front().variant_places;  // (row 222: and a named ask's places)
            e->put_variant_kind = e->due.front().variant_kind;      // (row 222: and the places' kind)
            e->own_action_ran = false;
        }
        e->due.pop_front();
    } else if (asking == Entry::Asking::late && !e->own_late.empty()) {
        if (std::holds_alternative<BossAction>(e->own_late.front())) {
            e->own_action_ran = true;  // (a move of its own, made late)
        }
        e->own_late.pop_front();
    }
}

bool Table::WithVariant(CoopPuppet::Guest& guest, const std::optional<std::string>& variant, OwnAsk& ask) {
    if (!variant) {
        ++counts_.variant_unknown;  // an older owner, or a name it could not read: its own AI's (AskFor)
        return false;
    }
    if (variant->empty()) {
        ask = OwnAsk{ask.node};  // the owner asked it plain: no plug (its own AI's last 'Ice' would run Ice)
        ++counts_.variant_plain;
        return false;
    }
    if (block_ == 0 || pools_.empty() || variant->size() >= addr::kVariantSlotSize) {
        ++counts_.variant_pool_full;  // (no block yet, or a name too long: ValidVariantName refuses those)
        return false;
    }
    const auto slot = static_cast<std::size_t>(std::find(pool_.begin(), pool_.end(), *variant) - pool_.begin());
    if (slot == pool_.size()) {
        if (slot >= pools_.size() * addr::kVariantSlots) {
            // Every pool full: another one from the game's heap first (the game's names are a bounded set; each
            // pool lives as long as the game, as our block). Only past kMaxVariantPools: its own plug, as before.
            if (pools_.size() < addr::kMaxVariantPools) {
                return true;
            }
            ++counts_.variant_pool_full;
            if (counts_.variant_pool_full == 1) {
                Note("native boss sync: the variation name pools are full (" +
                     std::to_string(addr::kMaxVariantPools * addr::kVariantSlots) + " names): '" + *variant +
                     "' asked with the copy's own plug");
            }
            return false;
        }
        // Written once with its end, then never moved or written again while the pool lives.
        if (!guest.Write(SlotAt(slot), variant->c_str(), variant->size() + 1)) {
            ++counts_.variant_pool_full;
            return false;
        }
        pool_.push_back(*variant);
    }
    ask = OwnAsk{ask.node, SlotAt(slot), true, true};
    ++counts_.variant_named;
    return false;
}

std::uint64_t Table::SlotAt(std::size_t slot) const {
    return pools_[slot / addr::kVariantSlots] + (slot % addr::kVariantSlots) * addr::kVariantSlotSize;
}

std::optional<CoopPuppet::Call> Table::PoolCall(std::uint64_t main_base) {
    if (allocating_) {
        return std::nullopt;  // (an allocation in flight: nothing more this frame)
    }
    CoopPuppet::Call c;
    c.function = main_base + addr::kMalloc;
    c.x[0] = addr::kPoolSize;
    allocating_ = true;
    pending_ = Pending::pool;
    return c;
}

std::optional<CoopPuppet::Call> Table::Ask(CoopPuppet::Guest& guest, Entry& e, std::uint64_t bsa, const std::variant<BossBrain, BossAction>& what,
                                           std::uint64_t main_base, bool* made) {
    // One change through the game's own function. made: the call is the
    // change (else nothing, or the request block's allocation first).
    *made = false;
    if (const auto* brain = std::get_if<BossBrain>(&what)) {
        CoopPuppet::Call c;
        c.function = main_base + addr::kSetBrainVerb;
        c.x[0] = bsa;
        c.x[1] = static_cast<std::uint32_t>(brain->verb);
        c.x[2] = brain->force ? 1 : 0;
        c.x[3] = 0;  // (the asking node: not read by the function)
        c.copy_game = true;
        *made = true;
        return c;
    }
    const BossAction& action = std::get<BossAction>(what);
    if (e.own_node == 0) {
        *made = true;  // (no node to ask with: dropped)
        return std::nullopt;
    }
    if (block_ == 0) {
        if (allocating_) {
            return std::nullopt;
        }
        CoopPuppet::Call c;
        c.function = main_base + addr::kMalloc;
        c.x[0] = addr::kBlockSize;
        allocating_ = true;
        pending_ = Pending::malloc;
        return c;
    }
    const Request r =
        RequestFromAction(action, AskFor(e.own_asks, e.own_slot_asks, e.own_node, action).value_or(OwnAsk{e.own_node}),
                          KindFor(std::nullopt, e.own_asks, action));  // (row 222: its own AI's places' kind)
    *made = true;  // (a block it cannot write: dropped)
    if (!guest.Write(block_, r.data(), r.size())) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    c.function = main_base + addr::kRequestActionVerb;
    c.x[0] = bsa;
    c.x[1] = block_;
    c.x[2] = action.changed ? 1 : 0;
    c.copy_game = true;
    return c;
}

Counts Table::Totals() const {
    std::lock_guard lock(mutex_);
    Counts c = counts_;
    c.copy_life_held = life_held_.load(std::memory_order_relaxed);
    c.copy_killed = life_killed_.load(std::memory_order_relaxed);
    c.copy_finished = life_finished_.load(std::memory_order_relaxed);
    return c;
}

std::vector<std::string> Table::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    by_bsa_.clear();
    wants_.clear();
    served_ = 0;
    counts_ = {};
    for (auto& slot : held_life_) {
        slot.store(0, std::memory_order_relaxed);
    }
    not_begun_.clear();
    dying_.clear();
    shot_ends_.clear();
    stream_notes_ = 0;
    for (auto& slot : dying_life_) {
        slot.store(0, std::memory_order_relaxed);
    }
    life_finished_.store(0, std::memory_order_relaxed);
    life_held_.store(0, std::memory_order_relaxed);
    life_killed_.store(0, std::memory_order_relaxed);
    notes_.clear();
    block_ = 0;  // (asked again)
    pool_.clear();
    pools_.clear();
    allocating_ = false;
    pending_ = Pending::none;
}

Table& TheTable() {
    static Table table;
    return table;
}

} // namespace CoopBossNative
