// SPDX-License-Identifier: GPL-3.0-or-later
//
// Boss rooms together: the rooms and the rules (coop_boss.h,
// docs/dungeons-boss.md). No emulator code.

#include "coop_boss.h"
#include "coop_dungeon.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace CoopBoss {
namespace {

namespace proto = coop::protocol;

// Actor names of each boss and its parts (Pack/Actor), for the shared boss
// (docs/dungeons-boss.md, "One boss").
constexpr std::array<std::string_view, 2> kGohma{
    "Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Gohma_Egg"};
constexpr std::array<std::string_view, 6> kDodongo{
    "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Dodongo_Attention",
    "Enemy_DungeonBoss_Dodongo_Attention_RollingCenter", "Enemy_DungeonBoss_Dodongo_SpawnBabyDodongo",
    "DungeonBossDodongo_Bullet_Fire", "DungeonBossDodongo_Bullet_StrongFire"};
constexpr std::array<std::string_view, 5> kBarinade{
    "Enemy_DungeonBoss_Barinade", "Enemy_DungeonBoss_Barinade_Arm",
    "Enemy_DungeonBoss_Barinade_Attention", "Enemy_DungeonBoss_Barinade_Beam",
    "Enemy_DungeonBoss_Barinade_Jellyfish"};
constexpr std::array<std::string_view, 2> kPhantomGanon{
    "Enemy_DungeonBoss_PhantomGanon", "DungeonBossPhantomGanon_Bullet_LightBall"};
constexpr std::array<std::string_view, 5> kVolvagia{
    "Enemy_DungeonBoss_Volvagia", "Dm_Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_Appear_Sign",
    "DungeonBossVolvagia_FireBreath", "DungeonBossVolvagia_FireBreathField"};
constexpr std::array<std::string_view, 4> kMorpha{
    "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_Attention",
    "Enemy_DungeonBoss_Morpha_Attention_Spin", "Enemy_DungeonBoss_Morpha_Tentacle"};
constexpr std::array<std::string_view, 3> kBongoBongo{
    "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_LeftHand",
    "Enemy_DungeonBoss_BongoBongo_RightHand"};
constexpr std::array<std::string_view, 3> kTwinrova{
    "Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake"};

// From the RomFS (checked 2026-11-09, docs/dungeons-boss.md): the room is
// the boss's DefaultDungeonRoomIndex; centre, radius and height band cover
// that room's placements (heart container, warp point, the room's own
// props); `arrive` is the boss rush's StartPos for that boss in DreamWorld
// (`<Boss>Stage`), the place the game itself starts that fight from; `start`
// a StartPos of the dungeon's own Banc in that room.
const std::array<Room, 8> kRooms{{
    {0, "InsideTheDekuTree", "Enemy_DungeonBoss_Gohma", Era::child, 10, 7.5f, 67.2f, 37.0f, 43.0f, 63.0f,
     {6.7f, 47.0f, 67.6f}, "StartPos_Get_StoneOfForest", "Dm_Deku_BossIn", "Dm_Deku_BossOut", kGohma},
    {1, "DodongoCavern", "Enemy_DungeonBoss_Dodongo", Era::child, 22, 24.0f, -63.0f, 22.0f, 69.0f, 90.0f,
     {24.0f, 73.4f, -77.5f}, "StartPos_Get_StoneOfFire", "Dm_DodongoCave_BossIn",
     "Dm_DodongoCave_BossOut", kDodongo},
    {2, "InsideJabuJabuBelly", "Enemy_DungeonBoss_Barinade", Era::child, 15, 42.3f, -110.3f, 21.0f, 97.0f,
     118.0f, {42.4f, 102.0f, -98.1f}, "StartPos_StoneOfWater", "Dm_InsideJabu_Jabu_BossIn",
     "Dm_InsideJabu_Jabu_BossOut", kBarinade},
    {3, "ForestTemple", "Enemy_DungeonBoss_PhantomGanon", Era::adult, 29, 0.0f, -62.6f, 20.0f, 71.0f, 95.0f,
     {-5.6f, 74.7f, -65.8f}, "StartPos_SageOfForest", "Dm_ForestTemple_BossIn", "Dm_ForestTemple_BossOut",
     kPhantomGanon},
    {4, "FireTemple", "Enemy_DungeonBoss_Volvagia", Era::adult, 33, -82.0f, 0.0f, 38.0f, 88.0f, 125.0f,
     {-64.5f, 102.6f, 0.0f}, "StartPos_SageOfFire", "Dm_FireTemple_BossIn", "Dm_FireTemple_BossOut", kVolvagia},
    {5, "WaterTemple", "Enemy_DungeonBoss_Morpha", Era::adult, 23, -4.5f, -91.8f, 20.0f, 120.0f, 145.0f,
     {0.2f, 131.4f, -87.1f}, "StartPos_SageOfWater", "Dm_WaterTemple_BossIn", "Dm_WaterTemple_BossOut", kMorpha},
    {6, "ShadowTemple", "Enemy_DungeonBoss_BongoBongo", Era::adult, 24, -138.3f, 37.0f, 22.0f, -16.0f, 6.0f,
     {-138.3f, -11.5f, 22.0f}, "StartPos_SageOfShadow", "Dm_ShadowTemple_BossIn", "Dm_ShadowTemple_BossOut",
     kBongoBongo},
    {7, "SpiritTemple", "Enemy_DungeonBoss_Twinrova", Era::adult, 33, 0.0f, -157.8f, 38.0f, 139.0f, 165.0f,
     {-0.7f, 142.2f, -158.6f}, "", "Dm_SpiritTemple_BossIn", "Dm_SpiritTemple_BossOut", kTwinrova},
}};

constexpr float kArrivalSpread = 1.5f;

}  // namespace

