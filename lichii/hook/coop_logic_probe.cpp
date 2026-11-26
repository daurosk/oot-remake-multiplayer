// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_logic_probe.h"

#include <cstdio>
#include <set>

namespace CoopLogicProbe {
namespace {

std::optional<std::uint64_t> Ptr(const ReadWord& read, std::uint64_t at) {
    if (at == 0) return std::nullopt;
    const auto v = read(at);
    if (!v || *v == 0) return std::nullopt;
    return v;
}

std::string Hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

}  // namespace

std::optional<std::vector<Plug>> Read(const ReadWord& read, std::uint64_t actor) {
    const auto outer = Ptr(read, actor + addr::kActorLogic);
    if (!outer) return std::nullopt;
    const auto logic = Ptr(read, *outer + addr::kLogicInner);
    if (!logic) return std::nullopt;
    const auto state = read(*logic + addr::kLogicState);
    const auto flags = read(*logic + addr::kLogicFlags);
    if (!state || !flags) return std::nullopt;
    const bool live = *state == ~std::uint64_t{0} ? false : (*flags & 1) == 0;
    if (!live) return std::nullopt;
    const auto table = Ptr(read, *logic + addr::kLogicTable);
    if (!table) return std::nullopt;
    const auto holder = Ptr(read, *table + addr::kTableRootHolder);
    if (!holder) return std::nullopt;
    const auto root = Ptr(read, *holder + addr::kRoot);
    std::vector<Plug> plugs;
    if (!root) return plugs;
    // In-order walk with an explicit stack; a node seen twice (a bad read) ends the walk.
    std::vector<std::uint64_t> stack;
    std::set<std::uint64_t> visited;
    std::uint64_t at = *root;
    while ((at != 0 || !stack.empty()) && plugs.size() < kMaxPlugs) {
        while (at != 0 && stack.size() < kMaxPlugs) {
            if (!visited.insert(at).second) return plugs;
            stack.push_back(at);
            at = read(at + addr::kLeft).value_or(0);
        }
        if (stack.empty()) break;
        const std::uint64_t node = stack.back();
        stack.pop_back();
        Plug p;
        p.node = node;
        p.key = static_cast<std::uint32_t>(read(node + addr::kKey).value_or(0));
        p.type = static_cast<std::uint8_t>(read(node + addr::kType).value_or(0));
        if (const auto v = read(node + addr::kValue); v && (*v & ~std::uint64_t{3}) != 0) {
            p.value_word = *v;
            if (const auto b = read(*v & ~std::uint64_t{3})) p.value = static_cast<std::uint8_t>(*b);
        }
        plugs.push_back(p);
        at = read(node + addr::kRight).value_or(0);
    }
    return plugs;
}

std::vector<std::string> Watch::Look(const ReadWord& read,
                                     const std::vector<std::pair<std::uint64_t, std::string>>& actors) {
    std::vector<std::string> lines;
    std::size_t n = 0;
    for (const auto& [address, name] : actors) {
        if (++n > kMaxActors || lines_ >= kMaxLines) break;
        const auto plugs = Read(read, address);
        if (!plugs) continue;
        auto [it, fresh] = seen_.try_emplace(address);
        auto& known = it->second;
        if (fresh) {
            std::string line = "logic probe: " + name + " plugs (key type value):";
            for (const Plug& p : *plugs) {
                line += " " + Hex(p.key) + ":" + std::to_string(p.type) + "=" +
                        (p.value ? std::to_string(*p.value) : std::string("?"));
                known[p.key] = p.value;
            }
            lines.push_back(std::move(line));
            ++lines_;
            continue;
        }
        for (const Plug& p : *plugs) {
            auto [k, added] = known.try_emplace(p.key, p.value);
            if (added || k->second == p.value) continue;
            lines.push_back("logic probe: " + name + " plug " + Hex(p.key) + " (type " + std::to_string(p.type) +
                            ") " + (k->second ? std::to_string(*k->second) : std::string("?")) + " -> " +
                            (p.value ? std::to_string(*p.value) : std::string("?")));
            ++lines_;
            k->second = p.value;
        }
    }
    return lines;
}

}  // namespace CoopLogicProbe
