# Known issues in the early test release

This is the first test release. Everything below was checked on one PC with two copies of the game talking through the server (two emulator windows, often driven by the [autotest](../tools/autotest) scripts). **It has not been played on two PCs yet**, over a LAN or the internet: that is what this release is for. Please report what you see ([setup.md, Reporting a problem](setup.md#reporting-a-problem)).

## Works (checked on one PC, two games)

- Joining, leaving, rejoining after a crash, and reconnecting when the server restarts.
- The co-op panel (top left): each player's name, area, distance and ping. The in-game chat box (Enter) and player list (hold Tab).
- **Other players as Link** (with the puppet pack): they stand where the other player is, follow them live, copy their whole pose (walking, running, sword stance) and wear their sword and shield. Navi flies beside each of them.
- **Items**: key items, bottles, songs and one-time pickups are shared; small keys, Gold Skulltula tokens and heart pieces add up across players. Rupees, ammo and health stay per player. A player who joins late gets the items found before.
- **Dungeons**: switches, doors, cleared rooms and water levels are shared through the game's saved dungeon flags.
- **Enemies**: their position, animation and life follow the player who owns them; a kill in one game is a kill in all. Townspeople and animals stand and move the same in every game.
- **Things a player makes**: Deku seeds and Deku nuts appear in the other games.

## Not yet, or not working

- **Bombs, arrows, the boomerang and the hookshot**: built, but no test has thrown a bomb or shot an arrow yet, so an explosion in the other games has not been seen. Arrows would point the wrong way (their rotation is not sent).
- **Hit reactions**: when you hit an enemy, the other games see it lose life and die, but not flinch or flash.
- **Pots, crates, grass, rocks and breakable walls** are not shared yet: each game has its own.
- **Heart containers**: picking one up does not raise the other players' maximum health. Heart pieces count up, but four of them do not make a heart for the others.
- **Gold Skulltulas**: the token count is shared, but the Skulltula you took may still be there in the other games.
- **Tunic, boots and masks** are not shown on the other players' Link yet (sword and shield are).
- **Dungeon sharing** is checked only at the flag level: no one has played through a dungeon door or the Water Temple's water level in two games yet.
- **Areas and cutscenes**: a player in another area shows only in the panel, not as Link, and the other Links hide during your cutscenes and stand still during theirs. This is built and unit-tested but not yet checked in the game.
- **Two PCs**: never tested. Delay, firewalls and routers are the unknowns.
- **Game version**: only build `e0f3ccfaf50065ae` of the game is supported; every player needs that one.

## Other notes

- Windows may warn about the kit's programs the first time (unsigned): **More info**, then **Run anyway**. The host's PC asks to allow `coop_server.exe` through the firewall.
- If an emulator is killed (not closed), its name stays taken on the server for a few seconds.
- The kit's emulator is portable and never reads or writes `%APPDATA%\eden`.
