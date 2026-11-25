// SPDX-License-Identifier: GPL-3.0-or-later
// See coop_emote.h.

#include "coop_emote.h"

#include <algorithm>
#include <cctype>

namespace CoopEmote {
namespace {

// AS commands: names in the game's own Player_Adult and Player_Child animation sets (the talk gestures Link
// plays in a conversation). The sound is a Link sound name (docs/puppet.md "Link sounds"); one the puppet's
// sound user does not have is not played, so a wrong name is silent, not an error.
constexpr Def kTable[] = {
    {1, "wave", "hi", "EvtTalkHello", "", 2500, "you wave"},
    {2, "yes", "nod", "EvtTalkYes", "", 2000, "you nod"},
    {3, "no", "shake", "EvtTalkNo", "", 2000, "you shake your head"},
    {4, "look", "around", "EvtLookAround", "", 3000, "you look around"},
    {5, "here", "ping", "EvtLookAroundAttention", "Pl_Voice_Attack_00", 2500, "you call: over here"},
    {6, "wow", "surprise", "EvtSurprise", "", 2000, "you are surprised"},
};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

std::span<const Def> Table() { return kTable; }

const Def* ById(std::uint8_t id) {
    for (const auto& d : kTable) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

const Def* ByName(std::string_view name) {
    const std::string wanted = Lower(name);
    if (wanted.empty()) {
        return nullptr;
    }
    for (const auto& d : kTable) {
        if (wanted == d.name || wanted == d.alias) {
            return &d;
        }
    }
    return nullptr;
}

std::optional<Command> Parse(std::string_view text) {
    text = Trim(text);
    if (text.size() < 2 || text.front() != '/') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    const auto space = text.find_first_of(" \t");
    const std::string word = Lower(text.substr(0, space));
    const std::string_view rest = space == std::string_view::npos ? std::string_view{} : Trim(text.substr(space));
    Command command;
    if (word == "emote") {
        if (rest.empty()) {
            return command;  // list
        }
        const std::string_view first = rest.substr(0, rest.find_first_of(" \t"));
        if (const Def* d = ByName(first)) {
            command.kind = Command::Kind::play;
            command.def = d;
        } else {
            command.kind = Command::Kind::unknown;
            command.name = std::string(first);
        }
        return command;
    }
    if (const Def* d = ByName(word)) {
        command.kind = Command::Kind::play;
        command.def = d;
        return command;
    }
    return std::nullopt;
}

std::string ListLine() {
    std::string line = "emotes:";
    bool first = true;
    for (const auto& d : kTable) {
        line += first ? " /" : ", /";
        line += d.name;
        first = false;
    }
    return line;
}

bool Limiter::Allow(std::uint64_t now_ms) {
    if (last_ms_ && now_ms >= *last_ms_ && now_ms - *last_ms_ < kCooldownMs) {
        return false;
    }
    last_ms_ = now_ms;
    return true;
}

bool Holds::Start(std::uint8_t number, std::uint8_t id, std::uint64_t now_ms) {
    const Def* d = ById(id);
    if (d == nullptr || number == 0 || number >= holds_.size()) {
        return false;
    }
    Hold& h = holds_[number];
    h.def = d;
    h.until_ms = now_ms + std::min(d->hold_ms, kMaxHoldMs);
    h.base_known = false;
    h.base.clear();
    return true;
}

const Def* Holds::Apply(std::uint8_t number, std::string_view sampled, std::uint64_t now_ms) {
    if (number == 0 || number >= holds_.size()) {
        return nullptr;
    }
    Hold& h = holds_[number];
    if (h.def == nullptr) {
        return nullptr;
    }
    if (now_ms >= h.until_ms) {
        h = Hold{};
        return nullptr;
    }
    if (!h.base_known) {
        h.base = std::string(sampled);
        h.base_known = true;
    } else if (sampled != h.base) {
        h = Hold{};  // the sender moved on
        return nullptr;
    }
    return h.def;
}

void Holds::Clear(std::uint8_t number) {
    if (number < holds_.size()) {
        holds_[number] = Hold{};
    }
}

void Holds::ClearAll() { holds_ = {}; }

} // namespace CoopEmote
