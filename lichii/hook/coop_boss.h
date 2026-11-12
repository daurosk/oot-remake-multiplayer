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
// - A game whose Link is in a boss room sends BOSS_ROOM at once and every 2 s
//   while there, saying how far it is: in_room (only near the room: its
//   circle also takes in the way to the door), intro_start (its intro runs
//   and it is past the door: its game's shut-in flag is set), intro_end (the
//   fight, once past the door). Until a game is past the door it sends only
//   in_room (issue 148: an event on the way, such as the door
//   opening, must not move anyone in). Past the door is the room's shut-in
//   flag (ShutInFlag) where one is known; else the end of an intro of at
//   least kMinIntroUs with Link still in the room; or having been moved in.
//   Another game in the same dungeon scene whose Link is not in that room is
//   moved in (Pull), again every 3 s up to 5 times, but only once a player
//   there has started the intro or the fight: never on in_room alone (issue
//   148: B was moved in when A came near the room; the door shut in B's game
//   and A, still on the way, was locked out). This is also what puts back a
//   player who joins or respawns during the fight (the game respawns a dead
//   Link at the dungeon's entrance, the same scene). A room entered with no
//   intro for kNoIntroUs counts as a fight here, but moves no one in: no
//   event says the player is past the door.
// - The first event that starts in the room is the intro (intro_start); when
//   it ends, intro_end. A game whose own intro still runs once any other
//   player in that room has finished theirs skips the rest of it (the game's
//   own force skip), so all leave the intro together.
// - The next event in the room after the fight began may be the boss's death
//   (outro). When the game says whether the boss is alive (Local::boss_alive,
//   from the object walk), that event is only taken as the death once the
//   boss is gone or at 0 life, or once it has run kLongEventUs; an event that
//   ends with the boss alive (a grab, a phase change) goes back to the fight,
//   so the others are still pulled in (issue 109: a walk-out after such an
//   event was not pulled back, and its false "defeated" stopped every game).
//   Without that reading, the first event is the death, as before. After a
//   real defeat nobody is pulled into that room again until they leave the
//   dungeon's scene: walking out of a beaten boss room is allowed.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
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

// The actors that are the boss itself, for "is it alive" (issue 109): the
// boss actor, and for Twinrova also Koume and Kotake (its first phase). Not
// its eggs, minions, hands, tentacles or shots.
std::span<const std::string_view> Bodies(const Room& room);

// The game's own flag that says Link is shut in that boss room (the door has
// shut behind Link), or "" when none is known. Issue 148: only a player whose
// game says this moves the others in. Only the Deku Tree's is known
// (InsideTheDekuTree_BossArea_ShutIn, the flag that locked players out when
// it was shared); for the other rooms the end of the intro stands for it.
std::string_view ShutInFlag(const Room& room);

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
    // The dungeon's boss as the object walk saw it last: true while its actor
    // is there with life left, false once it is gone or at 0 life, nothing
    // when the walk does not run (LICHII_COOP_OBJECTS off, or stale).
    std::optional<bool> boss_alive;
    // ShutInFlag(room) of the scene's boss room as the game has it: nothing
    // when the room has no such flag or it cannot be read (issue 148).
    std::optional<bool> shut_in;
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
    // An event in the fight this long is the boss's death even when the walk
    // still sees the boss (a dead boss actor may stay with its life unread).
    static constexpr std::uint64_t kLongEventUs = 15'000'000;
    // An event this soon after the fight began is never the boss's death: the
    // rest of an intro (a skipped one plays its next shot), not an outro
    // (integrate-p8: B skipped its intro and 0.1 s later took the next event
    // for Dm_..._BossOut, so the fight counted as won with nobody fighting).
    static constexpr std::uint64_t kMinFightUs = 10'000'000;
    // An event shorter than this near a boss room with no shut-in flag is not
    // its intro (a door opening, a text, Navi): it moves no one in (issue 148).
    static constexpr std::uint64_t kMinIntroUs = 3'000'000;

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
        bool committed = false; // their intro started or their fight is on: they are past the door (issue 148)
        std::uint64_t heard_at = 0;
    };
    void Take(const coop::protocol::BossRoom& m, std::uint64_t now_us);
    void Defeat(Actions& out);  // our game saw the boss's death
    void Say(Actions& out, const Room& room, coop::protocol::BossState state);
    bool FightActive(const Room& room, std::uint64_t now_us) const;
    bool OtherFinishedIntro(const Room& room, std::uint64_t now_us) const;
    void Commit(Actions& out, const char* why);  // we are past the door (issue 148)

    // Our side.
    const Room* here_ = nullptr;
    bool provisional_ = false;   // outro from an event while the boss may be alive
    std::uint64_t outro_at_ = 0;
    std::uint64_t fight_at_ = 0;  // when our fight phase began (kMinFightUs)
    Phase phase_ = Phase::none;
    std::uint64_t entered_at_ = 0;
    std::uint64_t sent_at_ = 0;
    std::uint64_t intro_at_ = 0;
    std::uint64_t skip_at_ = 0;
    bool skip_asked_ = false;
    std::uint64_t skip_since_ = 0;
    bool event_ = false;  // last Tick's event
    bool committed_ = false;  // we are past the door: shut in, or our intro is over (issue 148)
    std::uint64_t last_now_ = 0;  // the clock never goes back (server time can)
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

// Issue 148: an older build shared the boss fight's "shut in" flags, so saves made then can hold a
// shut boss door for good. Once per scene load, at the first frame where Link's place is
// known and Link is in no boss room, the names of CoopDungeon::NotShared() flags that are
// still set are given back to be cleared (0) in this game. Inside a boss room they are left
// alone (a real fight). Pure: the hook reads and writes the flags.
class StaleBossFlags {
public:
    std::vector<std::string_view> Check(const std::optional<std::string>& scene,
                                        const std::optional<Vec>& position,
                                        const std::function<bool(std::string_view)>& is_set);

private:
    std::optional<std::string> scene_;
    bool armed_ = false;
};

// The emulator side (coop_boss_game.cpp). LICHII_COOP_BOSS=0 turns it off;
// LICHII_COOP_BOSS_MOVE=matrix (default: auto) never uses the scene change;
// LICHII_COOP_DEBUG_BOSS=1 logs the phase each second.
//
// Present thread, once per presented frame. `main_base` 0 when the game is
// not up; `session` null when not connected.
// `shut_in`: Local::shut_in, read by the hook from the game's flags.
void Frame(Core::System& system, std::uint64_t main_base, const std::optional<std::string>& scene,
           const std::optional<coop::protocol::Vec3>& position, bool event_running,
           std::optional<bool> shut_in, coop::host::Session* session, std::uint64_t server_now_us);

// Network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

// For /teleport (coop_teleport.h), present thread:
// - our boss fight runs (its intro, fight or outro phase);
bool Fighting();
// - the game's scene change, as the pull into a boss room asks it (taken
//   only while an event runs); an empty `start` is the scene's own start, an
//   empty `scene` clears an earlier request;
bool AskSceneChange(Core::System& system, std::uint64_t main_base, std::string_view scene,
                    std::string_view start);
// - our Link moved to `to` in this scene, the boss pull's free-play move.
bool MoveLink(Core::System& system, std::uint64_t main_base, const Vec& to);

} // namespace CoopBoss
