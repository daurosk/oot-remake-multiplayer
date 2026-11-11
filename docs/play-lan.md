# Playing on the same network (LAN)

For players on the same home network (the same router, wired or Wi-Fi). Every player has done [setup.md](setup.md) first, with the same release.

One player is the **host**: their PC runs the server, and they play on it too. The others **join**.

## Host

1. Double-click `Host.bat`. The first time it asks for your player name.
2. A server window opens. It says `listening on port 27510` and lists this PC's addresses, for example:

   ```
   Co-op server on UDP port 27510. Players on other PCs use one of these addresses:
     192.168.1.20     (Ethernet)
   ```

   Keep that window open while you play; closing it ends the session for everyone.
3. Tell the others your address: the one that starts with `192.168.` or `10.` (or `172.16.` to `172.31.`). If you are not sure, open a Command Prompt, type `ipconfig` and use the **IPv4 Address** of the adapter you use (Ethernet or Wi-Fi).
4. The first time, Windows Firewall asks about `coop_server.exe`: tick **Private networks** and click **Allow access**. If you missed it, see [Firewall](#firewall).
5. Start the game in the emulator and load your save.

## Join

1. Double-click `Join.bat`.
2. Type the host's address (for example `192.168.1.20`) and press Enter. Next time Enter keeps the last address. The first time it also asks for your player name; use a different one from the others.
3. Start the game and load your save.

In the server window you see `player 2 joined: <name>`, and in each game the panel in the top-left corner lists the other players with their distance. In the same area, each player sees the others as Link.

## Firewall

Only the host needs this, and only UDP port 27510 (or the port in `emulator\user\config\coop.ini`). Joining players connect out and need nothing.

If the others cannot connect, allow the server by hand: open **Command Prompt as administrator** on the host, go to your kit folder (the one with `Host.bat`; put in your own path) and add the rule:

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

For testing: unzip the kit twice into two folders, do [setup.md](setup.md) in each, then `Host.bat` in one and `Join.bat` with `127.0.0.1` in the other. The keyboard goes only to the window that has focus.

## Something does not work

See the troubleshooting list in [play-online.md](play-online.md#troubleshooting); it applies here too.
