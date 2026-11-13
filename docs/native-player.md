# The other player's Link as a real second Player

Status: research and a first test switch. Not in any release. The puppet from the puppet pack
(docs/puppet.md) stays the way other players are shown until every step below is proven in game.

## Why

Today another player is a stripped copy of Link from a pack made on each player's PC, posed from
outside (about 300 bones, 30 times a second, about 160 KB/s for each player). The target is the
game's own `Player_Child` / `Player_Adult`, created by the game's own create path and run by the
game's own player code from the other player's buttons and sticks, with the position corrected.
Animation, equipment, items in hand, the Ocarina and riding then come from the game itself, and a
player sends tens of bytes a frame, which is what 16 players need.

## What is known (read from the game's code, build e0f3ccfa; not yet run)

- **Who "the player" is.** The game's own lookup (main+0x37cca0, 15 callers) reads the player
  preload lists, not a name: a Player made later by a direct create request is in none of the
  records, so the camera, the HUD and the save code keep finding the local Link.
- **No "active player" global.** The three globals the Player code writes (GOT 0x4c7e838,
  0x4c7eb20, 0x4c838a0) are type-information statics filled once behind a guard. Nothing to hook.
- **Where the Player reads the pad.** Two functions, each with one caller:
  - buttons: main+0x279ce80(out), called from the node update main+0x279cda0;
  - sticks: main+0x279e2c0(out), called from main+0x279e1e0 (`ExecutePlayerCalcStick`).

  Both ask the controller manager (`[[GOT 0x4c7ef30]]`) for controller 0 through
  main+0x3b25e00(manager, index 0, hash of "Player") and copy from it: held, pressed and released
  button words at +0x8, +0xc and +0x114, the sticks at +0x128 and +0x120. In both callers x19 is
  the AI node, and the node's actor is `[[[node+8]+8]+0x1f0]`. Both skip the read when
  main+0x3025850 says input is off.
- So a second Player made with nothing else changed reads controller 0: it would move with the
  local Link. The input has to be given per actor at those two functions.

## Steps

1. **A second Player, left alone** (`LICHII_COOP_NATIVE_PLAYER=1`, with `LICHII_COOP_TEST_PUPPET=1`
   for one beside you without a second game). The Director creates `Player_<Age>` as it creates the
   pack puppet and then makes no call and no write on it. The log line `native player: second ...`
   says once a second (on a change) or every 5 s: its state and place, its Player, Life and Physics
   components, who the game's own lookup says the player is, whether the second one moved and
   whether it moved with the local Link, and the size of the preload lists. This run answers what
   breaks: camera, HUD, saving, input.
2. **Its own input.** Two rows in the function-wrap list of the world hooks (Hookshot patch 0010,
   wrap ids 16 and 17) at the two functions above. On the sending game the wrap copies what the game
   read for the local Link; on the receiving game it fills the same bytes in for the second Player
   and skips the controller read. One game build, with the next batch of hooks.
3. **Correction.** Position, rotation and the few states that must not drift, set through the
   game's own setters.
4. **The switch.** The puppet code uses the real Player when it is proven, the pack puppet stays as
   the fallback.
5. **Numbers** for 16 players.

## The test switch

| Variable | Effect |
|---|---|
| `LICHII_COOP_NATIVE_PLAYER=1` | other players (and the test puppet) are `Player_Child` / `Player_Adult`, created and then left alone; the log has `native player:` lines. For tests only: the second Link reads your own pad until step 2 |

`LICHII_COOP_PUPPET_ACTOR` still overrides the actor name.
