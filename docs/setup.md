# Setting up co-op

Online co-op for the Ocarina of Time remake, on [Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0): a native PC version of the game that the kit builds on your PC from your own copy. There is no emulator: the game runs as a normal Windows program. This is an early test build (the newest is on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)); [known-issues.md](known-issues.md) lists what works and what does not yet.

The kit has one file to start: **`Start OoT Co-op.bat`**. Everything else is done by it or in the game. (The older emulator kits, with `Host.bat` and `Join.bat`, have their own guide: [setup-emulator.md](setup-emulator.md). A player on an emulator kit cannot join a game hosted with this kit, nor the other way round: every player needs this kit, the same version.)

## What you need

- **Windows 10 or 11, 64-bit.** No administrator rights are needed.
- **A graphics card with Vulkan 1.3 and mesh shaders**: NVIDIA RTX 20 series or newer, or AMD RX 6000 series or newer. Older cards (GTX 10 series, RX 5000 and older) cannot run it; Intel cards are mostly untested.
- **16 GB of RAM** (the game uses about 10 GB; 8 GB of video memory is recommended).
- **About 60 GB free** on the drive the kit is on, for the first build.
- **Your own game as a `.nup` file** (or its `romfs` and `exefs` folders): program `0400486024DEC000`, update v131072, build `e0f3ccfa`. No keys or firmware are needed. Every player needs their own copy of the game.
- An internet connection for the first build (it downloads Hookshot's build tools, about 400 MB) and to play.

<a id="play-on-hookshot-experimental"></a>

## Step by step

1. **Download** `oot-coop-<version>.zip` from the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases).
2. **Unzip it.** Right-click the zip, choose **Extract All...**, and extract it to a short folder on a drive with 60 GB free, for example `C:\OoT-Coop`. Not inside the zip, not in OneDrive (your Desktop and Documents are often in OneDrive), not under Program Files.
3. **Double-click `Start OoT Co-op.bat`** in that folder. A window opens and checks the kit first (see [If something goes wrong](#if-something-goes-wrong)).
4. **The first time, give it your game.** It asks you to drag your game's `.nup` file into the window and press Enter (for a `romfs` folder it then asks for the `exefs` folder). It remembers the file; it never asks again. Then it builds the game: **10 to 45 minutes, once**. The window shows each stage; the PC stays usable. It also makes the puppet pack (the other players' Link and tunic colours) from your game, and puts a **Play OoT Co-op** shortcut on your desktop.
5. **The game starts.** On the title screen, **Host** and **Join** are under the game's own rows (**Minus** opens them if they are not there).
   - **Host:** one press. The invite code is copied for you: send it to your friends (Discord, a text). For friends in the same house or on a VPN, the Host screen has **Same network or VPN**.
   - **Join:** paste your friend's code (it is filled in already when it is on your clipboard), then OK.
   - **Co-op Settings:** your name, your colour, the number of players when you host, voice chat.
6. **Next time**, double-click `Start OoT Co-op.bat` or the desktop shortcut: the game starts in seconds.

To check it works: in the game the other players show as Link with their name tags, and **T** opens the chat box. The log `hookshot_log.txt` beside `hookshot.exe` (in the kit's `hookshot\game\bin\`) says `coop: module loaded`, then `coop: connected`.

## Updating

Nothing to do: on every start the kit asks GitHub whether a new co-op version is out, and if so asks you once (`auto_update = yes` in `coop-options.ini` updates without asking, `no` never does). It downloads the new kit, checks it, keeps your settings, your game file and the built game, and puts the new co-op into the game in seconds. It builds the game again only when the update moves to a new Hookshot (it says so: about 15 minutes). Every player needs the same version.

`Update.bat` in the kit folder updates by hand (for example when the start said it could not reach GitHub). It is also attached to each release: dropped into an older emulator kit, it turns that kit into a Hookshot kit. The emulator, `Host.bat`, `Join.bat` and the other old files go into the kit's `backup-<version>\` folder; your emulator files (`emulator\user`: its saves, keys and settings) stay where they are, and the window says you can delete that folder when you no longer need it.

## The kit folder

| What | Why |
|---|---|
| `Start OoT Co-op.bat` | The only file you start |
| `README.txt` | The steps above, short |
| `Update.bat` | Updates by hand (the start does it by itself) |
| `coop-options.ini` | Co-op switches; the game's Co-op Settings write it too |
| `start-server.cmd`, `server\` | A dedicated co-op server, only if you want one (Host in the game needs neither) |
| `hookshot\` | The co-op module and Hookshot's pin and patches; after the first start also `tools\`, `downloads\`, `work\` (kept so updates are quick; about 50 GB, deleting it makes the next build take the full time) and `game\` (your built game) |
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
- **"Files of this kit are missing"**: the unzip did not finish. Unzip the kit again.
- **"The Microsoft Visual C++ runtime is missing"**: install it from Microsoft ([vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)) and start again.
- **"This is not the game version co-op needs"**: your `.nup` is another version of the game. Co-op needs program `0400486024DEC000`, update v131072, build `e0f3ccfa`. To give another file, delete `hookshot\game-files.txt` in the kit and start again.
- **The build stops on a download or a SHA-256 that does not match:** start again (a download may have broken off). If it keeps failing, report it with the window's text.
- **Not enough space:** the build warns below about 60 GB free. Free some space, or move the kit to a bigger drive.
- **Note: another co-op kit is on this PC.** An older kit folder: you can delete it once this one works. The new kit takes your game file from it, so it does not ask again.
- **The game closes at the start or the screen goes black:** check that your card has mesh shaders (above) and that its driver is up to date. Hookshot's own open problems are on [Hookshot's page](https://github.com/NotRexed/Hookshot). To try the game without co-op, set `HOS_COOP=0` before starting `hookshot.exe`; if it crashes without co-op too, it is Hookshot's problem, not co-op's.
- **Host or Join says a router blocks the way in**, or friends cannot reach you: the Host screen says what to do (the answer code, UPnP, **Same network or VPN**); [play-online.md](play-online.md) has more.

## In the game, saves, the emulator kit

<a id="co-op-on-hookshot"></a><a id="saves"></a><a id="emulator-setup-for-new-players"></a><a id="4-the-puppet-pack"></a><a id="linux-and-steam-deck"></a>

The co-op keys in the game (chat **T**, player list **F6**, panel **F7**, item cheats **F8**), the chat commands and how saves are shared are the same as in the emulator kits: [setup-emulator.md](setup-emulator.md#co-op-on-hookshot) and [setup-emulator.md, "Saves"](setup-emulator.md#saves). That guide also covers the older emulator kits themselves (keys, firmware, the puppet pack, Linux).

## Low frame rate

Hookshot builds its shaders the first time it sees each place, so the first minutes in a new place may stutter; it is smooth the next time. Close other programs that use the graphics card, and keep the driver up to date.

## Check the download

Optional. The updater checks every download against the SHA-256 published with the release, which proves the file arrived whole. Each release's zip and `Update.bat` are also signed by our release workflow on GitHub (an artifact attestation). To check a zip you downloaded, install the [GitHub CLI](https://cli.github.com), run `gh auth login` once, and in the folder with the zip:

```
gh attestation verify oot-coop-<version>.zip --repo daurosk/oot-remake-multiplayer --signer-workflow daurosk/oot-remake-multiplayer/.github/workflows/release-attest.yml
```

It must say the verification succeeded. If it fails, do not use the zip, and tell us ([Reporting a problem](#reporting-a-problem)).

## Reporting a problem

Close the game and double-click `scripts\collect-logs.cmd` in the kit. Describe what you did and what went wrong on the repo's [issue page](https://github.com/daurosk/oot-remake-multiplayer/issues), and send the zip it makes (`coop-logs-<date>.zip` in the kit folder: the game's log, the co-op settings, the server logs, the build's log and the kit's version) **privately** to the maintainer (a Discord DM), not as an attachment on the issue. Addresses, your Windows user name, your PC name and any session password are masked in it; still, never paste addresses or log lines with addresses into a public issue.
