// SPDX-License-Identifier: GPL-3.0-or-later
//
// A boss's move state goes with the boss (row 149): coop_brain_state.h.
#include "coop_brain_state.h"

#include "coop_move_probe.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace CoopBrainState {

namespace {

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%#llx", static_cast<unsigned long long>(v));
    return text;
}

const BrainValue* ValueOf(const BrainState& board, std::uint32_t id) {
    for (const BrainValue& v : board.values) {
        if (v.key == id) {
            return &v;
        }
    }
    return nullptr;
}

std::string Text(std::span<const Key> keys, const std::vector<BrainValue>& values) {
    std::string out;
    for (const BrainValue& v : values) {
        for (const Key& k : keys) {
            if (k.id == v.key) {
                out += (out.empty() ? "" : ", ") + std::string(k.name) + "=" + std::to_string(v.value);
            }
        }
    }
    return out;
}

}  // namespace

coop::protocol::KeyedValues ToKeyed(const BrainState& state) {
    coop::protocol::KeyedValues out;
    out.full = true;
    for (const BrainValue& v : state.values) {
        out.values.push_back(coop::protocol::KeyedValue{0, v.key, v.value});
    }
    // (the wire's order: owner, then name; ours are by rising key already, sorted again for a caller's own list)
    std::sort(out.values.begin(), out.values.end(),
              [](const coop::protocol::KeyedValue& a, const coop::protocol::KeyedValue& b) { return a.name < b.name; });
    return out;
}

BrainState FromKeyed(const coop::protocol::KeyedValues& keyed) {
    BrainState out;
    for (const coop::protocol::KeyedValue& v : keyed.values) {
        if (v.owner != 0) {
            continue;  // (a part's)
        }
        if (const std::int32_t* value = std::get_if<std::int32_t>(&v.value)) {
            out.values.push_back({v.name, *value});
        }
    }
    std::sort(out.values.begin(), out.values.end(),
              [](const BrainValue& a, const BrainValue& b) { return a.key < b.key; });
    return out;
}

std::vector<Key> KeysOf(std::string_view boss) {
    std::vector<Key> out;
    for (const Key& k : kKeys) {
        if (k.boss == boss) {
            out.push_back(k);
        }
    }
    out.push_back(kCctMoveKey);  // row 287: every actor's
    std::sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.id < b.id; });
    return out;
}

std::vector<BrainValue> Plan(std::span<const Key> keys, const BrainState& theirs, const BrainState& ours) {
    std::vector<BrainValue> out;
    for (const Key& k : keys) {
        const BrainValue* want = ValueOf(theirs, k.id);
        const BrainValue* have = ValueOf(ours, k.id);
        if (want == nullptr || have == nullptr) {
            continue;  // (not sent by that owner's build, or not read here)
        }
        if (k.kind == Kind::cct_move) {
            // Row 287: only a state the game has a slot for, and only when our body is in another one.
            if (have->value >= 0 && CoopMoveState::AtMadeOurs(static_cast<std::uint32_t>(have->value),
                                                              static_cast<std::uint32_t>(want->value))) {
                out.push_back({k.id, want->value});
            }
            continue;
        }
        const std::int32_t value = k.kind == Kind::boolean ? (want->value != 0 ? 1 : 0) : want->value;
        if (value != have->value) {
            out.push_back({k.id, value});
        }
    }
    return out;
}

