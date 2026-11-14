# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC.

The goal of this project is working multiplayer in two stages:

1. **Emulator first.** Add co-op to the Lichii emulator (Frizti's build), where the game already runs well. Up to 16 players see each other in the world, share progress, and fight the same enemies.
2. **Then a recomp.** Carry the same co-op design into a native PC recompilation of the game. This has started: co-op runs on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (the continuation of Hookshot) as an experiment, with the same code, protocol and server.

Both stages share one design, one wire protocol and one server, so what is learned on the emulator carries straight over to the recomp.

**Discord:** https://discord.gg/GwtRKAqng5, for players, testers and contributors.

## Play the newest release

**[OoT Co-op 0.10.1 (Longshot)](https://github.com/daurosk/oot-remake-multiplayer/releases/tag/v0.10.1)** is the newest build (the releases are pre-releases, which GitHub's "latest" link skips; the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases) lists them all). It runs on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (Team MagicBean and contributors, GPL-3.0; the continuation of [Hookshot](https://github.com/NotRexed/Hookshot) by NotRexed and contributors), a native PC version of the game that the kit builds on your PC from your own copy. Longshot is the version we build on until something better is out. Emulator support ends with 0.10.0: it is not in this release.

**New here?** Download `oot-coop-v0.10.1.zip`, unzip it to a short folder such as `C:\oot-coop`, and double-click **Start OoT Co-op.bat**. The first time it asks for your game file (drag your `.nup` into the window) and builds the game from it: 10 to 45 minutes and about 60 GB free, once. Then **Host** or **Join** in the game's menu, **Multiplayer** tab (see below). It needs 64-bit Windows, a graphics card with mesh shaders (NVIDIA RTX 20 or newer, AMD RX 6000 or newer) and 16 GB of RAM. Each player needs their own copy of the game. Full steps: **[setup.md](docs/setup.md)**.

**Opening the Multiplayer tab:** on a controller press **−** and **+** together (on an Xbox-style pad **View** and **Menu**), or **Home**; on the keyboard **F1** (Insert and Esc also open it unless you changed that in the menu); on the title screen **−** alone. The Multiplayer tab is the first one; the D-pad or the arrow keys choose, A or Enter confirms, B or Esc goes back.

**Already have a kit?** This release is protocol 11, so everyone updates. A kit from before this release cannot update itself to it: close the game and the server window, download **Update.bat** from the release page, put it in your kit folder (replace the old one) and double-click it. An emulator kit becomes the Longshot kit (the emulator and the old `.bat` files go into its `backup-<version>` folder; your saves, keys, settings, name and colour stay). From then on the kit updates itself when you start it. Players who stay on the emulator keep 0.10.0, the last release with the emulator; it cannot join a 0.10.1 game.

**Tested and working in 0.10.1:** friends who join get the host's save at once (their own saves are untouched); the other players as Link; proximity voice chat (off by default, push **V** to talk when turned on: heard near, not far); locked doors and `/teleport` inside dungeons (#197, checked in the Water Temple); the pause menu's **Multiplayer** tab with Host, Join, Local co-op, Settings and a Commands page; playing solo (the game starts with no session); Host and Join from the game itself, by invite code or the "Same network or VPN" line; one start file that builds, updates and starts the game; the Longshot build downloads nothing from the old GitHub page.

**Built in, not tested yet on Longshot** (tell us on [Discord](https://discord.gg/GwtRKAqng5) if something is off): Epona seen by everyone with more than one rider (#199); townspeople walking on every screen (#200); no black screen during another player's cutscene (#173), which worked on the emulator and Hookshot before; a controller of its own for each of two games on one PC.

1. [Setting up](docs/setup.md): the start file, the first build, updating, settings and troubleshooting.
2. Play on the [same network (LAN)](docs/play-lan.md), or [over the internet](docs/play-online.md): Host in the Multiplayer tab opens the port on your router by itself (UPnP), makes a random join password and copies an **invite code**; friends paste it under Join. Every session has a join password, and all game traffic is encrypted with it.
3. [Known issues](docs/known-issues.md): what works in this build and what does not yet.
4. [Questions testers ask](docs/faq.md): who is the host, a permanent server, shader stutter, where saves are.

What works in it: the other players as Link (child and adult) with Navi and a name tag, following them live, with their sword, shield, held items and gear (#91, #92); shared items, songs, keys, heart pieces and heart containers; a shared Slingshot or ammo bag comes with its seeds and room (#128); projectiles others make are shown both ways: slingshot seeds and arrows in flight, nuts, bombs and the hookshot (#85, #158); a bomb another player holds stays in their hands with its fuse blinking and explodes on every screen at once (#169); dungeon progress and room switches (#129); boss rooms entered together, and a player who walks out during the fight is pulled back in (#109); shared enemies, townspeople and Stalchildren (#124), all Deku Babas (#132); the host's save decides (a host's load resets the group, a guest's load takes the group's progress); guests who join from the title get the host's save (#167); chat, a player list, reconnects (also over VPNs such as Hamachi, #143) and rejoining after a crash; a join password with a lockout for repeated wrong guesses (#160); `collect-logs` masks addresses and names (#172); the updater adds new settings (#179); `/teleport <name>` moves you to another player (#156). Texture and model mods work; a game with a code or cheat mod stops co-op (#136).

**Saves: the host's save decides.** Progress is shared while you play; when the host loads a save, every game goes back to that save's progress, and a guest who loads a save gets the host's. To undo something, the host loads an older save; if the host restarts and loads, the guests' progress since the host's last save is undone. See [Saves](docs/setup.md#saves).

Known issues of this build: other players' ocarina songs are not heard yet (#152; the ocarina in their hand shows); after a join that cannot connect, Join refuses another code until the game is restarted; a kit from before this release needs the release's `Update.bat` to update; Microsoft Defender may warn about the zip or `Update.bat` (a false alarm on the scripts); others' masks and boots not yet seen on the other screen (#91); the spark where another player's shot hits does not show (#166); only game version 1.1.1 is checked (#97); shader stutter in new places the first time. See [Known issues](docs/known-issues.md) and the release notes.

**Safety.** Only the host opens a port: UDP 27510 for the co-op server; players who join open nothing. With the invite code, the game asks the host's router (UPnP) to forward that one port only while the server runs; it removes the mapping when the server stops. Every session has a join password: random for each start (unless the host sets one), never written to the logs, and the hole-punch messages are signed with it; the game traffic is always encrypted with it (X25519 and XChaCha20-Poly1305), so nobody on the path can read or change it. The server and the games check every message against fixed limits and never write where a message says, and the updater downloads only this project's releases over HTTPS and checks their signature and SHA-256. Share an invite code only with friends: it holds your address and the password, and it stops working when hosting stops. `upnp = no` and `invite = no` in `coop-options.ini` turn these off; `server_allow` lists the players who may join ([Who can join](docs/play-online.md#who-can-join)). Details and how to report a problem: [SECURITY.md](SECURITY.md).

Every player needs the same release: a game of one release refuses a server of another.

## Status

Stage 0 is done and stage 1 (co-op in the emulator) is well under way: each part is checked on one PC with the real game in two emulator instances, and the releases ([above](#play-the-newest-release)) are played by testers on separate PCs. The plan is in [docs/plan.md](docs/plan.md), the wire protocol in [docs/protocol.md](docs/protocol.md), and what a test kit can show in [docs/testing.md](docs/testing.md).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Done: ENet server and client, wire protocol, `tools/coop_test` (10 minute soak runs pass on one PC). Two-PC run not yet done. |
| 1. Presence: other players shown as markers | Done on one PC: player sampling and smoothing ([docs/presence.md](docs/presence.md)), the emulator hook with `coop.ini` settings, a status panel and markers drawn from the game's camera ([docs/lichii.md](docs/lichii.md)). |
| 2. Puppet Link in the world | Done on one PC: the puppet pack is built from the player's own game files ([docs/puppet.md](docs/puppet.md)); each game shows the other players as child Link, following them live and copying their whole pose every frame, with Navi beside them and a name tag overhead; a player who joins later gets a Link for everyone already there. Their sword, shield, held items (slingshot, bow, hookshot) and gear (masks, boots, tunics) show (#91, #92). Adult Link shows and walks (#111). Their sounds are built in (#131), not yet heard in a real game. A rider on Epona (#199) seen in our tests; the ocarina in their hand (#152) seen in the logs. Each player's Link wears that player's tunic colour (#202, partly: adult Link keeps some green). |
| 3. Scenes, following and cutscenes | Built: puppets only in your scene, hidden or held in cutscenes (unit-tested, not yet checked in game). The join-partner warp was dropped. Seen in our tests: the scrubs' order across players (#149); partly: the Kakariko gate (#171). Built in, not yet checked in a game: story cutscenes watched together by players near each other (#173), the time of day shared (#192). Seen working: no snaps back after cutscenes (#194), the Threads of Time shared (#193). |
| 4. Shared progress (chests, keys, doors, songs) | Done on one PC: world flags shared through the host ([docs/flags.md](docs/flags.md)); items, songs and one-time pickups shared while rupees and the rest stay per player ([docs/items.md](docs/items.md)); small keys, Skulltula tokens and heart pieces add up; a late joiner gets them; dungeon switches, doors, cleared rooms and water levels shared ([docs/dungeon.md](docs/dungeon.md)), and a save load no longer undoes them (#129); chests opened in one game are open in all (#87); a shared item comes with its ammo and room (#128, #138). Heart containers raise everyone's maximum health (#88). The host's save decides: a host's save load resets the group to it. A guest who joins from the title gets the host's save in a co-op save folder (#167; the transfer seen in a two-game test). Story and cutscene flags shared (#173, built). |
| 5. Shared enemies and objects | In progress: enemies, townspeople and animals move, animate and die the same in every game, owned by one player ([docs/objects.md](docs/objects.md)); seeds and nuts a player makes appear in the other games ([docs/spawns.md](docs/spawns.md)); crates and thrown rocks break in every game ([docs/world.md](docs/world.md)). Built, not yet fully checked in a game: pots, the time of day following the host ([docs/time.md](docs/time.md)), bombs, arrows and other projectiles, multi-part enemies and bosses. Signs cut in one game are cut in the others (seen in the logs; the pieces on the other screen not yet). Stalchildren in Hyrule Field are shared (#124); boss rooms pull a player who walks out back in (#109). A held bomb stays in the holder's hands and explodes on every screen at once (#169). Built, not yet checked in a game: the Deku Tree's switches, plates, ladder, bridge and webs (#176, #177), Gohma's larvae (#178) and the early move-in fix (#148; our boss-door test was set up wrong, not checked yet). Also built in, not yet checked: Gohma fighting both players (#150), enemy projectiles (#189), drops (#102), guards (#191), the moving platform (#196), the map chest (#146), the pushed block (#147), two players fishing (#151). |
| 6. Chat, player list, reconnects | Done on one PC: reconnects and rejoining after a crash; the host moves to another player when one leaves; an in-game chat box (Enter) and player list (Tab); a join password (#160). Fewer disconnects over the internet built in (#175, #143). Built in, not yet checked in a game: hosting with an invite code, UPnP and hole punching, no VPN needed (#165; [docs/play-online.md](docs/play-online.md)); signed updates (#161); a game-style HUD with the other players' hearts. Seen in our tests: PvP (off by default), with sword hits both ways; game traffic encrypted with the join password on the emulator kit (#201). |
| Recomp port of stages 0 to 6 | The release runs on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (the continuation of Hookshot), a static recompilation of the same game build: the same co-op code, server and protocol. Needs a mesh-shader GPU (RTX 20 or newer, RX 6000 or newer). From 0.10.1 on, releases are Longshot only. Plan and state: [docs/hookshot.md](docs/hookshot.md). |
| Recomp port for NX2navi | Experimental, as a mod of the NX2navi PC port (another static recompilation of the same game build): the same co-op code, server and protocol through its plugin interface; the other players as Link with name tags, hosting and joining from the title screen, the host's save for a joiner, shared enemies. State, limits and what was seen in a game: [nx2navi/README.md](nx2navi/README.md). |

The emulator side is a small patch on Frizti's Fritchii, pinned in [lichii/PIN](lichii/PIN); `lichii/fetch.ps1` fetches that source and applies the patches. See [lichii/README.md](lichii/README.md).

## Building

Needs CMake 3.24 or newer and a C++20 compiler (Visual Studio 2022 on Windows). ENet and doctest are fetched by CMake at pinned versions. `cmake -S . -B build`, then `cmake --build build --config Release` and `ctest --test-dir build -C Release`.

To test co-op on two PCs with the test kit, see [docs/testing.md](docs/testing.md).

## What you need

- Your own legally obtained copy of the game, dumped from your own console. This repository has no game files, keys or firmware and never will.
- Every player needs the same game version and the same co-op build.

## Contributing

Anyone can contribute: fork, branch, pull request. Every pull request is reviewed before it is merged. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Using an AI coding agent? It reads [AGENTS.md](AGENTS.md). Questions go to [Discussions](https://github.com/daurosk/oot-remake-multiplayer/discussions) or [Discord](https://discord.gg/GwtRKAqng5).

## Rules for this repository

- **No game content.** No game files, keys, firmware, saves, shader caches or builds that contain them. `tools/check_content.py` runs on every push and pull request and fails if any are found.
- **Our own code only.** Other projects (Lichii, Eden, the TOTK Online mod, recomp projects) may be read to understand how they behave. Their code is not copied in unless its licence allows it, and then it is credited.
- **Plain English** in commits, pull requests and docs.

## Credits

- [Fritchii](https://github.com/Frizti/Fritchii) by Frizti (GPL-3.0): the emulator co-op is built into.
- [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) by Team MagicBean and contributors (GPL-3.0), the continuation of Hookshot by NotRexed and contributors: the native PC build co-op also runs on ([docs/hookshot.md](docs/hookshot.md)).
- Other code and libraries we use, and their licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Thanks to zizerak and Hot Onion for development tips (Hot Onion's tip, sharing each moving platform's clock with the server's time, is how the platforms are kept in step), and to everyone who play-tested.

## Licence

GPL-3.0, see [LICENSE](LICENSE). This keeps the project compatible with Lichii and Eden, which are GPL-3.0.

This is a fan project. It is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.
