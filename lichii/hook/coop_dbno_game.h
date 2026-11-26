// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out, the hook's side (coop_dbno.h has the rules; docs/
// play-online.md "Down but not out"). Only where the game's own Life step
// calls the module (Hookshot patch 0007, LIFE_STEP) can a lethal hit be held
// back; anywhere else the rules stay out of the way and death is the game's.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop_dbno.h"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopDbno {

// The network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// "/dbno ..." typed in the chat: answered here (true), else false.
bool OnChat(std::string_view text, coop::host::Session& session);

// Once a frame, after the objects walk (our Link's life) and the puppets.
void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us);

// Chat notices since the last call.
std::vector<std::string> TakeLines();

// --- The game's threads ---

// A Life component's life value entry; the value is the int32 at its + 8
// (as the objects walk reads it).
inline constexpr std::uint64_t kLifeValueEntry = 0x1c10;

// The guard the Life step asks (LifeGuard::Step), for our Link only.
LifeGuard& TheGuard();

// The objects walk: our Link's Life component (0: none seen), for the Life
// step to know it. Any thread.
void SetOurLifeComponent(std::uint64_t component);
std::uint64_t OurLifeComponent();
// The objects walk: our Link's BSA and PerceptionEmitter components (0:
// none), for the crawl and unseen (coop_dbno_body.h), and whether our Link
// is Player_Adult (no crawl animations: Body::DownVerb), and our Link itself
// (the adult's lying pose, Body::kLyingPose; 0: none). Any thread.
void SetOurComponents(std::uint64_t bsa, std::uint64_t emitter, bool adult, std::uint64_t actor = 0);

// The Life step hook (any game thread): the sum to take off `component`,
// whose life is `life`, when the frame's own sum is `sum`. Our Link's goes
// through the guard, then takes what the rules lower the life to (out: 0,
// so the game's own death runs); any other is the game's.
std::int32_t LifeStep(std::uint64_t component, std::int32_t life, std::int32_t sum);

// The objects walk: the life our Link is to be raised to now, once (nothing:
// none): a revive, or a Link the game left at no life while downed.
std::optional<std::int32_t> TakeLifeWrite();

// The game's input thread, each poll: what the game gets from the pad.
Pad OnPad(const Pad& in);

// --- The screen ---

struct View {
    enum class Kind : std::uint8_t {
        downed,    // we are down: the bleed-out clock, the ring while revived
        reviving,  // we stand beside a downed teammate: hold A, the ring
        spectating,  // not revived in time: we watch `name` (A: the next one)
    };
    Kind kind = Kind::downed;
    std::string name;          // downed: who revives us ("" nobody); reviving, spectating: whom
    std::uint64_t left_us = 0; // the bleed-out time left (theirs, when reviving)
    float progress = 0.0f;     // 0 to 1; the ring
    bool holding = false;      // reviving: A is held
    bool elsewhere = false;    // spectating: the one watched is where our camera cannot follow (another scene or room)
};

// What to draw now; nothing when there is nothing. Any thread.
std::optional<View> Shown();

// Any thread: `number`'s DBNO state now (ours from the last frame, the others' from their DBNO_STATE); up when not
// known or DBNO is off. Row 160: a downed, spectating or out owner cannot play a room event (CoopEventResult).
State StateOfPlayer(std::uint8_t number);

// --- Spectating ---

// The objects walk (coop_objects.cpp), each walk: whether an enemy of our
// game (CoopDifficulty::IsEnemy, alive) is within kThreatReachM of our Link.
// Told to the others as DBNO_THREAT while we are up.
void NoteEnemiesNear(bool any);

// The hook's frame (coop_hook.cpp), while our Link is in the game: whether one
// of our bottles holds a fairy (coop::host::bottled_fairy, read with the
// game's own flag getters). A lethal hit then goes to the game's own death,
// whose fairy revives us, instead of downing us (tracker row 162).
void NoteBottledFairy(bool held);

// While we spectate a player in our scene: how far the camera moves from our
// Link to them (their puppet's feet less our Link's, metres). The game's own
// camera is shifted by it (hos_coop_view), so it follows them as it follows
// Link. Nothing otherwise. Any thread.
std::optional<Vec3> SpectateOffset();

}  // namespace CoopDbno