void Table::Note(std::string text) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(text));
    }
}

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        auto it = entries_.find(m.actor);
        const bool seen = it != entries_.end();
        Entry e = seen ? std::move(it->second) : Entry{};
        if (!seen) {
            for (const Key& k : KeysOf(m.name)) {
                e.slots.push_back(Slot{k, 0, 0});
            }
        } else if (e.member.role != m.role) {
            e.todo.clear();
            e.planned = false;
            e.mine.reset();
            // Made ours: the old owner's last values go in before our brain decides (Step). Made a copy: the new
            // owner's come.
            e.due = m.role == Role::owner && !e.in.values.empty();
            if (m.role == Role::owner) {
                Note(m.name + ": blackboard: ours now: " +
                     (e.due ? "player " + std::to_string(e.member.owner) + "'s move state is put in first"
                            : std::string("no move state came from its old owner: our own stays")));
            }
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

void Table::Receive(std::uint64_t actor, const BrainState& board) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || board == it->second.in) {
        return;
    }
    Entry& e = it->second;
    e.in = board;
    if (e.member.role == Role::owner && !board.values.empty()) {
        // The hand-over state came after the news: put in as well.
        Note(e.member.name + ": blackboard: its old owner's move state came after it was ours: put in now");
        e.due = true;
        e.planned = false;
        e.todo.clear();
    }
}

std::optional<BrainState> Table::OwnerBoard(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.member.role != Role::owner) {
        return std::nullopt;
    }
    const Entry& e = it->second;
    if (!e.due) {
        return e.mine;
    }
    // Still to put in: the old owner's, as they came (our keys only), never our stale own.
    BrainState out;
    for (const Slot& s : e.slots) {
        if (const BrainValue* v = ValueOf(e.in, s.key.id)) {
            out.values.push_back(*v);
        }
    }
    return out;
}

std::optional<std::int32_t> Table::MoveOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    const auto move = CoopMoveProbe::Read(
        [&guest](std::uint64_t at) -> std::optional<std::uint64_t> {
            std::uint64_t word = 0;
            return guest.Read(at, &word, sizeof word) ? std::optional<std::uint64_t>(word) : std::nullopt;
        },
        actor);
    if (!move || move->current > 0x7fffffffu) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(move->current);
}

bool Table::Find(CoopPuppet::Guest& guest, Entry& e) {
    const bool board = FindBoard(guest, e);
    return MoveOf(guest, e.member.actor).has_value() || board;
}

bool Table::FindBoard(CoopPuppet::Guest& guest, Entry& e) {
    std::uint64_t components = 0;
    std::uint64_t holder = 0;
    std::uint64_t blackboard = 0;
    if (!guest.Read(e.member.actor + addr::kActorComponents, &components, sizeof components) || components == 0 ||
        !guest.Read(components, &holder, sizeof holder) || holder == 0 ||
        !guest.Read(holder + addr::kHolderBlackboard, &blackboard, sizeof blackboard) || blackboard == 0 ||
        blackboard == ~std::uint64_t{0}) {
        e.blackboard = 0;
        return false;
    }
    const auto found = [&e] {
        return std::any_of(e.slots.begin(), e.slots.end(), [](const Slot& s) { return s.name_at != 0; });
    };
    if (blackboard == e.blackboard) {
        return found();
    }
    e.blackboard = blackboard;
    for (Slot& s : e.slots) {
        s.name_at = 0;
    }
    for (const Kind kind : {Kind::s32, Kind::boolean}) {
        const bool is_s32 = kind == Kind::s32;
        std::int32_t names = 0;
        std::int32_t values = 0;
        std::uint64_t names_at = 0;
        if (!guest.Read(blackboard + (is_s32 ? addr::kS32NameCount : addr::kBoolNameCount), &names, sizeof names) ||
            !guest.Read(blackboard + (is_s32 ? addr::kS32ValueCount : addr::kBoolValueCount), &values,
                        sizeof values) ||
            !guest.Read(blackboard + (is_s32 ? addr::kS32Names : addr::kBoolNames), &names_at, sizeof names_at) ||
            names <= 0 || names > addr::kMaxNames || names_at == 0) {
            continue;
        }
        std::vector<std::uint64_t> at(static_cast<std::size_t>(names));
        if (!guest.Read(names_at, at.data(), at.size() * sizeof(std::uint64_t))) {
            continue;
        }
        for (Slot& s : e.slots) {
            if (s.key.kind != kind) {
                continue;
            }
            std::string text(s.key.name.size() + 1, '\1');
            for (std::size_t i = 0; i < at.size(); ++i) {
                // The name and its end, so a longer key that starts the same is not taken for it.
                if (at[i] != 0 && guest.Read(at[i], text.data(), text.size()) &&
                    std::memcmp(text.data(), s.key.name.data(), s.key.name.size()) == 0 && text.back() == '\0') {
                    if (static_cast<std::int64_t>(i) < values) {
                        s.name_at = names_at + i * sizeof(std::uint64_t);
                        s.index = static_cast<std::uint32_t>(i);
                    }
                    break;
                }
            }
        }
    }
    return found();
}

