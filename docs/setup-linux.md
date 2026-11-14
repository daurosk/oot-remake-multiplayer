# Co-op on Linux and Steam Deck (Longshot)

The Linux kit is the same co-op as the Windows kit, on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)'s
Linux build: the game is compiled on your PC from your own copy of it, with co-op built in. There is no emulator.

**Experimental, untested:** this kit has not yet been run on a real Linux PC or a Steam Deck. CI builds it and
checks that the module loads, but no one has built the game with it or played on it yet, and Linux and Windows players
playing together (same server, same version) is not yet proven. Please report what happens (`bash scripts/collect-logs.sh`).

## What you need

- A 64-bit x86 PC with a Vulkan 1.3 graphics driver (Mesa or NVIDIA), or a Steam Deck.
- A Linux from 2022 or newer (glibc 2.35: Ubuntu 22.04, Debian 12, Fedora 36, Arch, SteamOS 3).
- Your own dump of the game as a `.nup` (program `0400486024DEC000`, update v131072). Each player needs their own.
- About 60 GB free for the first build (15 to 60 minutes, once), and the internet for Longshot's tools (about 270 MB).
- `bash`, and `curl` or `wget`. To unzip: `unzip`, a file manager, or Ark on the Steam Deck.

## Steps

1. Download `oot-coop-<version>-linux.zip` and unzip it to a folder of your own, such as `~/OoT-Coop`.
2. Open a terminal in that folder and type `bash start-oot-coop.sh`. (Typing `bash` first works even when the unzip
   tool dropped the file's "executable" mark.)
3. The first time it asks for your game: drag your `.nup` into the terminal and press Enter. (It finds it by itself
   when it is in Downloads, Desktop, Documents or your home folder.) Then it builds the game, once.
4. The game starts. **Host** or **Join** in the game's menu (the Multiplayer tab). Host gives you an invite code for
   your friends (it is also put on your clipboard); Join takes a friend's code.

After the first build, `bash start-oot-coop.sh` starts the game in seconds, and the kit adds an **OoT Co-op** entry to
your applications menu. Each start checks for a new co-op version and offers it; every player needs the same one.

## Steam Deck

1. Switch to **Desktop Mode** (Steam button > Power > Switch to Desktop).
2. Unzip the kit (Dolphin, right-click > Extract) to `~/OoT-Coop` (not on the SD card's root if it is formatted
   exFAT: the build needs a Linux file system; the Deck's own storage or an ext4 SD card).
3. Right-click the folder's empty space > **Open Terminal Here** (Konsole), type `bash start-oot-coop.sh`, give it your
   `.nup`, and let it build (plug the Deck in; it takes about an hour).
4. When the game has started once, close it. In Steam (still in Desktop Mode): **Games > Add a Non-Steam Game to My
   Library**, tick **OoT Co-op**, **Add Selected Programs**.
5. Back in **Game Mode**, start OoT Co-op from your library. The controls are the Deck's own (Steam Input gives the
   game a standard controller). **Steam + X** opens the on-screen keyboard for the chat box (T on a keyboard) and for
   typing an invite code in the Join page. Game Mode has no clipboard tool: the invite code is shown in the
   Multiplayer tab to read out or type.

Updates: the first start after a new version asks in the terminal; in Game Mode there is no terminal, so start it
once from Desktop Mode (step 3) when a friend has a newer version.

## If something goes wrong

The terminal says what happened and what to do. Common ones:

| It says | Do |
|---|---|
| "needs your game file ... from a terminal" | The game is not built yet: start it from a terminal once (Steam Deck: Desktop Mode, step 3). |
| "Longshot cannot build from these files" | The `.nup` is another version of the game: dump version 1.1.1 (update v131072). |
| "no Vulkan driver found" | Install your graphics driver's Vulkan part (Ubuntu: `sudo apt install libvulkan1 mesa-vulkan-drivers`). |
| "only N GB free" | Free space where the kit is (the build's work folder is `data/work`, about 50 GB). |
| "too old (glibc ...)" | Your Linux is older than 2022: update it. |
| "Missing on this PC: curl" | Install it (`sudo apt install curl`, `sudo dnf install curl`, `sudo pacman -S curl`). |
| The download or build stopped | Start again: it goes on from where it stopped. |

For a bug report: `bash start-oot-coop.sh --logs` writes `coop-logs-<time>.tar.gz` beside it (passwords, codes,
addresses and your user name are masked). Send that file.

## What is where

| Path | What |
|---|---|
| `start-oot-coop.sh` | The one file you start. |
| `README.txt` | The short version of this page. |
| `scripts/` | The kit's scripts (start, build, update, logs). |
| `data/game/` | The built game, your saves and settings (`longshot_log.txt` is its log). Kept by updates. |
| `data/coop-options.ini` | Co-op's settings (the Multiplayer tab writes them). Updates keep your values. |
| `data/builder/`, `data/work/` | Longshot's builder and tools, and the build's work folder (kept for quick rebuilds). |
| `data/longshot/` | The Longshot release co-op is built on, our patches and the co-op module (`hookshot_coop.so`). |
| `data/logs/` | The build log and the kit's own logs. |

To remove everything: delete the folder, and `~/.local/share/applications/oot-coop.desktop`.

## How it works (for the curious)

`scripts/build.sh` downloads Longshot's own Linux builder for the pinned release (`data/longshot/LINUX.txt`, SHA-256
checked), unpacks it without FUSE, copies its source and applies co-op's patches (`data/longshot/patches`), fetches
Longshot's Linux build tools with Longshot's own `linux_tools.py` (SHA-256 checked), and runs Longshot's
`nx2/make_game.py` on your game. The renderer is compiled from the patched source too. Then `hookshot_coop.so` goes
beside the game, which loads it at its first frame (log line `coop: module loaded`). The co-op code is the same as
on Windows; only the clipboard (wl-copy / xclip / xsel) and the folder names differ.
