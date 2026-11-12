# Known issues in the newest test release

This is an early test release. Everything under "Works" was seen in our two-game tests: two copies of the game on one PC talking through the server, driven by the [autotest](../tools/autotest) scripts, with the logs and screenshots of both games read afterwards. Testers play the releases on separate PCs, over a LAN and the internet. Please report what you see ([setup.md, Reporting a problem](setup.md#reporting-a-problem)); every open problem is on the [issues page](https://github.com/daurosk/oot-remake-multiplayer/issues).

**Every player needs the same release.** The release is protocol 9: a game and a server (or another game) of different protocol versions refuse each other, so every player updates (`Update.bat` in the kit folder, or answer **Y** when `Host.bat` / `Join.bat` offer it). Then run `make-puppet-pack.cmd` once more.

**The emulator is Fritchii v5.8.4.** The first start after an update prepares the shaders again ("Preparing shaders"); let it finish.

## Works (seen in two-game tests)

- **Joining and leaving**: joining, leaving, rejoining after a crash, and reconnecting when the server restarts. A join password turns away a wrong or missing password, and an address that keeps guessing wrong is shut out for a minute ([#160](https://github.com/daurosk/oot-remake-multiplayer/issues/160)).
- **The co-op panel** (top left) with each player's name, area, distance and ping; the chat box (Enter), the player list (hold Tab), name tags, and chat notices when someone joins or opens a chest.
- **Other players as Link**, child and adult ([#111](https://github.com/daurosk/oot-remake-multiplayer/issues/111)), following them live and copying their whole pose, with Navi beside them. A player who joins later gets a Link for everyone already there. A Deku Stick and a bottle show in their hand; the slingshot, bow and hookshot reach their Link too ([#92](https://github.com/daurosk/oot-remake-multiplayer/issues/92)).
- **Items**: key items, bottles, songs and one-time pickups are shared; small keys, Gold Skulltula tokens and heart pieces add up across players. Rupees, ammo and health stay per player. A shared Slingshot or bomb bag comes with its ammo ([#128](https://github.com/daurosk/oot-remake-multiplayer/issues/128)). A player who joins late gets the items found before.
- **Heart containers** raise every player's maximum health ([#88](https://github.com/daurosk/oot-remake-multiplayer/issues/88)).
- **Saves: the host's save decides.** When the host loads a save, every game goes back to that save's progress. A guest who joins from the title screen gets the host's save copied into a co-op save folder; press **Continue** to load it ([#167](https://github.com/daurosk/oot-remake-multiplayer/issues/167)).
- **Dungeons**: switches, doors and cleared rooms are shared, and a switch another player set stays set when you load your save ([#129](https://github.com/daurosk/oot-remake-multiplayer/issues/129)). A chest another player opens in your room disappears from your game.
- **Boss rooms**: when one player enters a boss room, the others are moved in and the intro starts together; a player who walks out during the fight is pulled back in.
- **Enemies**: their position, animation and life follow the player who owns them. All the Deku Babas of the Deku Tree's first room show in both games ([#132](https://github.com/daurosk/oot-remake-multiplayer/issues/132)), and the Stalchildren in Hyrule Field are shared, kills included. Townspeople and animals move the same in every game.
- **Crates and thrown rocks** break in every game.
- **Things a player makes**: Deku seeds and arrows in flight and Deku Nuts appear in the other games, and seeds and arrows stay where they hit. A bomb another player holds stays in their hands with its fuse blinking, and it explodes on every screen at once, held or thrown.
- **`/teleport <name>`** moves you next to another player, in the same area or another one.
- **Kit**: `collect-logs` masks addresses and your PC and user name; the updater adds every new `coop-options.ini` setting below your own lines.
- **Mods**: texture and model mods work; a code or cheat mod stops co-op (see Other notes).

## Known issues

- **Order puzzles split between players** do not count: the Deku Scrubs (2-3-1) only work when one player hits all three ([#149](https://github.com/daurosk/oot-remake-multiplayer/issues/149)).
- **Bosses** target one player at a time; Gohma can freeze or turn wrong in one game ([#150](https://github.com/daurosk/oot-remake-multiplayer/issues/150)). Since the gohma-sync change a boss keeps one owner for the whole fight (only a stun from the other player hands it over), so it goes after its owner's Link; a second player standing alone near it is not attacked until they stun it.
- **Max hearts** can flip up and down after the host loads a save ([#186](https://github.com/daurosk/oot-remake-multiplayer/issues/186); being fixed).
- **The Kakariko gate** opens for only one player, and Zelda's letter is gone from both ([#171](https://github.com/daurosk/oot-remake-multiplayer/issues/171)).
- **The ocarina**: other players' songs are not heard, and the ocarina is not shown in their hands while they play ([#152](https://github.com/daurosk/oot-remake-multiplayer/issues/152)).
- **Others' shots** show no impact spark or sound ([#166](https://github.com/daurosk/oot-remake-multiplayer/issues/166)).
- **Drops**: grass and bush drops are not shared; each game can have its own ([#102](https://github.com/daurosk/oot-remake-multiplayer/issues/102)).
- **Tunic, boots and masks** reach the other games but have not been seen on the other Link's screen yet ([#91](https://github.com/daurosk/oot-remake-multiplayer/issues/91)). Tell us if you see them.
- **The other Links' sounds** are in the puppet pack, but our tests have no sound: tell us whether their footsteps, sword and voice can be heard ([#131](https://github.com/daurosk/oot-remake-multiplayer/issues/131)).
- **Game version**: only version 1.1.1 of the game (build `e0f3ccfaf50065ae`) is supported; every player needs that one ([#97](https://github.com/daurosk/oot-remake-multiplayer/issues/97)).
- **Windows only** for now ([#127](https://github.com/daurosk/oot-remake-multiplayer/issues/127)).
- **Hosting** needs a VPN (Hamachi, Radmin VPN) or a forwarded port; the kit never opens a port by itself ([#165](https://github.com/daurosk/oot-remake-multiplayer/issues/165)).
- **Shader stutter**: the first minutes and new areas stutter while the emulator builds shaders; it gets better as its cache fills ([FAQ](faq.md#the-game-stutters-what-can-i-do)).

## Not yet checked in a game

These are fixed or built in, but no two-game test has reached them yet. Please tell us whether they work for you.

- **Deku Tree**: eye switches, floor plates and torches ([#176](https://github.com/daurosk/oot-remake-multiplayer/issues/176)); the slingshot room's trap, ladder, bridge and webs falling or burning live in the other game ([#129](https://github.com/daurosk/oot-remake-multiplayer/issues/129), [#177](https://github.com/daurosk/oot-remake-multiplayer/issues/177)); the map when two players open its chest ([#146](https://github.com/daurosk/oot-remake-multiplayer/issues/146)); the pushed block ([#147](https://github.com/daurosk/oot-remake-multiplayer/issues/147)); talking to a Deku Scrub both players beat ([#144](https://github.com/daurosk/oot-remake-multiplayer/issues/144)); Gohma's larvae ([#178](https://github.com/daurosk/oot-remake-multiplayer/issues/178)); the boss room no longer pulling players in too early ([#148](https://github.com/daurosk/oot-remake-multiplayer/issues/148)).
- **Story and cutscene flags** shared: Saria, the owl and the forest-exit wall once for everyone ([#173](https://github.com/daurosk/oot-remake-multiplayer/issues/173)).
- **Enemies**: kills seen by a player who enters the room later ([#133](https://github.com/daurosk/oot-remake-multiplayer/issues/133)); an enemy chasing a player into another player's view ([#101](https://github.com/daurosk/oot-remake-multiplayer/issues/101)); multi-part enemies keeping one owner ([#105](https://github.com/daurosk/oot-remake-multiplayer/issues/105)); the Peahat waking ([#157](https://github.com/daurosk/oot-remake-multiplayer/issues/157)).
- **World**: a Gold Skulltula or token another player took disappearing from your game ([#89](https://github.com/daurosk/oot-remake-multiplayer/issues/89)); pots and rolling boulders ([#84](https://github.com/daurosk/oot-remake-multiplayer/issues/84)); the pieces of a cut sign on the other screen ([#90](https://github.com/daurosk/oot-remake-multiplayer/issues/90)); the fishing pond with two players ([#151](https://github.com/daurosk/oot-remake-multiplayer/issues/151)); the host's time of day in every game ([time.md](time.md)); the Kokiri Shop's Deku Shield coming back after it is lost ([#130](https://github.com/daurosk/oot-remake-multiplayer/issues/130)).
- **Puppets**: other Links hidden in other areas and during cutscenes ([#86](https://github.com/daurosk/oot-remake-multiplayer/issues/86)); their Navi staying with them ([#145](https://github.com/daurosk/oot-remake-multiplayer/issues/145)); the boomerang and hookshot shots ([#85](https://github.com/daurosk/oot-remake-multiplayer/issues/85)).
- **Connection**: no world reset every few seconds when both players are in one area ([#159](https://github.com/daurosk/oot-remake-multiplayer/issues/159)); fewer disconnects over the internet ([#175](https://github.com/daurosk/oot-remake-multiplayer/issues/175)) and on small-packet links such as Hamachi ([#143](https://github.com/daurosk/oot-remake-multiplayer/issues/143)); the name retry after a crash ([#108](https://github.com/daurosk/oot-remake-multiplayer/issues/108)); high refresh (Ctrl+U) no longer speeding up co-op's checks ([#106](https://github.com/daurosk/oot-remake-multiplayer/issues/106)).
- **Kit**: updating itself on launch from a published release ([#104](https://github.com/daurosk/oot-remake-multiplayer/issues/104)).

## AMD graphics cards

- AMD's driver crashed with the emulator's shader libraries (`amdvlk64.dll`; [#118](https://github.com/daurosk/oot-remake-multiplayer/issues/118)). The kit turns them off on a PC with an AMD card and no NVIDIA card. [setup.md, Troubleshooting](setup.md#troubleshooting) has the details.
- Without them, new places stutter longer the first time on AMD than on NVIDIA. The "Preparing shaders" step of the first start makes up for most of it (the kit has Fritchii's shader recipe): let it finish, also after a driver update. [setup.md, "Low frame rate"](setup.md#low-frame-rate) has more.

## Low frame rate

- Slow or uneven (for example under 20 fps): see [setup.md, "Low frame rate"](setup.md#low-frame-rate). In short: let "Preparing shaders" finish (after "Play now" it keeps running beside the game and slows it; close the emulator and start again to finish it); the first 10 to 15 minutes stutter while shaders are built; click the **60 FPS** button at the bottom of the emulator window to switch to 30 FPS; laptops plugged in and on the graphics card; a current graphics driver. The kit's emulator already uses the fastest settings.
- The co-op code reads the game at most every 33 ms, and its debug logging is off by default.

## Other notes

- **Mods**: co-op does not allow mods that change the game's code, or cheats ([#136](https://github.com/daurosk/oot-remake-multiplayer/issues/136)). `Host.bat`, `Join.bat` and `play.cmd` stop before anything starts when one is installed (a mod folder with an `exefs` or `cheats` folder, a loose `cheat_*` file, or code on the SD card) and name it. Mods turned off in the game's Add-Ons do not count, and neither does Dark's Dynamic FPS mod (Fritchii has it built in). Texture and model mods (a `romfs` folder only) are fine; every player should use the same model mods. If the emulator is started another way with a code mod, the panel says **"Co-op is off: a code mod is installed"** and co-op stays off until the emulator restarts.
- Windows may warn about the kit's programs the first time (unsigned): **More info**, then **Run anyway**. Windows Firewall may ask to allow `coop_server.exe` (on the host) and `lichii.exe`: allow them on Private networks (and Public for Hamachi or Radmin VPN, [play-online.md](play-online.md)).
- Hosting: the host's server window lists the address to give for each case (VPN, internet with port forwarding, same house) and copies the best one. It asks api.ipify.org (HTTPS) for the internet address; `public_address_lookup = no` in `coop-options.ini` turns that off. If friends cannot connect, `check-hosting.cmd` checks the server and the firewall and prints the fix; it changes nothing itself.
- If an emulator is killed (not closed) and started again at once, joining takes a moment: the game asks again by itself for up to 12 seconds while the server lets the old one go.
- The kit's emulator is portable and never reads or writes `%APPDATA%\eden`.
