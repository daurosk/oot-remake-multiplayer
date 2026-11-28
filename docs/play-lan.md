# Playing on the same network (LAN)

For players on the same home network (the same router, wired or Wi-Fi). Every player has done [setup.md](setup.md) first, with the same release.

One player is the **host**: their PC runs the server, and they play on it too. The others **join**.

## In the game: the Multiplayer tab

Host and Join are in the game's menu, first tab **Multiplayer** ([setup.md](setup.md) step 5 says how to open it; no `.bat` file):

1. **The host** opens **Host** in the Multiplayer tab (and optionally picks another **Port** from 1 to 65535; default 27510), then chooses **Same network** (friends in the same house) or **VPN** (friends on Radmin VPN, Hamachi, ZeroTier, Tailscale or WireGuard with you). The game shows one line and has copied it: on the same network just the address and port, for example `192.168.1.20:27510` (no password: the host lets its own network in without it), else, for example on Radmin VPN or Hamachi, `26.12.34.56:27510 abcdefghijklmnopqrst` (a space, then the session's join password). **VPN** finds the VPN program's network card by its name and uses its address (Radmin VPN `26.x.x.x`, Hamachi `25.x.x.x`, Tailscale `100.x.x.x`, ZeroTier and WireGuard whatever they give); with no VPN running it says **No VPN found**.
2. The host pastes that line into a chat with the friends (Ctrl+V), or tells them the address, the port and, when the line has one, the password (the word after the address). A line with the join password stays among friends.
3. **The others** open **Join a game** in the Lobby tab and either paste the line on the page (Ctrl+V, or A on **Room or address** with a controller), or type the host's address into **Room or address**, then **Port** and **Password** (the word after the address), then choose **Join**. On the same network (an address such as `192.168.x.x`, `10.x.x.x`, `172.16.x.x` to `172.31.x.x` or `169.254.x.x`) leave the password empty: the host lets its own network in without it (Co-op Settings, **Password on this network**: **Not needed**; the host decides by the address your connection really comes from, so a player on the internet still needs the password). A host who set it to **Needed** turns an empty password away with *this host asks for its join password on its own network too*: type the password then. Choose **Needed** on a network you do not trust: a tunnel, a VPN or a port forward on it brings players from the internet in from such an address too. The pause menu's Multiplayer page, **Find a game**, lists the games on this network; pick one and **Join this game** (no password to type unless its host chose Needed; [protocol.md](protocol.md), "Same network").
4. If Windows asks about the firewall on the host's PC, allow the game on private networks.

The invite code is for friends over the internet: it holds the host's internet address, which many routers do not let back into the same house. A joiner on a newer version than 0.10.1 also tries this PC and the home network with it, so the invite code then works in the same house and on the same PC too (with Host.bat's own password, or one of 20 letters or more: a short password is not sent across the home network). On 0.10.1, use the **Same house** address. **Code** on the Host screen brings the invite code back (A copies it).

Host, Join and Firewall below are for the older emulator kits (up to the early test release of 2026-11-12, the last with the emulator) with `Host.bat` / `Join.bat`; the firewall rule also applies to `start-server.cmd`.

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

## Two to four players on one PC

One kit, one click: each player gets a game of their own in a window, with their own controller. The games fill the screen side by side (2 players) or in quarters (3 or 4). Each game needs about 10 GB of RAM (2 players about 20 GB, 4 about 40 GB) and a share of the graphics card.

