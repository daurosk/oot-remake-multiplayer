// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby's rules (coop_lobby.h).
#include "coop_lobby.h"

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
    } else {
        return std::nullopt;
    }
    std::string_view rest = space == std::string_view::npos ? std::string_view{} : Trim(text.substr(space));
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

}  // namespace CoopLobby
