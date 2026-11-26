// SPDX-License-Identifier: GPL-3.0-or-later
//
// PvP on real bodies, the hook's side (coop_pvp_native.h has the rules;
// docs/native-puppets.md section 5). Off unless LICHII_COOP_PVP_NATIVE=1.
//
// It says our PVP_NATIVE (glue on and a pvp pack), keeps the other games'
// PVP_NATIVE, and picks which players' copies are the _Pvp actor (Wants). The
// attacker: our game's own hit on a _Pvp copy (the object walk reads its Life
// record: NoteCopyHit) becomes a PVP_STRIKE when it is ours. The victim: path
// B (coop_pvp_native.h DecidePathB; the game's own hit on our Link through
// CoopObjects::HurtOurLinkNative, never a life write) and PVP_RESULT. Every
// other game: a landed result is replayed on its copy of the victim. Path B
// takes life only once its tables name the kinds (U2/U7); until then strikes
// are answered `kept off` and today's PVP_HIT still hurts (TakesStrikesFrom).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "coop/host/player_reader.hpp"
#include "coop_pvp_native.h"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopPvpNativeGame {

// LICHII_COOP_PVP_NATIVE=1, read once.
bool On();
// Once, at configure time: the puppet pack's manifests say "pvp": true.
void SetPackPvp(bool on);
// Network thread, from the session's on_event (the network lock is held).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
// Present thread, once per frame. `session` null when not connected.
void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us);
// Any thread: player `number`'s copy is to be the _Pvp actor now.
bool Wants(std::uint8_t number);
// Any thread: our game judges player `number`'s hits as strikes (path B can take life), so today's PVP_HIT from
// that player is dropped (coop_pvp_game.cpp): a hit never counts twice (docs 5.9 proof 9).
bool TakesStrikesFrom(std::uint8_t number);
// The object walk (coop_objects.cpp ReadPvpCopies): our game's own new hit on a _Pvp copy, for the next Frame.
void NoteCopyHit(const CoopPvpNative::CopyHit& hit);

} // namespace CoopPvpNativeGame
