// SPDX-License-Identifier: GPL-3.0-or-later
// Going to another player: "/teleport <name>" in the chat box, once to the
// host when a joiner first gets into the game, and after a player who took an
// exit (teleport_auto.hpp). The rules, the asking over JOIN_PARTNER and the
// scene change are the emulator hook's own (lichii/hook/coop_teleport.h, no
// emulator code); this is its game side for the port, in place of
// lichii/hook/coop_teleport_game.cpp:
// - the same scene: Link's position is written and the actor's set-matrix
//   function called (main+0x4c6580), as the hook's boss rooms move Link;
// - another scene: the game's own request flags (Sequence_ReservedNextBanc
//   and ...NextStartPosName) are written and a short event is started
//   (lichii/hook/coop_live_events.h), since the game loads a reserved scene
//   when an event ends, as after a loading zone.
// Every write and game call is made on the game's thread, in the callback
// puppet.cpp registers (Step). Nothing here writes from the DLL's own threads.
//
// Threads:
// - the DLL's frame thread: Frame (what our game does, who is where; sends
//   the requests) and OnChat;
// - the network thread: OnEvent (answers the others, takes their answers);
// - the game's thread: Step.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop_puppet.h"
#include "teleport_auto.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopNx2navi::Teleport {

// Once, before the session starts. `log` gets every line ("teleport: ...").
void Configure(const Auto::Options& options, void (*log)(const char* line));

// For tests (OOT_COOP_TEST_GOTO="<seconds>:<x>,<y>,<z>;..."): our own Link is
// moved to each place that many seconds after it first was in the game, the
// way a same-scene teleport moves it. Before the session starts.
void TestGoto(std::string_view spec);

// For tests (OOT_COOP_TEST_SCENE="<seconds>:<scene>:<start point>"): that many
// seconds after our Link first was in the game, our own game changes to that
// scene at that start point, by the scene change a trip to another player
// makes (the request flags and the short event). It stands in for a leader
// who takes an exit where no input script can walk one.
void TestScene(std::string_view spec);

// Whether a scene change also reserves the arrival event a loading zone
// reserves (off; OOT_COOP_TELEPORT_ARRIVE=1 turns it on, for tests: it made no
// difference to the black picture in the one run with it). Before the session
// starts.
void SetArriveEvent(bool on);

// Frame thread: true when `text` is a command (done here, not sent as chat).
bool OnChat(std::string_view text, coop::host::Session& session);

// Network thread, from the session's on_event (the network lock is held).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Frame thread, every frame. `game_thread`: the game-thread callback runs
// (without it nothing can be moved, and the others' requests are refused).
void Frame(const std::optional<coop::host::PlayerReading>& reading, coop::host::Session& session,
           bool connected, std::uint64_t server_now_us, bool game_thread);

// Frame thread: our Link has no life left (Game Over). After the game's reload
// this game goes to the player it follows (teleport_auto.hpp, Died).
void Died();
// Where a player's life comes from (nothing: not known; false: no life left), for the
// way back after a Game Over (teleport_auto.hpp, RegroupChoice). Null: not known.
void SetPeerAlive(std::optional<bool> (*alive)(std::uint8_t player));

// The notices since the last call, for the in-game panel.
std::vector<std::string> TakeLines();
// A trip of ours (a follow, a join, a test move) is under way: no item-get scene starts meanwhile (item_get.hpp).
bool Travelling();

// The game's thread, as CoopPuppet::Director::Step is asked: once a frame
// with after_call false, then again after each call it returned.
std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                     std::uint64_t main_base);

}  // namespace CoopNx2navi::Teleport
