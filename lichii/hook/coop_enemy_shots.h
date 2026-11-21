// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shots fired by enemies and bosses (Octorok rocks, Deku Scrub nuts, the
// bosses' *_Bullet_*): one shot on every screen (docs/objects.md, "Enemy
// shots"). No emulator code: tested in tests/lichii/enemy_shots_test.cpp.
//
// Every game runs the AI of its copy of a shared enemy, and a copy plays the
// owner's animations, so each game's copy fires its own shot (often on the
// same frame, but aimed at its own Link). The rule: the game that owns the
// enemy fires. A new enemy shot on our list is given to the shared enemy
// that fired it (the nearest one of the shot's maker's name):
//   - ours: it is sent like a player's projectile (OBJECT_SPAWN, samples,
//     OBJECT_REMOVE), and the others show it as the game's own actor, flown
//     on the owner's path; it can hit their Link where it meets him;
//   - another player's: ours is a copy's shot and is deleted (the owner's
//     comes as a copy);
//   - no owner known, or no maker found: left alone (each game its own, as
//     before), never sent.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopEnemyShots {

using coop::protocol::Vec3;

// The actor names of enemy and boss shots that are shared.
bool IsEnemyShot(std::string_view name);
// A split shot (docs/boss-multiplayer.md): an attack that is real in every game. Each game keeps the one its own
// copy of the boss fires (never stopped, never deleted) and the owner's is not sent (Decide says keep), so it
// hits every player as it would alone. Phantom Ganon's stage 1 thunder bolt and its four shockwaves.
// LICHII_COOP_SPLIT_SHOTS=0: off (one bolt, the owner's, as before).
bool IsSplitShot(std::string_view name);
bool SplitOn();
// What its maker's name starts with ("Enemy_Octorok" for Octorok_Bullet),
// one or more; empty for a name that is no enemy shot.
std::vector<std::string_view> MakersOf(std::string_view shot);
// How far from its maker a shot is first seen at most (metres): bosses are
// big and fire from far off their root.
float MakerRange(std::string_view shot);

enum class Owner : std::uint8_t { unknown, ours, theirs };

// A shared enemy of the objects sync's last walk.
struct Shooter {
    std::string name;
    Vec3 position{};
    Owner owner = Owner::unknown;
    std::uint64_t address = 0;  // the actor (for StopAtCreate's shooter); 0 when not known
};

enum class Verdict : std::uint8_t {
    send,  // our enemy fired it: shared
    hide,  // another player's enemy fired it here: deleted
    keep,  // not known: left alone, not sent
};

Verdict Decide(std::string_view shot, const Vec3& at, std::span<const Shooter> shooters);
const char* NameOf(Verdict verdict);

// The objects sync (coop_objects) notes the shooters after each of its walks;
// the spawn sync reads them. Older than kFreshUs: none.
static constexpr std::uint64_t kFreshUs = 1'500'000;
void NoteShooters(std::vector<Shooter> shooters, std::uint64_t now_us);
std::vector<Shooter> Shooters(std::uint64_t now_us);

// Hookshot with patch 0007: the game's create request asks this on its own
// thread before it makes the actor. True: `shot` at `at` is a copy enemy's
// own shot (Decide says hide from the shooters last noted, not older than
// kFreshUs on this PC's clock), so the game is made to refuse the create
// and the shot never exists here; the owner's comes as a copy. Never waits:
// with the shooters' list busy it answers false, and the walk's rule deletes
// the shot as before. A place of all zeros (not read) is never stopped.
// `shooter`: the actor the game's own shoot code makes the shot for (patch
// 0007's SHOOT_FROM), 0 when not known. A noted shooter at that address
// decides by its owner alone, with no guess by place; an actor that is not
// in the list falls back to the place. Also asked inside the native boss
// sync's calls on a copy (the owner's shoot action put in: CoopPuppet::Call::
// copy_game), so the shot has one maker, the owner's game (issue 189).
bool StopAtCreate(std::string_view shot, const Vec3& at, std::uint64_t shooter = 0);
std::uint64_t StoppedAtCreate();  // how many so far
// LICHII_COOP_ENEMY_SHOTS_STOP=0: never stopped at the create (deleted by the
// walk's rule, as on the emulator).
bool StopOn();

// LICHII_COOP_ENEMY_SHOTS=0: enemy shots are not shared at all (each game its
// own, as before). LICHII_COOP_ENEMY_SHOTS_HIDE=0: shared, but a copy's own
// shots are not deleted (to compare in a test).
bool On();
bool HideOn();

// Row 211 probe (LICHII_COOP_SHOT_KEEP_COPY=1; off by default, so nothing changes without it): a copy boss's
// own shot of the boss shots a sword sends back (Ganondorf's light and charge balls, Phantom Ganon's light
// ball) is kept, made by the copy's own shoot code (never stopped at its create, never deleted by the walk's
// rule), so a run can see whether such a real shot's own Sensor takes our Link's swing (a placed copy's never
// does: run 20261120-135622). The owner's copy still comes beside it: the probe only reads.
bool SwordReturned(std::string_view shot);
bool KeepCopyOn();
// Whether a copy's own `shot` is kept: SwordReturned and the probe on (`on`: KeepCopyOn() in the game).
bool KeepsCopyShot(std::string_view shot, bool on);

} // namespace CoopEnemyShots
