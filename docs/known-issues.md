# Known issues in the test release (v0.1.0-test2)

This is an early test release. Everything below was checked on one PC with two copies of the game talking through the server (two emulator windows, often driven by the [autotest](../tools/autotest) scripts), and then played once by hand in two games on that PC. **It has not been played on two PCs yet**, over a LAN or the internet: that is what this release is for. Please report what you see ([setup.md, Reporting a problem](setup.md#reporting-a-problem)).

Every player needs the same release. v0.1.0-test2 and v0.1.0-test3 speak protocol 3. **Protocol 4** arrives with v0.1.0-test4: a protocol 3 game cannot join a protocol 4 server, or the other way round, so every player updates (`Update.bat`). Builds from chunk 2 on speak **protocol 5** and refuse protocol 4 the same way. A v0.1.0-test1 game cannot join a test2 or later server.

## AMD graphics cards

- **test4 crashes on AMD cards** (`amdvlk64.dll` in Windows' Event Viewer; [issue 118](https://github.com/daurosk/oot-remake-multiplayer/issues/118)): AMD's driver crashes with the emulator's shader libraries. Fixed in test4.2: the kit turns them off on a PC with an AMD card and no NVIDIA card. [setup.md, Troubleshooting](setup.md#troubleshooting) has the details and the one line for older kits or a PC with both cards.

## Found in the first play test of this build (being fixed)

One PC, two games, played by hand on 2026-11-09.

- **Sword and shield on the other Links do not show.**
- **Cut grass is not shared**: grass you cut still stands in the other games. After this release the 13 grass tufts near the Kokiri side of Hyrule Field were added to the shared objects (in the next build; not yet checked in a game). The field's ordinary grass is still not shared: it is not an object the co-op code can see yet ([world.md](world.md)).
- **Hit reactions**: an enemy hit in one game loses life in the others but does not react there (a Stalchild keeps its head).
- Seen working: Stalchildren rise at the same places in both games.

## Works (checked on one PC, two games)

- Joining, leaving, rejoining after a crash, and reconnecting when the server restarts.
- The co-op panel (top left): each player's name, area, distance and ping. The in-game chat box (Enter) and player list (hold Tab). A name tag over each player's head.
- **Other players as Link** (with the puppet pack), checked as child Link. They stand where the other player is, follow them live and copy their whole pose (walking, running, sword stance). Navi flies beside each of them. A player who joins later gets a Link for everyone already there.
- **Items**: key items, bottles, songs and one-time pickups are shared; small keys, Gold Skulltula tokens and heart pieces add up across players. Rupees, ammo and health stay per player. A player who joins late gets the items found before.
- **Dungeons**: switches, doors, cleared rooms and water levels are shared through the game's saved dungeon flags.
- **Enemies**: their position, animation and life follow the player who owns them; a kill in one game is a kill in all. Townspeople and animals stand and move the same in every game. One game decides where the night Stalchildren rise, so they are the same for everyone.
- **Things a player makes**: Deku seeds and Deku nuts appear in the other games.

## Built, but not yet seen working in the game

- **Adult Link**: a player on an adult save should show as adult Link in the other games. Nothing adult has been run yet.
- **Time of day**: every game should follow the host's clock ([time.md](time.md)). No run has shown a second game following the host yet.
- **Pots, crates and rocks** are shared like enemies ([world.md](world.md)). Crates and a thrown rock were seen in the automatic tests; pots have not been seen, and nothing here has been checked by hand. Cut grass does not work (above).
- **Bombs, arrows, the boomerang and the hookshot**: no test has thrown a bomb or shot an arrow yet, so an explosion in the other games has not been seen. Arrows would point the wrong way (their rotation is not sent).
- **Sword and shield in hand or put away** as their owner has them: built, but in the play test they did not show at all (above).
- **Areas and cutscenes**: a player in another area shows only in the panel, not as Link, and the other Links hide during your cutscenes and stand still during theirs.
- **Dungeon sharing** is checked only at the flag level: no one has played through a dungeon door or the Water Temple's water level in two games yet.

## Not yet

- **Heart containers and heart pieces**: from the next build, a heart container one player picks up raises every player's maximum health (seen with the test cheat, not yet with a real container). Heart pieces count up across players, but whether four of them make a heart for the others is not checked yet.
- **Gold Skulltulas**: the token count is shared, but the Skulltula you took may still be there in the other games.
- **Cut signs** may not be shared: the sword swing is sent to the others and given to their sign, but whether that cuts it there has not been seen yet. A sign cut in one game may stay whole in the others.
- **Tunic, boots and masks** are not shown on the other players' Link.
- **Two PCs**: never tested. Delay, firewalls and routers are the unknowns.
- **Game version**: only build `e0f3ccfaf50065ae` of the game is supported; every player needs that one.

## Mods are not supported in co-op

Co-op does not allow game mods (issue 136). A mod's code patches can sit where co-op's own go (the Hyrule Cheat Menu's does), and cheats, refills or God Mode change the game under the sync and hide or fake sync bugs.

- `Host.bat`, `Join.bat` and `play.cmd` stop before anything starts when a mod is installed for the game, and name its folder. Mods turned off in the game's Add-Ons do not count, and neither does co-op's own `coop-puppet` pack, nor Dark's Dynamic FPS mod (Fritchii skips that one for its built-in Dynamic FPS). Loose `cheat_*` files count.
- When the game's code is patched where co-op patches it anyway (a mod started another way), co-op stays off. The panel says **"Co-op is off: a game mod is installed"**.

## Other notes

- Windows may warn about the kit's programs the first time (unsigned): **More info**, then **Run anyway**. Windows Firewall may ask to allow `coop_server.exe` (on the host) and `lichii.exe`: allow them on Private networks.
- If an emulator is killed (not closed) and started again at once, joining takes a moment: the game asks again by itself for up to 12 seconds while the server lets the old one go. A name someone else really uses shows "name taken" after those 12 seconds.
- The kit's emulator is portable and never reads or writes `%APPDATA%\eden`.