std::span<const Room> Rooms() {
    return kRooms;
}

const Room* RoomByIndex(std::uint8_t index) {
    return index < kRooms.size() ? &kRooms[index] : nullptr;
}

std::span<const std::string_view> Bodies(const Room& room) {
    static constexpr std::array<std::string_view, 3> kTwinrovaBodies{
        "Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake"};
    if (room.boss == kTwinrovaBodies[0]) return kTwinrovaBodies;
    return {&room.boss, 1};
}

std::string_view ShutInFlag(const Room& room) {
    return room.index == 0 ? std::string_view("InsideTheDekuTree_BossArea_ShutIn") : std::string_view();
}

const Room* RoomOfScene(std::string_view scene) {
    for (const Room& r : kRooms) {
        if (r.scene == scene) {
            return &r;
        }
    }
    return nullptr;
}

bool Inside(const Room& room, const Vec& p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        return false;
    }
    const float dx = p.x - room.centre_x;
    const float dz = p.z - room.centre_z;
    return dx * dx + dz * dz <= room.radius * room.radius && p.y >= room.y_low && p.y <= room.y_high;
}

const Room* RoomAt(std::string_view scene, const Vec& p) {
    const Room* room = RoomOfScene(scene);
    return room != nullptr && Inside(*room, p) ? room : nullptr;
}

Vec ArrivalFor(const Room& room, std::uint8_t number) {
    Vec v = room.arrive;
    if (number >= 1 && number <= 4) {
        const float angle = static_cast<float>(number) * 1.5707963f;  // a quarter turn each
        v.x += kArrivalSpread * std::cos(angle);
        v.z += kArrivalSpread * std::sin(angle);
    }
    return v;
}

std::vector<std::string_view> StaleBossFlags::Check(const std::optional<std::string>& scene,
                                                    const std::optional<Vec>& position,
                                                    const std::function<bool(std::string_view)>& is_set) {
    if (!scene) {
        scene_.reset();  // loading: the scene after it (the same one too) arms it again
        return {};
    }
    if (!scene_ || *scene_ != *scene) {
        scene_ = scene;
        armed_ = true;
    }
    if (!armed_ || !position) {
        return {};
    }
    armed_ = false;  // once per scene load
    if (RoomAt(*scene, *position) != nullptr) {
        return {};  // in a boss room: maybe a real fight
    }
    std::vector<std::string_view> out;
    for (const std::string_view name : CoopDungeon::NotShared()) {
        if (is_set(name)) {
            out.push_back(name);
        }
    }
    return out;
}

const char* PhaseName(Sync::Phase phase) {
    switch (phase) {
    case Sync::Phase::none: return "none";
    case Sync::Phase::waiting: return "waiting";
    case Sync::Phase::intro: return "intro";
    case Sync::Phase::fight: return "fight";
    case Sync::Phase::outro: return "outro";
    case Sync::Phase::done: return "done";
    }
    return "?";
}