std::optional<BrainState> Table::ReadValues(CoopPuppet::Guest& guest, const Entry& e) {
    std::uint64_t s32_at = 0;
    std::uint64_t bool_at = 0;
    if (e.blackboard != 0 && (!guest.Read(e.blackboard + addr::kS32Values, &s32_at, sizeof s32_at) ||
                              !guest.Read(e.blackboard + addr::kBoolValues, &bool_at, sizeof bool_at))) {
        return std::nullopt;
    }
    BrainState out;
    for (const Slot& s : e.slots) {
        if (s.key.kind == Kind::cct_move) {
            if (const auto move = MoveOf(guest, e.member.actor)) {
                out.values.push_back({s.key.id, *move});  // row 287
            }
            continue;
        }
        if (s.name_at == 0) {
            continue;
        }
        if (s.key.kind == Kind::s32) {
            std::int32_t value = 0;
            if (s32_at != 0 && guest.Read(s32_at + s.index * sizeof value, &value, sizeof value)) {
                out.values.push_back({s.key.id, value});
            }
        } else {
            std::uint8_t value = 0;
            if (bool_at != 0 && guest.Read(bool_at + s.index, &value, sizeof value)) {
                out.values.push_back({s.key.id, value != 0 ? 1 : 0});
            }
        }
    }
    return out;
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        // A new game (or none): our block was its heap's and every blackboard its; a call in flight is not ours to
        // finish (as coop_boss_orders').
        if (main_base_ != 0) {
            Note("blackboard: a new game: block, bosses and call in flight dropped");
            DropGame();
        }
        main_base_ = main_base;
        after_call = false;
    }
    if (!after_call) {
        malloc_failed_ = false;
        if (pending_ == Pending::malloc) {
            allocating_ = false;  // last frame's call was not made: asked again
        }
        pending_ = Pending::none;  // (a value not put in stays first in its plan)
    } else {
        switch (std::exchange(pending_, Pending::none)) {
        case Pending::malloc:
            allocating_ = false;
            if (result != 0) {
                block_ = result;
                Note("blackboard: value block at " + Hex(result));
            } else {
                malloc_failed_ = true;
            }
            break;
        case Pending::set:
            if (const auto it = entries_.find(pending_actor_); it != entries_.end() && !it->second.todo.empty()) {
                Entry& e = it->second;
                const BrainValue asked = e.todo.front();
                e.todo.erase(e.todo.begin());
                const std::optional<BrainState> now = ReadValues(guest, e);
                const BrainValue* have = now ? ValueOf(*now, asked.key) : nullptr;
                if (asked.key == kCctMoveKey.id) {
                    // Row 287: the game's own result word, and the state its body is in now.
                    std::uint32_t word = 0;
                    const bool read = block_ != 0 && guest.Read(block_ + addr::kNodeAt + CoopMoveState::block::kResult,
                                                                 &word, sizeof word);
                    const bool in = have != nullptr && have->value == asked.value;
                    if (in) {
                        ++counts_.put;
                        ++counts_.moves;
                        ++e.put;
                    } else {
                        ++counts_.refused;
                    }
                    Note(e.member.name + ": CCT move state " + std::to_string(asked.value) + " asked of the game: " +
                         (in ? "in" : "NOT in") + " (result " + (read ? Hex(word) : std::string("not read")) +
                         (read && !CoopMoveState::Done(word) ? ", not done" : "") + ")");
                } else if (have != nullptr && have->value == asked.value) {
                    ++counts_.put;
                    ++e.put;
                    e.board_put = true;
                } else {
                    ++counts_.refused;  // (a locked key: the game's own rule; ours stays)
                    Note(e.member.name + ": blackboard: the game left key " + Hex(asked.key) + " alone");
                }
            }
            break;
        case Pending::none:
            break;
        }
    }
    if (main_base == 0 || entries_.empty()) {
        return std::nullopt;
    }
    if (probe_ && !after_call) {
        ProbeStep(guest);  // row 323 probe: reads only
    }
    if (block_ == 0 && !allocating_ && !malloc_failed_ &&
        std::any_of(entries_.begin(), entries_.end(), [&guest](const auto& listed) {
            // (an actor with a blackboard key of ours, or a CCT: the others carry nothing, so no block for them)
            return listed.second.slots.size() > 1 || MoveOf(guest, listed.first).has_value();
        })) {
        // Made while the boss is still a copy, so the values go in the frame it is made ours.
        CoopPuppet::Call c;
        c.function = main_base + addr::kMalloc;
        c.x[0] = addr::kBlockSize;
        allocating_ = true;
        pending_ = Pending::malloc;
        return c;
    }
    for (auto& [actor, e] : entries_) {
        if (e.member.role != Role::owner) {
            continue;
        }
        if (!Find(guest, e)) {
            if (!e.unread_said && e.slots.size() > 1) {  // (an actor with no blackboard key and no CCT: silent)
                e.unread_said = true;
                ++counts_.unread;
                Note(e.member.name + ": blackboard: not found, or none of its keys in it: nothing carried");
            }
            continue;  // (asked again next frame: its blackboard may not be made yet)
        }
        std::vector<Key> keys;
        for (const Slot& s : e.slots) {
            keys.push_back(s.key);
        }
        if (e.due) {
            const std::optional<BrainState> ours = ReadValues(guest, e);
            if (!ours) {
                continue;
            }
            if (!e.planned) {
                e.todo = Plan(keys, e.in, *ours);
                e.planned = true;
                e.put = 0;
                e.same = 0;
                e.board_put = false;
                for (const BrainValue& v : e.in.values) {
                    if (ValueOf(*ours, v.key) != nullptr) {
                        ++e.same;
                    }
                }
                e.same -= static_cast<std::uint32_t>(e.todo.size());
                ++counts_.handed;
                counts_.same += e.same;
                if (!e.todo.empty()) {
                    Note(e.member.name + ": blackboard: put in: " + Text(keys, e.todo));
                }
            }
            if (!e.todo.empty() && block_ == 0) {
                continue;  // (no block this frame: asked again; our stale values are not sent meanwhile)
            }
            if (!e.todo.empty()) {
                const BrainValue& v = e.todo.front();
                const auto slot = std::find_if(e.slots.begin(), e.slots.end(),
                                               [&v](const Slot& s) {
                                                   return s.key.id == v.key &&
                                                          (s.name_at != 0 || s.key.kind == Kind::cct_move);
                                               });
                if (slot == e.slots.end()) {
                    e.todo.erase(e.todo.begin());  // (its key is gone with a new blackboard)
                    return std::nullopt;
                }
                if (slot->key.kind == Kind::cct_move) {
                    // Row 287: the game's own change of the move state, with a node block of ours (behind the value).
                    const std::uint64_t node = block_ + addr::kNodeAt;
                    const auto bytes = CoopMoveState::Block(node, actor, static_cast<std::uint32_t>(v.value));
                    if (!bytes || !guest.Write(node, bytes->data(), bytes->size())) {
                        e.todo.erase(e.todo.begin());  // (never: the block is the game's own heap's)
                        return std::nullopt;
                    }
                    CoopPuppet::Call c;
                    c.function = main_base + CoopMoveState::kChangeExecute;
                    c.x[0] = node;
                    pending_ = Pending::set;
                    pending_actor_ = actor;
                    return c;
                }
                // The value: an s32, or a bool in its first byte.
                std::array<std::uint8_t, addr::kValueBytes> block{};
                std::memcpy(block.data(), &v.value, sizeof v.value);
                if (!guest.Write(block_, block.data(), block.size())) {
                    return std::nullopt;
                }
                CoopPuppet::Call c;
                c.function = main_base + (slot->key.kind == Kind::s32 ? addr::kSetS32 : addr::kSetBool);
                c.x[0] = e.blackboard;
                c.x[1] = slot->name_at;
                c.x[2] = block_;
                pending_ = Pending::set;
                pending_actor_ = actor;
                return c;
            }
            Note(e.member.name + ": blackboard: its old owner's move state is ours: " + std::to_string(e.put) +
                 " put in, " + std::to_string(e.same) + " the same");
            e.due = false;
            if (e.board_put) {  // (row 287: its body's move state alone changes no choice of its AI)
                changed_.push_back(actor);
            }
            e.planned = false;
        }
        e.mine = ReadValues(guest, e);
    }
    return std::nullopt;
}

