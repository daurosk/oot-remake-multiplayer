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

// LICHII_COOP_ENEMY_SHOTS=0: enemy shots are not shared at all (each game its
// own, as before). LICHII_COOP_ENEMY_SHOTS_HIDE=0: shared, but a copy's own
// shots are not deleted (to compare in a test).
bool On();
bool HideOn();

} // namespace CoopEnemyShots
