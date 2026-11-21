# Bosses built for more than one player (design notes, 2026-11-21)

The question asked of every boss stage: **is it fun with two to four players, and if not, how should it change so
the fight feels built around a group** (the way Four Swords builds its fights around several Links)? This page is
the plan; [dungeons-boss.md](dungeons-boss.md) says how the sync works today.

## What every fight has today

- One game runs the boss (its owner); the others show a copy that follows it (native boss sync). Hits from every
  player count, the death plays once in each game, the heart container is one per player.
- **Boss turns** (#308 and `boss-turns-0119`): the boss goes to the next player by number at the end of **every**
  attack in its owner's game (4 s at least with one player, `boss_turn_us`, so a quick combo is not split), so its
  AI fights each player in turn. A player who is down, dead or paused is skipped, and is in the round again at the
  next attack's end once up (their puppet is seen by enemies again then too).
- **Never stuck on a player who cannot fight** (2026-11-21): a boss leaves an owner who goes down or dies at once
  and one paused after 2 s (`boss_owner_dead_us`, `boss_owner_paused_us`; a plain enemy still waits 10 s and 6 s),
  for a player up and playing. With nobody up it stays where it is.
- **A stun hands the boss to the stunner, also right after a turn:** a stun in another player's game (Gohma's eye,
  a ball sent back into a copy Phantom Ganon or Ganondorf) asks the boss over at most every 8 s
  (`boss_hitter_cooldown_us`), counted from the last change that was no turn (`Object::hitter_changed_at`): with a
  turn at every attack, a count from every change refused nearly every stun.
- The difficulty scales by the number of players (2 players: boss life 200%, damage 120%).

What that does not give yet: **an attack aimed at more than one player at once**. The game's AI picks one target,
and a shot or a grab is real only in the owner's game (a copy's own shots are refused, the owner's come as copies),
except the split shots below.

## Tools we have or can build

