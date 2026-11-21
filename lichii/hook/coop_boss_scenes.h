// SPDX-License-Identifier: GPL-3.0-or-later
//
// Boss intro and death cutscenes for everyone in the boss's area
// (docs/dungeons-boss.md "Boss cutscenes"). Pure: coop_boss_game.cpp gives it
// our game's own event requests (Hookshot's event request wrap, by name) and
// our Link, and does what it says: asks our game for an event by name (the
// live-event requester: the game's own event start), the game's own skip, the
// boss pull's move. Tested in tests/lichii/boss_scenes_test.cpp.
//
// - The area of a boss is its room and the room before its door: the room's
//   circle (CoopBoss::Inside) grown by kAnteGrow, its height band by kAnteHigh.
//   Elsewhere (Ganondorf and Ganon have no room in the table; the boss rush's
//   stages, GohmaStage, DodongoStage...) it is the starter's scene within
//   kNoRoomRange of the starter, and nobody is moved.
// - Intro: the first game whose own game takes the boss's intro event is its
//   starter and sends BOSS_SCENE intro_start. Every game in the area asks its
//   game for the same event at once, and each joiner is put at its own spot
//   of the room's start (ArrivalFor; the room's circle takes in the way to the
//   door, so nobody is left behind it). Two games starting together: the lower player
//   number is the starter in every game.
// - Skip: every intro ends when the starter's ends (played out or skipped):
//   the others' games skip the rest of theirs. A player who skips alone just
//   waits in the room; the boss starts with the starter's (its owner's phase).
//   So "all skipped" and "the starter skipped" both end it for all.
// - A game whose own intro starts while the area's fight is on (it came in
//   late, or could not join in time) skips it at once: straight into the fight.
//   One in the area but before the door is moved in.
// - Outside the area: no cutscene, no move, one chat line.
// - Death: the first game whose game takes the boss's death event sends
//   outro_start; every game in the area asks its game for it. Outside: a chat
//   line; the progress comes by the shared flags.
// - Heart container (one for all): our maximum life rising in a beaten boss's
//   area is a pickup: a BOSS_HEART claim. The server's answer (the first claim
//   it got) raises every game to that maximum, once. While in that area our
//   own maximum may not rise past (our maximum when the boss died + one
//   container, or the answer once known): the flag watcher puts such a rise
//   back unsent (FlagWatcher::cap_max), so a second pickup never reaches the
//   others through the max rule.
// - A boss beaten in this session: another game's new intro of it is not
//   joined (a refight on another save in the same session plays alone).
// - The starter leaving during the intro, or no end heard for kIntroMaxUs:
//   nobody waits for it (each intro plays to its own end).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_boss.h"

