// SPDX-License-Identifier: GPL-3.0-or-later
// Wait for the party (docs/party-wait.md): the hook's side. The rules are coop_party_wait.h's.
#include "coop_party_wait_game.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <variant>

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/scene_place.hpp"
#include "coop_boss.h"
#include "coop_boss_scenes.h"
#include "coop_fairy.h"
#include "coop_party_wait.h"
#include "coop/presence/presence.hpp"
#include "coop_teleport.h"

namespace CoopPartyWait {
namespace {

namespace proto = coop::protocol;

struct Glue {
    std::mutex mutex;
    std::atomic<bool> on{false};       // the session's PARTY_WAIT_MODE (network thread writes)
    std::uint8_t set_by = 0;
    Waiter waiter;                     // present thread
    std::vector<std::string> lines;    // -> our banner
    std::vector<std::string> chat;     // -> the others

    void Line(std::string text) {
        if (lines.size() < 16) lines.push_back(std::move(text));
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

std::string NameOf(const coop::net::Client& client, std::uint8_t number) {
    if (const auto* p = client.find_player(number); p != nullptr && !p->nickname.empty()) {
        return p->nickname;
    }
    return "P" + std::to_string(number);
}

std::string ModeLine(bool on, std::uint8_t set_by, const std::string& by) {
    std::string line = std::string("Wait for the party is ") + (on ? "on" : "off");
    if (set_by != 0) line += " (set by " + by + ")";
    return line;
}

// The boss door (docs/party-wait.md "The moments"): our Link in the area of one of the eight dungeon bosses
// (CoopBossScenes::InArea, the boss room's circle and its antechamber) but not in the room, and no fight on.
std::optional<Moment> BossDoor(const std::string& scene, const proto::Vec3& at) {
    if (CoopBoss::Fighting()) return std::nullopt;
    const CoopBossScenes::Vec p{at[0], at[1], at[2]};
    for (const CoopBossScenes::Boss& b : CoopBossScenes::Bosses()) {
        if (b.room < 0) continue;   // Ganondorf, Ganon, the stage changes: no room (not a moment)
        const CoopBoss::Room* room = CoopBoss::RoomByIndex(static_cast<std::uint8_t>(b.room));
        if (room == nullptr || !proto::same_place(room->scene, scene)) continue;
        if (CoopBossScenes::InArea(b, room->scene, std::nullopt, scene, p) &&
            !CoopBossScenes::InRoom(b, room->scene, std::nullopt, scene, p)) {
            return Moment{Kind::boss_door, "boss:" + std::string(room->scene) + ":" + std::to_string(room->index),
                          WhereText(Kind::boss_door, std::string(b.name))};
        }
    }
    return std::nullopt;
}

}  // namespace

bool On() {
    return TheGlue().on.load();
}

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = Parse(text);
    if (!command) return false;
    Glue& g = TheGlue();
    std::string line;
    session.with_client([&](coop::net::Client& client) {
        if (client.state() != coop::net::State::connected) {
            line = "wait: not connected";
            return;
        }
        switch (*command) {
        case Command::show: {
            std::uint8_t by = 0;
            {
                std::lock_guard lock(g.mutex);
                by = g.set_by;
            }
            line = ModeLine(g.on.load(), by, NameOf(client, by)) +
                   (client.is_host() ? ". /wait on or /wait off changes it" : "");
            return;
        }
        case Command::bad:
            line = "wait: /wait on, /wait off, or /wait alone to see it";
            return;
        case Command::on:
        case Command::off:
            if (!client.is_host()) {
                line = "wait: only the host can change it";
                return;
            }
            if (!client.send(proto::PartyWaitMode{0, *command == Command::on})) {
                line = "wait: could not be sent";
            }
            return;   // (the server's answer to everyone shows the change)
        }
    });
    if (!line.empty()) {
        std::lock_guard lock(g.mutex);
        g.Line(std::move(line));
    }
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    if (event.kind == EventKind::welcomed || event.kind == EventKind::reconnected ||
        event.kind == EventKind::disconnected) {
        // Each connection starts with it off; the server says so when it is on.
        std::lock_guard lock(g.mutex);
        g.on.store(false);
        g.set_by = 0;
        return;
    }
    if (event.kind != EventKind::message) return;
    if (const auto* m = std::get_if<proto::PartyWaitMode>(&event.message)) {
        std::lock_guard lock(g.mutex);
        const bool changed = g.on.load() != m->on || m->set_by != 0;
        g.on.store(m->on);
        g.set_by = m->set_by;
        if (changed) g.Line(ModeLine(m->on, m->set_by, NameOf(client, m->set_by)));
        LOG_INFO(Core, "coop: party wait: {} (set by {})", m->on ? "on" : "off", m->set_by);
    }
}

void Frame(const std::optional<std::string>& scene, const std::optional<proto::Vec3>& position,
           coop::host::Session* session, std::uint64_t server_now_us) {
    Glue& g = TheGlue();
    if (session == nullptr || !g.on.load()) {
        // Solo, co-op idle, or off: nothing read. A wait in progress ends quietly.
        std::lock_guard lock(g.mutex);
        if (g.waiter.waiting()) g.waiter.Reset();
        return;
    }
    Input in;
    in.on = true;
    in.now_us = server_now_us;
    if (scene && position) {
        in.scene = *scene;
        in.moment = BossDoor(*scene, *position);
    }
    // Everyone in the session counts, whatever presence shows of them (just joined, on the title, samples gone:
    // not here). Review c123-17: never from CoopPuppet::Collect, which hands over queued sounds and emotes.
    session->with_client([&](coop::net::Client& client) {
        for (const auto& p : client.players()) {
            if (p.number != client.player_number()) ++in.others;
        }
    });
    if (in.moment && in.others != 0) {   // (who is here is read only in a moment)
        static const CoopRooms::Table rooms = CoopFairy::RoomTable();
        const CoopTeleport::Vec ours{(*position)[0], (*position)[1], (*position)[2]};
        session->with_presence([&](coop::presence::Presence& p) {
            for (const std::uint8_t n : p.players()) {
                const coop::presence::Player* who = p.player(n);
                if (who == nullptr || !who->scene() || who->has_flag(proto::status::loading) ||
                    !proto::same_place(*who->scene(), *scene)) {
                    continue;
                }
                const auto pose = p.shown(n, server_now_us);
                if (pose && CoopTeleport::CheckMove(rooms, *scene, ours,
                                                    CoopTeleport::Vec{pose->position[0], pose->position[1],
                                                                      pose->position[2]}) == CoopTeleport::MoveCheck::ok) {
                    ++in.here;
                }
            }
        });
    }
    std::lock_guard lock(g.mutex);
    Output out = g.waiter.Frame(in);
    for (auto& line : out.notices) {
        LOG_INFO(Core, "coop: party wait: {}", line);
        g.Line(std::move(line));
    }
    if (out.chat && g.chat.size() < 4) g.chat.push_back(std::move(*out.chat));
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

std::vector<std::string> TakeChatLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.chat, {});
}

}  // namespace CoopPartyWait
