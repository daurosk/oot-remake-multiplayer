// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emotes and pings, the hook's side of the chat command (coop_emote.h).

#include "coop_emote_game.h"

#include <chrono>
#include <mutex>
#include <utility>
#include <variant>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_emote.h"

namespace CoopEmote {
namespace {

struct Glue {
    std::mutex mutex;
    std::vector<std::string> lines;  // any thread -> chat box
    std::vector<std::uint8_t> calls;  // the network thread -> the fairies (TakeCalls)
    Limiter limiter;

    void Line(std::string text) {
        if (lines.size() < 16) {
            lines.push_back(std::move(text));
        }
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = Parse(text);
    if (!command) {
        return false;
    }
    Glue& g = TheGlue();
    std::string line;
    if (command->kind == Command::Kind::list) {
        line = ListLine();
    } else if (command->kind == Command::Kind::unknown) {
        line = "emote: no emote '" + command->name + "'. " + ListLine();
    } else {
        session.with_client([&](coop::net::Client& client) {
            if (client.state() != coop::net::State::connected || client.players().size() < 2) {
                line = "emote: nobody else is in the game";  // solo: nothing is sent
                return;
            }
            {
                std::lock_guard lock(g.mutex);
                if (!g.limiter.Allow(SteadyMs())) {
                    line = "emote: wait a moment";
                    return;
                }
            }
            if (client.send(coop::protocol::Emote{coop::protocol::kPlayerSelf, command->def->id})) {
                line = command->def->notice;
            } else {
                line = "emote: could not be sent";
            }
        });
    }
    if (!line.empty()) {
        std::lock_guard lock(g.mutex);
        g.Line(std::move(line));
    }
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    if (event.kind != coop::net::EventKind::message) {
        return;
    }
    const auto* emote = std::get_if<coop::protocol::Emote>(&event.message);
    if (emote == nullptr) {
        return;
    }
    const Def* def = ById(emote->id);
    if (def == nullptr) {
        return;  // an id this build does not know
    }
    std::string name;
    for (const auto& p : client.players()) {
        if (p.number == emote->player_number) {
            if (p.self) {
                return;  // our own: the sender has its own notice
            }
            name = p.nickname;
            break;
        }
    }
    std::string line = HeardLine(*def, name, emote->player_number);
    if (line.empty()) {
        return;
    }
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    g.Line(std::move(line));
    if (g.calls.size() < 16) {
        g.calls.push_back(emote->player_number);
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    std::vector<std::string> out;
    out.swap(g.lines);
    return out;
}

std::vector<std::uint8_t> TakeCalls() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    std::vector<std::uint8_t> out;
    out.swap(g.calls);
    return out;
}

} // namespace CoopEmote
