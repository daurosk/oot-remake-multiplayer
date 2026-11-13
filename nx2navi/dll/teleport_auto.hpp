// SPDX-License-Identifier: GPL-3.0-or-later
// When this game goes to another player by itself (teleport.hpp does the
// going): once to the host when a joiner first gets into the game, and after
// a player it shared a scene with who took an exit ("follow"). No game and no
// Windows code: tested in tests/nx2navi_test.cpp.
//
// Follow, the rule: a leader (the host, or with `anyone` every player) whose
// scene was ours and is now another one, while our own Link stayed in the
// scene the whole time, is followed. That edge is the whole trigger:
// - no loop: the one who followed arrives in the leader's scene, which is no
//   change away from anybody's scene;
// - two players who take different exits at the same time do not swap: each
//   has left the shared scene (its Link is not readable during the load) when
//   the other's new scene shows, and leaving the scene forgets who was there;
// - our cutscene, talk or menu does not lose it: the want is tried again
//   every kRetryUs for kKeepUs (the trip refuses while an event runs, and the
//   leader's game says "busy" until its Link stands in the new scene).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace CoopNx2navi::Teleport {

enum class FollowMode : std::uint8_t { off, host, anyone };

struct Peer {
    std::uint8_t number = 0;         // 1 to 4
    std::string name;
    bool host = false;               // the session's host
    std::optional<std::string> scene;
    bool fresh = false;              // its samples arrive: it is in the game now
    std::optional<bool> alive;       // life above 0 (its last PLAYER_LIFE); nothing: not known
    bool busy = false;               // in a cutscene, talk or menu: not a place to arrive at now
};

// After our Game Over: the living player to come back beside, or nothing (then the
// game's own respawn stands). In the game (fresh, a scene, not busy) and not known
// to be dead; first one in `died_scene` (the scene we fell in), the host before the
// others, then the lowest number; else the same order over the other scenes.
const Peer* RegroupChoice(std::span<const Peer> peers, const std::optional<std::string>& died_scene);

struct AutoOptions {
    FollowMode follow = FollowMode::host;
    bool join_at_host = true;
};

class Auto {
public:
    static constexpr std::uint64_t kRetryUs = 2'500'000;
    static constexpr std::uint64_t kKeepUs = 60'000'000;
    // Our Link readable this long before the join: the save's own start has run.
    static constexpr std::uint64_t kJoinSettleUs = 2'000'000;
    // No host in the game this long after we got in: no join later (a sudden
    // move minutes into play would be a surprise).
    static constexpr std::uint64_t kJoinWindowUs = 30'000'000;

    using Options = AutoOptions;
    // After a Game Over (Died), back in the game through its reload: the players
    // are apart again and nothing else would bring them together (the join is
    // once a session, a follow needs the leader to take an exit).
    static constexpr std::uint64_t kRegroupWindowUs = 30'000'000;
    // Our Link at no life this long with no reload after it: it was brought
    // back where it fell (a fairy), nothing to do.
    static constexpr std::uint64_t kDiedForgetUs = 90'000'000;

    enum class Why : std::uint8_t { join, follow, regroup };
    struct Want {
        std::uint8_t player = 0;
        std::string name;
        Why why = Why::follow;
        bool first = true;  // false: a retry of the same want
    };

    explicit Auto(Options options = Options()) : options_(options) {}

    // A few times a second or more. `scene`: ours while our Link is readable,
    // nothing during a load, at the title screen or in a menu. `self_host`:
    // we are the session's host. Returns the player to go to now, if any.
    std::optional<Want> Tick(const std::optional<std::string>& scene, bool self_host,
                             std::span<const Peer> peers, std::uint64_t now_us);

    // What became of the last want: on its way or there (done), or refused for
    // good (another age's scene, no start point, said no).
    void Done(std::uint8_t player);
    // Not connected any more: nothing is wanted, and the next session joins again.
    void Reset();
    // Our Link has no life left (Game Over), in `scene`. Once the game has reloaded
    // and our Link stands again, the next Tick wants a living player
    // (RegroupChoice), the host's game too, unless following is off. With nobody
    // alive in the game for kRegroupWindowUs, the game's own respawn stands.
    void Died(std::uint64_t now_us, std::optional<std::string> scene = std::nullopt);
    bool died() const { return died_us_ != 0; }

    bool pending() const { return pending_.has_value(); }

private:
    struct Pending {
        std::uint8_t player = 0;
        std::string name;
        Why why = Why::follow;
        std::uint64_t until_us = 0;
        std::uint64_t next_us = 0;
        bool first = true;
    };
    Options options_;
    std::uint64_t in_game_since_us_ = 0;  // 0: not in the game
    std::uint64_t first_in_us_ = 0;       // when we first got in, this session
    bool joined_ = false;                 // the join was wanted, or its window is over
    std::array<bool, 5> together_{};      // by player number: was in our scene
    std::uint64_t died_us_ = 0;           // 0: no Game Over waits
    bool died_reloaded_ = false;          // our Link was gone since (the reload)
    std::optional<std::string> died_scene_;  // where we fell
    std::optional<Pending> pending_;
};

}  // namespace CoopNx2navi::Teleport
