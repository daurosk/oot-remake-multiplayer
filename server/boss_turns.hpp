// Boss turns (owner 2026-11-20, 0.11.8: "make bosses playable in multiplayer ... have it alternate between
// targeting players"; docs/dungeons-boss.md "Boss turns").
//
// Every boss and enemy attacks only after the game's attack permission (isForbidAttackPermission, the native
// GameBalance), which the game gives to the Player alone. So an enemy attacks only its own game's Link, and the
// owner's game runs it: the owner was the only target. Here the enemy takes turns. At its own attack
// boundaries (an attack ends in the owner's game: object_state::attacking, the game's own Attack attribute of
// the current action verb, falls) it is handed to the next player in its fight, whose game then runs its brain
// against that player's own Link. Every attack of that turn (shots, beams, breath, melee) is native there.
//
// Pure rules only (no Session state): when a turn is due, and who has it next. Tested in
// tests/server/boss_turns_test.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace coop::server::turns {

struct Config {
  bool on = true;                       // false: one owner for the fight, as before 0.11.8
  std::uint64_t min_turn_us = 4'000'000;  // a turn lasts at least this long...
  std::uint32_t min_attacks = 1;        // ...and at least this many of the owner's attacks
  // A PLAIN enemy with no attack started for this long in a fight: its target is not its owner's Link (a perceived
  // puppet gets no attack permission, so it waits), so the turn goes on. Never a boss (Sample::idle_turns false):
  // a boss has long phases of no attack by design (Phantom Ganon inside his paintings, Gohma on the ceiling,
  // Volvagia burrowed, King Dodongo's roll, Bongo Bongo's hands, Twinrova and Ganondorf in the air, Morpha in her
  // water, Barinade's jellyfish), and a hand-over there starts the new owner's move over: lane run
  // 20261120-154046 handed Phantom Ganon on 23 times this way and he never left his painting. A boss's turn ends
  // only on its own game's attack end.
  std::uint64_t idle_us = 8'000'000;
  float boss_reach_m = 40.0f;   // players this near a boss take turns (a boss room)
  float plain_reach_m = 15.0f;  // and this near a plain enemy (a scrub's or Octorok's range)
};

// What a turn knows about the enemy (kept per object by the Session).
struct State {
  std::uint16_t epoch = 0;          // the owner's epoch the turn started with (a new owner: a new turn)
  bool known = false;               // started (its epoch read once)
  std::uint64_t started_at = 0;
  std::uint32_t attacks = 0;        // the owner's attacks begun in this turn
  bool attacking = false;           // as of the owner's last sample
  std::uint64_t last_attack_at = 0; // the newest attack's start or end (0: none this turn)
};

// The owner's sample as the turn reads it.
struct Sample {
  std::uint16_t epoch = 0;
  bool attacking = false;   // object_state::attacking
  bool blocked = false;     // in an event, dying, stunned, stalled, a hand-over asked: no turn now
  bool in_fight = false;
  bool idle_turns = true;   // the idle rule may end this turn (plain enemies; never a boss)
};

enum class Why : std::uint8_t { none, attack_ended, idle };

inline const char* WhyText(Why w) {
  switch (w) {
    case Why::attack_ended: return "its attack ended";
    case Why::idle: return "no attack for its owner's Link";
    default: return "none";
  }
}

// Takes one owner sample and says whether the enemy's turn is over now. A new epoch (any hand-over, a stun's
// included) starts a new turn. A turn ends only BETWEEN moves: on the sample whose attack bit fell (the attack
// is over), or (a plain enemy) while not attacking after idle_us with no attack. Never while blocked.
inline Why Step(State& s, const Sample& in, std::uint64_t now_us, const Config& c) {
  if (!s.known || in.epoch != s.epoch) {
    s = State{};
    s.known = true;
    s.epoch = in.epoch;
    s.started_at = now_us;
    s.attacking = in.attacking;
    if (in.attacking) {
      s.attacks = 1;
      s.last_attack_at = now_us;
    }
    return Why::none;
  }
  const bool fell = s.attacking && !in.attacking;
  if (!s.attacking && in.attacking) {
    ++s.attacks;
    s.last_attack_at = now_us;
  }
  if (fell) s.last_attack_at = now_us;
  s.attacking = in.attacking;
  if (!c.on || in.blocked || !in.in_fight || in.attacking) return Why::none;
  if (now_us < s.started_at || now_us - s.started_at < c.min_turn_us) return Why::none;
  if (fell && s.attacks >= c.min_attacks) return Why::attack_ended;
  if (!in.idle_turns) return Why::none;
  const std::uint64_t quiet_since = s.last_attack_at != 0 ? s.last_attack_at : s.started_at;
  if (now_us >= quiet_since && now_us - quiet_since >= c.idle_us) return Why::idle;
  return Why::none;
}

// A player who could take the turn.
struct Player {
  std::uint64_t id = 0;            // the Session's ConnectionId
  std::uint8_t number = 0;         // player number (1..)
  bool fit = true;                 // joined, loaded it, not busy/downed/away, ready (its intro played)
  std::optional<double> distance;  // to the enemy; nullopt: not known
  std::int64_t aggro = 0;          // life (or stuns) this player took off it during the current turn
};

// Who has the next turn: never the owner, never an unfit player or one out of reach (an unknown place counts as
// out of reach, so a player elsewhere in the scene is skipped). Aggro first: the player who hurt it most in
// this turn (ties: nearer, then the lower number). Else round-robin by player number after the owner's,
// wrapping. nullopt: nobody else (one fit player: the game as it is).
inline std::optional<std::uint64_t> Next(const std::vector<Player>& players, std::uint8_t owner_number,
                                         float reach_m) {
  const Player* best_aggro = nullptr;
  const Player* after = nullptr;   // the lowest number above the owner's
  const Player* lowest = nullptr;  // the lowest number (wrap)
  for (const Player& p : players) {
    if (p.number == owner_number || !p.fit || !p.distance || *p.distance > reach_m) continue;
    if (p.aggro > 0) {
      const bool better = !best_aggro || p.aggro > best_aggro->aggro ||
                          (p.aggro == best_aggro->aggro &&
                           (*p.distance < *best_aggro->distance ||
                            (*p.distance == *best_aggro->distance && p.number < best_aggro->number)));
      if (better) best_aggro = &p;
    }
    if (p.number > owner_number && (!after || p.number < after->number)) after = &p;
    if (!lowest || p.number < lowest->number) lowest = &p;
  }
  if (best_aggro) return best_aggro->id;
  if (after) return after->id;
  if (lowest) return lowest->id;
  return std::nullopt;
}

}  // namespace coop::server::turns
