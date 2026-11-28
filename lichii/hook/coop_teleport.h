// SPDX-License-Identifier: GPL-3.0-or-later
//
// /teleport <player> in chat (docs/setup.md "Chat commands"). No emulator
// code: coop_teleport_game.cpp reads the game, sends JOIN_PARTNER and moves
// Link; this parses the command, decides what is refused and when to move.
// Tested in tests/lichii/teleport_test.cpp.
//
// The flow, on JOIN_PARTNER (docs/protocol.md "Join partner"), no new message:
// 1. "/teleport sa" picks the player (whole name first, then a unique start
//    of a name, or the number), checks our own game (Trip::Start) and sends a
//    request to that player.
// 2. That player's game answers (AnswerFor): accept with its scene, its last
//    grounded place (Grounded: where it last stood still on its feet, so we
//    never land in a wall or in mid-air) and its rotation; or refuse (busy:
//    loading, an event, no Link; cannot warp: its boss fight).
// 3. On accept (Trip::Accepted): the same scene moves our Link there at once;
//    another scene asks the game's scene change, at that scene's entrance
//    start point nearest the player (NearestStart), the way a loading zone
//    does: by default the loading zone's own event Dm_SY_BancChangeGo with
//    NextBanc, NextStartPos and the entrance's arrival event (ArrivalEvent),
//    started by our Link (map_event; teleport-direct-0116). It is asked again
//    3 s after the last event ended (never while the game reserved our scene,
//    row 329), at most kMaxEventAsks times, until the
//    game takes it (it starts loading, or empties the request), then waits
//    for the scene to settle, asks the player again for where it stands now,
//    and moves there (in a dungeon into any room: moving Link loads his room,
//    teleport-197). LICHII_COOP_TELEPORT_EVENT=0 (no map_event): the
//    String64s Sequence_ReservedNextBanc and ...NextStartPosName are written,
//    asked again every 0.5 s, and the game takes them when an event of the
//    player's own (a talk, a sign) ends (docs/scenes.md "Free-play scene
//    change", #197).
// Another scene with no start point known is refused: without one the game
// keeps the old scene's start name, which the new scene does not have, and
// the picture stays black after the load (docs/scenes.md "The start point").
// Refused before anything is sent: our own event, loading, our boss fight,
// a scene of the other age (X_Child / X_Adult) than our Link's.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_rooms.h"

