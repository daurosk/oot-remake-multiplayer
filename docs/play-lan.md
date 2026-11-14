# Playing on the same network (LAN)

For players on the same home network (the same router, wired or Wi-Fi). Every player has done [setup.md](setup.md) first, with the same release.

One player is the **host**: their PC runs the server, and they play on it too. The others **join**.

## On Hookshot's title screen

With Host and Join on the title screen (no `.bat` file):

1. **The host** opens **Host** on the title screen, then chooses **Same network or VPN**. The game shows one line, for example `192.168.1.20:27510 abcdefghijklmnopqrst` (this PC's address and port, a space, the session's join password), and has copied it. With a VPN program running, its address comes first (Radmin VPN `26.x.x.x`, Hamachi `25.x.x.x`, Tailscale `100.x.x.x`).
2. The host pastes that line into a chat with the friends (Ctrl+V). It holds the join password, so keep it among friends.
3. **The others** open **Join** on the title screen and paste the line where the invite code goes (A on the field, or Ctrl+V), then **OK**. Nothing else to type.
4. If Windows asks about the firewall on the host's PC, allow the game on private networks.

The invite code is for friends over the internet: it holds the host's internet address, which often does not work from inside the same house. **Copy the code again** on the Host screen brings the invite code back.

The rest of this page is for `Host.bat` / `Join.bat` (and `Host-Hookshot.bat` / `Join-Hookshot.bat`).

## Host

1. Double-click `Host.bat`. The first time it asks for your player name.
2. A server window opens with a box that starts **Give your friends ONE of these**, for example:

   ```
   Give your friends ONE of these (they type it in Join.bat):

     192.168.1.20          Same house (friends on your home network or Wi-Fi)
   ```

   Keep that window open while you play; closing it ends the session for everyone.
3. Tell the others the **Same house** address. Without a VPN program it is already copied: paste it into a chat with Ctrl+V. Also give them the line **Join password for this session** from the same window: `Host.bat` makes a new password each time it starts, unless you set your own with `server_password` in `coop-options.ini`. (The window also shows an invite code; that is for friends over the internet, see [play-online.md](play-online.md).)
4. The first time, Windows Firewall asks about `coop_server.exe`: tick **Private networks** and click **Allow access**. If you missed it, see [Firewall](#firewall).
5. Start the game in the emulator and load your save.

## Join

1. Double-click `Join.bat`.
2. Paste or type the host's address (for example `192.168.1.20`) and press Enter, then the join password the host gave you (Join.bat says when you are connected, or what is most likely wrong). Next time Enter keeps the last address and password. The first time it also asks for your player name; use a different one from the others.
3. Start the game and load your save.

In the server window you see `player 2 joined: <name>`, and in each game the panel in the top-left corner lists the other players with their distance. In the same area, each player sees the others as Link.

## Firewall

Only the host needs this, and only UDP port 27510 (or the port in `emulator\user\config\coop.ini`). Joining players connect out and need nothing.

If the others cannot connect, run `check-hosting.cmd` in the kit folder while the server window is open: it checks the server and the firewall and prints the exact command to fix what is wrong (it changes nothing itself). Or allow the server by hand: open **Command Prompt as administrator** on the host, go to your kit folder (the one with `Host.bat`; put in your own path) and add the rule:

```
cd /d "C:\Games\oot-coop"
netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow program="%CD%\server\coop_server.exe" protocol=UDP localport=27510 profile=private remoteip=localsubnet
```

The rule lets in only what it must:

- `program=...`: only the co-op server, not any other program that might listen on that port later.
- `profile=private`: only on networks Windows calls Private (your home), never on a café or hotel Wi-Fi.
- `remoteip=localsubnet`: only PCs on your own network, not the whole internet.

Windows must also see your home network as **Private** (Settings > Network & internet > your connection > Network profile type). On a **Public** network the rule does not apply: switch your home network to Private rather than widening the rule. Never allow the port on Public networks for every address.

If you move the kit to another folder, delete the rule and add it again from the new folder.

### When you are done

The server opens nothing by itself: when its window is closed, nothing listens. The rule stays until you delete it, so remove it when you stop playing for a while (Command Prompt as administrator):

```
netsh advfirewall firewall delete rule name="OoT co-op server UDP 27510"
```

## Two players on one PC

Two games side by side, each with its own controller, or one with a controller and one with the keyboard and mouse. Hookshot only (patch 0015); the emulator gives the keyboard and every pad to the window in front.

1. **Two kits.** Unzip the kit twice, into two short folders (for example `C:\oot-coop-1` and `C:\oot-coop-2`), and do [setup.md, "Play on Hookshot"](setup.md#play-on-hookshot-experimental) in each, or copy a kit folder in Windows (a "- Copy" folder works too). Each kit starts its own game: the second kit copies the game the first one built (a minute or two) instead of building it again, and the copy starts with the first kit's saves as they are then (from then on each kit keeps its own). A Hookshot game takes about 10 GB of RAM, so two need about 20 GB free. A 0.10.1 kit copied in Windows starts the first kit's game instead and says "the co-op game of another kit is running": in the copy, open `hookshot\game-folder.txt` and change it to the copy's own `hookshot\game` folder (or update the kit).
2. **Plug in the controllers** before you start the games. They are numbered in the order they were connected: the first one plugged in is controller 1. A controller that turns off or is unplugged gets its number back when it returns, and the others keep theirs. Steam Controller / Steam Deck pads with Steam Input on show twice (the real pad and Steam's stand-in): choose the stand-in, and the game gets no motion controls from it.
3. **Choose each game's controller.** In each kit's `coop-options.ini` add one line, or use the **Multiplayer** tab's **Settings > Controller** row in the game, which shows each controller's name and applies at once:

   | Setup | Kit 1 (`coop-options.ini`) | Kit 2 (`coop-options.ini`) |
   |---|---|---|
   | A controller each | `LICHII_COOP_PAD=1` | `LICHII_COOP_PAD=2` |
   | Controller + keyboard and mouse | `LICHII_COOP_PAD=1` | `LICHII_COOP_PAD=none` |

   A game with a controller chosen reads only that one, and keeps reading it while the other game's window is in front. With `none` the game reads no controller, only the keyboard and mouse.
4. **Start them:** `Play-OoT-Coop.bat` in kit 1, **Host** on its title screen; `Play-OoT-Coop.bat` in kit 2, **Join** with `127.0.0.1` (or `Host-Hookshot.bat` in one and `Join-Hookshot.bat` with `127.0.0.1` in the other).
5. **The keyboard and mouse** go to the window in front (click it to play there). A controller player does not need their window in front. To keep the keyboard out of the controller game altogether, add `LICHII_COOP_KEYBOARD=off` to its kit; `LICHII_COOP_KEYBOARD=always` makes a game read the keys even behind the other window (no mouse).

`hookshot_log.txt` beside each `hookshot.exe` says what it chose: `hid: this game's controller: one pad (1)` and `coop: controller 1, keyboard focus`. The default (no line, or `LICHII_COOP_PAD=auto`) is Hookshot's own way: every controller, in the window in front only (the Insert menu's **Controller in background** also lets them in behind). A game built before patch 0015 says `this Hookshot cannot choose a controller`: run `Build-Hookshot-Coop.bat` again.

## Something does not work

See the troubleshooting list in [play-online.md](play-online.md#troubleshooting); it applies here too.