void Table::DropGame() {
    entries_.clear();
    probes_.clear();
    block_ = 0;
    allocating_ = false;
    malloc_failed_ = false;
    pending_ = Pending::none;
    pending_actor_ = 0;
    changed_.clear();
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    DropGame();
    main_base_ = 0;
    counts_ = {};
    notes_.clear();
}

Counts Table::GetCounts() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Table::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

std::vector<std::uint64_t> Table::TakeChanged() {
    std::lock_guard lock(mutex_);
    return std::exchange(changed_, {});
}

void Table::SetProbe(bool on) {
    std::lock_guard lock(mutex_);
    probe_ = on;
}

// Row 323 probe. A blackboard group is (name count s32, names char**, value count s32, values): the s32 group at
// +0x98 and the bool group at +0x188 (addr above); the others are found here by the names they hold.
void Table::ProbeStep(CoopPuppet::Guest& guest) {
    static constexpr std::string_view kGohma = "Enemy_DungeonBoss_Gohma";
    static constexpr std::array<std::string_view, 9> kNames{
        "DungeonBossGohma_ContactWallPoint", "DungeonBossGohma_ContactWallNormal", "DungeonBossGohma_IsFinishClimb",
        "DungeonBossGohma_IsLeaveMove",      "DungeonBossGohma_IsStartSpawnEgg",   "DungeonBossGohma_IsEnabledGuard",
        "DungeonBossGohma_IsForceClimb",     "DungeonBossGohma_IsForceCeilingBattle",
        "DungeonBossGohma_IsClimbFromOutside"};
    static constexpr std::uint64_t kScanEnd = 0x300;
    const bool second = ++probe_frames_ % 60 == 0;
    for (auto it = probes_.begin(); it != probes_.end();) {
        it = entries_.count(it->first) == 0 ? probes_.erase(it) : std::next(it);
    }
    for (const auto& [actor, e] : entries_) {
        if (e.member.name != kGohma) {
            continue;
        }
        ProbeEntry& p = probes_[actor];
        const bool changed = !p.seen || p.role != e.member.role || p.owner != e.member.owner;
        if (!changed && !second) {
            continue;
        }
        if (changed) {
            Note("probe 323: Gohma " + Hex(actor) + " role " + (e.member.role == Role::owner ? "owner" : "copy") +
                 " owner " + std::to_string(e.member.owner) + (p.seen ? " (changed)" : " (first seen)"));
        }
        p.seen = true;
        p.role = e.member.role;
        p.owner = e.member.owner;
        std::uint64_t components = 0;
        std::uint64_t holder = 0;
        std::uint64_t board = 0;
        if (!guest.Read(actor + addr::kActorComponents, &components, sizeof components) || components == 0 ||
            !guest.Read(components, &holder, sizeof holder) || holder == 0 ||
            !guest.Read(holder + addr::kHolderBlackboard, &board, sizeof board) || board == 0 ||
            board == ~std::uint64_t{0}) {
            continue;
        }
        if (board != p.blackboard) {
            p.blackboard = board;
            p.keys.clear();
            for (const std::string_view n : kNames) {
                p.keys.push_back(ProbeKey{n});
            }
            for (std::uint64_t off = 0; off + 0x18 < kScanEnd; off += 8) {
                std::int32_t names = 0;
                std::int32_t values = 0;
                std::uint64_t names_at = 0;
                if (!guest.Read(board + off, &names, sizeof names) || names <= 0 || names > addr::kMaxNames ||
                    !guest.Read(board + off + 0x10, &values, sizeof values) || values != names ||
                    !guest.Read(board + off + 8, &names_at, sizeof names_at) || names_at == 0) {
                    continue;
                }
                std::vector<std::uint64_t> at(static_cast<std::size_t>(names));
                if (!guest.Read(names_at, at.data(), at.size() * sizeof(std::uint64_t))) {
                    continue;
                }
                for (ProbeKey& k : p.keys) {
                    if (k.group != 0) {
                        continue;
                    }
                    std::string text(k.name.size() + 1, '\1');
                    for (std::size_t i = 0; i < at.size(); ++i) {
                        if (at[i] != 0 && guest.Read(at[i], text.data(), text.size()) &&
                            std::memcmp(text.data(), k.name.data(), k.name.size()) == 0 && text.back() == '\0') {
                            k.group = off;
                            k.index = static_cast<std::uint32_t>(i);
                            Note("probe 323: Gohma " + Hex(actor) + " key " + std::string(k.name) + " in group +" +
                                 Hex(off) + " index " + std::to_string(i) + " of " + std::to_string(names));
                            break;
                        }
                    }
                }
            }
            for (const ProbeKey& k : p.keys) {
                if (k.group == 0) {
                    Note("probe 323: Gohma " + Hex(actor) + " key " + std::string(k.name) + " NOT found (scan to +" +
                         Hex(kScanEnd) + ")");
                }
            }
        }
        std::string line;
        for (const ProbeKey& k : p.keys) {
            if (k.group == 0) {
                continue;
            }
            std::uint64_t values_at = 0;
            if (!guest.Read(board + k.group + 0x18, &values_at, sizeof values_at) || values_at == 0) {
                continue;
            }
            const std::string_view shortname = k.name.substr(std::string_view("DungeonBossGohma_").size());
            char text[160];
            if (k.group == addr::kBoolNameCount) {
                std::uint8_t v = 0;
                guest.Read(values_at + k.index, &v, sizeof v);
                std::snprintf(text, sizeof text, " %.*s=%u", static_cast<int>(shortname.size()), shortname.data(),
                              static_cast<unsigned>(v));
            } else if (k.group == addr::kS32NameCount) {
                std::int32_t v = 0;
                guest.Read(values_at + k.index * 4u, &v, sizeof v);
                std::snprintf(text, sizeof text, " %.*s=%d", static_cast<int>(shortname.size()), shortname.data(),
                              static_cast<int>(v));
            } else {
                // Not the s32 or bool group (a vec3's): its element size is not known here, so both readings.
                float a[3] = {};
                float b[3] = {};
                guest.Read(values_at + k.index * 12u, a, sizeof a);
                guest.Read(values_at + k.index * 16u, b, sizeof b);
                std::snprintf(text, sizeof text, " %.*s=v12(%.1f,%.1f,%.1f)/v16(%.1f,%.1f,%.1f)",
                              static_cast<int>(shortname.size()), shortname.data(), a[0], a[1], a[2], b[0], b[1],
                              b[2]);
            }
            line += text;
        }
        if (changed || line != p.last) {
            Note("probe 323: Gohma " + Hex(actor) + " " + (e.member.role == Role::owner ? "owner" : "copy") +
                 " frame " + std::to_string(probe_frames_) + ":" + line);
            p.last = std::move(line);
        }
    }
}

Table& TheTable() {
    static Table table;
    return table;
}

}  // namespace CoopBrainState
