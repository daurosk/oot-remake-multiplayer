// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby, the hook's side (coop_lobby_game.h).
#include "coop_lobby_game.h"

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_lobby.h"
#include "coop_teleport.h"

namespace CoopLobby {
namespace {

struct Glue {
    std::mutex mutex;   // everything below
    Seen seen;          // the network thread's copy of the room, for the tab
    std::map<std::uint8_t, std::uint8_t> host_mutes;            // by number, from LOBBY_MUTES
    std::map<std::uint8_t, proto::DbnoState> dbno;              // by number, from DBNO_STATE
    Mutes own;                                                  // ours, by name
    std::deque<proto::LobbyAction> asked;                       // the tab -> Frame
    std::vector<std::string> lines;                             // -> the chat box
    Knocks knocks;                                              // who asks to join (we host)
    std::uint32_t refusals = 0;                                 // Seen::refusals (never reset)
    std::uint32_t names_taken = 0;                              // Seen::names_taken (never reset)

    void Line(std::string text) {
        if (lines.size() < 32) {
            lines.push_back(std::move(text));
        }
    }
    // Under mutex.
    std::string NameOf(std::uint8_t number) const {
        for (const auto& p : seen.players) {
            if (p.number == number && !p.name.empty()) {
                return p.name;
            }
        }
        return "P" + std::to_string(number);
    }
    // The room from the client, with what we keep of each player (under mutex).
    void Copy(const coop::net::Client& client) {
        const bool on = client.state() == coop::net::State::connected;
        seen.connected = on;
        seen.self = on ? client.player_number() : 0;
        seen.host = on ? client.host_number() : 0;
        seen.players.clear();
        if (!on) {
            return;
        }
        for (const auto& p : client.players()) {
            PlayerSeen s;
            s.number = p.number;
            s.name = p.nickname;
            s.self = p.self;
            s.host = p.number == seen.host;
            s.color = p.equipment && p.equipment->color ? *p.equipment->color : proto::slot_color(p.number);
            s.status = p.status.value_or(0);
            seen.players.push_back(std::move(s));
        }
        Refresh();
    }
    // Mutes and the down state into the copy (under mutex).
    void Refresh() {
        for (auto& s : seen.players) {
            const auto m = host_mutes.find(s.number);
            s.host_mutes = m == host_mutes.end() ? 0 : m->second;
            const auto d = dbno.find(s.number);
            s.dbno = d == dbno.end() ? proto::DbnoState::up : d->second;
            s.own_mutes = s.self ? 0 : own.Of(s.name);
        }
    }
    // Under mutex.
    std::uint8_t OwnOf(std::uint8_t player) const {
        for (const auto& p : seen.players) {
            if (p.number == player) {
                return p.self ? 0 : own.Of(p.name);
            }
        }
        return 0;
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

}  // namespace

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = Parse(text);
    if (!command) {
        return false;
    }
    Glue& g = TheGlue();
    std::vector<std::string> out;
    if (command->kind == Command::Kind::bad) {
        out.push_back(UsageLine(command->word));
    } else if (command->kind == Command::Kind::allow || command->kind == Command::Kind::deny) {
        // Host approval: the answer to a player who asks to join.
        session.with_client([&](coop::net::Client& client) {
            if (client.state() != coop::net::State::connected) {
                out.push_back(command->word + ": not connected");
                return;
            }
            if (!client.is_host()) {
                out.push_back(command->word + ": only the host can do that");
                return;
            }
            std::optional<proto::JoinKnock> knock;
            {
                std::lock_guard lock(g.mutex);
                knock = g.knocks.Find(command->name);
            }
            if (!knock) {
                out.push_back(NoKnockLine(command->word, command->name));
                return;
            }
            const bool allow = command->kind == Command::Kind::allow;
            if (!client.send(AnswerFor(*knock, command->kind, command->always))) {
                out.push_back(command->word + ": could not be sent");
                return;
            }
            std::lock_guard lock(g.mutex);
            g.knocks.Forget(knock->knock);
            out.push_back(AnswerLine(knock->nickname, allow, command->always));
        });
    } else {
        session.with_client([&](coop::net::Client& client) {
            if (client.state() != coop::net::State::connected) {
                out.push_back(command->word + ": not connected");
                return;
            }
            std::vector<CoopTeleport::Player> players;
            for (const auto& p : client.players()) {
                players.push_back({p.number, p.nickname});
            }
            CoopTeleport::Found found =
                CoopTeleport::FindPlayer(command->name, players, client.player_number());
            if (found.number == 0) {
                // FindPlayer's lines start with its own command's word.
                std::string line = found.error;
                if (line.rfind("teleport:", 0) == 0) {
                    line = command->word + line.substr(8);
                }
                out.push_back(std::move(line));
                return;
            }
            const bool mute = command->kind == Command::Kind::mute || command->kind == Command::Kind::unmute;
            if (!client.is_host()) {
                if (!mute) {
                    out.push_back(command->word + ": only the host can do that");
                    return;
                }
                // Not the host: our own mute, as the Lobby tab's.
                const bool on = command->kind == Command::Kind::mute;
                std::lock_guard lock(g.mutex);
                const bool changed = g.own.Set(found.name, command->what, on);
                g.Refresh();
                out.push_back(OwnMuteLine(found.name, command->what, on, changed));
                return;
            }
            for (const auto& action : ActionsFor(command->kind, command->what, found.number)) {
                if (!client.send(action)) {
                    out.push_back(command->word + ": could not be sent");
                    return;
                }
            }
            // The server's answer (LOBBY_RESULT, LOBBY_MUTES, PLAYER_LEFT) says what came of it.
        });
    }
    std::lock_guard lock(g.mutex);
    for (auto& line : out) {
        g.Line(std::move(line));
    }
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    switch (event.kind) {
    case EventKind::welcomed:
    case EventKind::reconnected:
    case EventKind::disconnected:
    case EventKind::refused:
        // Each connection learns the host's mutes again (LOBBY_MUTES after WELCOME).
        g.host_mutes.clear();
        g.dbno.clear();
        g.knocks.Clear();  // the server turns away whoever waited for this game's answer
        g.Copy(client);
        g.seen.refused = event.kind == EventKind::refused
                             ? std::string(coop::net::refuse_text(event))
                             : std::string();
        g.seen.banned = event.kind == EventKind::refused && event.refuse_reason == proto::RefuseReason::banned;
        if (event.kind == EventKind::refused && StopsRejoin(event.refuse_reason)) {
            ++g.refusals;   // (kept across the disconnect that clears `refused`: Seen::refusals)
        }
        if (event.kind == EventKind::refused && event.refuse_reason == proto::RefuseReason::name_taken) {
            ++g.names_taken;
        }
        if (!g.seen.refused.empty()) {
            g.Line("refused: " + g.seen.refused);   // the full reason in the chat (the tabs show its first part)
        }
        return;
    case EventKind::player_left:
        g.host_mutes.erase(event.player);
        g.dbno.erase(event.player);
        g.Copy(client);
        return;
    case EventKind::player_joined:
    case EventKind::player_status:
    case EventKind::player_equip:
        g.Copy(client);
        return;
    case EventKind::host_changed:
        g.Copy(client);
        if (event.player == client.player_number()) {
            g.Line("you host now: the Lobby's host controls are yours");
        }
        return;
    case EventKind::waiting_for_host:
        // Host approval: our own join waits for the host's answer.
        if (const auto* k = std::get_if<proto::JoinKnock>(&event.message)) {
            g.Line(WaitingLine(k->wait_s));
            LOG_INFO(Core, "coop: lobby: waiting for the host to let this game in (up to {} s)", k->wait_s);
        }
        return;
    case EventKind::message: break;
    default: return;
    }
    if (const auto* m = std::get_if<proto::LobbyMutes>(&event.message)) {
        const auto it = g.host_mutes.find(m->player);
        const std::uint8_t before = it == g.host_mutes.end() ? 0 : it->second;
        if (m->flags == 0) {
            g.host_mutes.erase(m->player);
        } else {
            g.host_mutes[m->player] = m->flags;
        }
        g.Refresh();
        if (const std::string line = MutesLine(before, m->flags, g.NameOf(m->player), m->player == client.player_number());
            !line.empty()) {
            g.Line(line);
        }
        LOG_INFO(Core, "coop: lobby: player {} muted by the host: {}", m->player, m->flags);
    } else if (const auto* r = std::get_if<proto::LobbyResult>(&event.message)) {
        if (auto line = ResultLine(*r, g.NameOf(r->target))) {
            g.Line(std::move(*line));
        }
        LOG_INFO(Core, "coop: lobby: action {} on player {}: result {}", static_cast<int>(r->action), r->target,
                 static_cast<int>(r->result));
    } else if (const auto* d = std::get_if<proto::DbnoStatus>(&event.message)) {
        g.dbno[d->player_number] = d->state;
        g.Refresh();
    } else if (const auto* k = std::get_if<proto::JoinKnock>(&event.message)) {
        // Host approval: a player asks to join (the server sends it to the host only).
        if (auto line = g.knocks.Take(*k)) {
            g.Line(std::move(*line));
        }
        LOG_INFO(Core, "coop: lobby: knock {}: {}", k->knock, k->wait_s == 0 ? "over" : "a player asks to join");
    }
}

void Frame(coop::host::Session* session) {
    Glue& g = TheGlue();
    std::deque<proto::LobbyAction> asked;
    {
        std::lock_guard lock(g.mutex);
        asked.swap(g.asked);
    }
    if (asked.empty()) {
        return;
    }
    if (session == nullptr) {
        std::lock_guard lock(g.mutex);
        g.Line("lobby: not connected");
        return;
    }
    bool sent = true;
    session->with_client([&](coop::net::Client& client) {
        for (const auto& a : asked) {
            sent = client.send(a) && sent;
        }
    });
    if (!sent) {
        std::lock_guard lock(g.mutex);
        g.Line("lobby: could not be sent");
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

void Ask(const proto::LobbyAction& action) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    if (g.asked.size() < 16) {
        g.asked.push_back(action);
    }
}

void SetOwnMute(std::uint8_t player, std::uint8_t what, bool on) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    for (const auto& p : g.seen.players) {
        if (p.number == player && !p.self) {
            const bool changed = g.own.Set(p.name, what, on);
            g.Line(OwnMuteLine(p.name, what, on, changed));
            g.Refresh();
            return;
        }
    }
}

bool ChatHidden(std::uint8_t player) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return (g.OwnOf(player) & kChat) != 0;
}

bool VoiceHidden(std::uint8_t player) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return (g.OwnOf(player) & kVoice) != 0;
}

bool OwnChatMuted() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    const auto it = g.host_mutes.find(g.seen.self);
    return g.seen.connected && it != g.host_mutes.end() && (it->second & kChat) != 0;
}

bool OwnVoiceMuted() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    const auto it = g.host_mutes.find(g.seen.self);
    return g.seen.connected && it != g.host_mutes.end() && (it->second & kVoice) != 0;
}

Seen Now() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    Seen s = g.seen;
    s.refusals = g.refusals;
    s.names_taken = g.names_taken;
    return s;
}

std::vector<std::string> Asking() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    std::vector<std::string> out;
    for (const proto::JoinKnock& k : g.knocks.open()) {
        out.push_back(k.nickname);
    }
    return out;
}

void Forget() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    g.seen = Seen{};
    g.host_mutes.clear();
    g.dbno.clear();
    g.asked.clear();
}

}  // namespace CoopLobby
