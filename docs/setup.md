# Setting up the co-op test release

This is an early test build of online co-op (the newest is on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)) for the Ocarina of Time remake, on the kit's Fritchii emulator or, as an experiment, on Hookshot. Expect bugs; [known-issues.md](known-issues.md) lists what works and what does not yet. After this page, [play-lan.md](play-lan.md) (same network) or [play-online.md](play-online.md) (over the internet) says how to play together.

This page goes: [Hookshot](#play-on-hookshot-experimental) (new and experimental), then the [emulator setup for new players](#emulator-setup-for-new-players), then [updating a kit you already have](#updating), then settings, chat commands, saves, [Linux](#linux-and-steam-deck) <!-- DROP if linux-2 is not in the release --> and [troubleshooting](#troubleshooting).

## Play on Hookshot (experimental)

Co-op now also runs on [Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0), a native PC version of the game that the kit builds on your PC from your own copy. No emulator: the game runs as a normal Windows program. It is **experimental**: it is newer than the emulator kit, fewer players have tried it, and parts of co-op are not on it yet (see [What is not on Hookshot yet](#what-is-not-on-hookshot-yet)). Hookshot players and emulator players can play in the same session, on the same server. If you are not sure, play on the emulator ([Emulator setup for new players](#emulator-setup-for-new-players)). **The test releases after this one are Hookshot only**: this release still ships the emulator kit, and this page still documents it.

### What you need for Hookshot

- **A graphics card with Vulkan 1.3 and mesh shaders**: NVIDIA RTX 20 series or newer, or AMD RX 6000 series or newer. Older cards (GTX 10 series, RX 5000 and older) cannot run it; Intel cards are mostly untested. Without mesh shaders, play on the emulator.
- **16 GB of RAM** (a Hookshot game uses about 10 GB; 8 GB of video memory is recommended) and a CPU with AVX2 for the fastest build.
- **About 60 GB free** on the drive the kit is on, while it builds.
- **Your own game as a `.nup` file**, or its `romfs` and `exefs` folders, the same version as for the emulator (program `0400486024DEC000`, update v131072, build `e0f3ccfa`). No keys or firmware are needed for Hookshot. Every player needs their own copy of the game.
- Windows 10 or 11 (64-bit) and an internet connection for the first build (it downloads Hookshot's build tools, about 400 MB).

### Hookshot step by step

1. **Download and unzip.** Download `oot-coop-<version>.zip` from the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases) (the same zip as for the emulator) and unzip it to a short folder you can write to, for example `C:\oot-coop`.
2. **Optional: check your game files first.** Open a command prompt in the kit folder and run `Build-Hookshot-Coop.bat -CheckOnly -Game <your .nup>`. It downloads the build tools, checks them and your game, and builds nothing. It ends with "Your game files are right for Hookshot" when all is well.
3. **Build the game.** Double-click `Build-Hookshot-Coop.bat`. It finds your `.nup` in the emulator's game list folders if you set the emulator up before; otherwise it asks you to drag the `.nup` (or its `romfs` folder, then its `exefs` folder) into the window and press Enter. Then it:
   - downloads Hookshot's build tools and its source for the version this kit was made for, each checked against the SHA-256 this kit expects (a file that does not match stops it);
   - adds co-op's patch and builds the game from your files: **10 to 45 minutes the first time**, with the window showing each stage;
   - puts co-op in it: `hookshot_coop.dll`, the co-op settings (`coop.ini` beside `hookshot.exe`) and the puppet pack (the other players' Link and the tunic colours), made from your own game files as `make-puppet-pack.cmd` makes them for the emulator.
   It ends with "Done." and the game's path, in the kit's `hookshot\game` folder (`-Out <folder>` puts it elsewhere).
4. **First start, as the host.** Double-click `Host-Hookshot.bat`. It works like `Host.bat`: it asks for your player name, starts the co-op server, makes a new random join password, asks your router to open the port and copies an **invite code** for your friends (see [Playing over the internet](play-online.md#the-easy-way-hostbat-and-an-invite-code)). Then it starts the Hookshot game instead of the emulator.
5. **Joining.** Your friends double-click `Join-Hookshot.bat` (Hookshot) or `Join.bat` (emulator) and paste the invite code. Invite codes have not been tested on Hookshot yet; joining by address on the same network or a VPN works the same as on the emulator ([play-lan.md](play-lan.md)).
6. **Check it works.** `Join-Hookshot.bat` does not watch for the connection the way `Join.bat` does. Open `hookshot_log.txt` beside `hookshot.exe`: it says `coop: module loaded`, then `coop: connected`. In the game, the other players show as Link.

**After `Update.bat`:** run `Build-Hookshot-Coop.bat` again. It copies the new co-op files and puppet pack into the game in a few minutes, and builds the whole game again only when the update moved to a newer Hookshot.

**Where it keeps things:** the kit's `hookshot\` folder holds `tools\` (Hookshot's build tools and source with our patch), `downloads\`, `work\` (the unpacked game, kept so the next build is quick; delete it to get the space back, and the next build takes the full time again) and `game\`. None of these are part of the kit, so `Update.bat` keeps them. Like the puppet pack, the built game is made from your game files: never share it or upload it.

### What is not on Hookshot yet

- **No co-op window:** no status panel, no chat box (Enter) and no player list (Tab), so no chat commands such as `/teleport`. The log says what the panel would.
- **Your own ocarina notes are not sent:** the others do not hear a Hookshot player's ocarina.
- **Your own Link stays green:** your colour from `coop-options.ini` is not put on your own Link on Hookshot (the emulator kit does it).
- What has been checked on Hookshot in our two-game tests: <!-- FILL from the integrator's Hookshot proofs 3-5 (reports/integrate-p11.md) --> the rest of co-op is the same code as on the emulator but not yet checked there.

### Hookshot troubleshooting

- **"No co-op Hookshot game in ... yet"** from `Host-Hookshot.bat` or `Join-Hookshot.bat`: run `Build-Hookshot-Coop.bat` first, and let it finish.
- **The build stops on a download or a SHA-256 that does not match:** run it again (a download may have broken off). If it keeps failing, report it with the window's text.
- **Not enough space:** it warns below about 60 GB free. Free some space, or move the kit to a bigger drive.
- **The game closes at the start or the screen goes black:** check that your card has mesh shaders (above) and that its driver is up to date. Hookshot's own open problems (crashes outside Kokiri Forest on some PCs, crashes on some AMD cards, a device loss soon after the start) are listed on [Hookshot's page](https://github.com/NotRexed/Hookshot); play on the emulator meanwhile.
- **To try the game without co-op:** set `HOS_COOP=0` before starting `hookshot.exe`; if it crashes without co-op too, it is Hookshot's problem, not co-op's.
- **Reporting a problem:** `collect-logs.cmd` does not pick up the Hookshot log yet: attach `hookshot_log.txt` from beside `hookshot.exe` to your report, and say in it that you played on Hookshot ([Reporting a problem](#reporting-a-problem)).

## Emulator setup for new players

The usual way to play: the kit's patched Fritchii emulator. It runs on most PCs that can run the game in an emulator. Steps 1 to 4 below, then [Check it works](#check-it-works); after that, [play-lan.md](play-lan.md) (same network) or [play-online.md](play-online.md) (over the internet).

## What you need

- A Windows 10 or 11 PC (64-bit) that can run the game on Fritchii.
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
| `Build-Hookshot-Coop.bat`, `Host-Hookshot.bat`, `Join-Hookshot.bat`, `hookshot\` | Hookshot, experimental ([above](#play-on-hookshot-experimental)) |
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

Double-click `Join.bat`. Type `127.0.0.1` for the address for now, and a player name (1 to 16 characters, different from the other players'). The emulator opens. Add the folder with your game dump to its game list (double-click the empty list) and start the game once to see that it runs. Close the emulator.

If Windows SmartScreen says it protected your PC, click **More info**, then **Run anyway**: the kit's programs are not signed. If Windows Firewall asks about `lichii.exe` (the emulator) or `coop_server.exe` (the server), tick **Private networks** and click **Allow access**; it asks once per folder the kit is in. If it says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing, install the Microsoft Visual C++ 2015-2022 Redistributable (x64).

## 4. The puppet pack

**Most players: your game is a `.nup`. Just double-click `make-puppet-pack.cmd`. No keys, no Dump RomFS.**

1. Double-click `make-puppet-pack.cmd` in the kit folder.
2. It finds the `.nup` in the emulator's game list folders (step 3). If it asks for it, type its path or drag the file into the window.
3. Wait for it to finish: it reads the game files straight from the `.nup` and builds the pack in about a minute. It also makes the co-op HUD's look from the same files (the game's font, heart pictures and place names, for the other players' names, hearts and places under your hearts): it prints `co-op HUD: font, 5 hearts and ... place names`. If that part fails it prints a note and co-op still works, with plain text.

Do not use the emulator's **Dump RomFS** for a `.nup`: you do not need it, and it often fails with "RomFS Extraction Failed" (for example when your keys are older than the game's update).

**Which file do you have?** Look at the file you added to the emulator's game list in step 3: its name ends in `.nup`, `.nsp` or `.xci` (the game list also shows it in its **File type** column: NUP, NSP or XCI). `.nup`: the steps above are all you need. `.nsp` or `.xci`: see below.

The other players appear as Link only with the puppet pack, which is made from your own game files, so it cannot come in the zip. Do this once per PC, and **again after every update** that changes the pack ([Updating](#updating); `Update.bat` does not rebuild it). Without the pack, co-op still works, but the other players show only as a name and a dot, and co-op's text is plain (no game font or hearts).

The pack also holds the **tunic colours**: each player's Link wears that player's colour in the other games, and your own Link wears yours (the Kokiri tunic; the Goron and Zora tunics keep their colours). Pick yours with `color =` in `coop-options.ini` ([Your colour](play-online.md#your-colour)). With a pack made before the colours, the other players stay green, and `Host.bat` / `Join.bat` say: "Puppet pack: made without the tunic colours ... Run make-puppet-pack.cmd again."

### Only if your game is an `.nsp` or `.xci`: dump the RomFS first

<details>
<summary>Dump steps for an <code>.nsp</code> or <code>.xci</code> (or when the tool cannot use your <code>.nup</code>)</summary>

1. Check first: the game starts in the kit's emulator (step 3), and the game **update** is installed in the kit's emulator (step 2), not only in another emulator. Without it you dump the old base game, which co-op cannot use.
2. Start `Join.bat`, right-click the game in the list and choose **Dump RomFS**. If it asks which RomFS, pick **Program [0400486024DEC000]**. Then pick **Full** (not Skeleton). It writes several GB to `emulator\user\dump\0400486024DEC000\romfs\` and needs that much free space plus 1 GB. Keep the progress window open until it says **RomFS Extraction Succeeded!**; then close the emulator.
3. Double-click `make-puppet-pack.cmd`. It checks the dump and builds the pack. If something is wrong it says what and how to fix it (see [Troubleshooting](#troubleshooting)). If it finds nothing, it asks for the `.nup` or the folder that holds `Pack`, `Model`, `AS`, `RSDB` and `System`.
4. You can delete the `dump` folder afterwards.

</details>

## Check it works

`Host.bat` on one PC opens a server window and the emulator. Start the game: a small panel in the top-left corner says `Co-op: player 1, ping ... ms`. The emulator's log, `emulator\user\log\eden_log.txt`, has lines starting with `coop:`, among them `coop: build ...` (which co-op build this is), `coop: welcomed as player 1` and `coop: game build e0f3ccfaf50065ae`. With a join password (`Host.bat` makes one each time) the game traffic is encrypted: the panel's first line ends in `, encrypted`, and the log has a `coop: traffic ...` line. `, not encrypted (no join password)` means the host turned the password off.

Next: [play-lan.md](play-lan.md) or [play-online.md](play-online.md).

## Updating

Every player needs the same version: a game of one version cannot join a server of another.

**This release is protocol 10: everyone updates.** A game of it refuses a server of an older release, and the other way round, so the host and every guest update before playing. In short:

1. Close the games and the server window.
2. Double-click `Update.bat` (or answer **Y** when `Host.bat` or `Join.bat` asks).
3. Double-click `make-puppet-pack.cmd` once more: `Update.bat` does not rebuild the pack, and the new one has the tunic colours (with an old pack the other players stay green).
4. The emulator changed, so the first start shows **"Preparing shaders"** again: let it finish ([Low frame rate](#low-frame-rate)).
5. On Hookshot: run `Build-Hookshot-Coop.bat` again ([Hookshot](#play-on-hookshot-experimental)).

The details:

**On every start** (a kit whose `coop-options.ini` has `auto_update`): when `Host.bat`, `Join.bat` or `play.cmd` start and a newer co-op release is out, they ask "Update now? [Y/n]" (yes after 10 s with no key, so an unattended start updates too). Yes updates the whole kit, the emulator included, then starts the new version. No, no network, or GitHub down: this version starts as it is, after at most 15 s, with a line saying why it could not check. If the update fails, the old files are put back and this version starts. `auto_update` in `coop-options.ini` sets it: `ask` (the default), `yes` (update without asking) or `no` (only say that one is out). An update stops this kit's server if it is still running, so a host should update when nobody is playing: guests on it are dropped (and need the same update anyway).

**By hand:** close the emulator and the server, then double-click `Update.bat` in the kit folder. It looks for the newest release on the [Releases](https://github.com/daurosk/oot-remake-multiplayer/releases) page, says which one, and asks before it changes anything. Then it downloads the zip, checks it against the SHA-256 published with the release and, when the GitHub CLI (`gh`) is installed and logged in, against the release's signature (its attestation, made by our release workflow; a zip that fails it is refused, `verify_signature = no` in `coop-options.ini` skips that check). Then it replaces the kit's own files, the emulator included. It needs no admin rights: it writes only in the kit folder and your temp folder.

The emulator is updated only this way, with the co-op build from our release. Fritchii's own updater is switched off in our builds and never used: it would put a plain Fritchii without co-op over the kit's. When Frizti publishes a newer Fritchii, the next co-op release moves to it.

**A kit without `Update.bat`** (the first kits had none): download `Update.bat` from the newest release once, put it in the kit folder beside `Host.bat`, and double-click it (Windows may warn about a file from the internet the first time: **More info**, then **Run anyway**). From then on the kit has its own. (Or download the whole zip once more and copy your `emulator\user` folder into it.)

What it keeps: everything in `emulator\user\` (keys, firmware, saves, settings, your name and server, the puppet pack), `logs\`, any file of yours that is not part of the kit, and `coop-options.ini`: your switches stay as they are, and every key the new version adds is written at the end with its comment, as a new kit has it (on, or behind a `#`). Keys an older `Update.bat` missed are added the next time `Host.bat` or `Join.bat` starts. When the new release brings a different emulator, the old shader cache (`emulator\user\cache\shader\`) is moved to the backup too, because a cache made by another emulator build can crash it; the first minutes of play may stutter while a new one is built. With the same emulator it is kept. Every file it replaces goes first to `backup-<old version>\` in the kit folder; if something fails half way, those are put back. If the update is stopped (window closed, power cut), run `Update.bat` again: it finishes it (Host.bat and Join.bat remind you). Delete the backup folder once the new version works.

If it cannot update, it says why and how to update by hand: download the newest `oot-coop-....zip` from the [releases](https://github.com/daurosk/oot-remake-multiplayer/releases), unzip it to a new short folder, copy `emulator\user` and `coop-options.ini` from your old kit into it, and play from there. `Host.bat` and `Join.bat` also say when they could not check for a new version (no network, or GitHub turned the check away); `Update.bat` checks again. A co-op server window still open from your last game is closed by the update started from `Host.bat` or `Join.bat` (`Host.bat` starts the new one); close the emulator yourself first.

It sends nothing about you: one request for the list of releases, and the two downloads. If it says the puppet pack tool changed, run `make-puppet-pack.cmd` once more.

## Check the download

Optional. `Update.bat` checks every download against the SHA-256 published with the release, which proves the file arrived whole. Each release's zip and `Update.bat` are also signed: when the release is published, our release workflow on GitHub makes a signed record (an artifact attestation) of those exact files. To check that a zip you downloaded is the one the release published, install the [GitHub CLI](https://cli.github.com), run `gh auth login` once, and in the folder with the zip:

```
gh attestation verify oot-coop-<version>.zip --repo daurosk/oot-remake-multiplayer --signer-workflow daurosk/oot-remake-multiplayer/.github/workflows/release-attest.yml
```

It must say the verification succeeded. If it fails, do not use the zip, and tell us ([Reporting a problem](#reporting-a-problem)). From a copy of this repository, `tools\release\verify_kit.ps1 -Zip <zip>` does the same and also checks the `.sha256` beside it.

## Settings

`coop-options.ini` turns features on and off; by default the puppets, shared enemies and townspeople, and objects (seeds, nuts, bombs, arrows) are on, debug logging off. Everyone should use the same settings. Your server address and name are in `emulator\user\config\coop.ini`; `Join.bat` asks for the address each time, and `play.cmd change` asks for both again. The **join password**: `Host.bat` makes a new random one each time it starts and puts it in the invite code, so nobody types it; it also turns on the encryption of the game traffic. A host who wants the same one every time sets `server_password = <one without spaces>` in `coop-options.ini`; then the others type it when `Join.bat` asks (unless they use the invite code; Enter keeps the last one, `-` for none; kept in `coop.ini` as `password`). `server_password =` with nothing after it means no password: then there is no invite code, no router forward and no encryption, and anyone who finds the server can join. A wrong one shows `wrong join password` in the `Join.bat` window and in the game's co-op panel ([play-online.md](play-online.md), "Who can join").

## Chat commands

Press Enter in the game to open the chat box. A line that starts with `/teleport` (or `/tp`) is a command, not chat:

- `/teleport` alone lists the other players.
- `/teleport <player>` takes you to that player. The whole name works in any case, and so does the start of a name if only one player's name starts that way, or the player's number (`/teleport 2`).
  - The other player's game answers with where they last stood on their feet, so you never land in a wall or in mid-air.
  - In the same scene you are moved there at once.
  - In another scene the game changes scene first: talk to someone or read a sign within a minute, and when the talk ends there is a fade and a load, arriving at that place's entrance nearest the other player. After the load you are moved next to them. A scene change not taken in 60 seconds is given up. (An earlier build changed scene with no talk, but that left your Link unable to move after the load, #197.)
  - Refused, with a line in the chat box: while your game loads or plays a cutscene or a talk, during your boss fight, while the other player is busy (loading, a cutscene, a talk, in the air) or in a boss fight, and when the other player is in a place only for the other age (`_Child` or `_Adult`) than your Link, or in a place with no entrance to arrive at (the boss-rush room, the ending, the Sage room). Lake Hylia, Lon Lon Ranch, the Temple of Time and the houses are parts of Hyrule Field, the Market or their village, so those work. A dungeon gives a warning: you may need its items to get on from there.
  - A player with an older build does not answer: "no answer from" after 5 seconds.

`LICHII_COOP_TELEPORT=0` turns it off in your game: the command says so, and the others' requests to come to you are refused.

## Chat notices

When another player's progress reaches your game, a line in the chat says so (only in your game, nothing is sent): "Tobi got the Fairy Slingshot - you got one too!", "Tobi found a Small Key (Deku Tree) - the group has 2", "Tobi got a Heart Container - your max health went up", "Tobi got the map (Deku Tree)", "Tobi learned Zelda's Lullaby", "Tobi got a Gold Skulltula token (12)", "Tobi opened a chest". Joining a session says one line ("Synced with the group: 14 items, 3 songs"), and so does the host loading a save. At most 4 lines in 3 seconds; the rest is one "...and N more" line. To turn them off, set `LICHII_COOP_NOTIFY=0`.

## Saves

The host's save decides. Everyone's progress is shared while you play, but when the host loads a save, every game in the session goes back to that save's progress: items, keys, chests, doors and switches. A guest who loads a save gets the host's progress, and nothing changes for the others. To undo something (a door that locked you out, a key used in the wrong place), the host loads an older save. Rupees, ammo, hearts and where you stand stay your own. Be careful: if the host restarts the game and loads its save, **everything the guests did since the host last saved is undone** for everyone: chests, keys, doors, items. The host should save often, especially after the guests make progress.

**Guests play from the host's save file.** When you join (`Join.bat`), your game uses a separate co-op save folder, so your own saves are never touched: the host's newest save is copied into it, and the chat says "The host's save is ready". On the title screen choose **Continue** (or **Load Game**, the newest save) to play from it; you start where the host's save starts, with its items, hearts and rupees. When the host loads another save, you get that one too after the host's next door or save point: load it to stand where it starts (your progress already follows the host at once). Without joining (playing alone, or hosting with `Host.bat`) the game shows your own saves as before. The host's own saves (`Host.bat`) are its normal ones.

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