void Sync::Receive(const proto::Message& message) {
    if (const auto* m = std::get_if<proto::BossRoom>(&message)) {
        std::lock_guard lock(inbox_mutex_);
        if (inbox_.size() < 256) {
            inbox_.push_back(*m);
        }
    }
}

void Sync::Reset() {
    here_ = nullptr;
    phase_ = Phase::none;
    entered_at_ = sent_at_ = intro_at_ = skip_at_ = skip_since_ = 0;
    committed_ = false;
    skip_asked_ = false;
    event_ = false;
    last_now_ = 0;
    scene_.reset();
    defeated_.clear();
    pulls_ = 0;
    pulled_at_ = 0;
    gave_up_ = false;
    remotes_.clear();
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

void Sync::Take(const proto::BossRoom& m, std::uint64_t now_us) {
    const Room* room = RoomByIndex(m.room);
    if (room == nullptr || room->scene != m.scene || m.player_number == 0) {
        return;  // a room this build does not know
    }
    Remote& r = remotes_[m.player_number];
    if (r.scene != m.scene || r.room != m.room) {
        r = Remote{};
        r.scene = m.scene;
        r.room = m.room;
    }
    r.heard_at = now_us;
    switch (m.state) {
    case proto::BossState::in_room:
        r.in_room = true;
        break;
    case proto::BossState::intro_start:
        r.in_room = true;
        r.fighting = false;
        r.committed = true;
        break;
    case proto::BossState::intro_end:
        r.in_room = true;
        r.fighting = true;
        r.committed = true;
        break;
    case proto::BossState::left:
        r.in_room = false;
        r.fighting = false;
        r.committed = false;
        break;
    case proto::BossState::defeated:
        r.in_room = false;
        r.fighting = false;
        r.committed = false;
        if (scene_ && *scene_ == m.scene &&
            std::find(defeated_.begin(), defeated_.end(), m.room) == defeated_.end()) {
            defeated_.push_back(m.room);
        }
        break;
    }
}

void Sync::Defeat(Actions& out) {
    Say(out, *here_, proto::BossState::defeated);
    if (std::find(defeated_.begin(), defeated_.end(), here_->index) == defeated_.end()) {
        defeated_.push_back(here_->index);
    }
    out.log.push_back("boss beaten (" + std::string(here_->outro) + ")");
}

void Sync::Commit(Actions& out, const char* why) {
    if (committed_ || here_ == nullptr) {
        return;
    }
    committed_ = true;
    const bool fight = phase_ == Phase::fight || phase_ == Phase::outro;
    Say(out, *here_, fight ? proto::BossState::intro_end : proto::BossState::intro_start);
    out.log.push_back(std::string("past the door (") + why + "): the others are moved in");
}

void Sync::Say(Actions& out, const Room& room, proto::BossState state) {
    proto::BossRoom m;
    m.scene = std::string(room.scene);
    m.room = room.index;
    m.state = state;
    out.send.push_back(std::move(m));
}

bool Sync::FightActive(const Room& room, std::uint64_t now_us) const {
    for (const auto& [number, r] : remotes_) {
        // Only a player past the door (intro started, or fighting): one who is
        // only near the room pulls no one (issue 148).
        if (r.in_room && r.committed && r.room == room.index && r.scene == room.scene &&
            now_us - r.heard_at <= kFightQuietUs) {
            return true;
        }
    }
    return false;
}

bool Sync::OtherFinishedIntro(const Room& room, std::uint64_t now_us) const {
    for (const auto& [number, r] : remotes_) {
        if (r.in_room && r.fighting && r.room == room.index && r.scene == room.scene &&
            now_us - r.heard_at <= kFightQuietUs) {
            return true;
        }
    }
    return false;
}

Actions Sync::Tick(const Local& local, std::uint64_t now_us) {
    Actions out;
    // Server time can step back when its offset is estimated again; every
    // "since" below would then wrap.
    now_us = std::max(now_us, last_now_);
    last_now_ = now_us;
    {
        std::vector<proto::BossRoom> in;
        {
            std::lock_guard lock(inbox_mutex_);
            in.swap(inbox_);
        }
        for (const auto& m : in) {
            Take(m, now_us);
        }
    }
    // Nothing is decided while the game cannot be read (a load, a menu).
    if (!local.scene || !local.position) {
        return out;
    }
    if (!scene_ || *scene_ != *local.scene) {
        scene_ = local.scene;
        defeated_.clear();
        pulls_ = 0;
        gave_up_ = false;
    }
    const auto beaten = [this](const Room& room) {
        return std::find(defeated_.begin(), defeated_.end(), room.index) != defeated_.end();
    };
    const bool rose = local.event && !event_;
    const bool fell = !local.event && event_;
    event_ = local.event;

    // In or out of a boss room.
    const Room* room = RoomAt(*local.scene, *local.position);
    const bool shut_in = local.shut_in.value_or(false);
    if (room != here_) {
        // Put there by our own pull a moment ago: inside, not on the way.
        const bool moved_in = pulls_ > 0 && now_us - pulled_at_ < kPullEveryUs;
        if (here_ != nullptr) {
            Say(out, *here_, proto::BossState::left);
            out.log.push_back("left the boss room of " + std::string(here_->scene));
        }
        here_ = room;
        provisional_ = false;
        entered_at_ = now_us;
        intro_at_ = 0;
        skip_asked_ = false;
        committed_ = false;
        phase_ = Phase::none;
        if (here_ != nullptr) {
            pulls_ = 0;
            gave_up_ = false;
            if (beaten(*here_)) {
                phase_ = Phase::done;
                out.log.push_back("in the boss room of " + std::string(here_->scene) + " (boss beaten)");
            } else {
                phase_ = Phase::waiting;
                Say(out, *here_, proto::BossState::in_room);
                sent_at_ = now_us;
                out.log.push_back("in the boss room of " + std::string(here_->scene) + ": " +
                                  std::string(here_->boss));
                if (local.event) {  // came in with an event running: that is the intro
                    phase_ = Phase::intro;
                    intro_at_ = now_us;
                    out.log.push_back("intro started (" + std::string(here_->intro) + ")");
                    if (moved_in) {
                        Commit(out, "moved in");
                    }
                }
            }
        }
    } else if (here_ != nullptr) {
        switch (phase_) {
        case Phase::waiting:
            if (rose) {
                // Not yet past the door: a door opening or a text on the way
                // is an event too (issue 148). Commit() below says when.
                phase_ = Phase::intro;
                intro_at_ = now_us;
                out.log.push_back("intro started (" + std::string(here_->intro) + ")");
            } else if (now_us - entered_at_ >= kNoIntroUs) {
                // No event said we are past the door (we may stand on the way
                // to it): a fight here, but nobody is moved in for it (issue 148).
                phase_ = Phase::fight;
                fight_at_ = now_us;
                out.log.push_back("no intro: the fight has begun here (the others are not moved in)");
            }
            break;
        case Phase::intro:
            if (fell) {
                // Past the door? The game's shut-in flag where the room has
                // one; else an intro long enough (or skipped to follow).
                const bool past = committed_ || (local.shut_in.has_value()
                                                     ? *local.shut_in
                                                     : now_us - intro_at_ >= kMinIntroUs || skip_asked_);
                if (!past) {
                    phase_ = Phase::waiting;
                    entered_at_ = now_us;  // the wait for the intro starts again
                    intro_at_ = 0;
                    skip_asked_ = false;
                    out.log.push_back("that event was not the intro (not shut in the room): nobody is moved in");
                    break;
                }
                phase_ = Phase::fight;
                fight_at_ = now_us;
                if (committed_) {
                    Say(out, *here_, proto::BossState::intro_end);
                }
                out.log.push_back(skip_asked_ ? "intro ended (skipped to follow the others)" : "intro ended");
                Commit(out, local.shut_in.has_value() ? "shut in" : "the intro is over");
            }
            break;
        case Phase::fight:
            if (rose && !committed_ && local.boss_alive.value_or(true)) {
                // The fight began here with no intro and nobody past the door
                // (no event yet, or one that was not the intro): this event
                // may be the real intro, not the boss's death (review of
                // issue 148: a real intro 10 s after a door event was taken
                // for an outro, so nobody was pulled, and a 15 s one for the
                // boss's death).
                phase_ = Phase::intro;
                intro_at_ = now_us;
                out.log.push_back("intro started (" + std::string(here_->intro) + ", after the wait)");
            } else if (rose && now_us - fight_at_ < kMinFightUs) {
                out.log.push_back("an event right after the intro: the rest of it, not the boss's death");
            } else if (rose) {
                phase_ = Phase::outro;
                outro_at_ = now_us;
                if (local.boss_alive.has_value() && *local.boss_alive) {
                    // Maybe the death, maybe a grab: decided while it runs.
                    provisional_ = true;
                    out.log.push_back("an event in the fight; the boss is still there: waiting to see");
                } else {
                    Defeat(out);
                }
            }
            break;
        case Phase::outro:
            if (provisional_) {
                const bool gone = local.boss_alive.has_value() && !*local.boss_alive;
                const bool long_event = now_us - outro_at_ >= kLongEventUs;
                if (gone || (long_event && local.event)) {
                    provisional_ = false;
                    Defeat(out);
                    if (!gone) out.log.back() += " after a long event";
                } else if (fell) {
                    provisional_ = false;
                    if (!local.boss_alive.has_value()) {
                        Defeat(out);  // no reading at the end: as before
                    } else {
                        phase_ = Phase::fight;
                        out.log.push_back("the event is over and the boss is alive: the fight goes on");
                        break;
                    }
                }
            }
            if (fell && !provisional_ && phase_ == Phase::outro) {
                phase_ = Phase::done;
            }
            break;
        case Phase::none:
        case Phase::done:
            break;
        }
        // The game says the door has shut behind us (issue 148).
        if (shut_in && phase_ != Phase::done && !(phase_ == Phase::outro && !provisional_)) {
            Commit(out, "shut in");
        }
        // A defeat heard from another game while we fight: stop the heartbeat.
        if ((phase_ == Phase::waiting || phase_ == Phase::fight) && beaten(*here_)) {
            phase_ = Phase::done;
            out.log.push_back("boss beaten in another game");
        }
    }

    if (here_ != nullptr) {
        // Leave the intro with the others: skip ours once another player in
        // this room has finished theirs.
        if (phase_ == Phase::intro && local.event && OtherFinishedIntro(*here_, now_us)) {
            if (!skip_asked_) {
                skip_asked_ = true;
                skip_since_ = now_us;
                skip_at_ = 0;
                out.log.push_back("another player's intro is over: skipping ours");
            }
            if (now_us - skip_since_ <= kSkipForUs && (skip_at_ == 0 || now_us - skip_at_ >= kSkipEveryUs)) {
                out.skip_intro = true;
                skip_at_ = now_us;
            }
        }
        const bool fighting = phase_ == Phase::waiting || phase_ == Phase::intro || phase_ == Phase::fight ||
                              (phase_ == Phase::outro && provisional_);
        if (fighting && now_us - sent_at_ >= kSendEveryUs) {
            // The heartbeat says how far we are, so a game that joins late
            // knows whether to move in (issue 148).
            const auto state = !committed_ ? proto::BossState::in_room
                               : phase_ == Phase::intro || phase_ == Phase::waiting ? proto::BossState::intro_start
                                                                                    : proto::BossState::intro_end;
            Say(out, *here_, state);
            sent_at_ = now_us;
        }
        return out;
    }

    // Not in a boss room: follow a fight in this dungeon.
    const Room* dungeon = RoomOfScene(*local.scene);
    if (dungeon == nullptr || beaten(*dungeon) || !FightActive(*dungeon, now_us)) {
        pulls_ = 0;
        gave_up_ = false;
        return out;
    }
    if (gave_up_ || (pulls_ > 0 && now_us - pulled_at_ < kPullEveryUs)) {
        return out;
    }
    if (pulls_ >= kPullTries) {
        gave_up_ = true;
        out.log.push_back("could not move into the boss room of " + std::string(dungeon->scene) +
                          " after " + std::to_string(kPullTries) + " tries; giving up for this fight");
        return out;
    }
    ++pulls_;
    pulled_at_ = now_us;
    out.pull = Pull{dungeon, ArrivalFor(*dungeon, local.self), local.event};
    out.log.push_back("another player is in the boss room of " + std::string(dungeon->scene) +
                      ": moving in (try " + std::to_string(pulls_) + ")");
    return out;
}

} // namespace CoopBoss