1. **Plug in the controllers** before you start. They are numbered in the order they were connected: the first one plugged in is controller 1. A controller that turns off or is unplugged gets its number back when it returns. Steam Controller / Steam Deck pads with Steam Input on show twice (the real pad and Steam's stand-in): choose the stand-in in the game, and it gets no motion controls from it.
2. **Double-click `Start Squadala.bat`, then press 2, 3 or 4** in its window while it starts. Its first lines say so in blue: *Local co-op (2 to 4 players on this PC): press 2, 3 or 4 in this window while Squadala starts, then answer its question*. The key counts at any moment before the game opens, also while another line or question is on screen (the NPC characters line, the update question). A quick start (no update, no game build) waits 3 seconds with the line *Local co-op: press 2, 3 or 4 now (the game starts in 3 s ...)*, so there is time to press it. Before the game starts it asks **Players on this PC**: press 2, 3 or 4 there (1, N or no key in 15 seconds: just you, one game). A key pressed by mistake never starts more games: only the answer does. A start with one player asks nothing. **The simple way, with no key at the start:** start Squadala as usual, then in the game open the **−** menu, **Multiplayer**, **Local co-op** and press **Start the games** (below, "From the game"). After you answered 2 to 4, the next start asks again by itself; answer 1 to play alone again.
3. **The games start one after another.** Game 1 is your kit's own game, with your saves; it **hosts** the session itself (only the games of this PC can join it). Games 2 to 4 join it by themselves. Each player loads their save, or presses **Continue**, as in any join: players 2 to 4 get the host's save when they join. Their own files stay in `hookshot\game\local-players\player2` to `player4` (saves, logs, name, controller and look).
4. **Controllers:** game 1 reads controller 1 and the keyboard and mouse (when its window is in front); game 2 reads controller 2, game 3 controller 3, game 4 controller 4, each also while another window is in front. A game that reads the wrong one: its **Multiplayer** tab, **Settings > Controller** (it applies at once and is kept for that player).
5. **Names and colours:** players 2 to 4 are "Player 2" to "Player 4" until they type a name in their game; each wears the colour of their player number until they pick one in **Customize**.
6. **Frame rate:** every game keeps your own frame cap (uncapped, as a single game). If the windows that are not in front look choppy (Windows gives the window in front the bigger share of the PC, which a weak PC feels), set `local_fps = 30` (or `60`) in the kit's `coop-options.ini`: every game then runs at that same steady frame rate.
7. **Settings** in the kit's `coop-options.ini` and those changed in any game's **Settings** count for every game (each player's controller and look are their own, in their game's `coop-player.ini`).
8. **To stop**, close game 1: the other games get "the host left" and can be closed. `scripts\collect-logs.cmd` takes every game's log.

**From the game, with no restart:** in a game started alone, open the **−** menu, **Multiplayer**, **Local co-op**. Pick **Games on this PC** (2, 3 or 4, this game included) and press **Start the games**: this game hosts the session (this PC's games only) and the other games open in their own windows beside it, each its own player (controller 2 to 4, its own colour and name, its own folder as above) and join it. This game reads controller 1 and the keyboard from then on (unless you chose another controller in Settings). The page then says **Players in: 2 of 3** and what the start did ("Did not start: player 3." with the reason in the Start row's help). A game that did not start, or that a player closed: **Start missing games** on the same page starts only the missing ones into the same session (a game closed by mistake comes back with its player's save, controller, colour and name). Raise the count later the same way. Game 1 closed while the others stay open: start it again and press **Start the games**: it hosts the same session again, and the others join it from their **Multiplayer** tab (or by themselves with Auto-rejoin on). The page is not offered in a friend's game or one you host online (leave it first), and not on Linux. The games started this way keep game 1's own frame cap; for the same steady `local_fps` in every game, start them from `Start Squadala.bat` as in step 2.

Friends on other PCs cannot join this session (it is this PC's only). To play with them too, host from game 1's **Multiplayer** tab instead (with 1 player on this PC), or use Nucleus Co-op below.

### By hand, with two kits

The way before 0.12.0 still works: two kits, each started once, each game with its own controller (`LICHII_COOP_PAD=1` in kit 1's `coop-options.ini`, `LICHII_COOP_PAD=2` or `none` in kit 2's, or the **Multiplayer** tab's **Settings > Controller** row), **Host** then **Same network** in kit 1's game (it copies one line), **Join** in kit 2's (the line is filled in). `LICHII_COOP_KEYBOARD=off` keeps the keyboard out of a controller game; `always` makes a game read the keys even behind the other window (no mouse).

Each game's log `longshot_log.txt` (in the `logs` folder of its game folder) says what it chose: `hid: this game's controller: one pad (1)` and `coop: controller 1, keyboard focus`. The default (no line, or `LICHII_COOP_PAD=auto`) is the game's own way: every controller, in the window in front only. A log line `this Hookshot cannot choose a controller` means that kit's game is older than the kit: start `Start Squadala.bat` again and let it update the game.

## Split screen (experimental)

[Nucleus Co-op](https://github.com/SplitScreen-Me/splitscreenme-nucleus) is a free Windows tool. It does what the section above does by hand, in one click, for 2 to 4 players: it starts one copy of the game per player and places the windows to fill the screen (or several screens). Player 1's game hosts on this PC, and the others join it. Longshot only.

Split screen is **experimental** in this release: each player runs a **full game**. Each copy takes about 10 GB of RAM, so 2 players need about 20 GB free and 4 need about 40 GB, and a strong graphics card, with its memory for each copy. Without Nucleus, `Start Squadala.bat` does the same from one kit ([above](#two-to-four-players-on-one-pc)); Nucleus adds friends on other PCs joining the same session. Native split screen (one game, one world, every player's view in it; [local-coop.md](local-coop.md)) comes in a later release.

1. **Once:** install Nucleus and our handler as [tools/nucleus/README.md](../tools/nucleus/README.md) says (unzip Nucleus, copy `OoTCoop.js` from the kit's `nucleus` folder into Nucleus's `handlers` folder, then **+ / Add game** and pick the `longshot.exe` of your kit's co-op game; the handler is not on Nucleus's handler hub). After that, each `Build-Hookshot-Coop.bat` and `Update.bat` puts the kit's handler over that copy. The co-op game must be built from this release or a later one. With an older one, every copy reads the same controller: run `Build-Hookshot-Coop.bat` again.
2. **Plug in the controllers**, start Nucleus and choose **OoT Co-op (Longshot)**. Drag each controller (and the keyboard, for a keyboard-and-mouse player) onto a part of the screen, give each player a name. Under **This PC**, keep **Host on this PC**, or choose **Join a game on this network** when another PC on your network hosts ([below](#join-a-game-on-this-network)). Nucleus keeps the choice in a saved profile. Then press **Play**.
3. **The first time**, Windows asks to let Nucleus make changes. Nucleus makes the links to the game's files as administrator. Later starts do not ask.
4. The games start one after another, about 15 seconds apart. With **Host on this PC**, player 1's start also opens the server in a window of its own (below). Each game on this PC joins it by itself: the players on this PC need nothing else. Player 1 hosts and plays their own saves. Players 2 to 4 get the host's save when they join, and their own files stay in `nucleus-players\player2` to `player4` beside the game.
5. Each controller works in its own window, even while another window is in front. The keyboard and mouse go to the keyboard player's window when it is in front: click it to play there. If a game reads the wrong controller, choose the right one on its **Multiplayer** tab, under **Settings > Controller** (it applies at once).
6. **To stop**, end the session in Nucleus. It closes the games and the server. It closes **every** `coop_server.exe` on this PC, so do not host with `Host.bat` or `start-server.cmd` at the same time. If one of those (or a session not ended) is still running when you press **Play**, nothing is started and the handler says so: this PC's games would join that old session as guests (with whoever took over as host there) instead of hosting. Close that server window, end the session and press **Play** again.

### The server and friends on other PCs

Player 1's start runs the kit's own `start-server.cmd`, as `Host.bat` does:

- The server listens on every address of this PC, on the kit's port (`port` in the kit's `coop.ini`, else 27510), with the kit's join password (`server_password` in `coop-options.ini`, else a new one for this session). `server_bind` and `server_allow` in `coop-options.ini` count too; the players on this PC are always let in.
- It asks your router to forward the port (UPnP; `upnp = no` turns it off) and shows an **invite code** in its window a few seconds later, already copied to the clipboard. Send it to your friends in DMs only: it holds your internet address and the join password.
- Each friend on another PC opens **Join** in the Multiplayer tab of their own game, pastes the code (it is filled in already when it is on their clipboard) and chooses **Join** ([play-online.md](play-online.md#in-the-game-host-and-an-invite-code-the-longshot-kit)).
- The players on this PC join by themselves at 127.0.0.1.
- The first time, Windows may ask to let `coop_server.exe` through its firewall: allow it ([Firewall](#firewall) above). The Nucleus windows may cover the prompt and the server window: press Alt+Tab to find them.

### Two PCs, two or more players on each (Join a game on another PC)

PC 1 uses Nucleus with **This PC: Host on this PC** (above) and its server window shows the invite code. On **PC 2**, Nucleus can join that host too, with every game of PC 2:

1. On PC 2, run `Update.bat` of the newer kit, or copy `OoTCoop.js` from its `nucleus` folder into Nucleus's `handlers` folder (the handler of 0.11.7 or older has no such choice).
2. Put the host's invite code (or, for a VPN or the same network, its line `address:port password`) as the **first line** of `nucleus-players\join.txt` beside the game (make the file; the folder appears at the first start), or just copy it: with no `join.txt`, the handler reads the clipboard. Nucleus's options are lists of choices, so there is no box to type it in.
3. Under **This PC** choose **Join a game on another PC**, set up the players and press **Play**. No server is started on PC 2: every game there writes its `coop.ini` to join the host directly, with the host's join password.
4. A code the handler cannot read (a letter missing, an answer code, a code from a newer version) is told to you in a message box (and Nucleus's log) and nothing is started (the first game's start is abandoned with an error, no game opens); fix `join.txt` and press **Play** again.

Ending the session closes only the games: this mode closes no `coop_server.exe`, so a server of your own on PC 2 keeps running. The games connect straight to the host's address from the code, with no join helper (the helper window of `Join.bat` serves one game only). That works when the host's router forwards the port (UPnP) or you are on a VPN or the same network (use the host's **Same network or VPN** line then); if the answer-code way through the routers is the only one that works for you, play one game per PC with the game's own **Join** instead.

**"Co-op is already running" when starting a second game by hand:** `play.ps1` refuses a second copy of the same kit's exe. Nucleus makes one folder per player, so this mode does not meet it; by hand, use a second kit folder (as above).

The game windows start in front of the server window. To see the invite code again, Alt+Tab to the window named **Squadala server**, or paste the clipboard right after the start.

### Join a game on this network

With **This PC: Join a game on this network**, nobody on this PC starts a server. Each game joins the first game hosted on your network that it can join, with no password: a host lets its own network in. To join one host only, put `join_lan_host = <host's name>` in the kit's `coop-options.ini`. With no host found after 60 seconds, the game says so: pick one in the pause menu's Lobby tab, **Find a game**. Saves, controllers and the 15 seconds between starts are as above. Ending the session closes only the games: this mode closes no `coop_server.exe`, so a server of your own on this PC keeps running.

### Join a game on another PC

With **This PC: Join a game on another PC**, nobody on this PC starts a server and every game joins one host elsewhere. Copy the host's invite code (or its Same network / VPN line, `address:port password`), or put it as the first line of `nucleus-players\join.txt` beside the game, then press Play. Each game connects directly to the host's address, so the host needs a router forward, a VPN or the same network. A missing or wrong code starts nothing and says why.

## Something does not work

See the troubleshooting list in [play-online.md](play-online.md#troubleshooting); it applies here too.
