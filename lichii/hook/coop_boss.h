// SPDX-License-Identifier: GPL-3.0-or-later
//
// Boss rooms together (issue 109, docs/dungeons-boss.md). No emulator code:
// coop_boss_game.cpp reads the game and moves Link; this decides what to send
// and when to move or skip. Tested in tests/lichii/boss_test.cpp.
//
// - A boss room is a room of its dungeon's own scene (the dungeon Banc), not
//   a scene of its own, so "in the boss room" is a test on where Link stands:
//   the dungeon's scene and a circle and height band around the room, taken
//   from the room's placements in the RomFS (Rooms()).
// - A game whose Link is in a boss room sends BOSS_ROOM in_room at once and
//   every 2 s while there. Another game in the same dungeon scene whose Link
//   is not in that room is moved in (Pull), again every 3 s up to 5 times;
//   this is also what puts back a player who joins or respawns during the
//   fight (the game respawns a dead Link at the dungeon's entrance, the same
//   scene).
// - The first event that starts in the room is the intro (intro_start); when
//   it ends, intro_end. A game whose own intro still runs once any other
//   player in that room has finished theirs skips the rest of it (the game's
//   own force skip), so all leave the intro together.
// - The next event in the room after the fight began is the boss's death
//   (outro): defeated. Nobody is pulled into that room again until they leave
//   the dungeon's scene.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopBoss {

struct Vec {
    float x = 0.0f;
    float y = 0.0f;  // height
    float z = 0.0f;
};

enum class Era : std::uint8_t { child, adult };

// One boss room. Positions are world units of the dungeon's scene (the
// `Translate` of the placements in Banc/<scene>/<scene>_{Static,Dynamic}).
struct Room {
    std::uint8_t index = 0;        // BOSS_ROOM's room field
    std::string_view scene;        // the dungeon's scene (Banc) name
    std::string_view boss;         // the boss actor
    Era era = Era::child;
    int game_room = 0;             // the placements' DefaultDungeonRoomIndex
    float centre_x = 0.0f;         // the room: a circle on the ground...
    float centre_z = 0.0f;
    float radius = 0.0f;
    float y_low = 0.0f;            // ...and a height band
    float y_high = 0.0f;
    Vec arrive;                    // where a moved player is put (the boss rush's own start)
    std::string_view start;        // a StartPos in that room of the scene, or "" when none is
    std::string_view intro;        // the intro event flow
    std::string_view outro;        // the boss's death
    std::span<const std::string_view> parts;  // actor names of the boss and its parts
};

// The eight boss rooms, by index: 0 to 2 child, 3 to 7 adult.
std::span<const Room> Rooms();
const Room* RoomByIndex(std::uint8_t index);
// The boss room of a dungeon scene, or null.
const Room* RoomOfScene(std::string_view scene);
// Whether a point of that room's scene is in the room.
bool Inside(const Room& room, const Vec& p);
// The room `p` is in, in scene `scene`, or null.
const Room* RoomAt(std::string_view scene, const Vec& p);
// Where player `number` (1 to 4) is put in a room: the room's arrival point,
// moved 1.5 units around it by player number so two moved players do not
// stand in each other.
Vec ArrivalFor(const Room& room, std::uint8_t number);

// What the game should do now.
struct Pull {
    const Room* room = nullptr;
    Vec to;
    bool by_event = false;  // our own event runs: the game's scene change can take it
};

struct Actions {
    std::vector<coop::protocol::BossRoom> send;
    std::optional<Pull> pull;
    bool skip_intro = false;  // force skip the event that runs now
    std::vector<std::string> log;
};

// This game's Link, read this frame.
struct Local {
    std::optional<std::string> scene;  // nothing while loading
    std::optional<Vec> position;
    bool event = false;                // an event (cutscene) runs
    std::uint8_t self = 0;             // our player number, 1 to 4 (0 unknown)
};

class Sync {
public:
    static constexpr std::uint64_t kSendEveryUs = 2'000'000;
    static constexpr std::uint64_t kFightQuietUs = 6'000'000;   // no in_room for this long: over
    static constexpr std::uint64_t kPullEveryUs = 3'000'000;
    static constexpr unsigned kPullTries = 5;
    static constexpr std::uint64_t kSkipEveryUs = 500'000;
    static constexpr std::uint64_t kSkipForUs = 6'000'000;
    // In the room this long without an event: the intro was not played
    // (seen before, for example), so the fight has begun.
    static constexpr std::uint64_t kNoIntroUs = 10'000'000;

    // Network thread: BOSS_ROOM from the others. Taken on the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, a few times a second.
    Actions Tick(const Local& local, std::uint64_t now_us);

    void Reset();

    // For the debug overlay and tests.
    enum class Phase : std::uint8_t { none, waiting, intro, fight, outro, done };
    Phase phase() const { return phase_; }

private:
    struct Remote {
        std::string scene;
        std::uint8_t room = 0;
        bool in_room = false;   // last heard in the room
        bool fighting = false;  // their intro is over (or never ran)
        std::uint64_t heard_at = 0;
    };
    void Take(const coop::protocol::BossRoom& m, std::uint64_t now_us);
    void Say(Actions& out, const Room& room, coop::protocol::BossState state);
    bool FightActive(const Room& room, std::uint64_t now_us) const;
    bool OtherFinishedIntro(const Room& room, std::uint64_t now_us) const;

    // Our side.
    const Room* here_ = nullptr;
    Phase phase_ = Phase::none;
    std::uint64_t entered_at_ = 0;
    std::uint64_t sent_at_ = 0;
    std::uint64_t intro_at_ = 0;
    std::uint64_t skip_at_ = 0;
    bool skip_asked_ = false;
    std::uint64_t skip_since_ = 0;
    bool event_ = false;  // last Tick's event
    std::optional<std::string> scene_;  // the scene of the last Tick with one
    std::vector<std::uint8_t> defeated_;  // rooms beaten while we were in this scene
    // Pulling.
    unsigned pulls_ = 0;
    std::uint64_t pulled_at_ = 0;
    bool gave_up_ = false;

    std::map<std::uint8_t, Remote> remotes_;  // by player number

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::BossRoom> inbox_;
};

const char* PhaseName(Sync::Phase phase);

// The emulator side (coop_boss_game.cpp). LICHII_COOP_BOSS=0 turns it off;
// LICHII_COOP_BOSS_MOVE=matrix (default: auto) never uses the scene change;
// LICHII_COOP_DEBUG_BOSS=1 logs the phase each second.
//
// Present thread, once per presented frame. `main_base` 0 when the game is
// not up; `session` null when not connected.
void Frame(Core::System& system, std::uint64_t main_base, const std::optional<std::string>& scene,
           const std::optional<coop::protocol::Vec3>& position, bool event_running,
           coop::host::Session* session, std::uint64_t server_now_us);

// Network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

} // namespace CoopBoss
