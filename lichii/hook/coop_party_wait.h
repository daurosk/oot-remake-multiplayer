// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wait for the party (tracker row 298, docs/party-wait.md): an optional host setting that, before a moment where
// players drift apart most (a boss door, a locked combat room, a big chest), tells our player on the game's banner
// to wait for the others. It never stops anyone: notices only, and every wait ends (everyone here, a time-out, or
// our Link leaving the moment).
//
// Pure: the moment, who is here and the clock come in; banner notices and one chat line for the others go out.
// Tests: tests/lichii/party_wait_test.cpp. The game side: coop_party_wait_game.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopPartyWait {

enum class Kind : std::uint8_t { boss_door, combat_room, big_chest };

// A moment our Link is in now. `id` names it (kind, dungeon and boss room or placement key) so the same moment
// waits once a visit; `where` is how the others' chat line names it ("the boss door (Forest Temple)").
struct Moment {
    Kind kind = Kind::boss_door;
    std::string id;
    std::string where;
    bool operator==(const Moment&) const = default;
};

// Each frame's facts.
struct Input {
    bool on = false;            // the session's setting (PARTY_WAIT_MODE), connected
    std::string scene;          // our scene ("" not in a game)
    std::optional<Moment> moment;
    unsigned others = 0;        // other players in the session
    unsigned here = 0;          // of them, here (same scene and room; docs/party-wait.md "Who is here")
    std::uint64_t now_us = 0;
};

struct Output {
    std::vector<std::string> notices;    // for our banner (chat_.Add(line, true))
    std::optional<std::string> chat;     // said once to the others when a wait starts
};

inline constexpr std::uint64_t kGiveUpUs = 60'000'000;   // never forever: a wait ends 60 s after it began
inline constexpr std::uint64_t kRecountUs = 3'000'000;   // a new count is shown at most every 3 s

// The text of the wait line, "Waiting for the other players to sync... (1/3 here)": `here` counts us too.
std::string WaitLine(unsigned here_with_us, unsigned all_with_us);
std::string WhereText(Kind kind, const std::string& dungeon);

// /wait in the chat: nothing for other text; "/wait" shows the value, "/wait on" / "/wait off" (also 1/0, yes/no,
// any case) ask for a change (the server takes it from the host only); anything else after "/wait" is `bad`.
enum class Command : std::uint8_t { show, on, off, bad };
std::optional<Command> Parse(std::string_view text);

class Waiter {
public:
    Output Frame(const Input& in);
    bool waiting() const { return waiting_.has_value(); }
    void Reset();

private:
    struct Waiting {
        Moment moment;
        std::uint64_t since_us = 0;
        std::uint64_t shown_us = 0;
        unsigned shown_here = 0;
        unsigned shown_all = 0;
    };
    std::optional<Waiting> waiting_;
    std::string scene_;                  // the visit: done moments are forgotten when it changes
    std::vector<std::string> done_;      // moment ids that waited (or needed none) in this visit
};

}  // namespace CoopPartyWait
