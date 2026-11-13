# Setting up the co-op test release

This page takes you from "I downloaded the zip" to "the game runs with co-op". Then [play-online.md](play-online.md) (friends over the internet) or [play-lan.md](play-lan.md) (same house) says how to play together.

It is an early test build: expect bugs. [known-issues.md](known-issues.md) lists what works and what does not yet. Stuck? Ask on [Discord](https://discord.gg/GwtRKAqng5).

## Start here

**Everyone, first:**

1. Download `oot-coop-<version>.zip` from the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases).
2. Unzip it to a short folder you can write to, for example `C:\oot-coop`. This is your **kit folder**.
3. Pick one of the two ways to run the game:

| | The emulator kit | Hookshot (experimental) |
|---|---|---|
| What it is | The game in the kit's own emulator | The game as a normal Windows program, built on your PC from your own game |
| You need | Your game, your console's keys and firmware | Your game as a `.nup` file, an NVIDIA RTX 20 or AMD RX 6000 card or newer, 16 GB of RAM, about 60 GB free |
| Go to | [Emulator setup for new players](#emulator-setup-for-new-players) | [Play on Hookshot](#play-on-hookshot-experimental) |

Not sure? Use the emulator kit. Emulator and Hookshot players can join the same session. The test releases after this one are for Hookshot only; this release still has both.

**Other parts of this page:**

- [Updating](#updating) a kit you already have
- [Settings](#settings), [chat commands](#chat-commands) and [saves](#saves)
- [Linux and Steam Deck](#linux-and-steam-deck)
- [Troubleshooting](#troubleshooting) and [low frame rate](#low-frame-rate)
- [Reporting a problem](#reporting-a-problem)

## Play on Hookshot (experimental)

[Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0) runs the game as a normal Windows program, with no emulator. The kit builds it on your PC from your own copy of the game. It is **experimental**: newer than the emulator kit, and fewer players have tried it.

### What you need for Hookshot

- **A graphics card with Vulkan 1.3 and mesh shaders**: NVIDIA RTX 20 series or newer, or AMD RX 6000 series or newer. Older cards (GTX 10 series, RX 5000 and older) cannot run it. Intel cards are mostly untested. Without such a card, use the emulator kit.
- **16 GB of RAM.** A Hookshot game uses about 10 GB. 8 GB of video memory is recommended.
- **About 60 GB free** on the drive the kit is on, while it builds.
- **Your own game as a `.nup` file** (or its `romfs` and `exefs` folders): version 1.1.1 (program `0400486024DEC000`, update v131072, build `e0f3ccfa`). No keys or firmware are needed. Every player needs their own copy.
- Windows 10 or 11 (64-bit), and an internet connection for the first build (it downloads Hookshot's build tools, about 400 MB).

### Hookshot step by step

1. **Double-click `Play-OoT-Coop.bat`** in the kit folder.
2. **The first time, it builds the game.** It asks for your game's `.nup` file once: drag the file into the window and press Enter. Then it downloads Hookshot's build tools and builds the game. This takes **10 to 45 minutes**, and the window shows each stage. Leave it open.
   - What you should see at the end: "Done.", and a **Play OoT Co-op** shortcut on your desktop.
   - It also makes the puppet pack (what lets you see the other players as Link) from your own game files.
3. **The game starts.** Its title screen has three more rows under the game's own: **Host**, **Join** and **Co-op Settings**.
   - If you see a **(-) Multiplayer** prompt at the bottom left instead, press **Minus** to open the same pages.
4. **Choose Co-op Settings once** and set **Your name** and **Your colour**, then **Save** ([below](#the-title-screens-co-op-rows)).
5. **Choose Host or Join** to play with friends: [play-online.md](play-online.md#host-and-join-on-hookshots-title-screen) has the steps. Or choose Continue or New Game to play alone.

Next time, double-click the **Play OoT Co-op** shortcut (or `Play-OoT-Coop.bat`). The build is done, so the game starts in seconds.

**It keeps itself up to date.** When a newer co-op release is out, the launcher asks once, updates the kit, and puts the new co-op into the game in seconds. It builds the whole game again only when the update moves to a newer Hookshot (it says so; about 15 minutes). `auto_update = yes` in `coop-options.ini` updates without asking.

**Check it works.** After Host or Join, you should see the other players as Link with their name tags, and **T** opens the chat box. If you are not sure, open `hookshot_log.txt` beside `hookshot.exe` (in the kit's `hookshot\game` folder): it says `coop: module loaded`, then `coop: connected`.

<details>
<summary>Advanced: build by hand, check your game file first, where the files are</summary>

- **Check your game file without building:** open a command prompt in the kit folder and run `Build-Hookshot-Coop.bat -CheckOnly -Game <your .nup>`. It downloads the build tools, checks them and your game, and builds nothing. It ends with "Your game files are right for Hookshot" when all is well.
- **Build by hand:** double-click `Build-Hookshot-Coop.bat`. It finds your `.nup` in the emulator's game list folders if you set the emulator up before; otherwise it asks for it. Every download is checked against the SHA-256 this kit expects, and a file that does not match stops the build. `-Out <folder>` puts the game somewhere other than the kit's `hookshot\game` folder.
- **What it installs:** `hookshot_coop.dll`, the co-op settings (`coop.ini` beside `hookshot.exe`) and the puppet pack.
- **Where it keeps things:** the kit's `hookshot\` folder holds `tools\` (Hookshot's build tools and source with our patches), `downloads\`, `work\` (the unpacked game, kept so the next build is quick; delete it to get the space back) and `game\`. None of these are part of the kit, so an update keeps them.
- The built game is made from your game files, like the puppet pack: **never share it or upload it.**
- **`Host-Hookshot.bat` and `Join-Hookshot.bat`** host or join from a window, as `Host.bat` and `Join.bat` do for the emulator (a server window, an invite code to paste). A game started by them is in that session already, so its title screen's Host and Join answer "Co-op is on already in this game: quit the game to host or join another."

</details>

### The title screen's co-op rows

They work with the controller or the keyboard (arrows, Enter, Esc, Ctrl+V to paste).

- **Host** starts a game your friends can join, and copies an invite code for them.
- **Join** takes the code your friend sent you.
- **Co-op Settings** changes your settings without editing a file:
  - **Your name** (1 to 16 characters; your Windows user name until you change it)
  - **Your colour** (your Link's tunic; "By player number" means blue for the host and red for the others)
  - **Players when you host** (2 to 16)
  - **Voice chat** (the row is there, but this release has no voice chat yet)
  - Choose **Save** to keep them. If you are already in a session, they count the next time the game starts.

The steps for Host and Join, and what their screens say, are in [play-online.md](play-online.md#host-and-join-on-hookshots-title-screen). In the same house or on a VPN: [play-lan.md](play-lan.md#on-hookshots-title-screen).

### Co-op on Hookshot

The same co-op runs on Hookshot as in the emulator. Some keys differ, because Enter and Tab are the game's Plus button on Hookshot:

| What | On Hookshot |
|---|---|
| The other players | As Link, with Navi, a name tag and edge arrows, in their tunic colours |
| Chat | **T** opens the chat box, Enter sends, Esc closes. Chat commands such as `/teleport` work as on the emulator. |
| Player list | Hold **F6** |
| Co-op panel (ping, distances) | **F7** |
| HUD | The other players' names, hearts and place under your hearts, in the game's font |
| Item cheats | **F8**, only when the host's kit has `LICHII_COOP_CHEATS=1`: type to find an item, Up/Down or the mouse to choose, Enter or a click to give it |
| Your tunic colour | Your own Link wears your colour too |
| Your ocarina | Your notes are sent to the other players |

While the chat box or the cheats panel is open, the game reads no keys.

If **T** is one of your game buttons, pick another chat key: add `LICHII_COOP_CHAT_KEY=` and a letter, a digit, `F1` to `F12`, or `off` to `coop-options.ini`.

**Not on Hookshot yet:** `collect-logs.cmd` does not pick up `hookshot_log.txt` (send it by hand). Hookshot on Linux takes the same keys, but that is not tested yet.

### Hookshot troubleshooting

- **"No co-op Hookshot game in ..."**: the build that the launcher starts by itself did not finish (the window says why). Run `Build-Hookshot-Coop.bat` and let it finish.
- **The build stops on a download, or on a SHA-256 that does not match:** run it again (a download may have broken off). If it keeps failing, report it with the window's text.
- **Not enough space:** it warns below about 60 GB free. Free some space, or move the kit to a bigger drive.
- **The game closes at the start, or the screen goes black:** check that your card has mesh shaders (above) and that its driver is up to date. Hookshot's own open problems (crashes outside Kokiri Forest on some PCs, crashes on some AMD cards, a device loss soon after the start) are listed on [Hookshot's page](https://github.com/NotRexed/Hookshot). Play on the emulator meanwhile.
- **The other players show as a fairy or a dot, not as Link:** the puppet pack is missing or old. Run `Build-Hookshot-Coop.bat` again; it makes the pack again in a few minutes.
- **To try the game without co-op:** set `HOS_COOP=0` before starting `hookshot.exe`. If it crashes without co-op too, it is Hookshot's problem, not co-op's.
- **Reporting a problem:** attach `hookshot_log.txt` from beside `hookshot.exe` to your report, and say that you played on Hookshot ([Reporting a problem](#reporting-a-problem)).

## Emulator setup for new players

The usual way to play: the kit's own emulator (Fritchii, with co-op built in). It runs on most PCs that can run the game in an emulator.

Do steps 1 to 4 below, then [Check it works](#check-it-works). After that, [play-online.md](play-online.md) (over the internet) or [play-lan.md](play-lan.md) (same network) says how to play together.

## What you need

- A Windows 10 or 11 PC (64-bit) that can run the game on Fritchii, or a 64-bit x86 Linux PC or Steam Deck (glibc 2.39 or newer: Ubuntu 24.04, Fedora 40, Debian 13, Arch and others) with the Linux kit ([Linux and Steam Deck](#linux-and-steam-deck)).
- Your own copy of the game, dumped from your own console, with its keys (`prod.keys`, `title.keys`) and the console's firmware. The release has no game files, keys or firmware and never will. Every player needs their own.
- The same game version as the other players (build id `e0f3ccfaf50065ae`; the emulator's log names it, see [below](#check-it-works)).
- The same release of the co-op kit as the other players.
- **No code or cheat mods.** Mods that change the game's code (an `exefs` folder: cheat menus, gameplay patches) and cheats (`cheats` folders, `cheat_*` files) are not supported in co-op. Remove them, or turn them off for the game (in the emulator, right-click the game, Properties, Add-Ons, untick them). **Texture and model mods are fine** (only a `romfs` folder, for example fixes for missing Water Temple or Fairy Fountain textures); if one changes models, every player should use the same ones. Fritchii's built-in Dynamic FPS is not a mod, and neither is co-op's own puppet pack (`coop-puppet`, step 4).

## 1. Unzip

Unzip `oot-coop-<version>.zip` into a short folder you can write to, for example `C:\oot-coop` or your desktop. Keep the path short: the puppet pack (step 4) stops if the folder is too deep.

You get:

| File | What it is for |
|---|---|
| `Host.bat` | Start a game others join: starts the server (it shows an invite code for your friends) and your emulator |
| `Join.bat` | Join someone's game: asks for the host's invite code (or address) |
| `make-puppet-pack.cmd` | Builds the puppet Link from your own game files (once) |
| `coop-options.ini` | Which co-op features are on |
| `collect-logs.cmd` | Zips your logs to send with a bug report (addresses, passwords and names masked) |
| `Update.bat` | Updates the kit to the newest release ([Updating](#updating)) |
| `check-hosting.cmd` | Says why friends cannot reach your server, and what to change ([play-online.md](play-online.md)) |
| `Play-OoT-Coop.bat`, `Build-Hookshot-Coop.bat`, `Host-Hookshot.bat`, `Join-Hookshot.bat`, `hookshot\` | Hookshot, experimental ([above](#play-on-hookshot-experimental)) |
| `start-server.cmd`, `play.cmd` | The server alone, and the emulator alone (for advanced setups) |
| `emulator\` | The patched emulator; `emulator\user\` holds all its data |
| `SOURCE.txt`, `licenses\`, `THIRD_PARTY_NOTICES.md` | Where the source is, and the licences |

## 2. Your keys, firmware and saves

The kit's emulator is portable: it keeps everything in `emulator\user\` and never touches a normal Fritchii, Lichii or Eden install.

If this PC already plays the game on Fritchii (or Lichii or Eden), copy (do not move) your `keys` and `nand` folders into the kit:

- the `keys` folder to `emulator\user\keys`
- the `nand` folder to `emulator\user\nand` (firmware, game updates and saves; a few GB)

Where to find them:

- **A normal install** keeps them in `%APPDATA%\eden` (paste that into the Explorer address bar).
- **A portable install** (a Fritchii, Lichii or Eden you unzipped, with a `user` folder next to its exe) keeps them in that folder: `user\keys` and `user\nand` next to the exe. `%APPDATA%\eden` is then empty or missing.
- **Not sure which?** Search your drives for `prod.keys` in Explorer. The folder it is in is the `keys` folder; the `nand` folder is beside it.

Otherwise put your keys in `emulator\user\keys\`, start the emulator once (`Join.bat`, any address) and use **Tools > Install Firmware** and **File > Install Files to NAND** for your firmware and game update.

## 3. First start

This start is only to see that the game runs. You are not joining anyone yet.

1. Double-click `Join.bat`.
2. When it asks for the invite code or address, type `127.0.0.1` (this means "this PC") and press Enter.
3. Type a player name: 1 to 16 characters, different from your friends'. If it asks for a join password, just press Enter.
4. The emulator opens. Double-click its empty game list and pick the folder that holds your game file.
5. Start the game once to see that it runs. Then close the emulator.

Windows may ask a few things the first time:

- **"Windows protected your PC" (SmartScreen):** click **More info**, then **Run anyway**. The kit's programs are not signed.
- **Windows Firewall asks about `lichii.exe` (the emulator) or `coop_server.exe` (the server):** tick **Private networks** and click **Allow access**. It asks once per folder the kit is in.
- **`MSVCP140.dll` or `VCRUNTIME140.dll` is missing:** install the Microsoft Visual C++ 2015-2022 Redistributable (x64).

## 4. The puppet pack

The puppet pack is what lets you see the other players as Link. It is made from your own game files, so it cannot come in the zip.

**`Host.bat` and `Join.bat` make it by themselves** when it is missing or old: they say "The puppet pack ... is missing or old: making it now (about a minute)". If that worked, skip this step.

To make it by hand:

**Most players: your game is a `.nup`. Just double-click `make-puppet-pack.cmd`. No keys, no Dump RomFS.**

1. Double-click `make-puppet-pack.cmd` in the kit folder.
2. It finds the `.nup` in the emulator's game list folders (step 3). If it asks for it, type its path or drag the file into the window.
3. Wait for it to finish: it reads the game files straight from the `.nup` and builds the pack in about a minute. It also makes the co-op HUD's look from the same files (the game's font, heart pictures and place names, for the other players' names, hearts and places under your hearts): it prints `co-op HUD: font, 5 hearts and ... place names`. If that part fails it prints a note and co-op still works, with plain text.

Do not use the emulator's **Dump RomFS** for a `.nup`: you do not need it, and it often fails with "RomFS Extraction Failed" (for example when your keys are older than the game's update).

**Which file do you have?** Look at the file you added to the emulator's game list in step 3: its name ends in `.nup`, `.nsp` or `.xci` (the game list also shows it in its **File type** column: NUP, NSP or XCI). `.nup`: the steps above are all you need. `.nsp` or `.xci`: see below.

Good to know:

- **One pack per PC.** After an update that changes the pack, the next `Host.bat` or `Join.bat` makes it again by itself.
- **Without the pack co-op still works**, but the other players show only as a name and a dot, and co-op's text is plain (no game font or hearts).
- **The pack also holds the tunic colours.** Each player's Link wears that player's colour, and your own Link wears yours (the Kokiri tunic; the Goron and Zora tunics keep their colours). Pick yours with `color =` in `coop-options.ini` ([Your colour](play-online.md#your-colour)).
- **Everyone is green?** Your pack was made before the colours. `Host.bat` and `Join.bat` then say "Puppet pack: made without the tunic colours ... Run make-puppet-pack.cmd again."

### Only if your game is an `.nsp` or `.xci`: dump the RomFS first

<details>
<summary>Dump steps for an <code>.nsp</code> or <code>.xci</code> (or when the tool cannot use your <code>.nup</code>)</summary>

1. Check first: the game starts in the kit's emulator (step 3), and the game **update** is installed in the kit's emulator (step 2), not only in another emulator. Without it you dump the old base game, which co-op cannot use.
2. Start `Join.bat`, right-click the game in the list and choose **Dump RomFS**. If it asks which RomFS, pick **Program [0400486024DEC000]**. Then pick **Full** (not Skeleton). It writes several GB to `emulator\user\dump\0400486024DEC000\romfs\` and needs that much free space plus 1 GB. Keep the progress window open until it says **RomFS Extraction Succeeded!**; then close the emulator.
3. Double-click `make-puppet-pack.cmd`. It checks the dump and builds the pack. If something is wrong it says what and how to fix it (see [Troubleshooting](#troubleshooting)). If it finds nothing, it asks for the `.nup` or the folder that holds `Pack`, `Model`, `AS`, `RSDB` and `System`.
4. You can delete the `dump` folder afterwards.

</details>

## Check it works

1. Double-click `Host.bat`. A server window opens, then the emulator.
2. Start the game and load a save.
3. Press **F7**. A small panel in the top-left corner says `Co-op: player 1, ping ... ms`, and its first line ends in `, encrypted`. Co-op is working. Press F7 again to hide it.

If you want to look closer: the emulator's log, `emulator\user\log\eden_log.txt`, has lines starting with `coop:`. Among them are `coop: build ...` (which co-op build this is), `coop: welcomed as player 1`, `coop: game build e0f3ccfaf50065ae` and `coop: traffic encrypted`.

Next: [play-online.md](play-online.md) (over the internet) or [play-lan.md](play-lan.md) (same network).

## Updating

Every player needs the same version: a game of one version cannot join a server of another.

**This release is protocol 11: everyone updates.** ("Protocol" is the version of the language the games and the server speak. A game of this release refuses a server of an older one, and the other way round.) The host and every friend update before playing:

1. Close the games and the server window.
2. Double-click `Update.bat` in the kit folder. (Or answer **Y** when `Host.bat`, `Join.bat` or `Play-OoT-Coop.bat` asks "Update now?".)
3. Start as usual. The first start does the rest by itself:
   - **Emulator:** `Host.bat` or `Join.bat` makes the puppet pack again (about a minute). If the emulator changed, the game shows **"Preparing shaders"** once: let it finish ([Low frame rate](#low-frame-rate)).
   - **Hookshot:** `Play-OoT-Coop.bat` puts the new co-op and puppet pack into the game. It builds the whole game again only when the update moved to a newer Hookshot (about 15 minutes).

Your saves, keys, settings and `coop-options.ini` are kept.

### Automatic updates

When `Host.bat`, `Join.bat`, `play.cmd` or `Play-OoT-Coop.bat` start and a newer release is out, they ask "Update now? [Y/n]".

- **Yes** (also after 10 seconds with no key): the whole kit is updated, the emulator included, and the new version starts.
- **No**, or no network, or GitHub is down: this version starts as it is, after at most 15 seconds, with a line saying why.
- If an update fails, the old files are put back and this version starts.

`auto_update` in `coop-options.ini` sets this: `ask` (the default), `yes` (update without asking) or `no` (only say that one is out).

An update stops this kit's server if it is still running. So a host should update when nobody is playing: the friends on it are dropped (and need the same update anyway).

### What an update keeps

- Everything in `emulator\user\`: keys, firmware, saves, settings, your name and server, the puppet pack.
- `logs\`, and any file of yours that is not part of the kit.
- `coop-options.ini`: your lines stay as they are. Every setting the new version adds is written at the end, with its comment.
- The Hookshot game in `hookshot\`.

Every file it replaces goes first to `backup-<old version>\` in the kit folder. If something fails half way, those files are put back. Delete the backup folder once the new version works.

When the new release brings a different emulator, the old shader cache (`emulator\user\cache\shader\`) is moved to the backup too, because a cache made by another emulator build can crash it. The first minutes of play may then stutter while a new one is built.

### If an update does not work

- **It was stopped** (window closed, power cut): run `Update.bat` again. It finishes the update.
- **It cannot update:** it says why. To update by hand, download the newest `oot-coop-....zip` from the [releases](https://github.com/daurosk/oot-remake-multiplayer/releases), unzip it to a new short folder, copy `emulator\user` and `coop-options.ini` from your old kit into it, and play from there.
- **It could not check for a new version** (no network, or GitHub turned the check away for a while): `Update.bat` checks again.
- **Your kit has no `Update.bat`** (the first kits had none): download `Update.bat` from the newest release once and put it in the kit folder beside `Host.bat`. Windows may warn about a file from the internet the first time: **More info**, then **Run anyway**.

<details>
<summary>Advanced: what the updater checks and sends</summary>

- `Update.bat` looks for the newest release on the [Releases](https://github.com/daurosk/oot-remake-multiplayer/releases) page, says which one, and asks before it changes anything.
- It checks the download against the SHA-256 published with the release.
- When the GitHub CLI (`gh`) is installed and logged in, it also checks the release's signature (its attestation, made by our release workflow). A zip that fails it is refused. `verify_signature = no` in `coop-options.ini` skips the signature check, not the SHA-256.
- It needs no admin rights: it writes only in the kit folder and your temp folder.
- It sends nothing about you: one request for the list of releases, and the two downloads.
- The emulator is updated only this way, with the co-op build from our release. Fritchii's own updater is switched off in our builds: it would put a plain Fritchii without co-op over the kit's. When Frizti publishes a newer Fritchii, the next co-op release moves to it.

</details>

## Check the download

Optional. `Update.bat` checks every download against the SHA-256 published with the release, which proves the file arrived whole. Each release's zip and `Update.bat` are also signed: when the release is published, our release workflow on GitHub makes a signed record (an artifact attestation) of those exact files. To check that a zip you downloaded is the one the release published, install the [GitHub CLI](https://cli.github.com), run `gh auth login` once, and in the folder with the zip:

```
gh attestation verify oot-coop-<version>.zip --repo daurosk/oot-remake-multiplayer --signer-workflow daurosk/oot-remake-multiplayer/.github/workflows/release-attest.yml
```

It must say the verification succeeded. If it fails, do not use the zip, and tell us ([Reporting a problem](#reporting-a-problem)). From a copy of this repository, `tools\release\verify_kit.ps1 -Zip <zip>` does the same and also checks the `.sha256` beside it.

## Settings

Most players never change a setting. If you want to:

**`coop-options.ini`** (in the kit folder; open it with Notepad) turns features on and off. Each setting has a comment above it. A line with `#` in front is off: add or remove the `#`, then close the game and start it again. Everyone in a session should use the same settings.

| Setting | What it does |
|---|---|
| `color = blue` | Your Link's tunic colour ([Your colour](play-online.md#your-colour)) |
| `auto_update = ask` | `ask`, `yes` or `no` ([Automatic updates](#automatic-updates)) |
| `pvp = off` | The host only: `on` lets players hurt each other ([PvP](play-online.md#pvp-players-hurting-each-other)) |
| `server_password =` | The host only: your own join password instead of a new random one each time |
| `server_allow =`, `server_bind =` | The host only: who may join ([Who can join](play-online.md#who-can-join)) |
| `upnp = no`, `invite = no` | The host only: turn off the router forward or the invite code |

On Hookshot, your name, your colour and the number of players are also in **Co-op Settings** on the title screen.

**Your name and the server address** are in `emulator\user\config\coop.ini`. `Join.bat` asks for the address each time; `play.cmd change` asks for both again.

**The join password.** Every session has one, and the game traffic is encrypted with it.

- `Host.bat` makes a new random one each time it starts and puts it in the invite code, so nobody types it.
- A host who wants the same one every time sets `server_password = <one without spaces>` in `coop-options.ini`. Friends who join by address (not by invite code) then type it when `Join.bat` asks. Enter keeps the last one.
- A wrong one shows `wrong join password` in the `Join.bat` window.

## Chat commands

Open the chat box (**Enter** on the emulator, **T** on Hookshot), type a line and press Enter. A line that starts with `/` is a command, not chat.

| Type | What it does |
|---|---|
| `/teleport` | Lists the other players |
| `/teleport <player>` (or `/tp <player>`) | Takes you to that player |
| `/pvp` | Says whether players can hurt each other |
| `/pvp on`, `/pvp off` | The host only: turns that on or off for everyone ([PvP](play-online.md#pvp-players-hurting-each-other)) |

### /teleport

`/teleport <player>` takes you next to another player. For `<player>` type their name (capitals do not matter), the start of their name if nobody else's starts that way, or their player number (`/teleport 2`).

- **Same area:** you are moved there at once.
- **Same dungeon, another room:** you are moved to them, in any room.
- **Another area:** the game has to load that area first. Talk to someone or read a sign within a minute. When the talk ends the screen fades, the area loads, and you are moved next to them.
- **Into a dungeon from outside:** you arrive at the dungeon's entrance. If they are not in that first room, type `/teleport <player>` once more.

**Stuck at a locked dungeon door another player already opened?** Type `/teleport <their name>`. This is the way round in this release.

It says no, with a line in the chat box, when:

- your game is loading, or you are in a cutscene, a talk or a boss fight;
- the other player is busy (loading, a cutscene, a talk, in the air) or in a boss fight;
- the other player is in a place your Link cannot go at that age, or a place with no entrance to arrive at (the boss-rush room, the ending, the Sage room).

<details>
<summary>More about /teleport</summary>

- The other player's game answers with where they last stood on their feet, so you never land in a wall or in mid-air.
- A scene change you did not take within 60 seconds is given up.
- Lake Hylia, Lon Lon Ranch, the Temple of Time and the houses are parts of Hyrule Field, the Market or their village, so those work.
- A dungeon gives a warning: you may need its items to get on from there.
- "no answer from" after 5 seconds: the other player has an older build.
- **If the screen stays black after a teleport into a dungeon** and Link cannot move ([issue 197](https://github.com/daurosk/oot-remake-multiplayer/issues/197), seen in an older release): close the game, start it again, and walk there instead.
- `LICHII_COOP_TELEPORT=0` in `coop-options.ini` turns it off in your game: the command says so, and nobody can teleport to you.

</details>

## Chat notices

When another player's progress reaches your game, a line in the chat says so (only in your game, nothing is sent): "Tobi got the Fairy Slingshot - you got one too!", "Tobi found a Small Key (Deku Tree) - the group has 2", "Tobi got a Heart Container - your max health went up", "Tobi got the map (Deku Tree)", "Tobi learned Zelda's Lullaby", "Tobi got a Gold Skulltula token (12)", "Tobi opened a chest". Joining a session says one line ("Synced with the group: 14 items, 3 songs"), and so does the host loading a save. At most 4 lines in 3 seconds; the rest is one "...and N more" line. To turn them off, set `LICHII_COOP_NOTIFY=0`.

## Saves

**The host's save decides.** In short:

- **While you play, progress is shared**: items, keys, chests, doors and switches. Rupees, ammo, hearts and where you stand stay your own.
- **When the host loads a save, every game goes back to that save's progress.** To undo something (a key used in the wrong place, for example), the host loads an older save.
- **A friend who loads a save** gets the host's progress. Nothing changes for the others.
- **The host should save often**, especially after the friends make progress. If the host restarts the game and loads its save, **everything the friends did since the host last saved is undone** for everyone.

**Friends play from the host's save file**, on the emulator and on Hookshot:

1. When you join, your game uses a separate co-op save folder. **Your own saves are never touched.**
2. The host's newest save is copied into it. The chat says "The host's save is ready".
3. On the title screen choose **Continue** (or **Load Game**, the newest save). You start where the host's save starts, with its items, hearts and rupees.

When the host loads another save, you get that one too, after the host's next door or save point. Load it to stand where it starts; your progress already follows the host at once.

When you are not joining (playing alone, or hosting), the game shows your own saves as before.

**Where are my saves?** On the emulator: `emulator\user\nand\user\save\` in the kit folder. Updates never touch them. Back up that folder if you want a copy.

## Linux and Steam Deck

There is a Linux kit too: `oot-coop-<version>-linux-x86_64.tar.gz` on the same release page. A Linux player and a Windows player can play together on the same release. Everything above applies; only the file names and a few commands differ.

**What you need on top:** a 64-bit x86 Linux (or a Steam Deck in Desktop Mode), and for the puppet pack, Python 3 with the `zstandard` package (`sudo apt install python3 python3-zstandard` on Debian or Ubuntu, `sudo pacman -S python python-zstandard` on Arch). The emulator is an AppImage. If it does not start and says FUSE is missing, install `libfuse2` (Debian, Ubuntu) or `fuse2` (Arch).

| Windows kit | Linux kit |
|---|---|
| unzip the `.zip` | `tar -xzf oot-coop-<version>-linux-x86_64.tar.gz` (keep the folder somewhere your user can write, for example `~/oot-coop`) |
| `emulator\user\keys\` | `emulator/user/keys/` (the same folders under `emulator/user/` for keys, firmware, saves and settings) |
| `make-puppet-pack.cmd` | `./make-puppet-pack.sh` |
| `Host.bat`, `Join.bat`, `play.cmd` | `./Host.sh`, `./Join.sh`, `./play.sh` (in a terminal, in the kit folder) |
| `start-server.cmd` | `./start-server.sh` (`Host.sh` starts it in the background; `./start-server.sh --stop` stops it) |
| `Update.bat` | `./update.sh` (also offered on every start, as `auto_update` in `coop-options.ini` says) |
| `collect-logs.cmd` | `./collect-logs.sh` (writes `coop-logs-<time>.tar.gz`) |

- **Portable folder.** On Linux the emulator uses the `user` folder of the folder it is started in, so start it through the scripts, not by double-clicking `emulator/lichii.AppImage`. Started anywhere else, it uses your normal emulator folders (`~/.local/share/eden`) and co-op settings from there.
- **Hosting.** `./Host.sh` works as `Host.bat` does: it makes a new join password each time (unless `server_password` in `coop-options.ini` sets one), asks your router to forward the port (UPnP), and prints an **INVITE CODE** in the terminal (copied to the clipboard when `wl-copy`, `xclip` or `xsel` is installed; also in `logs/invite-code.txt` while the server runs). Send it to your friends; they paste it into `Join.sh` or `Join.bat`. If a friend gets an **ANSWER CODE**, run `./start-server.sh --answer <code>` in the kit folder. The server runs in the background: `./start-server.sh --stop` stops it. Open the server's UDP port if a firewall runs, for example `sudo ufw allow 27510/udp`. `start-server.sh` lists this PC's addresses.
- **Joining.** `./Join.sh` asks for the invite code or the host's address (and then the join password, if the host gave you one). With an invite code a join helper runs in the background while you play and stops when the emulator closes; its log is `logs/join-*.log`. The terminal says when you are connected, or why not. A relay join code (`CODE@relay`) works only in the Windows kit for now.
- **Mods.** As on Windows: code and cheat mods stop `Host.sh` and `Join.sh` and are named; texture and model mods are fine.
- **Steam Deck.** Not tested yet: nobody has run the kit on a Deck so far, so tell us how it goes ([Reporting a problem](#reporting-a-problem)). Use Desktop Mode: open Konsole, `cd` to the kit folder and run `./Join.sh` (or `./Host.sh`). Play with the Deck's own controls once the game runs. To start it from Game Mode, add `play.sh` as a non-Steam game with its "Start in" set to the kit folder; the first start must still happen in Desktop Mode, because it asks for your name and server.

## Troubleshooting

- **"These mods for Ocarina of Time change the game's code or are cheats"** when you start `Host.bat`, `Join.bat` or `play.cmd`: the folders or files it names are code or cheat mods (in `emulator\user\load\0400486024DEC000\`, or the SD card's `atmosphere\contents\0400486024DEC000\`). Code and cheat mods are not supported in co-op; texture and model mods are fine. Remove the named ones, or turn them off in the game's Add-Ons (right-click the game, Properties, Add-Ons), then start again.
- **The co-op panel says "Co-op is off: a code mod is installed"**: a mod changed the game's code where co-op hooks in, so co-op left its code out and did not connect. The log says where (`coop: off: a code mod is installed (main+0x...)`). Turn the mod off as above and restart the emulator.
- **"RomFS Extraction Failed! There was an error copying the RomFS files or the user cancelled the operation." (step 4).** First check which game file you have ([step 4](#4-the-puppet-pack)): if it ends in `.nup`, you do not need Dump RomFS at all. Double-click `make-puppet-pack.cmd` and give it the `.nup`. Only with an `.nsp` or `.xci`: the emulator shows this one message for nearly every failure and writes nothing about the dump itself in its log. In order:
  - Your keys are older than the game's update. The log (`emulator\user\log\eden_log.txt`) then has `Cannot decrypt ...: keys for gen 0x17+ required.`, and `make-puppet-pack.cmd` says **"Your prod.keys are too old for this game's update"**. Gen 0x17 needs `master_key_16` in `prod.keys`; keys that stop at `master_key_15` cannot decrypt the update. The game can still run from a `.nup`, which is not encrypted. Fix: build the pack from the `.nup` (step 4), or dump newer keys from your console.
  - It came at once, before any progress bar: the emulator cannot read the game. Check that `emulator\user\keys\prod.keys` exists (the "No keys" item below), the firmware is installed, and the game starts in this emulator.
  - You pressed Cancel or closed a window: the dump-mode question, the "which RomFS" list, or the progress window. Dump again and keep the progress window open.
  - It came part way through: writing failed (disk full, the kit on a USB stick that was pulled out, or a too-deep folder). Free up space or move the kit to a short folder such as `C:\oot-coop`, then dump again.
- **"There is not enough free space at ... to extract the RomFS" (step 4).** The full dump needs the game's size plus 1 GB on the kit's drive. Free up space, or move the kit to another drive, then dump again.
- **make-puppet-pack.cmd says ".nup ... is damaged", "is cut short" or "has no ...".** The `.nup` file is incomplete (a copy or download that did not finish) or does not hold the whole updated game. Copy it again from where you made it.
- **make-puppet-pack.cmd says "Found no .nup game file ... and no RomFS dump".** Give it your `.nup` (type the path or drag it in). With an `.nsp`/`.xci`: the dump did not finish (it deletes its folder when it fails), or it went to another folder (**Emulation > Configure > System > Filesystem > Dump Root**; the tool looks there too). It lists what usually stops the dump: no keys, nothing in the NAND, little free space, a deep folder. Fix those and dump again, or type the folder of a dump you have elsewhere.
- **"... has folders but no files: that is a Skeleton dump."** You picked Skeleton. Delete the folder it names and dump again with **Full**.
- **"The dump is not complete: no ..." or "The dump has empty or damaged files".** The dump stopped part way (emulator closed, crashed, PC slept, disk filled). Delete the folder it names and dump again; keep the progress window open until it says it succeeded.
- **"... is not the game's RomFS ... the manual or legal text was dumped instead."** In the "which RomFS" list you picked HTML document or Legal information. Delete the folder and dump again, picking **Program [0400486024DEC000]**.
- **"This dump is game data version 100; co-op needs 111".** The kit's emulator does not have the game update, so it dumped the base game. Install the update (**File > Install Files to NAND**, or copy your `nand` folder, step 2), check the game starts as the updated version, delete the dump and dump again.
- **"Python is missing the zstandard package" or "No Python found".** Do what it says (a Command Prompt line), then run `make-puppet-pack.cmd` again.
- **"The kit folder is too deep for Windows".** Move the whole kit folder to a short path such as `C:\oot-coop` and run it again.
- **Still failing?** Comment on [issue 115](https://github.com/daurosk/oot-remake-multiplayer/issues/115) with the exact message (a screenshot of the box), everything `make-puppet-pack.cmd` printed, and your `VERSION.txt`; send the zip from `collect-logs.cmd` privately ([below](#reporting-a-problem)).
- **"No keys", or the game does not show in the list or does not start.** The kit's emulator does not see your keys. Check that `emulator\user\keys\prod.keys` exists (not one folder deeper) and that you copied from the right place (step 2: a portable install keeps them next to its exe, not in `%APPDATA%\eden`). Restart the emulator after copying.
- **The emulator crashes after an update, or after copying files from another emulator: delete the shader cache.** Close the emulator and delete the folder `emulator\user\cache\shader` in your kit folder (only that folder: your saves, keys and settings are elsewhere in `emulator\user`). The emulator builds a new cache; the first minutes of play may stutter. From another emulator copy only the keys, firmware and `nand` (step 2), never its `cache` folder.
- **The emulator closes by itself on a PC with an AMD graphics card** (while building shaders, or a few seconds after the game starts), and Windows' Event Viewer (Windows Logs > Application, Application Error) names `amdvlk64.dll`. AMD's driver crashes with the emulator's shader libraries ([issue 118](https://github.com/daurosk/oot-remake-multiplayer/issues/118)). `Host.bat`, `Join.bat` and `play.cmd` turn them off when the PC has an AMD card and no NVIDIA card, and say `AMD GPU found: shader libraries off (EDEN_NX2_GPL=0)`. With both an AMD and an NVIDIA card the game runs on NVIDIA and they stay on; if it still crashes in `amdvlk64.dll`, add the line `EDEN_NX2_GPL=0` to `coop-options.ini`. `EDEN_NX2_GPL=1` there turns them back on. Older kits: make a `Host-AMD.bat` next to `Host.bat` with the three lines `@echo off`, `set EDEN_NX2_GPL=0`, `call "%~dp0Host.bat" %*` and start that instead.
- **Over Hamachi, Radmin VPN, ZeroTier or Tailscale the game connects, then says "connection lost, reconnecting" every 15-20 seconds** ([issue 143](https://github.com/daurosk/oot-remake-multiplayer/issues/143)). Ten seconds with nothing from the other side ends a connection. Check in this order:
  1. **Windows Firewall on Public networks.** Windows puts a VPN's network adapter in the **Public** network type, and the first-start question above allows only **Private**. On **every** PC (the host and each player): Start, type `Allow an app through Windows Firewall`, click **Change settings**, find **every** `coop_server.exe` and `lichii.exe` entry (there is one per kit folder; a name can appear more than once) and tick **both Private and Public**. No entry: **Allow another app...**, browse to the kit's `server\coop_server.exe`, then again to `emulator\lichii.exe`.
  2. **The same version everywhere, and a current one.** The server and the game send packets small enough for a VPN or a tunnelled home line (1100 bytes; the server window says `datagrams of 1100 bytes`). Older kits sent 1392-byte packets, more than Hamachi carries in one piece. Run `Update.bat` on every PC.
     - **Without a VPN, the same drops:** some internet lines carry less than the usual 1500 bytes per packet. **DS-Lite** (common with cable and fibre providers that give an IPv6 line with shared IPv4, often behind an OpenWrt or Fritz!Box router) and **PPPoE** (DSL) lines put each packet in a tunnel, and a small tunnel (one player's was 1200 bytes) drops bigger packets. The 1100-byte packets fit; older kits' packets do not.
  3. **Join with the host's VPN address** (Hamachi's `25.x.x.x`, Radmin's `26.x.x.x`), not its home address.
  4. Still dropping: run `collect-logs.cmd` on **both** PCs and report it with each `VERSION.txt` ([how](#reporting-a-problem)). The game's log says `coop: no packets from the server for ... ms` when the server's packets stop arriving, and each `coop: connection lost; reconnecting (...)` line says how long it heard nothing, the round trip and the packet size.
- **Where is the log?** `emulator\user\log\eden_log.txt` in your kit folder. Log lines name the emulator's source files, such as `src\core\...`. An older kit shows a full folder such as `C:\...\src\...`: that is where the emulator was built, not your folder, and it is not a problem.
- **Still stuck?** Close the emulator, run `collect-logs.cmd` and report it (below).

## Low frame rate

The kit's emulator already starts with the fastest settings for this game (Vulkan, 1x resolution, low GPU accuracy, shaders built in the background). You do not need to change anything in its settings, and turning things up (a higher resolution, higher accuracy, frame generation) makes it slower. The frame rate is shown at the bottom of the emulator window (`Game: ... FPS`). If the game is slow or uneven, go through these in order:

1. **The first start prepares shaders once: let it finish.** The first time the game starts, the emulator shows **Preparing shaders** with a count (around 100,000) and a time left. Wait until it is done; it happens once, and afterwards new places stutter far less. If you clicked **Play now** (or closed that window), the preparation does not stop: it keeps going beside the game with part of your processor until it is done, and the game is slow and uneven all that time (a small "Preparing shaders N%" note in the corner shows it is still running). Fix: close the emulator, start the game again, and this time let the screen finish. It comes back by itself at the next start while it is not done, and goes on from where it was. After a **graphics driver update** it runs once more (it prepares the shaders for that driver), so after updating a driver (step 6) let it finish again.
2. **The first minutes stutter, and that is normal.** The first time you play, and after an update that brings a new emulator, the emulator builds the game's shaders as it goes: each new place, effect or enemy stutters the first time you see it. Play 10 to 15 minutes; it gets smooth, and stays smooth the next time you play (the shaders are kept). Do not delete `emulator\user\cache\shader` to fix this: that starts it over.
3. **Switch to 30 FPS.** Click the **60 FPS** button at the bottom of the emulator window once; it turns into **30 FPS** at once, also in the middle of a game. The game keeps its normal speed, only fewer frames are drawn. The emulator also asks this on its very first start. A PC that gets under 30 at 60 FPS is usually steady at 30. Not yet tested by us with one player at 30 and the other at 60; tell us if anything looks wrong.
4. **Laptops: plug in the charger** and set Windows to **Best performance** (Settings > System > Power & battery > Power mode). On battery, most laptops slow the processor and graphics a lot.
5. **Make sure the game runs on your graphics card, not the built-in graphics.** Laptops often have both. In Windows: Settings > System > Display > Graphics, **Browse** to `emulator\lichii.exe` in your kit folder, add it, click it, **Options**, **High performance**, Save. Then close and restart the emulator. `Host.bat` and `Join.bat` say `Only built-in graphics found` when the PC has no graphics card at all (then step 3 is the main fix).
6. **Update your graphics driver**: NVIDIA (the NVIDIA app), AMD (AMD Software: Adrenalin Edition) or Intel (Intel Driver & Support Assistant). Old drivers build shaders slowly and stutter more. Restart the PC after.
7. **Close other programs**: browser tabs playing video, other games, screen recorders, Discord screen sharing. Run one copy of the game per PC: two on one PC share its power.
8. **AMD Radeon graphics cards.** AMD's Windows driver crashed with the emulator's "shader libraries" (a way to build shaders faster), so the emulator leaves them off on AMD cards. On AMD, new places stutter longer the first time than on NVIDIA, and the first 10 to 15 minutes (step 2) matter more. What helps:
   - **A current driver** (AMD Software: Adrenalin Edition, then Check for Updates). Newer cards need a new one: an **RX 9060 XT** needs driver 25.6.1 or newer, the RX 9070 cards 25.3.1 or newer. Restart the PC after updating.
   - **The 30 FPS button** (step 3), which gives slow moments more room.
   - **Leave the emulator's settings as they are.** Turning "Shader libraries" on in its graphics settings on an AMD card brings back the crash (`amdvlk64.dll`, see Troubleshooting).
   - In AMD Software, leave Radeon Chill, Radeon Boost, Anti-Lag and frame generation (AFMF) off for `lichii.exe`. They can hold the frame rate down or make it uneven in emulators.
   - A fast graphics card does not help if the processor is slow: Switch emulation needs the processor most (step 9).
9. **Is the PC strong enough?** Switch emulation leans mostly on the processor. A rough guide (not measured by us): a 4-core processor from before about 2017 (Intel Core i5 6th generation or older, AMD FX) or built-in graphics only (Intel UHD or HD Graphics) will struggle even at 30 FPS. A 6-core processor from about 2019 on (such as a Ryzen 5 3600 or Core i5-10400) with a graphics card such as a GTX 1060 or RX 580 or newer, and 16 GB of memory, is comfortable. Free space on the drive helps the shader cache.
10. **Still slow?** Close the emulator, run `collect-logs.cmd` and send the zip with the FPS you see and where in the game (it records your processor, graphics card and driver, memory, whether a laptop is plugged in, and the emulator's graphics settings; no game files or passwords).

## Reporting a problem

Close the emulator and double-click `collect-logs.cmd`. Describe what you did and what went wrong on the repo's [issue page](https://github.com/daurosk/oot-remake-multiplayer/issues), and send the zip it makes **privately** to the maintainer (a Discord DM), not as an attachment on the issue.

The zip is masked: IP addresses show only their kind (`<lan-1>`, `<tailscale-1>`, `<hamachi-1>`, `<radmin-1>`, `<public-1>`, ...), MAC addresses, your Windows user name, your PC name and any session password are replaced, and crash dumps are left out (run `collect-logs.cmd -WithCrashDumps` from a command prompt only when asked). Still, never paste addresses, `coop.ini` or log lines with addresses into a public issue.
