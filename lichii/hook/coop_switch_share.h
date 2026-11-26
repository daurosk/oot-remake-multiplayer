// SPDX-License-Identifier: GPL-3.0-or-later
//
// room-objects (review of 60567704; issues 196, 249, 194): the timed switches' presses, so another game's room runs
// its own logic.
//
// A room object in a game that follows another moves only through its own logic (coop_room_objects.h). Most of the
// inputs reach the follower already: a shared flag, a remote event (CoopEyeSwitch sets the switch plug its asker
// reads), a player's contact (CoopLiftRide). The rest are timed switches: a step, eye or hit switch whose output plug
// (OnState, OffState) the room graph reads through its own timer (HoldGeneralTimerSwitch), delay or gates, with no
// event and no saved flag. Dodongo's Cavern's moving ground and shortcut floor (Obj_StepSwitch_A_01 OnState through a
// timer), its fire walls and shutters, the Forest Temple's lamp, air wall and torch, the GTG and Water Temple's
// shutters and fountain: 12 plugs (CoopRoomObjects::SwitchPlugs, generated from the room graphs). g3-objects: 4 more
// whose plug also raises an event asker (Jabu-Jabu room 1's shutters, the Deku Tree room D lifts, GTG room 5's chest,
// Spirit room 19's mirror), held only once their guard flags are on in our game (Seen::armed): the result, no replay.
//
// One mechanism for all of them, the switch's own state: the game whose player pressed the switch sends SWITCH_STATE
// on (the plug went 1 with no remote press held) and off (it went back to 0), again every kResendMs while on. A game
// in the same scene holds the same plug on its own switch through the game's set-bool (coop_logic_set.h, as
// CoopLiftRide and CoopEyeSwitch) every frame while the presser's is on, so its own timer starts, its own gates
// open and its own movers move; when the presser's goes off it stops, and the switch's AI puts its own 0 back. A plug
// that goes 1 while (or just after) we hold it for another player is not sent back (kEchoMs).
//
// Row 244 (two players on one switch): a game that holds a plug for another player cannot tell its own press from
// its hold, so it sent nothing while it held. Two players pressing the same switch each held it for the other and
// both went quiet; every third game let go after kRemoteHoldMs, both sent again after kEchoMs, and so on: the
// switch went off and on every ~5 s in every other game (the DC moving ground's timer, the platform raise and its
// camera ran again each time; piscesgamer97's log, 13 times "let go after 5.1 s held"). Now the game with the
// lowest player number among the pressers keeps resending its own press while it holds (only a press it had sent
// before: State::sent_on); the others stay quiet as before. One sender is always left, so the others' holds never
// lapse; when that player steps off, its hold of the others lapses within kRemoteHoldMs (they are quiet), its plug
// reads 0 and it sends off.
//
// Solo: no other player in the scene, nothing is sent; no SWITCH_STATE received, nothing is held. Reads only.
// LICHII_COOP_SWITCH_SHARE=0 turns it off; =probe sends and logs but never sets. No emulator code
// (tests/lichii/switch_share_test.cpp).
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"
#include "coop_room_objects.h"