| Tool | What it does | State |
|---|---|---|
| Turns | the boss fights each player in turn, between attacks | in (proof runs) |
| Turn length | per boss: every attack, every second attack, every N s | to build (a server table by boss) |
| Split shot | an attack's shot made in **every** game, aimed at that game's own Link: each player gets the ball / the rock / the beam at the same moment | in for Phantom Ganon's bolt, shockwaves and balls and Ganondorf's `DungeonBossGanondrof_Bullet_*` (#308, `CoopEnemyShots::IsSplitShot`); the rest to build |
| Half split | a split shot in half the games, the other half the next time (every other attack) | to build on split shot |
| Part targets | a boss of several bodies (Bongo Bongo's hands, Twinrova's sisters, Morpha's tentacles) gives each body its own target player | to build (owner per group member) |
| Stun for all | a stun in one game stuns the boss for everyone (the stun hand-over already moves him to the stunner) | in |

## Boss by boss

### Queen Gohma (Deku Tree, child)
- **Ceiling / wander:** she crawls toward her target and drops eggs. With turns she goes after each player; the
  eggs hatch in every game. Fun: yes, one player stuns the eye with the slingshot, the others slash.
- **Change:** none needed. Later: more eggs with more players (the enemies scale says 125%).

### King Dodongo (Dodongo's Cavern, child)
- **Inhale / fire breath:** aimed at one player; any player can throw the bomb in. **Roll:** chases one player.
- Fun with turns: yes (whoever he faces is the bait, another bombs him). **Change:** turn after **every** roll and
  every breath (turn length = every attack), so nobody waits a whole minute.

### Barinade (Jabu-Jabu, child)
- **Tentacles (3), jellyfish ring, body:** the beam is aimed at one player.
- **Change:** part targets for the tentacles' zaps (each player cuts one); the jellyfish ring spins toward its
  target, so turns every attack. Not tested yet (no save with Barinade's room before his fight).

### Phantom Ganon (Forest Temple, adult)
- **Stage 1, paintings:** he rides out of a painting toward his target; the others can shoot him. With turns each
  player gets a ride at them. Fun: yes.
- **Stage 1, the leap:** he leaps out of a painting and throws a bolt at the room's middle; four shockwaves run
  out from it in a cross. As copies flown on the owner's path the shockwaves hurt nobody but the owner. **Done
  (split shot):** every game makes its own bolt and its own four shockwaves, seen and felt in that game only, so
  each player dodges one cross, the same one, at the same moment.
- **Stage 2, the ball rally:** today one player rallies with him, the others watch. **Done (split shot):** each
  player gets their own ball at the same moment, seen and played in their own game only. The owner's Ganon sends
  it back as alone; a copy's cannot (his AI runs in the owner's game), so a ball sent back into him stuns him in
  that game and the stun hand-over gives him to that player for the sword hits.
- **Stage 2, the goal (asked for, the hardest):** he does not go down until **half of the players** have sent
  their ball back into him. Each game reports its return to the server (a new message, or a DAMAGE 0 with the
  rally's round), the server counts the players of that round, and the knockdown is held in every game (the
  owner's too: its own stun reaction waits) until the count reaches half (2 of 2 or 3, 2 of 4); then everyone sees
  him fall at once and the one who made the last return owns him. Needs: a way to hold the game's own knockdown on
  the owner's Ganon (patch 0007's Life step hold is for life, not for the reaction), and a round count per rally.

### Volvagia (Fire Temple, adult)
- **Holes / head out:** the hammer player hits the head; **flight + fire breath:** at one player; **falling rocks:**
  around one player.
- **Change:** split shot for the falling rocks (rocks around every player); turns every flight.

### Morpha (Water Temple, adult)
- **Tentacle grabs:** one target. **Nucleus:** one player hookshots it out, another slashes it: the best
  two-player moment of the game already.
- **Change:** part targets: with more players, one tentacle per player (the game makes two late in the fight);
  turns every grab.

### Bongo Bongo (Shadow Temple, adult)
- **Two hands, the eye:** both hands must be stunned, then the eye shot. Built for two players already: one takes
  the left hand, one the right.
- **Change:** part targets: the left hand goes for one player, the right for another (today both go for the
  owner). Turns between slams.

### Twinrova (Spirit Temple, adult)
- **Stage 1, Koume and Kotake:** fire and ice shots, three of one kind charge the Mirror Shield.
- **Change:** part targets: each sister has her own target player (two players each duel one sister). With more
  players the sisters swap targets every attack.
- **Stage 2, Twinrova:** one body, one target: turns every attack. Later: a charged Mirror Shield blast from any
  player stuns her for all (stun for all).

### Ganondorf (Ganon's Tower)
- **Energy ball rally, multi-ball, light arrows:** the same as Phantom Ganon's stage 2 (asked for): his ball at
  every player at once (split shot; `DungeonBossGanondrof_Bullet_` is on the list, his own `Ganondorf_Bullet_*`
  names to be read in a run), and he goes down only when half of the players have sent theirs back into him; a
  light arrow from any player stuns him for all.
- Not tested yet (the boss rush stage did not place him from this save).

### Ganon (final battle)
- **Stage 1:** he faces one player; the tail is the weak spot. With turns the player he faces is the bait and the
  others go for the tail: the most Four-Swords fight in the game as it is.
- **Stage 2 (sword lost):** same; turns every swing.

## Order of work

1. **Turns between attacks** (Dauro's `boss-turns-0119`, merged in our branch): prove in every boss's every stage
   with two games (proof runs per boss).
2. **Turn length per boss** (every attack for Dodongo's roll, Volvagia's flight, Morpha's grab; every second for
   the long ones), a server table.
3. **Split shot** for Phantom Ganon's and Ganondorf's ball, then Volvagia's rocks; half split as a setting.
4. **Part targets** for Bongo Bongo's hands, Twinrova's sisters, Morpha's tentacles, Barinade's tentacles.

## Testing every stage

Each boss gets a proof run that walks it through every stage with two games: the test `hit` command takes it to
each stage's life (as `hs-boss-turns-pg` takes Phantom Ganon 42 -> 12 into the rally), both players watch it act
there for a minute (turns, its attacks at each player), then the kill. Checked per stage: the same move on both
screens, an attack at each player, a turn at an attack's end, no stall, one death.
