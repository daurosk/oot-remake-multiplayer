# Known issues in the newest test release

This is an early test release. Everything under "Works" was seen in our two-game tests: two copies of the game on one PC talking through the server, driven by the [autotest](../tools/autotest) scripts, with the logs and screenshots of both games read afterwards. Testers have played earlier releases on separate PCs, over a LAN and the internet. Please report what you see ([setup.md, Reporting a problem](setup.md#reporting-a-problem)).

**Every player needs the same release.** A game and a server (or another game) of different protocol versions refuse each other when they connect, so every player updates (`Update.bat` in the kit folder, or answer **Y** when `Host.bat` / `Join.bat` offer it). Then run `make-puppet-pack.cmd` once more: the puppet pack now carries Link's sounds.

**The emulator is Fritchii v5.7.** The first start after the update prepares the shaders again, which takes a while. (Our tests start with Ctrl+U, high refresh, off; turn it on in the game as usual.)

## Works (seen in two-game tests)

- **Joining and leaving**: joining, leaving, rejoining after a crash, a name still held by a crashed game, and reconnecting when the server restarts.
- **The co-op panel** (top left) with each player's name, area, distance and ping; the chat box (Enter), the player list (hold Tab) and name tags.
- **Other players as Link**, child and adult, following them live and copying their whole pose, with Navi beside them. The adult Link puppet ([issue 111](https://github.com/daurosk/oot-remake-multiplayer/issues/111)): seen walking and running in Kakariko by day. A player who joins later gets a Link for everyone already there.
- **Items**: key items, bottles, songs and one-time pickups are shared; small keys, Gold Skulltula tokens and heart pieces add up across players. Rupees, ammo and health stay per player. A player who joins late gets the items found before.
- **Ammo comes with a shared item** ([issue 128](https://github.com/daurosk/oot-remake-multiplayer/issues/128)): when one player gets the Slingshot, the others get it with 30/30 seeds, not 0/0. The same for the bomb bag (20 bombs) and the other ammo items.
- **No more stuck ammo after joining** ([issue 138](https://github.com/daurosk/oot-remake-multiplayer/issues/138)): a player who joins from the title screen, or is loading while an item arrives, gets the item's capacity and ammo once the save has loaded ("capacities mended" in the log). Seen for Deku Sticks (10/10) and the Slingshot (30/30).
- **Heart containers**: a heart container raises every player's maximum health (seen with the test cheat).
- **Dungeons**: switches, doors, cleared rooms, water levels and the dungeons' one-time objects are shared through the game's saved flags (seen with one Deku Tree chest; webs and cracked walls not checked yet). A switch the others set stays set when you load your save ([issue 129](https://github.com/daurosk/oot-remake-multiplayer/issues/129)).
- **Chests** ([issue 87](https://github.com/daurosk/oot-remake-multiplayer/issues/87)): 60 chests outside the dungeons are shared too. A chest another player opens while you are in the same room disappears from your game (seen with one chest, in the logs; that it shows open when you come back is not checked yet). A chest right beside you is left alone, so that two players opening one chest at once do not break it; that case has not been tried yet.
- **Boss rooms**: when one player enters a boss room, the others are moved in with them and the intro starts together (Water Temple).
- **Enemies**: their position, animation and life follow the player who owns them. The Deku Babas of the Deku Tree's first room are placed in the other game too (seen in the logs; whether all show, and kills seen from both games, are not checked yet: below). Townspeople and animals stand and move the same in every game.
- **Cut signs** ([issue 90](https://github.com/daurosk/oot-remake-multiplayer/issues/90)): a sign cut in one game is cut in the others too (all its pieces' values are written there; not yet seen on the other screen).
- **Things a player makes**: Deku seeds and Deku nuts appear in the other games; a pulled bomb is copied to the other games.

## Known issues

- **Stalchildren** in Hyrule Field at night are not shared: each game has its own ([issue 124](https://github.com/daurosk/oot-remake-multiplayer/issues/124)).
- **Bombs in our tests** vanish about 0.1 s after they are pulled out ([issue 125](https://github.com/daurosk/oot-remake-multiplayer/issues/125)). They do the same with co-op switched off, so this is our test setup, not co-op; if you see it in a real game, please report it.
- **The other Links' sounds** ([issue 131](https://github.com/daurosk/oot-remake-multiplayer/issues/131)): the puppet pack now carries Link's sound parts, and nothing crashes, but our tests cannot hear sound: please tell us whether the other Links' footsteps, sword and voice can be heard.
- **Held items** ([issue 92](https://github.com/daurosk/oot-remake-multiplayer/issues/92)): a Deku Stick and a bottle show in the other Links' hands; the Deku Nut and the Slingshot were not seen.
- **Boss rooms**: a player who walks out of a boss room after the fight has started is not pulled back in.
- **A code mod with the emulator started directly**: `Host.bat` and `Join.bat` refuse to start with a code mod (below). If the emulator is started some other way with a mod that patches where co-op does, co-op disconnects as soon as the game loads and the panel says **"Co-op is off: a code mod is installed"**. After such a stop co-op stays off until the emulator restarts: remove the mod, then start again.
- **Not yet checked in a game**: the Kokiri Shop selling each player their own Deku Shield ([issue 130](https://github.com/daurosk/oot-remake-multiplayer/issues/130); the Deku Shield is each player's own), the Deku Tree's Skulltula and other kills seen from both games ([issues 132](https://github.com/daurosk/oot-remake-multiplayer/issues/132) and [133](https://github.com/daurosk/oot-remake-multiplayer/issues/133)), an enemy chasing a player into another player's game ([issue 101](https://github.com/daurosk/oot-remake-multiplayer/issues/101)), pots ([issue 102](https://github.com/daurosk/oot-remake-multiplayer/issues/102)), the host's time of day in every game ([time.md](time.md)), arrows, the boomerang and the hookshot.
- **Gold Skulltulas**: the token count is shared. A Skulltula or token another player takes while you are in the same area should disappear in your game too (not checked yet). If it does not and you take it, your count is put back instead of counting the token twice ([issue 89](https://github.com/daurosk/oot-remake-multiplayer/issues/89)).
- **Tunic, boots and masks** are not shown on the other players' Link. The field's ordinary grass is not shared.
- **Game version**: only version 1.1.1 of the game (build `e0f3ccfaf50065ae`) is supported; every player needs that one.

## Mods

Co-op does not allow mods that change the game's code, or cheats ([issue 136](https://github.com/daurosk/oot-remake-multiplayer/issues/136)). A mod's code patches can sit where co-op's own go (the Hyrule Cheat Menu's does), and cheats, refills or God Mode change the game under the sync. **Texture and model mods are fine**: mods that only replace game files (a `romfs` folder), such as fixes for missing Water Temple or Fairy Fountain textures. If a mod changes models, every player should use the same one, or the games may differ.

- `Host.bat`, `Join.bat` and `play.cmd` stop before anything starts when a code or cheat mod is installed for the game (a mod folder with an `exefs` or `cheats` folder, a loose `cheat_*` file, or code on the SD card), and name it. Mods turned off in the game's Add-Ons do not count, and neither does Dark's Dynamic FPS mod (Fritchii has it built in).
- When the game's code is patched where co-op patches it anyway (a mod started another way), co-op stays off: if it had already connected (the emulator started directly, not through the kit), it disconnects as soon as the game loads. The panel says **"Co-op is off: a code mod is installed"**, and co-op stays off until the emulator restarts.

## AMD graphics cards

- AMD's driver crashed with the emulator's shader libraries (`amdvlk64.dll`; [issue 118](https://github.com/daurosk/oot-remake-multiplayer/issues/118)). The kit turns them off on a PC with an AMD card and no NVIDIA card. [setup.md, Troubleshooting](setup.md#troubleshooting) has the details.
- Without them, new places stutter longer the first time on AMD than on NVIDIA. Fritchii makes up for that with its "Preparing shaders" step, which needs Fritchii's `recipes` folder beside the emulator; the newest kit does not have that folder yet, so AMD players get neither (not yet measured). [setup.md, "Low frame rate"](setup.md#low-frame-rate), step 8, has what helps now.

## Low frame rate

- Slow or uneven (for example under 20 fps): see [setup.md, "Low frame rate"](setup.md#low-frame-rate). In short: if a "Preparing shaders" screen shows when the game starts, let it finish (after "Play now" it keeps running beside the game and slows it; close the emulator and start again to finish it); the first 10 to 15 minutes stutter while shaders are built; click the **60 FPS** button at the bottom of the emulator window to switch to 30 FPS; laptops plugged in and on the graphics card; a current graphics driver. The kit's emulator already uses the fastest settings.
- The co-op code reads the game at most every 33 ms (4 to 30 times a second), and its debug logging is off by default. How much frame time it costs has not been measured yet.

## Other notes

- Windows may warn about the kit's programs the first time (unsigned): **More info**, then **Run anyway**. Windows Firewall may ask to allow `coop_server.exe` (on the host) and `lichii.exe`: allow them on Private networks (and Public for Hamachi or Radmin VPN, [play-online.md](play-online.md)).
- Hosting: the host's server window lists the address to give for each case (VPN, internet with port forwarding, same house) and copies the best one. It asks api.ipify.org (HTTPS) for the internet address; `public_address_lookup = no` in `coop-options.ini` turns that off. If friends cannot connect, `check-hosting.cmd` checks the server and the firewall and prints the fix; it changes nothing itself. Join.bat says when it is connected, and after 30 seconds without an answer the three likely reasons. The kit never opens a port by itself (no UPnP).
- If an emulator is killed (not closed) and started again at once, joining takes a moment: the game asks again by itself for up to 12 seconds while the server lets the old one go.
- The kit's emulator is portable and never reads or writes `%APPDATA%\eden`.
