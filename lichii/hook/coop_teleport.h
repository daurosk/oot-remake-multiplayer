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
//    start point nearest the player (NearestStart), the way its Main sequence
//    takes one in free play (docs/scenes.md "Free-play scene change": the
//    String64 Sequence_ReservedNextBanc plus the Bool 0x3fc35576 that makes
//    Main's selector pick its BancChange case), asks it again every 0.5 s
//    until the game takes it (it starts loading, or empties the request),
//    drops the Bool once taken (so Main does not take an empty request
//    again), then waits for the scene to settle, asks the player again for
//    where it stands now, and moves there. During an event the event's own
//    scene change takes the same request.
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

namespace CoopTeleport {

struct Vec {
    float x = 0.0f;
    float y = 0.0f;  // height
    float z = 0.0f;
};

// The entrance start point of `scene` nearest `to` (coop_start_points.inc,
// written from the game's scene files by tools/puppet_pack/start_points.py).
// Nothing for a scene we have no start point of.
std::optional<std::string> NearestStart(std::string_view scene, const Vec& to);

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
    static constexpr std::uint64_t kSettleUs = 1'500'000;
    // A taken scene change that ended somewhere else is asked again after this.
    static constexpr std::uint64_t kRearmUs = 5'000'000;

    struct Actions {
        std::vector<std::string> lines;      // chat notices
        std::optional<std::string> scene;    // ask the game's scene change to it (free play or event)
        std::string start;                   // with scene: the start point there (never empty)
        std::optional<Vec> move;             // move our Link here (same scene)
        std::optional<std::uint8_t> ask;     // send a JOIN_PARTNER request to this player
        bool cancel_scene = false;           // clear our scene change request and its switch (arrived, given up)
        bool release = false;                // the game took it: drop only the free-play switch
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
    Actions Accepted(std::uint8_t number, const coop::protocol::JoinTarget& target, const Local& local,
                     std::uint64_t now_us);
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
    std::uint64_t settle_us_ = 0; // settling: since the scene was in
    bool taken_ = false;          // travelling: the game took the request
    bool seen_request_ = false;   // travelling: the game's flag showed our request
    bool loaded_ = false;         // this trip: a load ran (then the old start name is stale)
    std::uint64_t taken_us_ = 0;
};

} // namespace CoopTeleport
