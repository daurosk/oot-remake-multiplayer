# Setting up Squadala!

Squadala!, online co-op for the Ocarina of Time remake, on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (Team MagicBean and contributors, GPL-3.0; its changes are being merged into [Hookshot](https://git.notrexed.dev/Hookshot/Hookshot) by NotRexed and contributors): a native PC version of the game that the kit builds on your PC from your own copy. There is no emulator: the game runs as a normal Windows program. This is an early test build (the newest is on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)); [known-issues.md](known-issues.md) lists what works and what does not yet.

The kit has one file to start: **`Start Squadala.bat`**. Everything else is done by it or in the game. (The older emulator kits, with `Host.bat` and `Join.bat`, have their own guide: [setup-emulator.md](setup-emulator.md). A player on an emulator kit cannot join a game hosted with this kit, nor the other way round: every player needs this kit, the same version.)

## What you need

- **Windows 10 or 11, 64-bit.** No administrator rights are needed.
- **A graphics card with Vulkan 1.3 and mesh shaders**: NVIDIA RTX 20 series or newer, or AMD RX 6000 series or newer. Older cards (GTX 10 series, RX 5000 and older) cannot run it; Intel cards are mostly untested.
- **16 GB of RAM** (the game uses about 10 GB; 8 GB of video memory is recommended).
- **About 60 GB free** on the drive the kit is on, for the first build. The kit builds Longshot on your PC with co-op built into its source; the 60 GB is the build's peak. Afterwards about 37 GB stay (measured on a built kit): the game in `hookshot\game` is 35 GB (24 GB of it your game's unpacked files, 7 GB its shader programs), and the build files in `hookshot\work`, `tools` and `downloads` are about 2 GB, kept so that updates take minutes.
- **Your own game as a `.nup` file** (or its `romfs` and `exefs` folders): program `0400486024DEC000`, update v131072, build `e0f3ccfa`. No keys or firmware are needed. Every player needs their own copy of the game.
- An internet connection for the first build (it downloads Longshot's build tools, about 400 MB) and to play.

<a id="play-on-hookshot-experimental"></a>

## Step by step

1. **Download** `Squadala-<version>-windows.zip` (up to v0.11.2 `oot-coop-<version>-windows.zip`; the `oot-coop-<version>.zip` beside it is the same file, for older kits' updaters) from the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases).
2. **Unzip it.** Right-click the zip, choose **Extract All...**, and extract it to a short folder on a drive with 60 GB free, for example `C:\OoT-Coop`. Not inside the zip, not in OneDrive (your Desktop and Documents are often in OneDrive), not under Program Files.
3. **Double-click `Start Squadala.bat`** in that folder. A window opens and checks the kit first (see [If something goes wrong](#if-something-goes-wrong)).
4. **The first time, give it your game.** It asks you to drag your game's `.nup` file into the window and press Enter (for a `romfs` folder it then asks for the `exefs` folder). It remembers the file; it never asks again. Then it builds the game: **10 to 45 minutes, once**. The window shows each stage; the PC stays usable. It also makes the puppet pack (the other players' Link and tunic colours) from your game, and puts a **Play OoT Co-op** shortcut on your desktop.
5. **The game starts.** **Host** and **Join** are in the game's menu, on its first tab, **Multiplayer**. To open it: on a controller **−** and **+** together (on an Xbox-style pad **View** and **Menu**), or **Home**; on the keyboard **F1** (Insert and Esc also open it unless you changed that in the menu); on the title screen **−** alone. The D-pad or the arrow keys choose, A or Enter confirms, B or Esc goes back.
   - **Host:** one press. The Host screen's first line says it: *Invite code copied to your clipboard. Share it in DMs only: it contains your internet address.* Send the code in a DM (Discord, a text), never in a public channel; that is all most hosts need. Below it: **Paste friend's answer (or Ctrl+V)** when a friend's game shows an answer code, **Same network** for friends in the same house, **VPN** for friends on Radmin VPN, Hamachi, ZeroTier, Tailscale or WireGuard with you, and **Port** to choose a custom UDP port (1 to 65535; default 27510; changing it while hosting restarts hosting: friends join again with the new code).
   - **Join:** type your friend's room name or address into **Room or address**, then their **Password** (and **Port** for an address), then **Join**. A code or an address line on your clipboard pastes with Ctrl+V, or with A on **Room or address** on a controller.
   - **Co-op Settings:** your name, your colour, the number of players and port when you host (`Players when you host`, `Port when you host`), voice chat.
6. **Next time**, double-click `Start Squadala.bat` or the desktop shortcut: the game starts in seconds.

To check it works: in the game the other players show as Link with their name tags, and **T** opens the chat box. The game's log `longshot_log.txt` (in the `logs` folder of the kit's `hookshot\game\`; before Longshot rc.4, beside `longshot.exe`) says `coop: module loaded`, then `coop: connected`.

## Updating

Nothing to do: on every start the kit asks GitHub whether a new co-op version is out, and if so asks you once (`auto_update = yes` in `coop-options.ini` updates without asking, `no` never does). It downloads the new kit, checks it, keeps your settings, your game file and the built game, and puts the new co-op into the game in seconds. It builds the game again only when the update moves to a new Longshot (it says so: about 15 minutes). Every player needs the same version.

**A kit older than 0.11.3** (its start file is `Start OoT Co-op.bat`) updates in two steps by itself: first to 0.11.3, then, on the next start, to the newest version (its updater only knows the old download names, which releases from 0.11.4 on no longer have). To go straight to the newest version, download `Update.bat` from the newest release into your kit folder and double-click it. From 0.11.4 the kit has one start file, `Start Squadala.bat`: the update moves your desktop and Start menu shortcuts and your Steam entry over to it and removes `Start OoT Co-op.bat` (if Steam was open, restart it once).

`Update.bat` in the kit folder updates by hand (for example when the start said it could not reach GitHub). It is also attached to each release: dropped into an older emulator kit, it turns that kit into a Longshot kit. The emulator, `Host.bat`, `Join.bat` and the other old files go into the kit's `backup-<version>\` folder; your emulator files (`emulator\user`: its saves, keys and settings) stay where they are, and the window says you can delete that folder when you no longer need it. **To keep playing on the emulator**, keep your emulator kit (the last emulator release is the early test release of 2026-11-12 on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)) as it is (do not put a newer `Update.bat` in it) and unzip the Longshot kit into a new folder.

## Going back to an older version

If a new version gives you trouble, double-click `Revert.bat` in the kit folder: it puts back the version your kit had before its last update. Every update keeps the version it replaces in the kit's `previous\` folder (the last two; only the kit's own files, a few MB), and a kit updated by an older updater goes back from its `backup-<version>\` folder.

- **Every player in a session needs the same version**: the versions do not talk to each other. Tell your friends to go back too (their own `Revert.bat`), or you cannot play together.
- Your saves, your settings, `coop-options.ini` and the built game are kept, and so is the version you left: `Update.bat` goes forward again.
- The game: when only the co-op patches differ, the next start updates it in a few minutes. When the older version is on another Longshot, it is built again (15 to 45 minutes; it asks first). Going back to 0.11 or later, a build of that version already in this PC's game cache (testers' PCs: `%LOCALAPPDATA%\oot-coop\hs-cache`) is copied in instead; going back to 0.10.1 always builds again.
- Starting the kit does not update straight back: it stays on the version you went back to and asks again only when a release newer than the one you left comes out (with `auto_update = yes` too).
- Any older release by its tag: open a terminal in the kit folder and run `Update.bat -Version v0.10.1` (downloaded from the release and checked against its SHA-256, as an update is).

On Linux the same is `bash revert.sh` and `bash scripts/update.sh --version <tag>`.

## The kit folder

| What | Why |
|---|---|
| `Start Squadala.bat` | The only file you start |
| `README.txt` | The steps above, short |
| `Update.bat` | Updates by hand (the start does it by itself) |
| `Revert.bat` | Goes back to the version before the last update ([above](#going-back-to-an-older-version)); `previous\` holds the versions kept for it |
| `coop-options.ini` | Co-op switches; the game's Co-op Settings write it too |
| `NVIDIA DLSS.bat` | Builds the game again with NVIDIA DLSS ([below](#nvidia-dlss)) |
| `start-server.cmd`, `server\` | A dedicated co-op server, only if you want one (Host in the game needs neither) |
| `hookshot\` | The co-op module and Longshot's pin and patches; after the first start also `tools\` (1.1 GB), `downloads\` (0.3 GB), `work\` (0.7 GB; kept so updates are quick, deleting them makes the next build take the full time) and `game\` (your built game, 35 GB) |
| `scripts\`, `tools\` | What the start runs; `scripts\collect-logs.cmd` makes a log zip for a bug report |
| `docs\`, `licenses\` | This guide, the known issues, the licences and where the source is |
| `logs\`, `VERSION.txt`, `MANIFEST.txt` | Logs, the kit's version, its file list (for the updater) |

Nothing of the game is in the kit. The built game and the puppet pack are made from your game: never share or upload them.

<a id="troubleshooting"></a>

## If something goes wrong

The start window says what is wrong and what to do. What it checks:

- **"This kit is still inside its zip file."** You opened the zip and double-clicked the file inside it. Close it, right-click the zip, **Extract All...**, and start `Start Squadala.bat` in the folder it made.
- **"Squadala is already starting in another window."** You double-clicked twice. Wait for the first window (the first build takes a while).
- **"This folder is in OneDrive"**, **"...letters the game build cannot handle"**, **"...too long"**, **"...cannot be written to"**: the first build cannot work in this folder. It offers to copy the kit to `C:\OoT-Coop` and start it there; say yes (or move the folder yourself). Once the game is built, the kit no longer asks.
- **"Your antivirus has most likely removed..."**: Windows Security (or another antivirus) took a file of the kit or of the built game. Open **Windows Security > Virus & threat protection > Protection history**, restore the file, and add the kit folder under **Exclusions**. Then start again: the game is not built again.
- **"The game isn't set up yet... Set it up now?"**: the first start. Answer **Y** (or wait a minute) and it builds the game once; **n** builds nothing until the next start.
- **"Windows protected your PC"** when you start `Start Squadala.bat`: SmartScreen, because the kit's files came from a downloaded zip. Click **More info**, then **Run anyway**. The kit then takes Windows' "downloaded from the internet" mark off its own files, once, so Windows does not ask about them again. To avoid it from the start: right-click the zip, **Properties**, tick **Unblock**, **OK**, then extract it. Defender's exclusions and turning real-time protection off do not change SmartScreen.
- **"Windows Smart App Control is blocking this kit's PowerShell scripts"** or **"...blocked <file>"**: Smart App Control (Windows 11) blocks programs and scripts without a known signature, whatever Defender's settings are; the kit's are not signed. To use the kit, turn it off: **Windows Security > App & browser control > Smart App Control settings > Off**. Windows cannot turn it back on without a reset of Windows, so that is your choice. (Longshot's own builder is a single program that downloads its tools itself, so nothing it runs carries the internet mark; the kit is PowerShell scripts from a zip.)
- **"PowerShell on this PC runs in ConstrainedLanguage mode"** or **"A Group Policy on this PC sets PowerShell's execution policy"**: a policy (work or school PCs, or a privacy or tweak tool) stops unsigned scripts. On your own PC the message says which setting to change; otherwise ask the PC's owner.
- **"Windows would not let the build move or delete ...\hookshot\tools.new"** (in 0.10.1: a red `Access to the path '...\tools.new' is denied`, even as administrator): a program still had a file of the freshly unpacked tools open. The kit now tries again for 30 seconds, then moves the folder with robocopy, which copies even files another program has open. When even that fails, the message names the program holding it (an antivirus, Explorer, the search indexer, a sync client such as Google Drive or Dropbox): close it or add the kit folder to its exclusions, and start again. A kit inside a cloud-sync folder: move it out, to `C:\OoT-Coop`.
- **"Windows refused to let PowerShell write in this kit's folder (access denied)"**: Controlled folder access or another antivirus. The message says where to allow `powershell.exe`.
- **"Controlled folder access protects <folder>"**: Windows Security's ransomware protection guards that folder (Documents, Desktop and others) and stops the game build writing there, even with real-time protection off. Move the kit to `C:\OoT-Coop`.
- Whatever Windows said, `logs\windows-checks.txt` in the kit has what the start found. It is in the log zip `scripts\collect-logs.cmd` makes; when that zip cannot be made, it makes `coop-logs-windows-checks.zip` instead.
- What the start window and the update said is in `hookshot\logs\start-log.txt` (the start before: `start-log.1.txt`), also in that zip (with the user and PC names masked).
- **"The co-op game this kit built is not in <folder> any more"**: the built game folder was moved or deleted. Moved it? Answer **n**, move it back to that folder and start again. Otherwise answer **Y** and it is built again.
- **"The co-op module hookshot_coop.dll is missing"**: antivirus took it, as in the line above: restore it, add the exclusion, start again. Building again does not help.
- **"Files of this kit are missing"**: the unzip did not finish. Unzip the kit again.
- **"The Microsoft Visual C++ runtime is missing"**: install it from Microsoft ([vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)) and start again.
- **"This is not the game version co-op needs"**: your `.nup` is another version of the game. Co-op needs program `0400486024DEC000`, update v131072, build `e0f3ccfa`. To give another file, delete `hookshot\game-files.txt` in the kit and start again.
- **The build stops on a download or a SHA-256 that does not match:** start again (a download may have broken off). If it keeps failing, report it with the window's text.
- **Not enough space:** the build warns below about 60 GB free. Free some space, or move the kit to a bigger drive.
- **Note: another co-op kit is on this PC.** An older kit folder: you can delete it once this one works. The new kit takes your game file from it, so it does not ask again.
- **The game closes at the start or the screen goes black:** check that your card has mesh shaders (above) and that its driver is up to date. Longshot's own open problems are on [Longshot's page](https://git.magicbean.dev/MagicBeanDev/Longshot/issues). To try the game without co-op, set `HOS_COOP=0` before starting `longshot.exe`; if it crashes without co-op too, it is Longshot's problem, not co-op's.
- **A Steam Controller (or another pad Steam Input runs) does nothing in play, or the camera only swings around:** started outside Steam while Steam is open, Steam turns the pad into a mouse and keyboard (its Desktop layout), so the menus seem to work and play does not. Add `Start Squadala.bat` to Steam (**Games > Add a Non-Steam Game to My Library**), start it from Steam, and set its controller layout (the game's page in Steam > **Controller**) to a gamepad layout. If you picked a pad in the Multiplayer tab's **Settings > Controller**, set it back to **Auto**.
- **Host or Join says a router blocks the way in**, or friends cannot reach you: the Host screen says what to do (the answer code, UPnP, **Same network**, **VPN**); [play-online.md](play-online.md) has more.

## Built Longshot yourself?

**You do not need your own Longshot build, and you never start `longshot.exe` yourself:** the kit builds its own game (in `hookshotgame`, with co-op inside) and **Start Squadala.bat** is the only way to start it. A Longshot you built or downloaded has no co-op, so it cannot see your friends. The kit's own `longshot.exe` started by hand has co-op but misses what the start file adds (the puppet pack, your settings, the update check): the game's chat then says it was not started with Start Squadala.bat.

The kit never plays a Longshot you built with Longshot's own builder, and moving that game into the kit folder does nothing: co-op needs its own build of the Longshot release the kit is made for (`hookshot\PIN`, now v3.0.0-rc.6), with co-op's changes in its source. A nightly or dev build is another version, so it cannot be used either.

Before the first build the kit asks once: **"Built Longshot yourself? ... Drag your Longshot game folder ... into this window"** (the folder with `longshot.exe`; just Enter, or 60 seconds, for no). The answer is kept in `hookshot\longshot-folder.txt`: to be asked again, delete that file before the game is built (once it is built, copy saves by hand, below). It then:
- copies the tool downloads your Longshot's setup already has, when they are the very same files (by SHA-256): less to download;
- copies its compiled files too when it is the same release (v3.0.0), so that what co-op does not change is not compiled again (the game is still translated again, with co-op in it);
- copies your saves into the co-op game after the build (below).

Nothing is ever changed in your Longshot's folders. Without a folder, the kit still reuses the downloads in Longshot Builder's home (`%LOCALAPPDATA%\LongshotBuilder`) when it finds one.

### Bring your own Longshot save

Given your Longshot folder (above), the first build copies its save slots (`user\save\<account>\slot_NN`, with `account.sav` and `option.sav`) into the co-op game's `user\save\<account>\`. It only copies: your Longshot's saves stay as they are, and a slot the co-op game has already is never overwritten (it says which it left). The saves are the game's own files, so a save from a nightly should load too when it is the same game version (not tested yet).

Already set up? Copy by hand: close both games, keep a copy of the kit's `hookshot\game\user\save` if it has slots you want, then copy the `slot_NN` folders (and `account.sav`, `option.sav`) from `<your Longshot>\bin\user\save\<account>\` (or `<your Longshot>\user\save\...`) to the same place under the kit's `hookshot\game\`.

## The randomizer

The kit brings the randomizer, OOTRR: the first build (and an update that moves it) downloads it into `hookshot\game\tools\ootrr\`, and it builds seeds from the game files the build already unpacked. Nothing to download or set. It is off until you, or the host in co-op, turn it on in the Mods tab (**OoT Remake Randomizer**, **Settings**): [mods.md](mods.md#the-randomizer). Step by step for co-op: [randomizer.md](randomizer.md). Do not install OOTRR by hand next to co-op: with a second OOTRR the game crashes at start.

## Customize Link

The Multiplayer tab's **Customize Link** page colours your tunic and each part of your gear (shields, swords, boots, gauntlets). Your own Link wears it when the menu closes, the other players see it on your Link, and it is saved in `coop-options.ini` for the next start ([puppet.md, The Link customizer](puppet.md#the-link-customizer)). The kit makes the puppet pack these colours are drawn with from your own game by itself, and again after an update that changes it.

## Discord status

With Discord open on the same PC, your Discord profile shows **Playing Squadala!** with the Squadala! logo (the Korok with the propeller leaf and four fairies), the release (*Squadala! 0.11.4*), *Solo* or *Co-op: 3 players*, and how long you have played. Only that goes to Discord: no names, no addresses, no invite code. It works the same on Windows and Linux, hosting, joining or playing alone.

- **Turn it off:** in the game press **Insert** (or F1), and set the **Discord status** row to off. It stays off.
- **Always Squadala!:** the status is always Squadala!'s, at the title, in play, paused or idle: the Squadala! application's name and icon, its release on the first line and *Solo* / *Co-op* on the second. Longshot's own status (since Longshot v3.0.0-rc.8 the place Link is, his age and hearts, and its `discord_text=`, `discord_image=` and `discord_image_text=` lines) is never shown in a co-op game, and another `discord_app_id=` is not taken. The kit writes `discord_app_id=`, `discord_image=none`, `discord_details=` and `discord_state=` into `longshot_options.ini` next to the game before each start; you may change the words of `discord_details=` (the first line), and the kit leaves a line you changed alone from then on. The *Solo* / *Co-op* line comes from the co-op module and wins over `discord_state=` ([hookshot/README.md, patch 0026](../hookshot/README.md)).

## In the game, saves, the emulator kit

<a id="co-op-on-hookshot"></a><a id="saves"></a><a id="emulator-setup-for-new-players"></a><a id="4-the-puppet-pack"></a><a id="linux-and-steam-deck"></a>

The co-op keys in the game (chat **T**, player list **F6**; the debug panel **F7** and item cheats **F8** are only in developer builds, not in release kits), the chat commands and how saves are shared are the same as in the emulator kits: [setup-emulator.md](setup-emulator.md#co-op-on-hookshot) and [setup-emulator.md, "Saves"](setup-emulator.md#saves). That guide also covers the older emulator kits themselves (keys, firmware, the puppet pack, Linux).

## NVIDIA DLSS

Longshot has **NVIDIA DLSS** and **DLAA** (and DLSS Frame Generation on RTX 40 and newer) only in a game built with NVIDIA's DLSS SDK. That SDK is under NVIDIA's own license, and nobody may pass it on: the kit never carries it, and a normal build offers only AMD FSR 3.1 and the game's own upscaler. With an NVIDIA RTX card:

1. Double-click `NVIDIA DLSS.bat` in the kit folder. It shows NVIDIA's license and asks whether you accept it (**y**).
2. It downloads NVIDIA's DLSS SDK (into `hookshot\work\ngx-sdk`) and builds the game again with it: much shorter than the first build, since only what changes is compiled. If the Longshot Builder or the Longshot Launcher already fetched the SDK on this PC, the kit starts from that copy and downloads only what differs (`"NVIDIA DLSS.bat" -DlssSdk <folder>` names another one). If the download fails (offline?), `"NVIDIA DLSS.bat" -NoDlss` builds the game without DLSS.
3. In the game: **Graphics → Upscaler: NVIDIA DLSS** (DLAA is DLSS's **Native** on the Render resolution row).

The kit remembers the choice (`hookshot\dlss.txt`): the builds after an update keep DLSS. `"NVIDIA DLSS.bat" -NoDlss` builds the game again without it. A game built with DLSS holds NVIDIA's files (`ngx\`): never share that game folder. With Nucleus split screen, each game has its own settings: pick **Upscaler: NVIDIA DLSS** once in each Nucleus game (the handler links in the `ngx` folder at every start). Linux: `bash start-squadala.sh --dlss` ([setup-linux.md](setup-linux.md#nvidia-dlss)).

## Low frame rate

Longshot builds its shaders the first time it sees each place, so the first minutes in a new place may stutter; it is smooth the next time. Close other programs that use the graphics card, and keep the driver up to date.

## Check the download

Optional. The updater checks every download against the SHA-256 published with the release, which proves the file arrived whole. Each release's zip and `Update.bat` are also signed by our release workflow on GitHub (an artifact attestation). To check a zip you downloaded, install the [GitHub CLI](https://cli.github.com), run `gh auth login` once, and in the folder with the zip:

```
gh attestation verify Squadala-<version>-windows.zip --repo daurosk/oot-remake-multiplayer --signer-workflow daurosk/oot-remake-multiplayer/.github/workflows/release-attest.yml
```

It must say the verification succeeded. If it fails, do not use the zip, and tell us ([Reporting a problem](#reporting-a-problem)).

## Reporting a problem

After the game crashes, the next title screen asks once: "The game crashed last time. Please send /bugreport on Discord with the zip of collect-logs.cmd." Any button closes it (the cursor is back on the title's first button, as before the page came up), and it does not come back for that crash (it is never shown for a game that was only closed). A load that never ended (2 minutes of the game's loading with no world, for example a mod missing a file) is asked about the same way: "Loading got stuck last time." To turn it off, set `LICHII_COOP_PROBLEM_ASK=0` in `coop-options.ini`.

Close the game and double-click `scripts\collect-logs.cmd` in the kit. It makes `coop-logs-<date>.zip` in the kit folder: the game's log, the co-op settings, the server logs, the build's log and the kit's version. With Nucleus split screen, every player's game log is in it (under `nucleus`). Then, on the [Squadala! Discord](https://discord.gg/GwtRKAqng5), type **`/bugreport`**, describe what you did and what went wrong, and attach that zip; the bot answers with your report's number. You may also describe it on the repo's [issue page](https://github.com/daurosk/oot-remake-multiplayer/issues) (give the report number there), but never attach the zip to a public issue. Addresses, your Windows user name, your PC name and any session password are masked in it; still, never paste addresses or log lines with addresses into a public issue.
