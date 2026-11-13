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
};

// The ten bosses, by BOSS_SCENE's boss index (0 to 7: CoopBoss::Rooms() order).
std::span<const Boss> Bosses();
// The boss and kind of an event name, or nothing.
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
};

// One of our game's own event requests since the last tick.
struct Request {
    std::string name;
    bool taken = false;  // the event manager took it
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
    // While we are in a beaten boss's area: our maximum may not rise past this by our own game
    // (FlagWatcher::cap_max); nothing: no cap.
    std::optional<std::int32_t> max_life_cap;
    std::vector<ChatLine> chat;
    std::vector<std::string> log;
};

class Director {
public:
    static constexpr std::uint64_t kJoinUs = 3'000'000;      // a start we could not join in time is missed
    static constexpr std::uint64_t kIntroMaxUs = 90'000'000;  // no intro end heard: nobody waits
    static constexpr std::uint64_t kSkipEveryUs = 500'000;
    static constexpr std::uint64_t kSkipForUs = 6'000'000;
    static constexpr std::uint64_t kMoveEveryUs = 3'000'000;
    static constexpr unsigned kMoveTries = 3;
    static constexpr std::int32_t kContainer = 4;  // one heart container: 4 quarter hearts
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
        std::int32_t base = 0;     // our maximum when the boss died in our area (0: not yet)
        std::int32_t heart = 0;    // the server's answer (0: none yet)
        std::uint8_t heart_by = 0;
    };
    struct Playing {
        std::uint8_t boss = 0;
        Kind kind = Kind::intro;
        bool skip = false;
        bool seen = false;  // our game has run it (its end counts only then)
        std::uint64_t skip_since = 0, skip_at = 0;
    };
    void Take(const coop::protocol::Message& m, const Local& local, std::uint64_t now_us, Actions& out);
    void Ours(const Request& r, const Local& local, std::uint64_t now_us, Actions& out);
    void Join(std::uint8_t b, Kind kind, const Local& local, std::uint64_t now_us, Actions& out);
    bool Here(std::uint8_t b, const Local& local) const;
    void Say(Actions& out, std::uint8_t b, coop::protocol::BossSceneKind kind, const Local& local) const;
    void Skip(const char* why, std::uint64_t now_us, Actions& out);

    std::array<Fight, coop::protocol::kMaxSceneBoss + 1> fights_{};
    std::optional<Playing> playing_;  // the boss event our game runs now
    bool event_ = false;
    std::optional<std::int32_t> max_life_;  // last tick's
    std::uint64_t last_now_ = 0;

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

const char* PhaseName(Director::Phase phase);

} // namespace CoopBossScenes
