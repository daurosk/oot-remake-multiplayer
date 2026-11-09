# Testing co-op on two PCs

How to test the co-op build with the test kit: the server on one PC, the patched emulator on both, each with its own copy of the game. The kit is made by `tools/release/make_kit.ps1` (below); in the kit this file is `TESTING.md`.

What this test shows (plan stage 1, presence): the two emulators join one server, each lists the other player, and the scene and distance follow as you move. Nothing else is shared yet: no puppet Link, no shared progress.

## The kit

```
oot-coop-<date>\
  start-server.cmd     starts the server (one PC only)
  play.cmd             asks for the server address and your name once, then starts the emulator
  collect-logs.cmd     zips the logs to send back
  emulator\            the patched emulator; user\ is its portable data folder
  server\              coop_server.exe
  logs\                the server's logs
  VERSION.txt          which builds these are
```

Copy the whole folder (or the zip, then unzip it) to each PC, anywhere you can write to, for example the desktop. Both PCs must use the same kit.

## What each PC needs

The kit has no game files, keys or firmware. Each PC uses its own, from its own console, put into the kit's portable folder `emulator\user\`:

| What | Where in `emulator\user\` |
|---|---|
| Keys (`prod.keys`, `title.keys`) | `keys\` |
| Firmware | `nand\system\Contents\registered\` (or start `play.cmd` once and use the emulator's Tools > Install Firmware) |
| Game updates you installed | `nand\user\Contents\registered\` (or File > Install Files to NAND) |
| Saves (optional; a new save works for the test) | `nand\user\save\` |
| The game dump itself | Anywhere. Load it with File > Load File, or add its folder to the game list. |

If a PC already runs Lichii or Fritchii normally, all of that is in `%APPDATA%\eden`. Copy (do not move) from there into `emulator\user\`:

- `%APPDATA%\eden\keys` to `emulator\user\keys`
- `%APPDATA%\eden\nand` to `emulator\user\nand` (firmware, updates and saves, about 3 GB)
- optional, Ocarina's own settings (GPU mode and the rest): `%APPDATA%\eden\config\custom\0400486024DEC000.ini` to `emulator\user\config\custom\`

The kit's emulator never reads or writes `%APPDATA%\eden`: because `emulator\user\` exists, it keeps everything there. Your normal install and saves are not touched.

Both PCs need the same game version. Co-op only reads and sends for build id `e0f3ccfaf50065ae`; the emulator log says which build is running (below).

The Visual C++ runtime is in the kit, beside each program. If Windows still says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing, install the Microsoft Visual C++ 2015-2022 Redistributable (x64).

## The network

The two PCs must reach each other over UDP:

- **Same LAN:** use PC 1's LAN address, `192.168.x.y` or `10.x.y.z` (`ipconfig`, "IPv4 Address").
- **Tailscale:** both PCs signed in to the same tailnet. Use PC 1's Tailscale address, `100.x.y.z` (`tailscale ip -4` on PC 1, or the Tailscale window).

`start-server.cmd` prints PC 1's addresses when it starts.

On PC 1 (the server), allow the server's UDP port in Windows Firewall once, from a PowerShell or Command Prompt run as administrator:

```
netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow protocol=UDP localport=27510
```

(Or click **Allow** for private networks when Windows asks about `coop_server.exe` the first time.) Use the same port in the rule if you start the server on another port. To remove the rule after testing:

```
netsh advfirewall firewall delete rule name="OoT co-op server UDP 27510"
```

PC 2 needs no rule: the emulator only connects out.

## Steps

1. **PC 1:** double-click `start-server.cmd` (or `start-server.cmd 27600` for another port). It shows `coop_server: listening on port 27510` and the addresses to use. Leave the window open.
2. **PC 1:** double-click `play.cmd`. Server address: `127.0.0.1` (the server is on this PC). Name: anything, 1 to 16 characters, for example `Saria`.
3. **PC 2:** double-click `play.cmd`. Server address: PC 1's LAN or Tailscale address from step 1. Name: a different one, for example `Mido`.
4. Load the game in both emulators and get into the world on both (load a save or start a new file).

`play.cmd` asks only the first time; after that it reuses the answers. `play.cmd change` asks again. The answers are in `emulator\user\config\coop.ini`.

## What should happen

- In the server window: `player 1 joined: Saria`, then `player 2 joined: Mido` (the order is who started first).
- In each emulator, a small see-through panel in the top-left corner of the game:

  ```
  Co-op: player 1, ping 3 ms
  you: KokiriForest_Child
  P2 Mido: KokiriForest_Child, 412 units away
  ```

  The other player's line is grey while they are in another scene or have not sent a position yet.
- When either player walks, the distance changes. When one goes through a door or a loading zone, the scene name changes on both panels.
- The emulator log, `emulator\user\log\eden_log.txt`, has (in this order):
  - `coop: loaded (ENet 1.3.18)` and `coop: on, server ..., name '...'` at start;
  - `coop: welcomed as player N on ...`;
  - `coop: player M '...' joined` for the other player;
  - after the game loads, `coop: game build e0f3ccfaf50065ae, main at 0x...`.

## If something is wrong

| You see | Check |
|---|---|
| No panel, and the log says `coop: off (...)` | The line above it says what is wrong in `coop.ini`. Run `play.cmd change`. |
| The panel stays on `Co-op: connecting` (PC 2) | The address, the firewall rule on PC 1, and that the server window is still open. On Tailscale, that both PCs show as connected. |
| `name taken` or refused in the log | Both players used the same name: `play.cmd change` on one. |
| `coop: game build ... is not e0f3ccfaf50065ae` | That PC has another game version; co-op reads nothing there. |
| The other player's line never gets a scene or distance | Both must be in the game world, not the title screen. Then send the logs. |
| The emulator crashes or freezes | Send the logs. |

On each PC with a problem, close the emulator (the log is complete only after it closes), then double-click `collect-logs.cmd`. It writes `coop-logs-<PC name>-<date>.zip` in the kit folder with the emulator log, `coop.ini`, the server logs (on PC 1), `VERSION.txt`, the newest crash dumps and a note of the PC's Windows, GPU and addresses. Send that zip back. It has no keys, saves or game files.

## Checklist

Write down pass or fail for each, and the time, so the logs can be matched.

| # | Check | Pass |
|---|---|---|
| 1 | Server starts and shows `listening on port ...` | |
| 2 | PC 1's emulator starts; log has `coop: welcomed as player 1` | |
| 3 | PC 2's emulator starts; log has `coop: welcomed as player 2`; the server shows both joined | |
| 4 | With the game loaded on both, each panel lists the other player by name | |
| 5 | Both in the same scene: the distance goes down as you walk towards each other and up as you walk away | |
| 6 | One player changes scene: both panels show the new scene, and the line goes grey | |
| 7 | Ping on the panel stays under about 100 ms (LAN: a few ms) | |
| 8 | PC 2 closes its emulator: the server shows `player 2 left`, PC 1's log has `coop: player 2 '...' left`, the line goes from PC 1's panel | |
| 9 | PC 2 starts `play.cmd` again and loads the game: welcomed again, and both panels list each other again | |
| 10 | Stop the server (Ctrl+C) and start it again: both logs show `connection lost; reconnecting`, then welcomed again, without restarting the emulators | |
| 11 | Both emulators close without an error or a crash dump | |
| 12 | Ten minutes of play together with no drop (no `connection lost` in either log) | |

After the test, run `collect-logs.cmd` on both PCs anyway and send both zips, even if everything passed.

## Making the kit

On the PC that builds (needs the patched emulator built as [lichii/README.md](../lichii/README.md) says, and this repo's server built Release):

```powershell
cmake -S . -B build
cmake --build build --config Release --target coop_server
powershell -ExecutionPolicy Bypass -File tools\release\make_kit.ps1
```

It writes `..\coop-kit\oot-coop-<yyyymmdd-hhmm>\` and a zip of it beside the main checkout. Options: `-EmulatorBin <folder>` (default `..\fritchii-src\build\coop\bin`), `-BuildDir <folder>` (default `build`), `-OutRoot <folder>`, `-Port <n>` (the default port written into the kit, 27510), `-IncludePdb` (adds `lichii.pdb`, about 60 MB, for crash dumps with symbols), `-NoZip`. It refuses an emulator built without co-op and a Debug server, and writes which commits went in to `VERSION.txt`.
