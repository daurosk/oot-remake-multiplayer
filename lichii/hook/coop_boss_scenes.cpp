// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_scenes.h"

#include <algorithm>
#include <cmath>

namespace CoopBossScenes {

namespace proto = coop::protocol;

namespace {

// 0 to 7 in CoopBoss::Rooms() order (its intro/outro names are the same
// event flows); 8 and 9 have no room in that table. Event flows checked in
// the RomFS (Event/EventFlow, 2026-11-13). No mini-boss has an intro or
// death event of its own (no Dm_ flow names one), so none is listed.
constexpr std::array<Boss, 10> kBosses{{
    {"Queen Gohma", "Dm_Deku_BossIn", "Dm_Deku_BossOut", 0},
    {"King Dodongo", "Dm_DodongoCave_BossIn", "Dm_DodongoCave_BossOut", 1},
    {"Barinade", "Dm_InsideJabu_Jabu_BossIn", "Dm_InsideJabu_Jabu_BossOut", 2},
    {"Phantom Ganon", "Dm_ForestTemple_BossIn", "Dm_ForestTemple_BossOut", 3},
    {"Volvagia", "Dm_FireTemple_BossIn", "Dm_FireTemple_BossOut", 4},
    {"Morpha", "Dm_WaterTemple_BossIn", "Dm_WaterTemple_BossOut", 5},
    {"Bongo Bongo", "Dm_ShadowTemple_BossIn", "Dm_ShadowTemple_BossOut", 6},
    {"Twinrova", "Dm_SpiritTemple_BossIn", "Dm_SpiritTemple_BossOut", 7},
    {"Ganondorf", "Dm_InsideGanonCastle_BossIn", "Dm_InsideGanonCastle_BossOut", -1},
    {"Ganon", "Dm_FinalBattle_BossIn", "Dm_FinalBattle_BossOut", -1},
}};

const CoopBoss::Room* RoomOf(const Boss& b) {
    return b.room < 0 ? nullptr : CoopBoss::RoomByIndex(static_cast<std::uint8_t>(b.room));
}

bool Finite(const Vec& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

} // namespace

std::span<const Boss> Bosses() {
    return kBosses;
}

std::optional<std::pair<std::uint8_t, Kind>> OfEvent(std::string_view name) {
    for (std::size_t i = 0; i < kBosses.size(); ++i) {
        if (name == kBosses[i].intro) {
            return std::pair{static_cast<std::uint8_t>(i), Kind::intro};
        }
        if (name == kBosses[i].outro) {
            return std::pair{static_cast<std::uint8_t>(i), Kind::outro};
        }
    }
    return std::nullopt;
}

bool InArea(const Boss& b, std::string_view boss_scene, const std::optional<Vec>& starter, std::string_view scene,
            const Vec& p) {
    if (!Finite(p)) {
        return false;
    }
    if (const CoopBoss::Room* room = RoomOf(b); room != nullptr && scene == room->scene) {
        const float dx = p.x - room->centre_x;
        const float dz = p.z - room->centre_z;
        const float r = room->radius + kAnteGrow;
        return dx * dx + dz * dz <= r * r && p.y >= room->y_low - kAnteHigh && p.y <= room->y_high + kAnteHigh;
    }
    if (scene != boss_scene || !starter || !Finite(*starter)) {
        return false;
    }
    const float dx = p.x - starter->x, dy = p.y - starter->y, dz = p.z - starter->z;
    return dx * dx + dy * dy + dz * dz <= kNoRoomRange * kNoRoomRange;
}

bool InRoom(const Boss& b, std::string_view boss_scene, const std::optional<Vec>& starter, std::string_view scene,
            const Vec& p) {
    if (const CoopBoss::Room* room = RoomOf(b); room != nullptr && scene == room->scene) {
        return CoopBoss::Inside(*room, p);
    }
    return InArea(b, boss_scene, starter, scene, p);
}

const char* PhaseName(Director::Phase phase) {
    switch (phase) {
    case Director::Phase::none: return "none";
    case Director::Phase::intro: return "intro";
    case Director::Phase::fight: return "fight";
    case Director::Phase::outro: return "outro";
    case Director::Phase::done: return "done";
    }
    return "?";
}

void Director::Receive(const proto::Message& message) {
    if (std::holds_alternative<proto::BossScene>(message) || std::holds_alternative<proto::BossHeart>(message) ||
        std::holds_alternative<proto::PlayerLeft>(message)) {
        std::lock_guard lock(inbox_mutex_);
        inbox_.push_back(message);
    }
}

void Director::Reset() {
    fights_ = {};
    playing_.reset();
    event_ = false;
    max_life_.reset();
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

bool Director::Here(std::uint8_t b, const Local& local) const {
    const Fight& f = fights_[b];
    return local.scene && local.position && InArea(kBosses[b], f.scene, f.starter_at, *local.scene, *local.position);
}

void Director::Say(Actions& out, std::uint8_t b, proto::BossSceneKind kind, const Local& local) const {
    proto::BossScene m;
    m.scene = local.scene.value_or(std::string());
    m.boss = b;
    m.kind = kind;
    if (local.position) {
        m.position = {local.position->x, local.position->y, local.position->z};
    }
    if (!m.scene.empty()) {
        out.send.push_back(std::move(m));
    }
}

void Director::Skip(const char* why, std::uint64_t now_us, Actions& out) {
    if (!playing_ || playing_->skip) {
        return;
    }
    playing_->skip = true;
    playing_->skip_since = now_us;
    playing_->skip_at = 0;
    out.log.push_back(std::string(kBosses[playing_->boss].intro) + ": " + why + ": skipping ours");
}

// Another game's start in the area: ours plays it too (the game's own event
// start, by name); a Link before the door is moved in.
void Director::Join(std::uint8_t b, Kind kind, const Local& local, std::uint64_t now_us, Actions& out) {
    Fight& f = fights_[b];
    const Boss& boss = kBosses[b];
    if (!Here(b, local)) {
        if (!f.chatted) {
            f.chatted = true;
            out.chat.push_back({kind == Kind::intro ? f.starter : f.outro_by,
                                kind == Kind::intro ? "entered the boss room of " + std::string(boss.name)
                                                    : "beat " + std::string(boss.name)});
        }
        out.log.push_back(std::string(kind == Kind::intro ? boss.intro : boss.outro) +
                          ": not played here (we are not in the boss's area)");
        return;
    }
    if (playing_ && playing_->boss == b && playing_->kind == kind) {
        return;  // our own game has it running already
    }
    f.asked_at = now_us;
    out.play = std::string(kind == Kind::intro ? boss.intro : boss.outro);
    out.log.push_back(*out.play + ": playing it here too (player " +
                      std::to_string(kind == Kind::intro ? f.starter : f.outro_by) + " started it)");
}

void Director::Take(const proto::Message& m, const Local& local, std::uint64_t now_us, Actions& out) {
    if (const auto* left = std::get_if<proto::PlayerLeft>(&m)) {
        for (std::size_t b = 0; b < fights_.size(); ++b) {
            Fight& f = fights_[b];
            if (f.phase == Phase::intro && f.starter == left->player_number) {
                f.phase = Phase::fight;
                f.at = now_us;
                out.log.push_back(std::string(kBosses[b].intro) + ": its starter (player " +
                                  std::to_string(left->player_number) +
                                  ") left during it: nobody waits, ours plays to its end");
            }
        }
        return;
    }
    if (const auto* h = std::get_if<proto::BossHeart>(&m)) {
        if (!h->given || h->boss >= kBosses.size()) {
            return;
        }
        Fight& f = fights_[h->boss];
        if (f.heart == 0) {
            out.log.push_back(std::string(kBosses[h->boss].name) + "'s heart container: player " +
                              std::to_string(h->player_number) + " took it, everyone's maximum life " +
                              std::to_string(h->max_life));
            if (h->player_number != local.self) {
                out.chat.push_back({h->player_number, "got the heart container for everyone"});
            }
        }
        f.heart = std::max(f.heart, h->max_life);
        f.heart_by = h->player_number;
        if (f.phase < Phase::done) {
            f.phase = Phase::done;  // the boss is beaten (a late joiner, or the death not heard)
        }
        return;
    }
    const auto* s = std::get_if<proto::BossScene>(&m);
    if (s == nullptr || s->boss >= kBosses.size() || s->player_number == local.self) {
        return;
    }
    Fight& f = fights_[s->boss];
    const Vec at{s->position[0], s->position[1], s->position[2]};
    switch (s->kind) {
    case proto::BossSceneKind::intro_start:
        if (f.phase == Phase::none) {
            f.phase = Phase::intro;
            f.starter = s->player_number;
            f.scene = s->scene;
            f.starter_at = at;
            f.at = now_us;
            Join(s->boss, Kind::intro, local, now_us, out);
        } else if (f.phase == Phase::intro && s->player_number < f.starter) {
            // Two games took it together: the lower number starts it in every game.
            out.log.push_back(std::string(kBosses[s->boss].intro) + ": player " +
                              std::to_string(s->player_number) + " started it too: theirs leads (lower number)");
            f.starter = s->player_number;
            f.starter_at = at;
        } else if ((f.phase == Phase::fight || f.phase == Phase::outro) && local.scene == s->scene) {
            // A game that did not hear the start (it joined the session late)
            // took the intro while the fight is on here: tell it so.
            Say(out, s->boss, proto::BossSceneKind::intro_end, local);
            out.log.push_back(std::string(kBosses[s->boss].intro) + ": player " +
                              std::to_string(s->player_number) + " started it, but the fight is on: told them");
        }
        break;
    case proto::BossSceneKind::intro_end:
        // From the starter; or, to a starter that is us, from a game whose fight is on already.
        if (f.phase == Phase::intro && (s->player_number == f.starter || f.starter == local.self)) {
            f.phase = Phase::fight;
            f.at = now_us;
            if (playing_ && playing_->boss == s->boss && playing_->kind == Kind::intro) {
                Skip("the starter's intro is over", now_us, out);
            }
        }
        break;
    case proto::BossSceneKind::outro_start:
        if (f.phase < Phase::outro) {
            f.phase = Phase::outro;
            f.outro_by = s->player_number;
            if (f.scene.empty()) {
                f.scene = s->scene;
                f.starter_at = at;
            }
            f.at = now_us;
            Join(s->boss, Kind::outro, local, now_us, out);
        }
        break;
    case proto::BossSceneKind::outro_end:
        if (f.phase == Phase::outro && !(playing_ && playing_->boss == s->boss)) {
            f.phase = Phase::done;
        }
        break;
    }
}

// Our game took (or was refused) a boss event.
void Director::Ours(const Request& r, const Local& local, std::uint64_t now_us, Actions& out) {
    const auto of = OfEvent(r.name);
    if (!of) {
        return;
    }
    const auto [b, kind] = *of;
    if (!r.taken) {
        out.log.push_back(r.name + ": our game did not take it");
        return;
    }
    Fight& f = fights_[b];
    const bool asked = f.asked_at != 0 && now_us - f.asked_at <= kJoinUs;
    f.asked_at = 0;
    playing_ = Playing{b, kind};
    if (kind == Kind::intro) {
        if (f.phase == Phase::none || f.phase == Phase::done) {
            f = Fight{};
            f.phase = Phase::intro;
            f.starter = local.self;
            f.scene = local.scene.value_or(std::string());
            f.starter_at = local.position;
            f.at = now_us;
            Say(out, b, proto::BossSceneKind::intro_start, local);
            out.log.push_back(r.name + ": our game started it: the others in the area play it too");
        } else if (f.phase == Phase::intro) {
            out.log.push_back(r.name + (asked ? ": playing with player " : ": ours started with player ") +
                              std::to_string(f.starter) + "'s");
        } else {
            Skip("the fight is on already (we came in late)", now_us, out);
        }
        return;
    }
    if (f.phase < Phase::outro) {
        f.phase = Phase::outro;
        f.outro_by = local.self;
        if (f.scene.empty()) {
            f.scene = local.scene.value_or(std::string());
            f.starter_at = local.position;
        }
        f.at = now_us;
        Say(out, b, proto::BossSceneKind::outro_start, local);
        out.log.push_back(r.name + ": our game started the death: the others in the area play it too");
    } else {
        out.log.push_back(r.name + (asked ? ": playing with player " : ": ours started with player ") +
                          std::to_string(f.outro_by) + "'s");
    }
}

Actions Director::Tick(const Local& local, const std::vector<Request>& ours, std::uint64_t now_us) {
    Actions out;
    now_us = std::max(now_us, last_now_);
    last_now_ = now_us;
    std::vector<proto::Message> in;
    {
        std::lock_guard lock(inbox_mutex_);
        in.swap(inbox_);
    }
    for (const auto& m : in) {
        Take(m, local, now_us, out);
    }
    for (const auto& r : ours) {
        Ours(r, local, now_us, out);
    }

    // Our boss event ended.
    const bool fell = event_ && !local.event;
    event_ = local.event;
    if (fell && playing_) {
        const std::uint8_t b = playing_->boss;
        Fight& f = fights_[b];
        if (playing_->kind == Kind::intro) {
            if (f.phase == Phase::intro && f.starter == local.self) {
                f.phase = Phase::fight;
                f.at = now_us;
                Say(out, b, proto::BossSceneKind::intro_end, local);
                out.log.push_back(std::string(kBosses[b].intro) + ": ours is over: the others' end too");
            } else if (f.phase == Phase::intro) {
                out.log.push_back(std::string(kBosses[b].intro) + ": ours is over, player " +
                                  std::to_string(f.starter) + "'s still runs: the boss starts with theirs");
            }
        } else {
            if (f.outro_by == local.self) {
                Say(out, b, proto::BossSceneKind::outro_end, local);
            }
            f.phase = Phase::done;
        }
        playing_.reset();
    }

    for (std::size_t i = 0; i < fights_.size(); ++i) {
        Fight& f = fights_[i];
        const auto b = static_cast<std::uint8_t>(i);
        // Nobody waits for an end that never came (a lost message, a stuck game).
        if (f.phase == Phase::intro && f.starter != local.self && now_us - f.at >= kIntroMaxUs) {
            f.phase = Phase::fight;
            f.at = now_us;
            out.log.push_back(std::string(kBosses[b].intro) + ": no end heard from its starter: the fight is on");
        }
        // Moved in (the boss pull's move), so nobody in the area is shut out by the door: during the
        // intro every joiner is put at its own spot of the room's start (the room's circle takes in the
        // way to the door, so "in the room" alone does not say past it); during the fight one who
        // stands in the area outside the room.
        const CoopBoss::Room* room = RoomOf(kBosses[b]);
        if (room != nullptr && local.scene == room->scene && Here(b, local) && f.moves < kMoveTries &&
            (f.moves == 0 || now_us - f.moved_at >= kMoveEveryUs) && !out.move_to) {
            const Vec to = CoopBoss::ArrivalFor(*room, local.self);
            const float dx = local.position->x - to.x, dy = local.position->y - to.y, dz = local.position->z - to.z;
            const bool move = f.phase == Phase::intro
                                  ? f.starter != local.self && dx * dx + dy * dy + dz * dz > kNearStart * kNearStart
                                  : f.phase == Phase::fight &&
                                        !InRoom(kBosses[b], f.scene, f.starter_at, *local.scene, *local.position);
            if (move) {
                ++f.moves;
                f.moved_at = now_us;
                out.move_to = to;
                out.log.push_back(std::string(kBosses[b].name) + ": moving in to the room's start (try " +
                                  std::to_string(f.moves) + ")");
            }
        }
        // The one heart container: our maximum life rose in its beaten boss's area.
        if (local.max_life && max_life_ && *local.max_life > *max_life_ &&
            (f.phase == Phase::outro || f.phase == Phase::done) && Here(b, local)) {
            if (f.heart != 0 && !f.claimed && *max_life_ >= f.heart) {
                // Already given to us by the winner's claim: this pickup gives nothing more.
                out.set_max_life = f.heart;
                out.log.push_back(std::string(kBosses[b].name) + "'s heart container: player " +
                                  std::to_string(f.heart_by) + " took it first: our pickup is put back to " +
                                  std::to_string(f.heart));
            } else if (!f.claimed) {
                f.claimed = true;
                out.claims.push_back(proto::BossHeart{0, b, false, *local.max_life});
                out.log.push_back(std::string(kBosses[b].name) + "'s heart container: our pickup (maximum " +
                                  std::to_string(*local.max_life) + "): asking the server");
            }
        }
        // The server's answer: everyone's maximum is at least the winner's.
        if (f.heart != 0 && local.max_life && *local.max_life < f.heart && !out.set_max_life) {
            out.set_max_life = f.heart;
        }
    }
    if (local.max_life) {
        max_life_ = out.set_max_life.value_or(*local.max_life);  // what our game has once the write is done
    }

    // Skipping: the game's own skip, a few times until the event is gone.
    if (playing_ && playing_->skip && local.event && now_us - playing_->skip_since <= kSkipForUs &&
        (playing_->skip_at == 0 || now_us - playing_->skip_at >= kSkipEveryUs)) {
        playing_->skip_at = now_us;
        out.skip = true;
    }
    return out;
}

} // namespace CoopBossScenes
