// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby, the hook's side (coop_lobby_game.h).
#include "coop_lobby_game.h"

#include <deque>
#include <map>
#include <mutex>
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
        g.Copy(client);
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

Seen Now() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.seen;
}

}  // namespace CoopLobby
