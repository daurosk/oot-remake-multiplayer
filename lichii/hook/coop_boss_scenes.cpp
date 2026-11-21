// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_scenes.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace CoopBossScenes {

namespace proto = coop::protocol;

namespace {

// 0 to 7 in CoopBoss::Rooms() order (its intro/outro names are the same
// event flows); 8 and 9 have no room in that table. Event flows checked in
// the RomFS (Event/EventFlow, 2026-11-13). No mini-boss has an intro or
// death event of its own (no Dm_ flow names one), so none is listed.
// The settings a boss's logic asks (intro_asks/outro_asks) are from
// RSDB/EventSetting and each boss pack's DungeonBossCommonParam (2026-11-14).
constexpr std::array<Boss, kBossCount> kBosses{{
    {"Queen Gohma", "Dm_Deku_BossIn", "Dm_Deku_BossOut", 0, {"InsideTheDekuTree_BossAppear"},
     {"InsideTheDekuTree_BossDie", "InsideTheDekuTree_BossDie_DreamWorld"}},
    {"King Dodongo", "Dm_DodongoCave_BossIn", "Dm_DodongoCave_BossOut", 1, {}, {"Get_StoneOfFire_DefeatBoss"}},
    {"Barinade", "Dm_InsideJabu_Jabu_BossIn", "Dm_InsideJabu_Jabu_BossOut", 2, {}, {"Get_StoneOfWater_DestroyBoss"}},
    {"Phantom Ganon", "Dm_ForestTemple_BossIn", "Dm_ForestTemple_BossOut", 3, {},
     {"ForestTemple_SageOfForest_PhantomGanonDefeat"}},
    {"Volvagia", "Dm_FireTemple_BossIn", "Dm_FireTemple_BossOut", 4, {}, {"SageOfFire_DefeatBoss"}},
    {"Morpha", "Dm_WaterTemple_BossIn", "Dm_WaterTemple_BossOut", 5, {}, {"Demo_SageOfWater_BossDefeat"}},
    {"Bongo Bongo", "Dm_ShadowTemple_BossIn", "Dm_ShadowTemple_BossOut", 6, {}, {"ShadowTemple_BossWin"}},
    {"Twinrova", "Dm_SpiritTemple_BossIn", "Dm_SpiritTemple_BossOut", 7, {}, {"SageOfSpirit_BossEnd"}},
    {"Ganondorf", "Dm_InsideGanonCastle_BossIn", "Dm_InsideGanonCastle_BossOut", -1, {},
     {"InsideGanonCastle_Dungeon_BossDie_1"}},
    // (Ganon's first half asks InsideGanonCastle_Dungeon_Boss_GetMasterSword at 1% life: mid-fight, not its death)
    {"Ganon", "Dm_FinalBattle_BossIn", "Dm_FinalBattle_BossOut", -1, {}, {"InsideGanonCastle_Dungeon_BossDie_2"}},
}};

const CoopBoss::Room* RoomOf(const Boss& b) {
    return b.room < 0 ? nullptr : CoopBoss::RoomByIndex(static_cast<std::uint8_t>(b.room));
}

// The death another player started, as our game is asked for it (player report #88: guests could not use the
// blue warp). A boss in a dungeon room: its own death setting (outro_asks), the entry of the boss's quest flow
// the boss's own logic asks (Gohma's InsideTheDekuTree_BossDie is Get_StoneOfForest's Boss_Win). That entry
// plays the death flow as a sub-event and moves the quest on in our game (EventExecuteProgressChallengeStep:
// DefeatBoss, GoToWarpPoint), and the room's logic makes the blue warp from that step (Logic
// InsideTheDekuTree_Level_Room_L_Common: Create_WarpPrefab, the Gimmick_Warp_FromDungeon module whose area asks
// the warp's event). The death flow alone (Dm_Deku_BossOut, a timeline) shows the warp in the cutscene but
// moves no quest, and the shared quest step waits until our Link is out of the dungeon (coop_hook.cpp): no
// warp to enter. Only in the boss room's own dungeon scene, and only when our game has the boss (Join); elsewhere (the boss rush, Ganondorf and Ganon, who
// have no room) the death flow, as before.
std::string_view DeathToAsk(const Boss& b, const std::optional<std::string>& scene) {
    const CoopBoss::Room* room = RoomOf(b);
    if (room == nullptr || !scene || *scene != room->scene || b.outro_asks[0].empty()) {
        return b.outro;
    }
    return b.outro_asks[0];
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
        const Boss& b = kBosses[i];
        const auto asks = [&](const std::array<std::string_view, 2>& list) {
            return std::find(list.begin(), list.end(), name) != list.end() && !name.empty();
        };
        if (name == b.intro || asks(b.intro_asks)) {
            return std::pair{static_cast<std::uint8_t>(i), Kind::intro};
        }
        if (name == b.outro || asks(b.outro_asks)) {
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

std::int32_t Director::Base(const Local& local) const {
    // Last tick's reading (before anything this tick), else this tick's; 0: unread.
    return max_life_.value_or(local.max_life.value_or(0));
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
    if (playing_->own_setting) {
        // The boss's own logic asked it: skipping it (the game's force skip) left its fight's start half done and
        // the player held in it (player report #30). It plays out; the boss is synced by its owner meanwhile.
        if (!std::exchange(playing_->kept_said, true)) {
            out.log.push_back(std::string(kBosses[playing_->boss].intro) + ": " + why +
                              ", but our game asked it as the boss's own start of the fight: not skipped");
        }
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
    // The death setting only when our game has the boss (our walk saw its body): a save that beat it already
    // (no boss here, its warp there) would get its Complete quest set back by the setting's step (King Dodongo's
    // and Barinade's do not ignore a completed quest), and its death signal would go to no boss (review-guest-warp).
    // Not while our copy still lives (player report #105, a black screen after Gohma's death in the host's game):
    // the setting's flow waits for the boss's own death. Our copy is released this tick (the glue's SetDying runs
    // after this Tick: its own changes made late, its Life step lets it die), so the setting waits up to
    // kCopyWaitUs for our walk to read it dead (WaitForCopy); then the death flow alone, and nothing more. Review
    // boss-0113 must-fix 1: without that, every guest's copy (always alive here) got the flow alone and #88 came
    // back at every boss.
    const bool copy_alive = local.boss_ours == false && local.boss_alive == true;
    const std::string_view setting = DeathToAsk(boss, local.scene);
    if (kind == Kind::outro && copy_alive && setting != boss.outro) {
        f.wait_copy_since = now_us;
        out.log.push_back(std::string(setting) + ": player " + std::to_string(f.outro_by) +
                          "'s boss died; our copy still lives: waiting for it to die here before asking it");
        return;
    }
    f.asked_at = now_us;
    out.play = std::string(kind == Kind::intro                         ? boss.intro
                           : local.boss_ours.has_value() && !copy_alive ? setting
                                                                        : boss.outro);
    out.log.push_back(*out.play + ": playing it here too (player " +
                      std::to_string(kind == Kind::intro ? f.starter : f.outro_by) + " started it)");
}

// Another's death heard while our copy lived (Join): the boss's own death setting once our walk reads the copy
// dead (it runs the boss's death, the quest step and the warp here, as in the owner's game); the death flow alone
// if it still lives after kCopyWaitUs. Either way one death cutscene: once the flow alone played nothing is asked
// again, and a later ask of the setting by our own game is refused (refuse_deaths; review-boss-0114 risk 3).
void Director::WaitForCopy(std::uint8_t b, const Local& local, std::uint64_t now_us, Actions& out) {
    Fight& f = fights_[b];
    const Boss& boss = kBosses[b];
    if (out.play || f.wait_copy_since == 0) {
        return;
    }
    if (!Here(b, local)) {
        f.wait_copy_since = 0;
        return;
    }
    if (local.boss_alive == false) {
        f.wait_copy_since = 0;
        f.asked_at = now_us;
        out.play = std::string(DeathToAsk(boss, local.scene));
        out.log.push_back(*out.play + ": our copy of the boss died here: asking its own death (player " +
                          std::to_string(f.outro_by) + " started it)");
    } else if (now_us - f.wait_copy_since >= kCopyWaitUs) {
        f.wait_copy_since = 0;
        f.asked_at = now_us;
        out.play = std::string(boss.outro);
        out.log.push_back(*out.play + ": playing it here too (player " + std::to_string(f.outro_by) +
                          " started it), the death flow alone: our copy of the boss still lives (its own death "
                          "setting would wait for it); one death cutscene here, nothing asked again");
    }
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
                out.refill = true;
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
            f.base = Base(local);  // our maximum when the boss died, wherever we are
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
    if (r.searched) {
        const Fight& f = fights_[b];
        const auto* room = RoomOf(kBosses[b]);
        const bool here = room != nullptr && local.scene && *local.scene == room->scene;
        const bool fresh = kind == Kind::intro ? f.phase == Phase::none : f.phase < Phase::outro;
        if (!here || !fresh || !r.taken) {
            return;  // a leftover name: nothing starts or skips because of it
        }
        if (kind == Kind::outro && local.boss_ours != true) {
            // Only the owner's game's death is the boss's: a copy waits for the owner's (its outro_start).
            out.log.push_back(std::string(kBosses[b].outro) + ": the boss died here, but we do not own it: "
                                                              "waiting for its owner's death");
            return;
        }
    }
    const std::string_view flow = kind == Kind::intro ? kBosses[b].intro : kBosses[b].outro;
    // The flow's name starts each line (the proof lines name it); the setting the game asked, when not the flow,
    // ends every line this call adds.
    const std::string label(flow);
    struct Suffix {
        std::vector<std::string>& log;
        std::size_t first;
        std::string text;
        ~Suffix() {
            for (std::size_t i = first; i < log.size() && !text.empty(); ++i) {
                log[i] += text;
            }
        }
    } suffix{out.log, out.log.size(), r.name == flow ? std::string() : " (asked as " + r.name + ")"};
    Fight& f = fights_[b];
    if (!r.taken) {
        if (kind == Kind::outro && f.death_played) {
            if (!std::exchange(f.refused_said, true)) {  // (its logic asks every frame: said once per fight)
                out.log.push_back(label + ": our game asked it again, not taken (refused at the request once its "
                                          "event is over): its death cutscene played here already (one per game)");
            }
            return;
        }
        out.log.push_back(label + ": our game did not take it");
        if (kind == Kind::outro && r.name != flow && f.asked_at != 0 && now_us - f.asked_at <= kJoinUs) {
            // Our ask of the boss's own death setting was refused: the death flow alone, once (the cutscene
            // still plays; the quest step and the warp then wait until our Link is out of the dungeon).
            f.asked_at = now_us;
            out.play = std::string(flow);
            out.log.push_back(label + ": asking the death flow alone instead (no quest step, no warp here)");
        }
        return;
    }
    const bool asked = f.asked_at != 0 && now_us - f.asked_at <= kJoinUs;
    f.asked_at = 0;
    if (kind == Kind::outro) {
        f.wait_copy_since = 0;  // our game runs the death now (asked, or its own logic's): nothing more to ask
        f.death_played = true;  // one death cutscene here: a later ask is refused (refuse_deaths)
    }
    playing_ = Playing{b, kind};
    playing_->own_setting = r.name != flow;
    if (kind == Kind::intro) {
        if (f.phase == Phase::none || f.phase == Phase::done) {
            f = Fight{};
            f.phase = Phase::intro;
            f.starter = local.self;
            f.scene = local.scene.value_or(std::string());
            f.starter_at = local.position;
            f.at = now_us;
            Say(out, b, proto::BossSceneKind::intro_start, local);
            out.log.push_back(label + ": our game started it: the others in the area play it too");
        } else if (f.phase == Phase::intro) {
            out.log.push_back(label + (asked ? ": playing with player " : ": ours started with player ") +
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
        f.base = Base(local);
        Say(out, b, proto::BossSceneKind::outro_start, local);
        out.log.push_back(label + ": our game started the death: the others in the area play it too");
    } else {
        out.log.push_back(label + (asked ? ": playing with player " : ": ours started with player ") +
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

    // Our boss event ended. Only once it was seen running: the door's own event can end on the very
    // tick our game takes the intro, before the intro runs.
    const bool fell = event_ && !local.event;
    event_ = local.event;
    if (fell && playing_ && playing_->seen) {
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
    if (playing_ && local.event) {
        playing_->seen = true;
    }

    for (std::size_t i = 0; i < fights_.size(); ++i) {
        Fight& f = fights_[i];
        const auto b = static_cast<std::uint8_t>(i);
        WaitForCopy(b, local, now_us, out);
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
        // The one heart container, in its beaten boss's area.
        const bool beaten_here = (f.phase == Phase::outro || f.phase == Phase::done) && Here(b, local);
        if (beaten_here && f.base == 0 && f.heart == 0 && max_life_) {
            f.base = *max_life_;  // the death was not heard (our maximum was unread then): from now
        }
        if (beaten_here && (f.base != 0 || f.heart != 0) && !out.max_life_cap) {
            // The flag watcher puts back, unsent, any rise of our own past this (a second pickup of
            // the container: the max rule would give it to everyone): one container on our maximum when
            // the boss died, or the server's answer once known, never below that maximum of ours.
            out.max_life_cap = f.heart != 0 ? std::max(f.heart, f.base) : f.base + kContainer;
        }
        if (beaten_here && local.max_life && max_life_ && *local.max_life > *max_life_ && !f.claimed &&
            local.max_life_from_others) {
            // Another game's container raised it (the shared MaxLife flag): not our pickup. Every game claimed then,
            // and the first claim to reach the server (not the player who took it) was named (2026-11-20).
            f.claimed = true;
            out.log.push_back(std::string(kBosses[b].name) + "'s heart container: our maximum " +
                              std::to_string(*local.max_life) + " came from another player's pickup: no claim");
        }
        if (beaten_here && local.max_life && max_life_ && *local.max_life > *max_life_ && !f.claimed) {
            f.claimed = true;
            if (*local.max_life > proto::kMaxBossHeartLife) {
                out.log.push_back(std::string(kBosses[b].name) + "'s heart container: our maximum " +
                                  std::to_string(*local.max_life) + " is past what a claim carries: none sent");
            } else {
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
    // One death cutscene per game (review-boss-0114 risk 3): while we are in its area and no event runs, a played
    // death is refused. (While its event runs the game's boss logic asks it again every frame and the manager
    // refuses those itself, as it always has: hs-gohma-death 20261115-191432 B, about 50 asks.)
    for (std::size_t i = 0; i < fights_.size(); ++i) {
        if (!local.event && fights_[i].death_played && Here(static_cast<std::uint8_t>(i), local)) {
            out.refuse_deaths |= static_cast<std::uint16_t>(1u << i);
        }
    }
    return out;
}

bool Director::DeathMayRun(std::uint8_t boss) const {
    if (boss >= fights_.size()) {
        return false;
    }
    if (event_ || (playing_ && playing_->boss == boss && playing_->kind == Kind::outro)) {
        return true;
    }
    const Fight& f = fights_[boss];
    if (f.wait_copy_since != 0 || (f.asked_at != 0 && last_now_ - f.asked_at <= kJoinUs)) {
        return true;
    }
    std::lock_guard lock(inbox_mutex_);
    return std::any_of(inbox_.begin(), inbox_.end(), [&](const proto::Message& m) {
        const auto* s = std::get_if<proto::BossScene>(&m);
        return s != nullptr && s->boss == boss && s->kind == proto::BossSceneKind::outro_start;
    });
}

std::atomic<std::uint16_t>& RefusedDeaths() {
    static std::atomic<std::uint16_t> mask{0};
    return mask;
}

bool RefuseAsk(std::string_view name) {
    const std::uint16_t mask = RefusedDeaths().load(std::memory_order_acquire);
    if (mask == 0) {
        return false;
    }
    const auto of = OfEvent(name);
    return of && of->second == Kind::outro && (mask >> of->first & 1u) != 0;
}

} // namespace CoopBossScenes
