// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-rejoin (the pause menu's Multiplayer page, "Auto-rejoin"): with it on, this game joins the last friend's game
// again by itself (coop.ini's last_invite, the page's "Join <host> again"): once a few seconds after the game starts,
// and when the connection to that game drops without the player leaving it (a few tries, further apart). Never when
// co-op came with the game's start (Host.bat, Join.bat, coop.ini), and every Host, Join or Leave of the player's own
// stops what is planned. The logic only: CoopPause feeds it co-op's state every update and joins when it says so.
#pragma once

#include <cstdint>
#include <string>

namespace CoopAutoRejoin {

inline constexpr std::uint64_t kStartDelayMs = 4000;   // after the game's first update: the title is up
inline constexpr std::uint64_t kFirstRetryMs = 5000;   // after a drop, the first try
inline constexpr std::uint64_t kRetryMs = 15000;       // between the tries after that
inline constexpr int kTries = 5;

// What co-op is doing now, as the planner needs it.
struct Facts {
    bool enabled = false;       // the setting (coop-options.ini auto_rejoin)
    bool code = false;          // a last game is kept to join again
    bool launcher = false;      // co-op came with the game's start: never
    bool idle = false;          // co-op neither hosts nor joins nor is in a session (a failed try counts as idle)
    bool guest_in = false;      // this game is in a friend's game now (joined, connected)
    bool player_left = false;   // the player stopped co-op (Leave) since the last update
    bool player_acted = false;  // the player hosted or joined since the last update
    bool turned_away = false;   // the session turned this game away since the last update (the host's Kick or ban,
                                // a full server, a wrong password): the host's choice, not a drop to undo
};

class Planner {
public:
    // True: join the last game now. `Why` then says why, for the log.
    bool Tick(const Facts& f, std::uint64_t now_ms);
    const std::string& Why() const { return why_; }
    // A drop is being tried again (for the page's row: "Rejoining...").
    bool Retrying() const { return next_ms_ != 0; }

private:
    std::uint64_t first_ms_ = 0;
    bool start_done_ = false;   // the start's join was made, or is not wanted any more
    bool was_in_ = false;       // this game was in a friend's game: a drop is noticed when it is idle again
    int tries_ = 0;
    std::uint64_t next_ms_ = 0; // the next try after a drop (0: none planned)
    bool turned_away_ = false;  // the session turned this game away; until the player's own Host or Join
    std::string why_;
};

}  // namespace CoopAutoRejoin