namespace CoopBossScenes {

using Vec = CoopBoss::Vec;

enum class Kind : std::uint8_t { intro, outro };

// One boss whose intro and death are events.
struct Boss {
    std::string_view name;   // for chat lines
    std::string_view intro;  // the intro's event flow
    std::string_view outro;  // the death's
    int room = -1;           // CoopBoss::RoomByIndex, or -1: no room in the table
    // The event settings the boss's own logic asks the game for instead (RSDB EventSetting, read from each boss's
    // DungeonBossCommonParam EventStartParameters, 2026-11-14): e.g. Gohma's "Appear" asks
    // InsideTheDekuTree_BossAppear (Get_StoneOfForest's Boss_Appear entry), whose flow plays Dm_Deku_BossIn as a
    // sub-event the request wrap never sees. Empty: none.
    std::array<std::string_view, 2> intro_asks{};
    std::array<std::string_view, 2> outro_asks{};
};

inline constexpr std::size_t kBossCount = 10;
static_assert(kBossCount <= coop::protocol::kMaxSceneBoss + 1u);

// The ten bosses, by BOSS_SCENE's boss index (0 to 7: CoopBoss::Rooms() order).
std::span<const Boss> Bosses();
// The boss and kind of an event name (its flow or one of the settings its logic asks), or nothing.
std::optional<std::pair<std::uint8_t, Kind>> OfEvent(std::string_view name);

inline constexpr float kAnteGrow = 14.0f;      // the room before the door, around the room's circle
inline constexpr float kAnteHigh = 6.0f;
inline constexpr float kNoRoomRange = 30.0f;   // Ganondorf, Ganon: from the starter

// Whether `p` in `scene` is in the area of boss `b`, whose cutscene started in
// `boss_scene` with its starter at `starter`.
bool InArea(const Boss& b, std::string_view boss_scene, const std::optional<Vec>& starter, std::string_view scene,
            const Vec& p);
// Whether `p` is inside the boss room itself (no room in the table: InArea).
bool InRoom(const Boss& b, std::string_view boss_scene, const std::optional<Vec>& starter, std::string_view scene,
            const Vec& p);

// Our game, this tick.
struct Local {
    std::optional<std::string> scene;
    std::optional<Vec> position;
    bool event = false;                  // an event runs in our game
    std::uint8_t self = 0;               // our player number (0 unknown)
    std::optional<std::int32_t> max_life;  // PlayerStatus.MaxLife as our game has it
    // That maximum came from another game's pickup (the shared MaxLife flag put into our Link): no claim of ours.
    bool max_life_from_others = false;
    // Whether our game owns the boss (the object walk): a death seen with no name is taken only from the owner,
    // so a copy that dies in its own game (its hit judged lethal there, refused by the owner) starts nothing.
    std::optional<bool> boss_ours;
    // Whether the room's boss lives in our game (the object walk: life above 0; nothing when not seen). Another's
    // death is asked as the boss's own death setting only once our copy does not (player report #105).
    std::optional<bool> boss_alive;
};

// One of our game's own event requests since the last tick.
struct Request {
    std::string name;
    bool taken = false;  // the event manager took it
    // Seen with no request naming it (the older rules' reading of our event in the boss room): taken only for a fresh intro (no fight
    // known, none beaten) or a death of a fight not already over, in the boss's own scene; never anything else
    // (a name left in the event session must not restart or skip anything).
    bool searched = false;
};

struct ChatLine {
    std::uint8_t player = 0;  // the hook puts the player's name first
    std::string text;
};

struct Actions {
    std::vector<coop::protocol::BossScene> send;
    std::vector<coop::protocol::BossHeart> claims;
    std::optional<std::string> play;       // ask our game for this event (by name)
    std::optional<Vec> move_to;            // put our Link here (in the boss room)
    bool skip = false;                     // the game's own skip of the event that runs
    std::optional<std::int32_t> set_max_life;  // write our maximum life (the server's answer)
    // Another player took the boss's heart container: our hearts filled as a pickup of it would (once per boss).
    // Without it a game that did not pick it up got the heart but kept its life (four players, 2026-11-20).
    bool refill = false;
    // While we are in a beaten boss's area: our maximum may not rise past this by our own game
    // (FlagWatcher::cap_max); nothing: no cap.
    std::optional<std::int32_t> max_life_cap;
    std::vector<ChatLine> chat;
    std::vector<std::string> log;
    // Review-boss-0114 risk 3: one death cutscene per game. Bit b: boss b's death (its flow or a setting) played in
    // our game, our Link is still in its area and no event runs, so any later ask of it by our game is refused at
    // the request (RefuseAsk: the manager's own "not taken"), whoever asks (its own death logic, late). While an
    // event runs the manager refuses such asks itself (the boss's logic asks every frame during its death).
    std::uint16_t refuse_deaths = 0;
};
static_assert(kBossCount <= 16);

// The glue publishes the last Tick's refuse_deaths here; the game's event request (any game thread) reads it.
std::atomic<std::uint16_t>& RefusedDeaths();
// Whether our game's ask of `name` is refused now: the death (flow or setting) of a boss in RefusedDeaths.
bool RefuseAsk(std::string_view name);

class Director {
public:
    static constexpr std::uint64_t kJoinUs = 3'000'000;      // a start we could not join in time is missed
    static constexpr std::uint64_t kIntroMaxUs = 90'000'000;  // no intro end heard: nobody waits
    static constexpr std::uint64_t kSkipEveryUs = 500'000;
    static constexpr std::uint64_t kSkipForUs = 6'000'000;
    static constexpr std::uint64_t kMoveEveryUs = 3'000'000;
    static constexpr unsigned kMoveTries = 3;
    static constexpr std::int32_t kContainer = 4;  // one heart container: 4 quarter hearts
    // Another's death while our copy lives: how long the setting waits for our released copy to read dead before
    // the death flow alone. A fault guard, not the usual path: the released copy is given its whole life on the next
    // share (two frames) and its Life step goes past its own stops (patch 0007 LIFE_STOPS), so it reads dead in a
    // few frames; only a game without that hook (a stop holds it) or a copy off the walk gets here.
    static constexpr std::uint64_t kCopyWaitUs = 1'500'000;
    static constexpr float kNearStart = 6.0f;  // during the intro a joiner this near its spot is not moved