namespace CoopTeleport {

struct Vec {
    float x = 0.0f;
    float y = 0.0f;  // height
    float z = 0.0f;
};

// Whether the game draws `to` while our Link stands at `from` in `scene`: a
// dungeon has only the rooms around Link loaded. In a dungeon both must be in
// one room of the puppet pack's room table, else (no table) within kNearMove;
// outside dungeons always. Used by the down-but-not-out spectate camera
// (coop_dbno_game.cpp), which looks at a teammate without moving Link.
// Not a rule for moving Link (issue 197, teleport-197): moving Link loads the
// room he is put in, right after a scene change too. The "next to" move into
// another room of a dungeon was refused from 2026-11-12 (blamed on bigrun4's
// black, held picture), but that lock came from the request emptied too early
// (teleport-b3, the same lock with no move); in big-run-0117 B itself was put
// from the Deku Tree's entrance into the slingshot room on 2F right after its
// load by the same move (CoopBoss::MoveLink) and the room was drawn.
enum class MoveCheck : std::uint8_t {
    ok,
    other_room,  // the room table puts `to` in another room of the dungeon
    too_far,     // no room known for one of them, and farther than kNearMove
};
inline constexpr float kNearMove = 10.0f;  // game units (about metres)
MoveCheck CheckMove(const CoopRooms::Table& rooms, std::string_view scene, const Vec& from, const Vec& to);

// The entrance start point of `scene` nearest `to` (coop_start_points.inc,
// written from the game's scene files by tools/puppet_pack/start_points.py).
// Nothing for a scene we have no start point of.
std::optional<std::string> NearestStart(std::string_view scene, const Vec& to);

// Whether `scene` is a scene name the game has, as far as the code knows: every scene of coop_start_points.inc
// (the entrance scenes) and the boss-rush interval room. Exact match, case and all. The test warp checks it before
// it writes a scene into the game (a name the game does not have, such as "Market" for Market_Child, crashed a game
// worker right after the load began, 2026-11-23). A scene the list lacks is a scene to add there, not to let through.
bool IsKnownScene(std::string_view scene);
// The first start point the list has for `scene` (a boss stage has just one, named like the scene): what a warp with no
// entrance uses. Nothing for a scene the list lacks.
std::optional<std::string> DefaultStart(std::string_view scene);
// The known scenes that `name` is the start of (a name given without its age: "Market" gives Market_Adult and
// Market_Child), for the "did you mean" of an error. At most `most`, in list order.
std::vector<std::string> ScenesLike(std::string_view name, std::size_t most = 6);

// The arrival event (BootEventName of Dm_SY_BancChangeGo) of the entrance at `start` in `scene`: the one the game's
// own entrance there names (a few have their own, e.g. Kakariko's Dm_SY_BancChangeArrive_KakarikoVillage_01, as
// Longshot's teleport table took them from the game), else the plain Dm_SY_BancChangeArrive.
std::string ArrivalEvent(std::string_view scene, std::string_view start);

// Whether the arrival at `start` in `scene` has a starter: false only for a row of coop_start_points.inc whose scene
// has no entrance actor (FieldAndDungeonEntranceEventRequestActor) of that name: the 25 boss stages and EnemyMuseum.
// A start the list lacks is taken to have one (nothing of the game's reservation is changed for it).
bool ArrivalHasStarter(std::string_view scene, std::string_view start);

// Issue 289. Dm_SY_BancChangeGo reserves the arrival event's starter by the start's name
// (Sequence_ReservedEventStarterName = its argument NextStartPos), and after the load Sequence/AfterChangeBanc.module
// looks the entrance actor of that name up (SeqQueryEventSubstituteStarterMgrTryGetActor). A boss stage has none:
// the arrival event never began there (no event after the load in three runs' logs) and the module's
// SeqExecuteWaitEventStart kept the loading fade up for good. The game's own entries to those stages reserve the
// starter '' (DreamBossStage's EventReserveBancChange: EventStarterName ''), which the module answers by asking the
// arrival event at once with no starter. So for a start with no entrance actor the reserved starter is emptied after
// the game's event wrote it: the same reservation the game makes. Pure: the glue reads the flags and writes.
class ArrivalStarter {
public:
    // Our map change event was asked for `scene` at `start`. Nothing to do (not active) when that start has a starter.
    void Begin(std::string_view scene, std::string_view start, std::uint64_t now_us);
    // Once a frame while active, with the game's Sequence_ReservedNextBanc, Sequence_ReservedEventStarterName and
    // Sequence_IsLoadingBanc (nothing: not read). True: write '' into Sequence_ReservedEventStarterName now.
    bool Tick(const std::optional<std::string>& reserved_banc, const std::optional<std::string>& reserved_starter,
              bool loading, std::uint64_t now_us);
    bool active() const { return active_; }
    int writes() const { return writes_; }  // '' asked so far for this map change
    const std::string& scene() const { return scene_; }
    const std::string& start() const { return start_; }