namespace CoopSwitchShare {

inline constexpr std::uint64_t kResendMs = 2000;      // while on, so a player who comes later gets it
inline constexpr std::uint64_t kRemoteHoldMs = 5000;  // a remote on not sent again in this long is let go
inline constexpr std::uint64_t kEchoMs = 1000;        // our plug after we held it for another player is not ours
inline constexpr std::uint64_t kSetHoldMs = 400;      // the caller keeps setting a held plug this long after a walk

// Murmur3 of a plug's name (coop::protocol::flag_hash), as SWITCH_STATE and the set-bool take it.
std::uint32_t PlugKey(std::string_view plug);

// One of our switches seen this walk, and its line's plug as our game has it now (nothing: not read).
// `armed` (g3-objects): the line's live guard flags (CoopRoomObjects::SwitchPlug::guard) are all on in our game, so
// our own event asker can no longer fire; a line with no guard is always armed. A line not armed is never held: another
// player's press waits (its played flags come first, then the hold gives the result only, no replay of the event).
// `load_on`: the line's LOAD guards ("load:<flag>", read by the room only when it loads) are all on in our game now
// (none: true). The Sharer keeps the value of the switch's first sight after a room load: a load guard on only from a
// mid-visit turn-on deleted nothing (our asker still exists, a hold would replay the event), so it never arms.
struct Seen {
    const CoopRoomObjects::SwitchPlug* line = nullptr;
    std::uint64_t address = 0;
    std::optional<bool> on;
    bool armed = true;
    bool load_on = true;
};

// A plug to set this frame (another player's switch is on, ours is not).
struct Hold {
    std::uint64_t address = 0;
    std::uint32_t key = 0;
    bool operator==(const Hold&) const = default;
};

class Sharer {
public:
    // Network thread.
    void Receive(const coop::protocol::SwitchState& message, std::uint64_t now_ms);
    // Network thread: PLAYER_LEFT. That player's switches are let go at the next walk, not after kRemoteHoldMs.
    void Forget(std::uint8_t player);

    // Present thread, each objects walk. `others_here`: another player is in our scene (else nothing is sent).
    // `self`: our player number (0: not known yet). Fills `send` (our presses) and returns what to hold; notes are
    // log lines. `away`: the players whose world stands still now (PLAYER_STATUS paused or loading: the pause or
    // quick-switch menu, a load). Their presses are not held while so (row 164 follow-up): their own switch's timer
    // stands still with their world, ours would run on held.
    std::vector<Hold> Tick(const std::string& scene, const std::vector<Seen>& seen, bool others_here,
                           std::uint8_t self, std::uint64_t now_ms, std::vector<coop::protocol::SwitchState>& send,
                           std::vector<std::string>& notes, const std::set<std::uint8_t>& away = {});

    void Clear();

private:
    struct Remote {
        bool on = false;
        std::uint64_t until_ms = 0;
    };
    struct State {
        std::map<std::uint8_t, Remote> remote;  // by player
        bool sent_on = false;
        std::uint64_t last_send_ms = 0;
        std::uint64_t echo_until_ms = 0;
        bool held = false;
        bool told_held = false;
        bool told_sent = false;
        bool told_waiting = false;  // another player's is on, ours not armed yet (its guard flags)
        bool told_lead = false;     // row 244: our press resent while we hold it for another (logged once a hold)
        // Seen::load_on at the switch's first sight since its room loaded. Forgotten after a walk without the switch
        // (its room unloaded or reloading) and replaced when the switch is another actor (a new load). Not by time:
        // a pause menu stops the walks mid-visit, and a later sight there is no load.
        std::optional<bool> load_on;
        std::uint64_t load_address = 0;  // the switch actor of that sight
        std::uint64_t seen_tick = 0;     // the last Tick that saw this line
        std::uint64_t held_since_ms = 0;  // for the log: how long we held it, how long our press lasted
        std::uint64_t pressed_since_ms = 0;
    };
    bool RemoteOn(State& s, std::uint64_t now_ms, const std::set<std::uint8_t>& away);

    std::mutex mutex_;
    std::string scene_;
    std::uint64_t tick_ = 0;
    std::map<const CoopRoomObjects::SwitchPlug*, State> states_;
};

// The svc caller (CoopCallers contract, as CoopLiftRide::Rider): each frame the game's set-bool on each held plug
// that is not 1 already.
class Setter {
public:
    void Want(const std::vector<Hold>& holds, std::uint64_t main, std::uint64_t now_ms);
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t now_ms);
    std::vector<std::string> TakeNotes();
    void Clear();

private:
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, std::uint64_t main);

    std::mutex mutex_;
    std::vector<std::pair<Hold, std::uint64_t>> wanted_;  // (plug, until ms)
    std::uint64_t main_ = 0;
    std::vector<std::string> notes_;
    std::deque<Hold> frame_;  // the calc thread's own
    std::uint64_t block_ = 0;
    bool asked_malloc_ = false;
    bool failed_ = false;
};

Sharer& TheSharer();
Setter& TheSetter();

}  // namespace CoopSwitchShare