    // Network thread: BOSS_SCENE, BOSS_HEART, PLAYER_LEFT. Taken on the next Tick.
    void Receive(const coop::protocol::Message& message);
    // Present thread, a few times a second.
    Actions Tick(const Local& local, const std::vector<Request>& ours, std::uint64_t now_us);
    // Disconnected: every fight forgotten (a new session starts again).
    void Reset();

    enum class Phase : std::uint8_t { none, intro, fight, outro, done };
    Phase phase(std::uint8_t boss) const { return boss < fights_.size() ? fights_[boss].phase : Phase::none; }
    std::uint8_t starter(std::uint8_t boss) const { return boss < fights_.size() ? fights_[boss].starter : 0; }
    // Row 136: whether our game's own death of `boss` may still run here: an outro_start of it not yet taken (the
    // next Tick joins it), our ask of it within kJoinUs, its event playing, the wait for our copy to read dead, or
    // any event running in our game (last Tick). Otherwise nothing of ours will run it: CoopBossBeaten::Apply gives
    // the boss's beaten set in its area then. Present thread (the inbox under its lock).
    bool DeathMayRun(std::uint8_t boss) const;

private:
    struct Fight {
        Phase phase = Phase::none;
        std::uint8_t starter = 0;  // the intro's starter (player number)
        std::uint8_t outro_by = 0;
        std::string scene;
        std::optional<Vec> starter_at;
        std::uint64_t at = 0;      // when the phase began
        std::uint64_t asked_at = 0;  // we asked our game for its event (0: not)
        bool chatted = false;
        unsigned moves = 0;
        std::uint64_t moved_at = 0;
        bool claimed = false;      // we sent our heart claim (or our pickup was seen)
        std::int32_t base = 0;     // our maximum when the boss's death was heard or started (0: unread)
        std::int32_t heart = 0;    // the server's answer (0: none yet)
        std::uint8_t heart_by = 0;
        std::uint64_t wait_copy_since = 0;  // the setting waits for our copy to read dead (0: not)
        bool death_played = false;          // our game took its death (flow or setting): never a second one here
        bool refused_said = false;          // a late ask of it was logged (once per fight)
    };
    struct Playing {
        std::uint8_t boss = 0;
        Kind kind = Kind::intro;
        bool skip = false;
        bool seen = false;  // our game has run it (its end counts only then)
        // Our game asked it as the boss's own setting (Gohma's InsideTheDekuTree_BossAppear), not the flow:
        // the fight's own start in our game, never skipped (boss-fix, player report #30).
        bool own_setting = false;
        bool kept_said = false;  // logged once that it is not skipped
        std::uint64_t skip_since = 0, skip_at = 0;
    };
    void Take(const coop::protocol::Message& m, const Local& local, std::uint64_t now_us, Actions& out);
    void Ours(const Request& r, const Local& local, std::uint64_t now_us, Actions& out);
    void Join(std::uint8_t b, Kind kind, const Local& local, std::uint64_t now_us, Actions& out);
    void WaitForCopy(std::uint8_t b, const Local& local, std::uint64_t now_us, Actions& out);
    bool Here(std::uint8_t b, const Local& local) const;
    std::int32_t Base(const Local& local) const;
    void Say(Actions& out, std::uint8_t b, coop::protocol::BossSceneKind kind, const Local& local) const;
    void Skip(const char* why, std::uint64_t now_us, Actions& out);

    std::array<Fight, kBossCount> fights_{};  // one per Bosses() entry
    std::optional<Playing> playing_;  // the boss event our game runs now
    bool event_ = false;
    std::optional<std::int32_t> max_life_;  // last tick's
    std::uint64_t last_now_ = 0;

    mutable std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

const char* PhaseName(Director::Phase phase);

} // namespace CoopBossScenes