    // The game's event reserves within its fade; never seen in this time, the event was not taken.
    static constexpr std::uint64_t kUnseenUs = 10'000'000;
    // Seen: the reservation is the game's until its load ended and it cleared it; a load never takes this long.
    static constexpr std::uint64_t kSeenUs = 120'000'000;

private:
    bool active_ = false;
    bool seen_ = false;    // the game's reservation of our scene was read
    bool loaded_ = false;  // a load ran since
    int writes_ = 0;
    std::uint64_t since_us_ = 0;
    std::string scene_, start_;
};

// A chat line that is a command, not chat. "/teleport" alone lists players.
struct Command {
    enum class Kind : std::uint8_t { list, go };
    Kind kind = Kind::list;
    std::string name;  // go: what the player typed after the command
};

// Nothing for text that is chat (sent as is). "/teleport", "/teleport x",
// any case; also "/tp".
std::optional<Command> Parse(std::string_view text);

struct Player {
    std::uint8_t number = 0;
    std::string name;  // the nickname; may be empty
};

struct Found {
    std::uint8_t number = 0;  // 0: not found, see `error`
    std::string name;         // as shown
    std::string error;        // the chat line when not found
};

// Whole name (any case) first, then a unique start of a name, then "2" or
// "P2". Never ourselves.
Found FindPlayer(std::string_view wanted, std::span<const Player> players, std::uint8_t self);

// "players: Saria (2), Mido (3)": the line "/teleport" alone shows.
std::string ListLine(std::span<const Player> players, std::uint8_t self);

// Our own game, read each frame.
struct Local {
    std::optional<std::string> scene;
    std::optional<Vec> position;
    std::optional<coop::protocol::Quat> rotation;
    std::optional<coop::protocol::Age> age;
    bool event = false;     // an event (cutscene, talk) has control
    bool fighting = false;  // our boss fight runs (CoopBoss)
    // The game's Sequence_ReservedNextBanc this frame (nothing: not read) and
    // Sequence_IsLoadingBanc (BeforeChangeBanc sets it while a scene loads).
    std::optional<std::string> reserved_banc;
    bool loading_banc = false;
};

// The age a scene is for, from its name (X_Child, X_Adult); nothing for a
// scene of both ages (dungeons, most interiors).
std::optional<coop::protocol::Age> AgeOfScene(std::string_view scene);

// Where our Link last stood on its feet: for kSteadyUs its height changed
// no faster than kMaxClimbMps (walking, even on a slope), with no event, in
// this scene. Our answer gives this, not where Link is this frame (a jump,
// a fall, a climb, a ledge).
class Grounded {
public:
    static constexpr std::uint64_t kSteadyUs = 300'000;
    static constexpr double kMaxClimbMps = 2.5;

    void Add(const Local& local, std::uint64_t now_us);
    std::optional<Vec> Last() const { return last_; }
    const std::string& Scene() const { return last_scene_; }

private:
    std::optional<Vec> prev_;
    std::uint64_t prev_us_ = 0;
    std::uint64_t since_us_ = 0;
    std::string scene_;
    std::optional<Vec> last_;
    std::string last_scene_;
};

// What our game answers to another player's request.
struct Answer {
    bool accept = false;
    coop::protocol::JoinRefuseReason reason = coop::protocol::JoinRefuseReason::busy;
    coop::protocol::JoinTarget target;  // accept
};
Answer AnswerFor(const Local& local, const Grounded& grounded);

// The chat line for a refusal from `name`.
std::string RefusedLine(std::string_view name, coop::protocol::JoinRefuseReason reason);

// Our own teleport, the asker's side. One at a time; a new /teleport ends
// the old one.
class Trip {
public:
    // A scene change not taken within this is given up.
    static constexpr std::uint64_t kGiveUpUs = 60'000'000;
    static constexpr std::uint64_t kAskAgainUs = 500'000;
    // After the new scene is in with our Link standing, before asking again.
    // The request is emptied then too, not on the first frame of the new
    // scene (#197): the warp of tools/autotest never empties it and its
    // arrivals play the game's arrival event and hand Link back; every
    // /teleport that emptied it on that first frame stayed black and held
    // (teleport-lock, teleport-other runs of 2026-11-12).
    static constexpr std::uint64_t kSettleUs = 1'500'000;
    // A taken scene change that ended somewhere else is asked again after this.
    static constexpr std::uint64_t kRearmUs = 5'000'000;
    // map_event: the map change event is asked again after this while no
    // event runs and the game has not taken it, at most kMaxEventAsks times
    // in all; then the trip is given up (the game refused it: Link in the
    // air, swimming, held). #197: the old short event DefaultParentEvent
    // (one EventWait binding the nearest actor) held Link after the load and
    // is never started any more; Dm_SY_BancChangeGo is a loading zone's own
    // event, whose arrival event hands Link back.
    static constexpr std::uint64_t kEventAgainUs = 3'000'000;
    static constexpr int kMaxEventAsks = 3;

