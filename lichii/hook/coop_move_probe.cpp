// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_move_probe.h"

#include <cmath>
#include <cstdio>

namespace CoopMoveProbe {
namespace {

std::optional<std::uint64_t> Ptr(const ReadWord& read, std::uint64_t at) {
    const auto v = read(at);
    if (!v || *v == 0) {
        return std::nullopt;
    }
    return v;
}

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string Metres(float v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v));
    return b;
}

std::string Text(const std::optional<Move>& m) {
    if (!m) {
        return "none";
    }
    return std::to_string(m->current) + " (type " + std::to_string(m->type) + ", before " +
           std::to_string(m->before) + ")";
}

std::string Whose(bool ours, std::uint8_t owner) {
    return ours ? std::string("ours") : "a copy of player " + std::to_string(owner);
}

}  // namespace

std::optional<Move> Read(const ReadWord& read, std::uint64_t actor) {
    if (actor == 0) {
        return std::nullopt;
    }
    const auto table = Ptr(read, actor + addr::kActorComponents);
    const auto physics = table ? Ptr(read, *table + addr::kComponentPhysics) : std::nullopt;
    const auto inner = physics ? Ptr(read, *physics + addr::kPhysicsInner) : std::nullopt;
    const auto holder = inner ? Ptr(read, *inner + addr::kInnerHolder) : std::nullopt;
    const auto cct = holder ? Ptr(read, *holder + addr::kHolderCct) : std::nullopt;
    const auto machine = cct ? Ptr(read, *cct + addr::kCctMachine) : std::nullopt;
    const auto index = machine ? read(*machine + addr::kMachineCurrent) : std::nullopt;
    if (!index) {
        return std::nullopt;
    }
    Move m;
    m.cct = *cct;
    m.current = static_cast<std::uint32_t>(*index);
    m.before = static_cast<std::uint32_t>(*index >> 32);
    const std::uint64_t slot = m.current < 3 ? std::uint64_t{m.current} * 8 : 0;
    const auto state = Ptr(read, *machine + addr::kMachineStates + slot);
    const auto type = state ? read(*state + addr::kStateType) : std::nullopt;
    if (!type) {
        return std::nullopt;
    }
    m.type = static_cast<std::uint32_t>(*type);
    return m;
}

void Watch::Class(const ReadWord& read, const Seen& seen, std::uint64_t cct, std::vector<std::string>& lines) {
    const auto vtable = Ptr(read, cct);
    if (!vtable || !classes_.insert(*vtable).second) {
        return;
    }
    const auto actor_vtable = read(seen.actor).value_or(0);
    std::string line = "move probe: " + seen.name + " cct class: vtable " + Hex(*vtable) + ", main+" +
                       Hex(*vtable - actor_vtable + addr::kActorVtable);
    for (const std::uint64_t slot : {addr::kSlotSetGravityScale, addr::kSlotGetGravity}) {
        line += ", +" + Hex(slot) + " code";
        const auto function = Ptr(read, *vtable + slot);
        for (std::uint64_t i = 0; i < 4; ++i) {
            const auto words = function ? read(*function + 8 * i) : std::nullopt;
            line += " " + (words ? Hex(*words) : std::string("?"));
        }
    }
    lines.push_back(std::move(line));
    ++lines_;
}

std::vector<std::string> Watch::Look(const ReadWord& read, const Seen& seen) {
    std::vector<std::string> lines;
    if (lines_ >= kMaxLines) {
        return lines;
    }
    const auto move = Read(read, seen.actor);
    if (last_.size() >= kMaxActors && !last_.contains(seen.actor)) {
        last_.clear();  // (actors come and go with scenes)
    }
    auto [it, fresh] = last_.try_emplace(seen.actor);
    Last& last = it->second;
    const bool moved = last.move != move;
    const bool handed = last.ours != seen.ours || last.owner != seen.owner;
    const bool asked = last.verbs != seen.verbs;
    const bool rose = std::fabs(seen.at[1] - last.y) >= kRise;
    if (!fresh && !moved && !handed && !asked && !rose) {
        return lines;
    }
    if (move) {
        Class(read, seen, move->cct, lines);
    }
    std::string line =
        "move probe: " + seen.name + " " + Hex(seen.actor) + " " + Whose(seen.ours, seen.owner) + ":";
    if (fresh) {
        line += " first seen, move state " + Text(move);
    } else {
        if (handed) {
            line += " HANDED OVER (was " + Whose(last.ours, last.owner) + ")";
        }
        line += moved ? " move state " + Text(last.move) + " -> " + Text(move) : " move state " + Text(move);
    }
    line += ", verbs";
    for (const std::int32_t v : seen.verbs) {
        line += " " + std::to_string(v);
    }
    line += ", " + seen.animation + ", at " + Metres(seen.at[0]) + " " + Metres(seen.at[1]) + " " +
            Metres(seen.at[2]);
    lines.push_back(std::move(line));
    ++lines_;
    last = Last{move, seen.ours, seen.owner, seen.verbs, seen.at[1]};
    return lines;
}

}  // namespace CoopMoveProbe
