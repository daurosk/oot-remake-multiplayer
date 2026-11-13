# Playing on the same network (LAN)

For players in the same house: the same router, wired or Wi-Fi. Everyone has done [setup.md](setup.md) first, and everyone has the **same release**.

One player is the **host**: their PC runs the server, and they play on it too. The others **join**.

In the same house you join with the host's **address** and the **join password**, not with the invite code. (The invite code holds the host's internet address, which often does not work from inside the same house.)

- **On the emulator kit:** [Host](#host) and [Join](#join) below.
- **On Hookshot:** [On Hookshot's title screen](#on-hookshots-title-screen).

## Host

On the emulator kit:

1. Double-click `Host.bat`. The first time it asks for your player name.
2. A server window opens with a box that starts **Give your friends ONE of these**, for example:

   ```
   Give your friends ONE of these (they type it in Join.bat):

     192.168.1.20          Same house (friends on your home network or Wi-Fi)
   ```

3. Tell the others two things from that window:
   - the **Same house** address (for example `192.168.1.20`);
   - the line **Join password for this session**.
4. The first time, Windows Firewall asks about `coop_server.exe`: tick **Private networks** and click **Allow access**. If you missed it, see [Firewall](#firewall).
5. Start the game and load your save.

Keep the server window open while you play. Closing it ends the session for everyone.

`Host.bat` makes a new join password each time it starts. To keep the same one, set `server_password` in `coop-options.ini` ([Who can join](play-online.md#who-can-join)).

## Join

On the emulator kit:

1. Double-click `Join.bat`.
2. Type the host's address (for example `192.168.1.20`) and press Enter.
3. Type the join password the host gave you and press Enter.
4. The first time it also asks for your player name. Use a different one from the others.
5. Start the game. On the title screen choose **Continue** to play from the host's save.

Next time, Enter keeps the last address and password.

**What you should see:** `Join.bat` says when you are connected, or what is most likely wrong. The host's server window says `player 2 joined: <name>`. In the same area of the game, you see each other as Link with a name tag.

## On Hookshot's title screen

On Hookshot you host and join on the game's title screen ([all the steps](play-online.md#host-and-join-on-hookshots-title-screen)). Everyone starts the game with `Play-OoT-Coop.bat`. In the same house, or on a VPN:

1. **The host** chooses **Host** on the title screen, then **Same network or VPN**.
2. The game shows one line, for example `192.168.1.20:27510 abcdefghijklmnopqrst`: this PC's address and port, a space, and the join password. It is already copied. With a VPN program running, the VPN address comes first (Radmin VPN `26.x.x.x`, Hamachi `25.x.x.x`, Tailscale `100.x.x.x`).
3. The host pastes that line to the friends (Ctrl+V). It holds the join password, so keep it among friends.
4. **Each friend** chooses **Join** on the title screen, pastes the line where the invite code goes (**A** on the field, or Ctrl+V), then **OK**. Nothing else to type.
   - A friend on the emulator kit uses `Join.bat` instead and gives it the two parts one after the other: first the address with its port (`192.168.1.20:27510`), then, when it asks for the join password, the second part.
5. If Windows asks about the firewall on the host's PC, allow the game on private networks.

**Copy the code again** on the Host screen brings the invite code back, for friends over the internet.

## Firewall

Only the host needs this, and only for UDP port 27510. Players who join connect out and need nothing.

**The easy way:** if the others cannot connect, the host runs `check-hosting.cmd` in the kit folder while the server window is open. It checks the server and the firewall and prints the exact command that fixes what is wrong. It changes nothing itself.

<details>
<summary>By hand: add the firewall rule yourself</summary>

Open **Command Prompt as administrator** on the host's PC, go to your kit folder (the one with `Host.bat`; put in your own path) and add the rule:

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

If your server uses another port than 27510 (the server window's first line names it), use that number in the rule.

</details>

### When you are done

The server opens nothing by itself: when its window is closed, nothing listens. A rule you added by hand stays until you delete it, so remove it when you stop playing for a while (Command Prompt as administrator):

```
netsh advfirewall firewall delete rule name="OoT co-op server UDP 27510"
```

## Two players on one PC

For testing: unzip the kit twice into two folders and do [setup.md](setup.md) in each. Then start `Host.bat` in one, and `Join.bat` with the address `127.0.0.1` in the other. The keyboard goes only to the window that has focus.

## Something does not work

See the troubleshooting table in [play-online.md](play-online.md#troubleshooting). It applies here too.