    // map_event false: no event of ours, the request waits for the player's
    // own talk or sign (LICHII_COOP_TELEPORT_EVENT=0).
    explicit Trip(bool map_event = false) : map_event_(map_event) {}
    bool map_event() const { return map_event_; }

    struct Actions {
        std::vector<std::string> lines;      // chat notices
        std::optional<std::string> scene;    // ask the game's scene change to it (free play or event)
        std::string start;                   // with scene: the start point there (never empty)
        std::optional<Vec> move;             // move our Link here (same scene)
        std::optional<std::uint8_t> ask;     // send a JOIN_PARTNER request to this player
        bool cancel_scene = false;           // clear our scene change request and its switch (settled, given up)
        bool release = false;                // the game took it: drop only the free-play switch
        bool game_cleared = false;           // the game emptied our request itself (after its arrival): log only
        bool start_event = false;            // map_event: ask the map change event to scene/start (no event runs)
        // With cancel_scene, not arrived (given up, the player left, a new
        // /teleport) and no load ran in this trip: put back the start name the
        // game had before our first ask, so it does not keep a start of a
        // scene we never went to. After a load elsewhere the current one is
        // that scene's own and is kept.
        bool restore_start = false;
    };

    // The command: refused (a line, nothing to send) or asked (`ask` set).
    Actions Start(std::uint8_t number, std::string name, const Local& local, std::uint64_t now_us);
    // That player's answers. Answers from another player than ours are dropped.
    // their_age: the partner's age (their samples), for a scene of both ages.
    Actions Accepted(std::uint8_t number, const coop::protocol::JoinTarget& target, const Local& local,
                     std::uint64_t now_us, std::optional<coop::protocol::Age> their_age = std::nullopt);
    Actions Refused(std::uint8_t number, coop::protocol::JoinRefuseReason reason);
    Actions TimedOut(std::uint8_t number);
    // That player left, or we lost the connection.
    Actions Left(std::uint8_t number);
    // Each frame (a few times a second is enough).
    Actions Tick(const Local& local, std::uint64_t now_us);

    enum class Phase : std::uint8_t { idle, asking, travelling, settling, asking_again };
    Phase phase() const { return phase_; }
    std::uint8_t player() const { return player_; }

private:
    Phase phase_ = Phase::idle;
    std::uint8_t player_ = 0;
    std::string name_;
    std::string scene_;           // travelling: where to
    std::string start_;           // travelling: the start point there
    Vec at_{};                    // travelling: the first answer's place, the fallback
    std::uint64_t since_us_ = 0;  // travelling: since the first ask
    std::uint64_t asked_us_ = 0;  // travelling: the last ask
    bool map_event_ = false;      // the game's map change event carries the trip (else a talk or sign)
    std::uint64_t event_us_ = 0;  // travelling: the last map change event asked
    int event_asks_ = 0;          // travelling: map change events asked in this trip
    std::uint64_t settle_us_ = 0; // settling: since the scene was in
    bool taken_ = false;          // travelling: the game took the request
    bool seen_request_ = false;   // travelling: the game's flag showed our request
    bool loaded_ = false;         // this trip: a load ran (then the old start name is stale)
    bool requested_ = false;      // our request may still be in the game (asked, not yet emptied)
    std::uint64_t taken_us_ = 0;
};

} // namespace CoopTeleport
