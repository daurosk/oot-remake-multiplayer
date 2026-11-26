// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Forest Temple's twisting hallways (docs/dungeon.md, "Twisting hallways"): an eye above each hallway's door
// (Obj_ForestTemple_TwistRelief_A_01 west, _A_02 east) turns its hallway when shot: the eye asks the event setting
// ForestTemple_Room11_RoomSpin or _Room22_RoomSpin, whose flow plays Dm_ForestTemple_0005 or _0007 (it runs
// EventTriggerRelocateTwistCorridor). In co-op a turn changes the
// hallway for everyone, so it must never turn under another player standing in it: while a player who can move
// stands inside the hallway on the shooter's side, our game's ask of the turn is refused at the request (the eye
// does nothing, no cutscene), the shooter is told who is in the way, and the player inside is told to step out
// (a chat line). A player who is down, spectating or away (paused, in a menu, loading, in their own event, dead)
// never blocks it. Plain logic: tested in tests/lichii/hallway_test.cpp; coop_hookshot.cpp asks it at the
// game's event request, coop_hook.cpp posts its lines.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CoopHallway {

using Vec = std::array<float, 3>;

inline constexpr std::string_view kScene = "ForestTemple";

// The hallway a place is in: 0 the west one, 1 the east one; none elsewhere. Each is its trigger box from the
// map (an Area 5 x 5 x 21 at x -56.43 / 56.57, y 121.67, z -32.89; its door markers at z -12.8 and -53.2), a
// little wider so a Link at its walls counts.
std::optional<int> HallwayAt(std::string_view scene, const Vec& p);

// The hallway the eye a player at `shooter` shot turns: the one on its side of the temple (the eyes are at
// x -56.5 and 56.5, each above its own hallway's door).
int HallwayOfShooter(const Vec& shooter);

// Whether `event` is a hallway turn: the eye's settings ForestTemple_Room11_RoomSpin / _Room22_RoomSpin, or the
// twist flows Dm_ForestTemple_0005 / _0007.
bool IsTurn(std::string_view event);

struct Other {
    std::uint8_t number = 0;
    std::string name;
    Vec position{};
    std::optional<std::string> scene;
    bool can_move = true;  // false: down, spectating or away (never blocks)
};

struct Verdict {
    bool refuse = false;
    int hallway = -1;
    std::vector<std::string> who;  // the names of the players inside, by number
};

// Our game asks `event` with our Link at `self` in `scene`: refused when it is a turn and a player who can move
// stands inside the hallway it turns.
Verdict Decide(std::string_view event, std::string_view scene, const Vec& self, std::span<const Other> others);

// What we say about a refusal (to us), and the chat line we send (to the ones inside).
std::string OurLine(const Verdict& v);
std::string TheirLine(const Verdict& v);

// The refusals' lines, said at most once every kSayEveryUs per hallway (the eye can be shot again at once).
class Lines {
public:
    static constexpr std::uint64_t kSayEveryUs = 4'000'000;
    void Refused(const Verdict& v, std::uint64_t now_us);
    std::vector<std::string> TakeOurs();
    std::vector<std::string> TakeChats();

private:
    std::array<std::uint64_t, 2> said_at_{};
    std::vector<std::string> ours_;
    std::vector<std::string> chats_;
};

// --- The module's glue (any thread; the event request is on a game thread) ---

// Whether our game's ask of `event` is refused now (reads the last frame's map view and who can move); a refusal
// queues its lines. False when co-op is off or nothing is known.
bool RefuseAsk(std::string_view event);
// Players who cannot move now, bit n for player n (set once a frame by the down-but-not-out glue).
void SetCannotMove(std::uint32_t mask);
// The lines queued since the last call: ours for the chat box, theirs to send as chat.
std::vector<std::string> TakeOurLines();
std::vector<std::string> TakeChatLines();

} // namespace CoopHallway
