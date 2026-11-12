# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC.

The goal of this project is working multiplayer in two stages:

1. **Emulator first.** Add co-op to the Lichii emulator (Frizti's build), where the game already runs well. Two to four players see each other in the world, share progress, and fight the same enemies.
2. **Then a recomp.** Carry the same co-op design into a native PC recompilation of the game. This has started: co-op runs on [Hookshot](https://github.com/NotRexed/Hookshot) as an experiment, with the same code, protocol and server.

Both stages share one design, one wire protocol and one server, so what is learned on the emulator carries straight over to the recomp.

**Discord:** https://discord.gg/GwtRKAqng5, for players, testers and contributors.

## Play the test release

The newest test build is on the **[releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)** (the top one; the test builds are pre-releases, which GitHub's "latest" link skips): the patched emulator (Fritchii v5.8.4, with a Saves window to export, import and copy saves), the server and start scripts in one zip, and the scripts that build co-op into Hookshot on your own PC. You bring your own game dump (and, for the emulator, your keys and firmware). It is an early build for testers, checked in two-game tests on one PC and by players online.

**New: play on Hookshot (experimental).** Co-op now also runs on [Hookshot](https://github.com/NotRexed/Hookshot), a native PC version of the game, with no emulator. The kit builds it on your PC from your own game (`Build-Hookshot-Coop.bat`, 10 to 60 minutes the first time), then `Host-Hookshot.bat` and `Join-Hookshot.bat` start it. It needs a graphics card with mesh shaders: NVIDIA RTX 20 or newer, or AMD RX 6000 or newer, plus 16 GB of RAM and about 60 GB free while it builds. Hookshot and emulator players can play together. Not on Hookshot yet: the co-op window and chat box, a Hookshot player's own ocarina notes, your own Link in your colour. Full steps: **[setup.md, "Play on Hookshot"](docs/setup.md#play-on-hookshot-experimental)**. **Test releases after this one are Hookshot only:** this release still ships and documents the emulator kit, but the next test builds and their tests are on Hookshot.

**New here? The emulator kit.** Download the `oot-coop-...zip` of the newest release, unzip it to a short folder such as `C:\oot-coop`, add your own keys, firmware and game, run `make-puppet-pack.cmd` once, then the host double-clicks `Host.bat` and sends the invite code it copies, and the others paste it into `Join.bat` ([setup guide](docs/setup.md#emulator-setup-for-new-players)). Each player needs their own copy of the game.

**Already have a kit? Update.** This release is protocol 10, so everyone updates: a game of it refuses an older server and the other way round. Close the games and the server window and run `Update.bat` (or answer **Y** when `Host.bat` / `Join.bat` asks), then run `make-puppet-pack.cmd` once more, since `Update.bat` does not rebuild the pack (the new one has the tunic colours; with an old pack the other players stay green). The emulator changed, so the first start prepares the shaders again (let the "Preparing shaders" screen finish, see [Low frame rate](docs/setup.md#low-frame-rate)). On Hookshot, run `Build-Hookshot-Coop.bat` again. [Updating](docs/setup.md#updating).

1. [Setting up](docs/setup.md): Hookshot, or the emulator kit (unzip, your keys and firmware, the puppet pack, first start), updating, settings and troubleshooting.
2. Play on the [same network (LAN)](docs/play-lan.md), or [over the internet](docs/play-online.md): the quickest way is the **invite code**. `Host.bat` opens the game port on the host's router by itself (UPnP), makes a random join password and copies an invite code; friends paste it into `Join.bat`, and when the routers need help the two windows trade an answer code and punch through. No VPN and no router settings. With the join password the game traffic is encrypted. A free VPN (Radmin VPN, Hamachi) still works when the invite code cannot connect, and `check-hosting.cmd` says what to fix.
3. [Known issues](docs/known-issues.md): what works in this build and what does not yet.
4. [Questions testers ask](docs/faq.md): who is the host, a permanent server, shader stutter, where saves are.

What works in it: the other players as Link (child and adult) with Navi and a name tag, following them live, with their sword, shield, held items and gear (#91, #92); shared items, songs, keys, heart pieces and heart containers; a shared Slingshot or ammo bag comes with its seeds and room (#128); projectiles others make are shown both ways: slingshot seeds and arrows in flight, nuts, bombs and the hookshot (#85, #158); a bomb another player holds stays in their hands with its fuse blinking and explodes on every screen at once (#169); dungeon progress and room switches (#129); boss rooms entered together, and a player who walks out during the fight is pulled back in (#109); shared enemies, townspeople and Stalchildren (#124), all Deku Babas (#132); the host's save decides (a host's load resets the group, a guest's load takes the group's progress); guests who join from the title get the host's save (#167); chat, a player list, reconnects (also over VPNs such as Hamachi, #143) and rejoining after a crash; a join password with a lockout for repeated wrong guesses (#160); `collect-logs` masks addresses and names (#172); the updater adds new settings (#179); `/teleport <name>` moves you to another player (#156). Texture and model mods work; a game with a code or cheat mod stops co-op (#136).

New in this build, built in and not yet checked in a real game: <!-- MOVE proven items to 'What works' from the integrator's PASS table --> co-op on Hookshot (experimental); game traffic encrypted with the join password (#201); every player's Link in their own tunic colour, your own Link included (#202, needs a new puppet pack); a Linux and Steam Deck kit (experimental, #127) <!-- DROP if linux-2 is not in the release -->; hosting with an invite code, UPnP and hole punching (#165); signed updates (#161); a game-style HUD with the other players' names, hearts and place under your own hearts (the old panel is debug only, F7); PvP, off unless the host turns it on (`pvp = on`); story cutscenes watched together by players near each other (#173) and fewer snaps back after cutscenes (#194); the time of day (#192) and the Threads of Time (#193) shared; in the Deku Tree the scrubs' 2-3-1 order across players (#149, #144), its switches, plates, torches, moving platform, ladder, bridge and webs (#176, #177, #129, #196), the map chest (#146) and the pushed block (#147); Gohma fighting both players with its larvae in step (#150, #178) and no early move-in (#148); the Peahat waking (#157); enemy projectiles (#189), drops (#102), chasers (#101), late kills (#133), multi-part enemies (#105) and guards (#191) in step; pots, boulders and cut signs (#84, #90); Gold Skulltula tokens (#89); the Kakariko gate (#171); the Kokiri Shop's Deku Shield for each player (#130); two players fishing (#151); no world reset when both are in one area (#159); the impact of others' shots (#166); the ocarina in the other Link's hand (#152) and a given ocarina that works (#190); Navi staying in her player's room (#145); a rider on Epona (#199); fewer disconnects over the internet and Hamachi (#175, #143); a safer rejoin (#163).

**Saves: the host's save decides.** Progress is shared while you play; when the host loads a save, every game goes back to that save's progress, and a guest who loads a save gets the host's. To undo something, the host loads an older save; if the host restarts and loads, the guests' progress since the host's last save is undone. See [Saves](docs/setup.md#saves).

Known issues of this build: other players' ocarina songs are not heard (#152); others' masks and boots not yet seen on the other screen (#91); the Gohma fix needs a save from before Gohma to be checked (#150); the Stalchildren sharing is only unit-tested (#124); only game version 1.1.1 is checked, other builds are unproven (#97); shader stutter in new places the first time; Hookshot is experimental (no co-op window or chat there yet, a Hookshot player's own ocarina notes are not sent, it can crash outside Kokiri Forest or on some AMD cards); the Linux kit is experimental and not yet played on a real Linux PC or Deck. See [Known issues](docs/known-issues.md) and the release notes.

**Safety.** Only the host opens a port: UDP 27510 for the co-op server; players who join open nothing. With the invite code, `Host.bat` asks the host's router (UPnP) to forward that one port only while the server runs, and only when a join password is set; it removes the mapping when the server stops. The password is random for each start, never written to the logs, and the hole-punch messages are signed with it; with it the game traffic is encrypted (X25519 and XChaCha20-Poly1305), so nobody on the path can read or change it. The server and the games check every message against fixed limits and never write where a message says, and the updater downloads only this project's releases over HTTPS and checks their signature and SHA-256. Share an invite code only with friends: it holds your address and the password, and it stops working when `Host.bat` closes. `upnp = no` and `invite = no` in `coop-options.ini` turn these off; `server_allow` lists the players who may join ([Who can join](docs/play-online.md#who-can-join)). Details and how to report a problem: [SECURITY.md](SECURITY.md).

Every player needs the same release: a game of one release refuses a server of another. A kit updates itself with `Update.bat` (see [Updating](docs/setup.md#updating)).

## Status

Stage 0 is done and stage 1 (co-op in the emulator) is well under way: each part is checked on one PC with the real game in two emulator instances, and the test releases ([above](#play-the-test-release)) are played by testers on separate PCs. The plan is in [docs/plan.md](docs/plan.md), the wire protocol in [docs/protocol.md](docs/protocol.md), and what a test kit can show in [docs/testing.md](docs/testing.md).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Done: ENet server and client, wire protocol, `tools/coop_test` (10 minute soak runs pass on one PC). Two-PC run not yet done. |
| 1. Presence: other players shown as markers | Done on one PC: player sampling and smoothing ([docs/presence.md](docs/presence.md)), the emulator hook with `coop.ini` settings, a status panel and markers drawn from the game's camera ([docs/lichii.md](docs/lichii.md)). |
| 2. Puppet Link in the world | Done on one PC: the puppet pack is built from the player's own game files ([docs/puppet.md](docs/puppet.md)); each game shows the other players as child Link, following them live and copying their whole pose every frame, with Navi beside them and a name tag overhead; a player who joins later gets a Link for everyone already there. Their sword, shield, held items (slingshot, bow, hookshot) and gear (masks, boots, tunics) show (#91, #92). Adult Link shows and walks (#111). Their sounds are built in (#131), not yet heard in a real game. The ocarina in their hand (#152) and a rider on Epona (#199) are built in, not yet checked in a game. |
| 3. Scenes, following and cutscenes | Built: puppets only in your scene, hidden or held in cutscenes (unit-tested, not yet checked in game). The join-partner warp was dropped. Built in, not yet checked in a game: story cutscenes watched together by players near each other (#173), fewer snaps back after cutscenes (#194), the time of day (#192) and the Threads of Time (#193) shared. |
| 4. Shared progress (chests, keys, doors, songs) | Done on one PC: world flags shared through the host ([docs/flags.md](docs/flags.md)); items, songs and one-time pickups shared while rupees and the rest stay per player ([docs/items.md](docs/items.md)); small keys, Skulltula tokens and heart pieces add up; a late joiner gets them; dungeon switches, doors, cleared rooms and water levels shared ([docs/dungeon.md](docs/dungeon.md)), and a save load no longer undoes them (#129); chests opened in one game are open in all (#87); a shared item comes with its ammo and room (#128, #138). Heart containers raise everyone's maximum health (#88). The host's save decides: a host's save load resets the group to it. A guest who joins from the title gets the host's save in a co-op save folder (#167; the transfer seen in a two-game test). Story and cutscene flags shared (#173, built). |
| 5. Shared enemies and objects | In progress: enemies, townspeople and animals move, animate and die the same in every game, owned by one player ([docs/objects.md](docs/objects.md)); seeds and nuts a player makes appear in the other games ([docs/spawns.md](docs/spawns.md)); crates and thrown rocks break in every game ([docs/world.md](docs/world.md)). Built, not yet fully checked in a game: pots, the time of day following the host ([docs/time.md](docs/time.md)), bombs, arrows and other projectiles, multi-part enemies and bosses. Signs cut in one game are cut in the others (seen in the logs; the pieces on the other screen not yet). Stalchildren in Hyrule Field are shared (#124); boss rooms pull a player who walks out back in (#109). A held bomb stays in the holder's hands and explodes on every screen at once (#169). Built, not yet checked in a game: the Deku Tree's switches, plates, ladder, bridge and webs (#176, #177), Gohma's larvae (#178). Also built in, not yet checked: Gohma fighting both players (#150), enemy projectiles (#189), drops (#102), guards (#191), the moving platform (#196), the map chest (#146), the pushed block (#147), the scrubs' order across players (#149), the Kakariko gate (#171), two players fishing (#151). |
| 6. Chat, player list, reconnects | Done on one PC: reconnects and rejoining after a crash; the host moves to another player when one leaves; an in-game chat box (Enter) and player list (Tab); a join password (#160). Fewer disconnects over the internet built in (#175, #143). Built in, not yet checked in a game: hosting with an invite code, UPnP and hole punching, no VPN needed (#165; [docs/play-online.md](docs/play-online.md)); signed updates (#161); a game-style HUD with the other players' hearts; PvP, off by default. |
| Recomp port of stages 0 to 6 | Experimental on [Hookshot](https://github.com/NotRexed/Hookshot), a static recompilation of the same game build: the same co-op code, server and protocol, so Hookshot and emulator players play together. Needs a mesh-shader GPU (RTX 20 or newer, RX 6000 or newer). Test releases after the current one are Hookshot only; the current release still has the emulator kit. Plan and state: [docs/hookshot.md](docs/hookshot.md). |

The emulator side is a small patch on Frizti's Fritchii, pinned in [lichii/PIN](lichii/PIN); `lichii/fetch.ps1` fetches that source and applies the patches. See [lichii/README.md](lichii/README.md).

## Building

Needs CMake 3.24 or newer and a C++20 compiler (Visual Studio 2022 on Windows). ENet and doctest are fetched by CMake at pinned versions. `cmake -S . -B build`, then `cmake --build build --config Release` and `ctest --test-dir build -C Release`.

To test co-op on two PCs with the test kit, see [docs/testing.md](docs/testing.md).

## What you need

- Your own legally obtained copy of the game, dumped from your own console. This repository has no game files, keys or firmware and never will.
- Every player needs the same game version and the same co-op build.

## Contributing

Anyone can contribute: fork, branch, pull request. Every pull request is reviewed before it is merged. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Questions go to [Discussions](https://github.com/daurosk/oot-remake-multiplayer/discussions) or [Discord](https://discord.gg/GwtRKAqng5).

## Rules for this repository

- **No game content.** No game files, keys, firmware, saves, shader caches or builds that contain them. `tools/check_content.py` runs on every push and pull request and fails if any are found.
- **Our own code only.** Other projects (Lichii, Eden, the TOTK Online mod, recomp projects) may be read to understand how they behave. Their code is not copied in unless its licence allows it, and then it is credited.
- **Plain English** in commits, pull requests and docs.

## Credits

- [Fritchii](https://github.com/Frizti/Fritchii) by Frizti (GPL-3.0): the emulator co-op is built into.
- [Hookshot](https://github.com/NotRexed/Hookshot) by NotRexed and contributors (GPL-3.0): the native PC build co-op also runs on ([docs/hookshot.md](docs/hookshot.md)).
- Other code and libraries we use, and their licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

GPL-3.0, see [LICENSE](LICENSE). This keeps the project compatible with Lichii and Eden, which are GPL-3.0.

This is a fan project. It is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.
