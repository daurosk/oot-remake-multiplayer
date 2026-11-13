# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC. Up to 16 players see each other as Link in the same world, share items and progress, fight the same enemies and chat.

It is an early test build: expect bugs. You bring your own copy of the game; nothing of the game is in this project.

**Need help?** Ask on [Discord](https://discord.gg/GwtRKAqng5). Questions and ideas also go to [Discussions](https://github.com/daurosk/oot-remake-multiplayer/discussions).

## Play the test release

The newest build is at the top of the **[releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)**. (The test builds are pre-releases, so GitHub's "latest" link skips them.)

### 1. What you need

- A Windows 10 or 11 PC (64-bit). There is also an experimental [Linux and Steam Deck kit](docs/setup.md#linux-and-steam-deck).
- **Your own copy of the game** (version 1.1.1), dumped from your own console. Every player needs their own.
- The **same release** as your friends. A game of one release cannot join a game of another.
- No code or cheat mods. Texture and model mods are fine.

### 2. Pick how you run the game

One zip, `oot-coop-<version>.zip`, has both ways. Players of both can be in the same session.

| | The emulator kit | Hookshot (experimental) |
|---|---|---|
| What it is | The game in the kit's own emulator (Fritchii v5.8.4) | The game as a normal Windows program, built on your PC from your own game |
| Good for | Most PCs. The safe choice. | A strong PC, and players who want no emulator |
| Needs | Your game, plus your console's keys and firmware | Your game as a `.nup` file; an NVIDIA RTX 20 or AMD RX 6000 card or newer; 16 GB of RAM; about 60 GB free while it builds |
| First start | A few minutes | 10 to 45 minutes (it builds the game once) |
| Start it with | `Host.bat` or `Join.bat` | `Play-OoT-Coop.bat` |

Not sure? Use the emulator kit. The test releases after this one are for Hookshot only, so Hookshot is where new features go.

### 3. Set up (once)

1. Download `oot-coop-<version>.zip` from the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases).
2. Unzip it to a short folder, for example `C:\oot-coop`.
3. Follow the setup guide for your way to play:
   - **[Emulator setup](docs/setup.md#emulator-setup-for-new-players)**: copy your keys and firmware in, start the game once.
   - **[Hookshot setup](docs/setup.md#play-on-hookshot-experimental)**: double-click `Play-OoT-Coop.bat` and give it your game file. It builds the game once.

### 4. Play together

One player is the **host**. The others **join**. Nobody types an address or a password.

**On the emulator kit**

1. The host double-clicks `Host.bat`. A server window opens and shows an **invite code**. It is already copied.
2. The host pastes the code to their friends in a private message.
3. Each friend double-clicks `Join.bat`, pastes the code and presses Enter.
4. Everyone starts the game in the emulator window. Friends choose **Continue** to play from the host's save.

**On Hookshot**

1. Everyone double-clicks `Play-OoT-Coop.bat` (or the **Play OoT Co-op** desktop shortcut). The game starts.
2. The host chooses **Host** on the title screen. The screen shows an **invite code**. It is already copied.
3. The host pastes the code to their friends in a private message.
4. Each friend copies the code, chooses **Join** on the title screen, then **OK**.
5. Friends choose **Continue** to play from the host's save.

You should now see each other as Link, with a name tag overhead. The full steps, and what to do when a code does not connect, are in **[Playing over the internet](docs/play-online.md)**. In the same house? See **[Playing on the same network](docs/play-lan.md)**.

### 5. In the game

| | Emulator kit | Hookshot |
|---|---|---|
| Chat | **Enter** | **T** |
| Player list | hold **Tab** | hold **F6** |
| Go to another player | type `/teleport <name>` in the chat | the same |

- **Saves: the host's save decides.** Friends who join play from the host's save, and their own saves are never touched. The host should save often. [More about saves](docs/setup.md#saves).
- **Stuck at a locked dungeon door** that another player already opened? Type `/teleport <their name>` in the chat.
- **Your name and tunic colour**: `color =` in `coop-options.ini` in the kit folder, or **Co-op Settings** on Hookshot's title screen. [Your colour](docs/play-online.md#your-colour).

### Already have a kit? Update

This release is protocol 11, so **everyone updates**, the host too. Close the games and the server window, then double-click `Update.bat`. After this one the kit keeps itself up to date: the launchers ask when a newer release is out. [Updating](docs/setup.md#updating).

### Where to find things

| I want to... | Page |
|---|---|
| Set up the kit, update it, or fix a setup problem | [docs/setup.md](docs/setup.md) |
| Play with friends over the internet | [docs/play-online.md](docs/play-online.md) |
| Play with someone in the same house | [docs/play-lan.md](docs/play-lan.md) |
| See what works and what does not yet | [docs/known-issues.md](docs/known-issues.md) |
| Read short answers to common questions | [docs/faq.md](docs/faq.md) |
| Report a bug | [Reporting a problem](docs/setup.md#reporting-a-problem) |
| Find any other document | [docs/README.md](docs/README.md) |

## What works

Seen working in our tests:

- The other players as Link (child and adult) with Navi, a name tag, their sword, shield and held items, in each player's own tunic colour.
- Shared items, songs, small keys, heart pieces and heart containers. Rupees, ammo and health stay your own.
- Shared dungeon progress: switches, doors, cleared rooms and opened chests.
- Shared enemies, townspeople and bosses. Boss rooms are entered together.
- Shots and bombs other players make show on your screen.
- Chat, a player list, `/teleport`, rejoining after a crash, and PvP when the host turns it on.
- Hookshot and emulator players in one session.

Not everything is checked in a real game yet, and some things do not work yet (other players' ocarina songs are not heard, for example). The full, honest list is in **[known issues](docs/known-issues.md)**.

## Safety

- Only the host opens a port (UDP 27510), and only while the server runs. Players who join open nothing.
- Every session has a join password. It is inside the invite code, so nobody types it, and all game traffic is encrypted with it.
- Share an invite code only with friends: it holds your internet address and the password. It stops working when the host's server closes.
- The updater downloads only this project's releases and checks them before it changes anything.

Details, and how to report a security problem: [SECURITY.md](SECURITY.md).

## For contributors

Anyone can contribute: fork, branch, pull request. Every pull request is reviewed before it is merged. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. All the design documents are listed in [docs/README.md](docs/README.md).

### Building

Needs CMake 3.24 or newer and a C++20 compiler (Visual Studio 2022 on Windows). ENet and doctest are fetched by CMake at pinned versions.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The emulator side is a small patch on Frizti's Fritchii, pinned in [lichii/PIN](lichii/PIN); see [lichii/README.md](lichii/README.md). The Hookshot side is in [hookshot/](hookshot/README.md). To test co-op on two PCs with a kit, see [docs/testing.md](docs/testing.md).

### Rules for this repository

- **No game content.** No game files, keys, firmware, saves, shader caches or builds that contain them. `tools/check_content.py` runs on every push and pull request and fails if any are found.
- **Our own code only.** Other projects may be read to understand how they behave. Their code is copied in only when its licence allows it, and then it is credited.
- **Plain English** in commits, pull requests and docs.

### Status

The project has two stages. Stage 1 is co-op in the emulator, which ships in the test releases. Stage 2 carries the same co-op, protocol and server into a native PC build of the game; it runs on [Hookshot](https://github.com/NotRexed/Hookshot) as an experiment. The plan is in [docs/plan.md](docs/plan.md) and the wire protocol in [docs/protocol.md](docs/protocol.md).

The numbers such as #91 below are issues on the [issues page](https://github.com/daurosk/oot-remake-multiplayer/issues).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Done: ENet server and client, wire protocol, `tools/coop_test` (10 minute soak runs pass on one PC). Two-PC run not yet done. |
| 1. Presence: other players shown as markers | Done on one PC: player sampling and smoothing ([docs/presence.md](docs/presence.md)), the emulator hook with `coop.ini` settings, a status panel and markers drawn from the game's camera ([docs/lichii.md](docs/lichii.md)). |
| 2. Puppet Link in the world | Done on one PC: the puppet pack is built from the player's own game files ([docs/puppet.md](docs/puppet.md)); each game shows the other players as child Link, following them live and copying their whole pose every frame, with Navi beside them and a name tag overhead; a player who joins later gets a Link for everyone already there. Their sword, shield, held items (slingshot, bow, hookshot) and gear (masks, boots, tunics) show (#91, #92). Adult Link shows and walks (#111). Their sounds are built in (#131), not yet heard in a real game. A rider on Epona (#199) seen in our tests; the ocarina in their hand (#152) seen in the logs. Each player's Link wears that player's tunic colour (#202, partly: adult Link keeps some green). |
| 3. Scenes, following and cutscenes | Built: puppets only in your scene, hidden or held in cutscenes (unit-tested, not yet checked in game). The join-partner warp was dropped. Seen in our tests: the scrubs' order across players (#149); partly: the Kakariko gate (#171). Built in, not yet checked in a game: story cutscenes watched together by players near each other (#173), the time of day shared (#192). Seen working: no snaps back after cutscenes (#194), the Threads of Time shared (#193). |
| 4. Shared progress (chests, keys, doors, songs) | Done on one PC: world flags shared through the host ([docs/flags.md](docs/flags.md)); items, songs and one-time pickups shared while rupees and the rest stay per player ([docs/items.md](docs/items.md)); small keys, Skulltula tokens and heart pieces add up; a late joiner gets them; dungeon switches, doors, cleared rooms and water levels shared ([docs/dungeon.md](docs/dungeon.md)), and a save load no longer undoes them (#129); chests opened in one game are open in all (#87); a shared item comes with its ammo and room (#128, #138). Heart containers raise everyone's maximum health (#88). The host's save decides: a host's save load resets the group to it. A guest who joins from the title gets the host's save in a co-op save folder (#167; the transfer seen in a two-game test). Story and cutscene flags shared (#173, built). |
| 5. Shared enemies and objects | In progress: enemies, townspeople and animals move, animate and die the same in every game, owned by one player ([docs/objects.md](docs/objects.md)); seeds and nuts a player makes appear in the other games ([docs/spawns.md](docs/spawns.md)); crates and thrown rocks break in every game ([docs/world.md](docs/world.md)). Built, not yet fully checked in a game: pots, the time of day following the host ([docs/time.md](docs/time.md)), bombs, arrows and other projectiles, multi-part enemies and bosses. Signs cut in one game are cut in the others (seen in the logs; the pieces on the other screen not yet). Stalchildren in Hyrule Field are shared (#124); boss rooms pull a player who walks out back in (#109). A held bomb stays in the holder's hands and explodes on every screen at once (#169). Built, not yet checked in a game: the Deku Tree's switches, plates, ladder, bridge and webs (#176, #177), Gohma's larvae (#178) and the early move-in fix (#148; our boss-door test was set up wrong, not checked yet). Also built in, not yet checked: Gohma fighting both players (#150), enemy projectiles (#189), drops (#102), guards (#191), the moving platform (#196), the map chest (#146), the pushed block (#147), two players fishing (#151). |
| 6. Chat, player list, reconnects | Done on one PC: reconnects and rejoining after a crash; the host moves to another player when one leaves; an in-game chat box (Enter) and player list (Tab); a join password (#160). Fewer disconnects over the internet built in (#175, #143). Built in, not yet checked in a game: hosting with an invite code, UPnP and hole punching, no VPN needed (#165; [docs/play-online.md](docs/play-online.md)); signed updates (#161); a game-style HUD with the other players' hearts. Seen in our tests: PvP (off by default), with sword hits both ways; game traffic encrypted with the join password on the emulator kit (#201). |
| Recomp port of stages 0 to 6 | Experimental on [Hookshot](https://github.com/NotRexed/Hookshot), a static recompilation of the same game build: the same co-op code, server and protocol, so Hookshot and emulator players can join the same game and see each other as Link in their colours (checked by hand). Needs a mesh-shader GPU (RTX 20 or newer, RX 6000 or newer). Test releases after the current one are Hookshot only; the current release still has the emulator kit. Plan and state: [docs/hookshot.md](docs/hookshot.md). |

## Credits

- [Fritchii](https://github.com/Frizti/Fritchii) by Frizti (GPL-3.0): the emulator co-op is built into.
- [Hookshot](https://github.com/NotRexed/Hookshot) by NotRexed and contributors (GPL-3.0): the native PC build co-op also runs on ([docs/hookshot.md](docs/hookshot.md)).
- Other code and libraries we use, and their licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Thanks to zizerak and Hot Onion for development tips (Hot Onion's tip, sharing each moving platform's clock with the server's time, is how the platforms are kept in step), and to everyone who play-tested.

## Licence

GPL-3.0, see [LICENSE](LICENSE). This keeps the project compatible with Lichii and Eden, which are GPL-3.0.

This is a fan project. It is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.
