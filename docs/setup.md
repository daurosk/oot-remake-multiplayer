# Setting up co-op

Online co-op for the Ocarina of Time remake, on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (Team MagicBean and contributors, GPL-3.0; the continuation of Hookshot by NotRexed and contributors): a native PC version of the game that the kit builds on your PC from your own copy. There is no emulator: the game runs as a normal Windows program. This is an early test build (the newest is on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)); [known-issues.md](known-issues.md) lists what works and what does not yet.

The kit has one file to start: **`Start OoT Co-op.bat`**. Everything else is done by it or in the game. (The older emulator kits, with `Host.bat` and `Join.bat`, have their own guide: [setup-emulator.md](setup-emulator.md). A player on an emulator kit cannot join a game hosted with this kit, nor the other way round: every player needs this kit, the same version.)

## What you need

- **Windows 10 or 11, 64-bit.** No administrator rights are needed.
- **A graphics card with Vulkan 1.3 and mesh shaders**: NVIDIA RTX 20 series or newer, or AMD RX 6000 series or newer. Older cards (GTX 10 series, RX 5000 and older) cannot run it; Intel cards are mostly untested.
- **16 GB of RAM** (the game uses about 10 GB; 8 GB of video memory is recommended).
- **About 60 GB free** on the drive the kit is on, for the first build. The kit builds Longshot (the continuation of Hookshot) on your PC with co-op built into its source; the 60 GB is the build's peak. Afterwards about 37 GB stay (measured on a built kit): the game in `hookshot\game` is 35 GB (24 GB of it your game's unpacked files, 7 GB its shader programs), and the build files in `hookshot\work`, `tools` and `downloads` are about 2 GB, kept so that updates take minutes.
- **Your own game as a `.nup` file** (or its `romfs` and `exefs` folders): program `0400486024DEC000`, update v131072, build `e0f3ccfa`. No keys or firmware are needed. Every player needs their own copy of the game.
- An internet connection for the first build (it downloads Longshot's build tools, about 400 MB) and to play.

<a id="play-on-hookshot-experimental"></a>

## Step by step

1. **Download** `oot-coop-<version>.zip` from the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases).
2. **Unzip it.** Right-click the zip, choose **Extract All...**, and extract it to a short folder on a drive with 60 GB free, for example `C:\OoT-Coop`. Not inside the zip, not in OneDrive (your Desktop and Documents are often in OneDrive), not under Program Files.
3. **Double-click `Start OoT Co-op.bat`** in that folder. A window opens and checks the kit first (see [If something goes wrong](#if-something-goes-wrong)).
4. **The first time, give it your game.** It asks you to drag your game's `.nup` file into the window and press Enter (for a `romfs` folder it then asks for the `exefs` folder). It remembers the file; it never asks again. Then it builds the game: **10 to 45 minutes, once**. The window shows each stage; the PC stays usable. It also makes the puppet pack (the other players' Link and tunic colours) from your game, and puts a **Play OoT Co-op** shortcut on your desktop.
5. **The game starts.** **Host** and **Join** are in the game's menu, on its first tab, **Multiplayer**. To open it: on a controller **−** and **+** together (on an Xbox-style pad **View** and **Menu**), or **Home**; on the keyboard **F1** (Insert and Esc also open it unless you changed that in the menu); on the title screen **−** alone. The D-pad or the arrow keys choose, A or Enter confirms, B or Esc goes back.
   - **Host:** one press. The invite code is copied for you: send it to your friends (Discord, a text). For friends in the same house or on a VPN, the Host screen has **Same network or VPN**.
   - **Join:** paste your friend's code (it is filled in already when it is on your clipboard), then OK.
   - **Co-op Settings:** your name, your colour, the number of players when you host, voice chat.
6. **Next time**, double-click `Start OoT Co-op.bat` or the desktop shortcut: the game starts in seconds.

To check it works: in the game the other players show as Link with their name tags, and **T** opens the chat box. The log `longshot_log.txt` beside `longshot.exe` (in the kit's `hookshot\game\bin\`) says `coop: module loaded`, then `coop: connected`.

## Updating

Nothing to do: on every start the kit asks GitHub whether a new co-op version is out, and if so asks you once (`auto_update = yes` in `coop-options.ini` updates without asking, `no` never does). It downloads the new kit, checks it, keeps your settings, your game file and the built game, and puts the new co-op into the game in seconds. It builds the game again only when the update moves to a new Longshot (it says so: about 15 minutes). Every player needs the same version.

`Update.bat` in the kit folder updates by hand (for example when the start said it could not reach GitHub). It is also attached to each release: dropped into an older emulator kit, it turns that kit into a Longshot kit. The emulator, `Host.bat`, `Join.bat` and the other old files go into the kit's `backup-<version>\` folder; your emulator files (`emulator\user`: its saves, keys and settings) stay where they are, and the window says you can delete that folder when you no longer need it.

## The kit folder

| What | Why |
|---|---|
| `Start OoT Co-op.bat` | The only file you start |
| `README.txt` | The steps above, short |
| `Update.bat` | Updates by hand (the start does it by itself) |
| `coop-options.ini` | Co-op switches; the game's Co-op Settings write it too |
| `start-server.cmd`, `server\` | A dedicated co-op server, only if you want one (Host in the game needs neither) |
| `hookshot\` | The co-op module and Longshot's pin and patches; after the first start also `tools\` (1.1 GB), `downloads\` (0.3 GB), `work\` (0.7 GB; kept so updates are quick, deleting them makes the next build take the full time) and `game\` (your built game, 35 GB) |
| `scripts\`, `tools\` | What the start runs; `scripts\collect-logs.cmd` makes a log zip for a bug report |
| `docs\`, `licenses\` | This guide, the known issues, the licences and where the source is |
| `logs\`, `VERSION.txt`, `MANIFEST.txt` | Logs, the kit's version, its file list (for the updater) |

Nothing of the game is in the kit. The built game and the puppet pack are made from your game: never share or upload them.

<a id="troubleshooting"></a>

## If something goes wrong

The start window says what is wrong and what to do. What it checks:

- **"This kit is still inside its zip file."** You opened the zip and double-clicked the file inside it. Close it, right-click the zip, **Extract All...**, and start `Start OoT Co-op.bat` in the folder it made.
- **"OoT Co-op is already starting in another window."** You double-clicked twice. Wait for the first window (the first build takes a while).
- **"This folder is in OneDrive"**, **"...letters the game build cannot handle"**, **"...too long"**, **"...cannot be written to"**: the first build cannot work in this folder. It offers to copy the kit to `C:\OoT-Coop` and start it there; say yes (or move the folder yourself). Once the game is built, the kit no longer asks.
- **"Your antivirus has most likely removed..."**: Windows Security (or another antivirus) took a file of the kit or of the built game. Open **Windows Security > Virus & threat protection > Protection history**, restore the file, and add the kit folder under **Exclusions**. Then start again: the game is not built again.
- **"The game isn't set up yet... Set it up now?"**: the first start. Answer **Y** (or wait a minute) and it builds the game once; **n** builds nothing until the next start.
- **"Windows protected your PC"** when you start `Start OoT Co-op.bat`: SmartScreen, because the kit's files came from a downloaded zip. Click **More info**, then **Run anyway**. The kit then takes Windows' "downloaded from the internet" mark off its own files, once, so Windows does not ask about them again. To avoid it from the start: right-click the zip, **Properties**, tick **Unblock**, **OK**, then extract it. Defender's exclusions and turning real-time protection off do not change SmartScreen.
- **"Windows Smart App Control is blocking this kit's PowerShell scripts"** or **"...blocked <file>"**: Smart App Control (Windows 11) blocks programs and scripts without a known signature, whatever Defender's settings are; the kit's are not signed. To use the kit, turn it off: **Windows Security > App & browser control > Smart App Control settings > Off**. Windows cannot turn it back on without a reset of Windows, so that is your choice. (Longshot's own builder is a single program that downloads its tools itself, so nothing it runs carries the internet mark; the kit is PowerShell scripts from a zip.)
- **"PowerShell on this PC runs in ConstrainedLanguage mode"** or **"A Group Policy on this PC sets PowerShell's execution policy"**: a policy (work or school PCs, or a privacy or tweak tool) stops unsigned scripts. On your own PC the message says which setting to change; otherwise ask the PC's owner.
- **"Windows would not let the build move or delete ...\hookshot\tools.new"** (in 0.10.1: a red `Access to the path '...\tools.new' is denied`, even as administrator): a program still had a file of the freshly unpacked tools open. The kit now tries again for 30 seconds, then moves the folder with robocopy, which copies even files another program has open. When even that fails, the message names the program holding it (an antivirus, Explorer, the search indexer, a sync client such as Google Drive or Dropbox): close it or add the kit folder to its exclusions, and start again. A kit inside a cloud-sync folder: move it out, to `C:\OoT-Coop`.
- **"Windows refused to let PowerShell write in this kit's folder (access denied)"**: Controlled folder access or another antivirus. The message says where to allow `powershell.exe`.
- **"Controlled folder access protects <folder>"**: Windows Security's ransomware protection guards that folder (Documents, Desktop and others) and stops the game build writing there, even with real-time protection off. Move the kit to `C:\OoT-Coop`.
- Whatever Windows said, `logs\windows-checks.txt` in the kit has what the start found. It is in the log zip `scripts\collect-logs.cmd` makes; when that zip cannot be made, it makes `coop-logs-windows-checks.zip` instead.
- **"The co-op game this kit built is not in <folder> any more"**: the built game folder was moved or deleted. Moved it? Answer **n**, move it back to that folder and start again. Otherwise answer **Y** and it is built again.
- **"The co-op module hookshot_coop.dll is missing"**: antivirus took it, as in the line above: restore it, add the exclusion, start again. Building again does not help.
- **"Files of this kit are missing"**: the unzip did not finish. Unzip the kit again.
- **"The Microsoft Visual C++ runtime is missing"**: install it from Microsoft ([vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)) and start again.
- **"This is not the game version co-op needs"**: your `.nup` is another version of the game. Co-op needs program `0400486024DEC000`, update v131072, build `e0f3ccfa`. To give another file, delete `hookshot\game-files.txt` in the kit and start again.
- **The build stops on a download or a SHA-256 that does not match:** start again (a download may have broken off). If it keeps failing, report it with the window's text.
- **Not enough space:** the build warns below about 60 GB free. Free some space, or move the kit to a bigger drive.
- **Note: another co-op kit is on this PC.** An older kit folder: you can delete it once this one works. The new kit takes your game file from it, so it does not ask again.
- **The game closes at the start or the screen goes black:** check that your card has mesh shaders (above) and that its driver is up to date. Longshot's own open problems are on [Longshot's page](https://git.magicbean.dev/MagicBeanDev/Longshot/issues). To try the game without co-op, set `HOS_COOP=0` before starting `longshot.exe`; if it crashes without co-op too, it is Longshot's problem, not co-op's.
- **Host or Join says a router blocks the way in**, or friends cannot reach you: the Host screen says what to do (the answer code, UPnP, **Same network or VPN**); [play-online.md](play-online.md) has more.

## Built Longshot yourself?

The kit never plays a Longshot you built with Longshot's own builder, and moving that game into the kit folder does nothing: co-op needs its own build of the Longshot release the kit is made for (`hookshot\PIN`, now v3.0.0-rc.3), with co-op's changes in its source. A nightly or dev build is another version, so it cannot be used either.

Before the first build the kit asks once: **"Built Longshot yourself? ... Drag your Longshot game folder ... into this window"** (the folder with `longshot.exe`; just Enter, or 60 seconds, for no). The answer is kept in `hookshot\longshot-folder.txt`: to be asked again, delete that file before the game is built (once it is built, copy saves by hand, below). It then:
- copies the tool downloads your Longshot's setup already has, when they are the very same files (by SHA-256): less to download;
- copies its compiled files too when it is the same release (v3.0.0-rc.3), so that what co-op does not change is not compiled again (the game is still translated again, with co-op in it);
- copies your saves into the co-op game after the build (below).

Nothing is ever changed in your Longshot's folders. Without a folder, the kit still reuses the downloads in Longshot Builder's home (`%LOCALAPPDATA%\LongshotBuilder`) when it finds one.

### Bring your own Longshot save

Given your Longshot folder (above), the first build copies its save slots (`user\save\<account>\slot_NN`, with `account.sav` and `option.sav`) into the co-op game's `user\save\<account>\`. It only copies: your Longshot's saves stay as they are, and a slot the co-op game has already is never overwritten (it says which it left). The saves are the game's own files, so a save from a nightly should load too when it is the same game version (not tested yet).

Already set up? Copy by hand: close both games, keep a copy of the kit's `hookshot\game\bin\user\save` if it has slots you want, then copy the `slot_NN` folders (and `account.sav`, `option.sav`) from `<your Longshot>\bin\user\save\<account>\` (or `<your Longshot>\user\save\...`) to the same place under the kit's `hookshot\game\bin\`.

## In the game, saves, the emulator kit

<a id="co-op-on-hookshot"></a><a id="saves"></a><a id="emulator-setup-for-new-players"></a><a id="4-the-puppet-pack"></a><a id="linux-and-steam-deck"></a>

The co-op keys in the game (chat **T**, player list **F6**, panel **F7**, item cheats **F8**), the chat commands and how saves are shared are the same as in the emulator kits: [setup-emulator.md](setup-emulator.md#co-op-on-hookshot) and [setup-emulator.md, "Saves"](setup-emulator.md#saves). That guide also covers the older emulator kits themselves (keys, firmware, the puppet pack, Linux).

## Low frame rate

Longshot builds its shaders the first time it sees each place, so the first minutes in a new place may stutter; it is smooth the next time. Close other programs that use the graphics card, and keep the driver up to date.

## Check the download

Optional. The updater checks every download against the SHA-256 published with the release, which proves the file arrived whole. Each release's zip and `Update.bat` are also signed by our release workflow on GitHub (an artifact attestation). To check a zip you downloaded, install the [GitHub CLI](https://cli.github.com), run `gh auth login` once, and in the folder with the zip:

```
gh attestation verify oot-coop-<version>.zip --repo daurosk/oot-remake-multiplayer --signer-workflow daurosk/oot-remake-multiplayer/.github/workflows/release-attest.yml
```

It must say the verification succeeded. If it fails, do not use the zip, and tell us ([Reporting a problem](#reporting-a-problem)).

## Reporting a problem

Close the game and double-click `scripts\collect-logs.cmd` in the kit. Describe what you did and what went wrong on the repo's [issue page](https://github.com/daurosk/oot-remake-multiplayer/issues), and send the zip it makes (`coop-logs-<date>.zip` in the kit folder: the game's log, the co-op settings, the server logs, the build's log and the kit's version) **privately** to the maintainer (a Discord DM), not as an attachment on the issue. Addresses, your Windows user name, your PC name and any session password are masked in it; still, never paste addresses or log lines with addresses into a public issue.
