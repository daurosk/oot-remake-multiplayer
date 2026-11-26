// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby's rules (coop_lobby.h).
#include "coop_lobby.h"

#include <algorithm>

namespace CoopLobby {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace

std::optional<Command> Parse(std::string_view text) {
    text = Trim(text);
    if (text.empty() || text.front() != '/') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    const auto space = text.find_first_of(" \t");
    Command c;
    c.word = Lower(text.substr(0, space));
    if (c.word == "kick") {
        c.kind = Command::Kind::kick;
    } else if (c.word == "ban") {
        c.kind = Command::Kind::ban;
    } else if (c.word == "mute") {
        c.kind = Command::Kind::mute;
    } else if (c.word == "unmute") {
        c.kind = Command::Kind::unmute;
    } else if (c.word == "allow") {
        c.kind = Command::Kind::allow;
    } else if (c.word == "deny") {
        c.kind = Command::Kind::deny;
    } else {
        return std::nullopt;
    }
    std::string_view rest = space == std::string_view::npos ? std::string_view{} : Trim(text.substr(space));
    if (c.kind == Command::Kind::allow || c.kind == Command::Kind::deny) {
        // "/allow Riley always", "/allow always": the last word says to remember the answer.
        const auto last = rest.find_last_of(" \t");
        const std::string_view tail = last == std::string_view::npos ? rest : rest.substr(last + 1);
        if (Lower(tail) == "always") {
            c.always = true;
            rest = last == std::string_view::npos ? std::string_view{} : Trim(rest.substr(0, last));
        }
        c.name = std::string(rest);  // "": the oldest who asks
        return c;
    }
    if (c.kind == Command::Kind::mute || c.kind == Command::Kind::unmute) {
        // "/mute Saria voice": the last word says what, when it is voice or chat.
        const auto last = rest.find_last_of(" \t");
        if (last != std::string_view::npos) {
            const std::string tail = Lower(rest.substr(last + 1));
            if (tail == "voice" || tail == "chat") {
                c.what = tail == "voice" ? kVoice : kChat;
                rest = Trim(rest.substr(0, last));
            }
        }
    }
    if (rest.empty()) {
        c.kind = Command::Kind::bad;
        return c;
    }
    c.name = std::string(rest);
    return c;
}

std::vector<proto::LobbyAction> ActionsFor(Command::Kind kind, std::uint8_t what, std::uint8_t target) {
    using proto::LobbyActionKind;
    std::vector<proto::LobbyAction> out;
    if (what == 0) {
        what = kBoth;
    }
    switch (kind) {
    case Command::Kind::kick: out.push_back({LobbyActionKind::kick, target}); break;
    case Command::Kind::ban: out.push_back({LobbyActionKind::ban, target}); break;
    case Command::Kind::mute:
    case Command::Kind::unmute: {
        const bool on = kind == Command::Kind::mute;
        if ((what & kVoice) != 0) {
            out.push_back({on ? LobbyActionKind::mute_voice : LobbyActionKind::unmute_voice, target});
        }
        if ((what & kChat) != 0) {
            out.push_back({on ? LobbyActionKind::mute_chat : LobbyActionKind::unmute_chat, target});
        }
        break;
    }
    case Command::Kind::allow:
    case Command::Kind::deny:
    case Command::Kind::bad: break;
    }
    return out;
}

std::string UsageLine(std::string_view word) {
    const std::string w(word);
    if (w == "mute" || w == "unmute") {
        return w + ": /" + w + " <player>, or /" + w + " <player> voice (or chat) for only that";
    }
    return w + ": /" + w + " <player> (a name, the start of one, or the number)";
}

std::optional<std::string> ResultLine(const proto::LobbyResult& result, std::string_view name) {
    using proto::LobbyActionKind;
    using proto::LobbyResultCode;
    const std::string n(name);
    const bool kick = result.action == LobbyActionKind::kick;
    const bool ban = result.action == LobbyActionKind::ban;
    switch (result.result) {
    case LobbyResultCode::done:
        if (kick) {
            return "you took " + n + " out of the game";
        }
        if (ban) {
            return "you banned " + n + " until you stop hosting";
        }
        return std::nullopt;  // LOBBY_MUTES's line says it
    case LobbyResultCode::not_host: return std::string("lobby: only the host can do that");
    case LobbyResultCode::no_player: return n + " is not in the game any more";
    case LobbyResultCode::is_host: return std::string("lobby: not to yourself");
    case LobbyResultCode::unchanged: return std::nullopt;
    }
    return std::string("lobby: the server did not do it");
}

std::string WhatWords(std::uint8_t what) {
    if ((what & kBoth) == kBoth) {
        return "voice and chat";
    }
    return (what & kVoice) != 0 ? "voice" : "chat";
}

std::string MutesLine(std::uint8_t before, std::uint8_t after, std::string_view name, bool self) {
    const std::uint8_t added = static_cast<std::uint8_t>(after & ~before & kBoth);
    const std::uint8_t taken = static_cast<std::uint8_t>(before & ~after & kBoth);
    const std::string who = self ? std::string("your") : std::string(name) + "'s";
    std::string line;
    if (added != 0) {
        line = "the host muted " + who + " " + WhatWords(added) + " for everyone";
    }
    if (taken != 0) {
        line += line.empty() ? "the host " : "; ";
        line += "turned " + who + " " + WhatWords(taken) + " back on";
    }
    return line;
}

std::uint8_t Mutes::Of(std::string_view name) const {
    const auto it = by_name_.find(Lower(name));
    return it == by_name_.end() ? 0 : it->second;
}

bool Mutes::Set(std::string_view name, std::uint8_t what, bool on) {
    const std::string key = Lower(name);
    const std::uint8_t before = Of(key);
    const std::uint8_t after = on ? static_cast<std::uint8_t>(before | what)
                                  : static_cast<std::uint8_t>(before & ~what);
    if (after == before) {
        return false;
    }
    if (after == 0) {
        by_name_.erase(key);
    } else {
        by_name_[key] = after;
    }
    return true;
}

std::string OwnMuteLine(std::string_view name, std::uint8_t what, bool on, bool changed) {
    const std::string n(name);
    if (!changed) {
        const std::string was = (what & kBoth) == kBoth ? " were" : " was";
        return n + "'s " + WhatWords(what) + was + (on ? " muted already" : " not muted");
    }
    return on ? "you muted " + n + "'s " + WhatWords(what) + " (only you: nobody is told)"
              : "you hear " + n + "'s " + WhatWords(what) + " again";
}

std::optional<std::string> Knocks::Take(const proto::JoinKnock& knock) {
    const auto it = std::find_if(open_.begin(), open_.end(),
                                 [&](const proto::JoinKnock& k) { return k.knock == knock.knock; });
    if (knock.wait_s == 0) {
        if (it == open_.end()) {
            return std::nullopt;  // answered already
        }
        open_.erase(it);
        return knock.nickname + " is no longer waiting to join";
    }
    if (it != open_.end()) {
        *it = knock;
    } else {
        open_.push_back(knock);
    }
    const bool known = (knock.flags & proto::join_knock::known) != 0;
    // With more than one waiting, the commands name the player.
    const std::string who = open_.size() > 1 ? " " + knock.nickname : std::string();
    return knock.nickname + " wants to join" + (known ? " (you let this player in before)" : "") + ": /allow" +
           who + " or /deny" + who + " (" + std::to_string(knock.wait_s) + " s)";
}

std::optional<proto::JoinKnock> Knocks::Find(std::string_view who) const {
    if (open_.empty()) {
        return std::nullopt;
    }
    if (who.empty()) {
        return open_.front();
    }
    const std::string want = Lower(who);
    for (const auto& k : open_) {
        if (Lower(k.nickname) == want) {
            return k;
        }
    }
    for (const auto& k : open_) {
        if (Lower(k.nickname).rfind(want, 0) == 0) {
            return k;
        }
    }
    return std::nullopt;
}

void Knocks::Forget(std::uint32_t knock) {
    std::erase_if(open_, [&](const proto::JoinKnock& k) { return k.knock == knock; });
}

proto::JoinAllow AnswerFor(const proto::JoinKnock& knock, Command::Kind kind, bool always) {
    return proto::JoinAllow{knock.knock, kind == Command::Kind::allow, always};
}

std::string AnswerLine(std::string_view name, bool allow, bool always) {
    const std::string n(name);
    if (allow) {
        return "you let " + n + " in" + (always ? "; they join without asking until you stop hosting" : "");
    }
    return "you turned " + n + " away" + (always ? "; they are kept out until you stop hosting" : "");
}

std::string HostChangedLine(std::uint8_t before, std::uint8_t after, std::string_view name, bool self) {
    // (this game: the host_changed event's own line says it, "you host now: ...", in the player's language)
    if (before == 0 || after == 0 || before == after || self) return {};
    return std::string(name) + " is now the host: their save is the one played";
}

std::string TitleHostLine(bool connected, bool self_hosts, std::string_view host_name) {
    if (!connected) return {};
    if (self_hosts) return "Co-op: you host. Continue loads your save.";
    return "Co-op: " + (host_name.empty() ? std::string("another player") : std::string(host_name)) +
           " hosts. Continue loads their save.";
}

std::string NoKnockLine(std::string_view word, std::string_view who) {
    const std::string w(word);
    if (who.empty()) {
        return w + ": nobody is asking to join";
    }
    return w + ": nobody called " + std::string(who) + " is asking to join";
}

std::string WaitingLine(std::uint16_t wait_s) {
    return "the host is asked to let you in: waiting up to " + std::to_string(wait_s) + " s";
}

}  // namespace CoopLobby
