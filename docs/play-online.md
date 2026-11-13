# Playing over the internet

For friends in different places. Everyone has done [setup.md](setup.md) first, and everyone has the **same release**.

One player is the **host**: their PC runs the server, and they play on it too. The others **join**. In the same house? [play-lan.md](play-lan.md) is shorter.

**On this page:**

- [The easy way: an invite code](#the-easy-way-hostbat-and-an-invite-code) (start here on the emulator kit)
- [Host and Join on Hookshot's title screen](#host-and-join-on-hookshots-title-screen) (start here on Hookshot)
- [When the invite code does not connect](#when-the-invite-code-does-not-connect)
- [A free VPN network](#when-the-invite-code-does-not-work-a-free-vpn-network), when nothing else works
- [Your colour](#your-colour), [PvP](#pvp-players-hurting-each-other), [who can join](#who-can-join)
- [Troubleshooting](#troubleshooting)
- Advanced: [port forwarding](#without-a-vpn-port-forwarding), [a relay](#advanced-relay-with-join-codes)

## The easy way: Host.bat and an invite code

No router settings, no VPN, no address or password to type. These are the steps for the emulator kit. On Hookshot the same thing is on the game's title screen: [Host and Join on Hookshot's title screen](#host-and-join-on-hookshots-title-screen).

**The host:**

1. Double-click `Host.bat`.
2. A server window opens. A few seconds later it shows `invite code: XXXXX-XXXXX-...`. The code is already copied.
3. Paste the code to your friends in a **private** message (Ctrl+V), not in a public channel.
4. Start the game and load your save. Keep the server window open while you play: closing it ends the session for everyone.

The first time, Windows Firewall may ask about `coop_server.exe`: click **Allow access**.

**Each friend:**

1. Double-click `Join.bat`.
2. Paste the invite code (right-click or Ctrl+V) and press Enter.
3. A second window opens, the **join helper**. Keep it open while you play.
4. Wait for the helper to say `the host's server answered directly`. The game then connects by itself.
5. On the title screen choose **Continue** to play from the host's save. (The chat says "The host's save is ready".)

**What you should see:** the host's server window says `player 2 joined: <name>`. In the game you see each other as Link with a name tag.

**If the helper shows an ANSWER CODE instead** (step 4), the routers need one more step:

1. The friend sends the answer code back to the host. It is already copied.
2. The host clicks the server window, pastes the code (right-click pastes) and presses Enter.
3. Within a few seconds the helper says `the way through both routers is open`, and the game connects.

**Good to know:**

- **Keep the invite code among friends.** It holds your internet address and the join password. Anyone who has it can join while your server runs.
- **The code stops working when the server window closes.** `Host.bat` makes a new one each time it starts.
- **The host's game closed or crashed?** Leave the server window open and start `Host.bat` (or `Host-Hookshot.bat`) again. It says `Your server is still running: joining that session again as the host` and starts only the game. Your friends stay connected.
- **Emulator and Hookshot players can be in the same session.** A friend on Hookshot pastes your code into **Join** on their title screen; a friend on the emulator pastes a Hookshot host's code into `Join.bat`.

<details>
<summary>What Host.bat does by itself, and privacy</summary>

- It makes a **new random join password** each time it starts and puts it in the invite code. If you set your own `server_password` in `coop-options.ini`, that one is used instead. Every session has a join password: the server does not start without one.
- With the join password the **game traffic is encrypted**. Someone on the path (a shared Wi-Fi, a VPN, a relay) can neither read nor change it. There is no plain mode.
- It asks your router to **forward the game's UDP port** (UPnP) while the server runs; see [Let the router forward the port](#let-the-router-forward-the-port-upnp). Many routers have UPnP off. The invite code then still works for most friends, through the answer code.
- It asks a free public **STUN server** (Google's `stun.l.google.com`, then Cloudflare's `stun.cloudflare.com`) which internet address and port your router gives the game's port. That address goes into the invite code. The STUN server sees your internet address (as every website you visit does) and nothing of the game. `invite = no` in `coop-options.ini` turns this off; `stun_servers = name:port, ...` picks other ones.
- The invite code is never written to the server's log or into `collect-logs` output.
- For the answer-code way, Windows Firewall needs nothing more. If friends should connect directly through a UPnP forward, allow `coop_server.exe` for the network you are on, or run the rule `check-hosting.cmd` prints.

How the codes are built and checked: [invite-design.md](invite-design.md).

</details>

### When the invite code does not connect

| What you see | What to do |
|---|---|
| Join: `Not used: ...` after pasting the code | A letter is wrong or missing. Ask the host to copy the code again from the server window. |
| Host's server window: `answer code not used: ...` | The line says why: a letter wrong or missing, an answer older than 10 minutes, or one made from an old invite code. The friend closes the join helper and starts `Join.bat` again to get a new answer code. |
| The helper waits, then after 3 minutes says **No way through the routers was found** | Both routers block this way (it happens on some mobile, dorm and shared connections). Use a [free VPN network](#when-the-invite-code-does-not-work-a-free-vpn-network), which always works. |
| Host's server window: `no invite code: no STUN server answered` | The host's PC has no internet, or a firewall blocks it. Check the connection; otherwise use a VPN network. |
| Host's server window: `your router gives every address its own port (symmetric NAT)` | The answer-code way will probably not work for this host. Use a VPN network. |
| `refused: wrong protocol version` | You do not all have the same release. Everyone runs `Update.bat`, the host too. |
| Friends in the same house cannot join with the code | The code holds your internet address, which often does not work from inside the same house. See [play-lan.md](play-lan.md). |

Still stuck? The host runs `check-hosting.cmd` in the kit folder while the server window is open. It says what is wrong and prints the command that fixes it. It changes nothing by itself.

## Host and Join on Hookshot's title screen

On Hookshot you host and join from inside the game: no server window, no helper window, nothing to type.

Start the game with `Play-OoT-Coop.bat` (or the **Play OoT Co-op** desktop shortcut). Its title screen has **Host**, **Join** and **Co-op Settings** rows under the game's own. If you see a **(-) Multiplayer** prompt at the bottom left instead, press **Minus** to open the same pages.

**Host**

1. Choose **Host**. The server starts inside the game, and the screen says "Send this code to your friends:" with the invite code. It is already copied.
2. Paste the code to your friends in a private message.
3. The screen says "Waiting for your friends to join...", then how many joined. Choose **OK** and play.

The Host screen has four rows:

| Row | What it is for |
|---|---|
| **OK** | Close the screen and play |
| **Copy the code again** | Puts the invite code back on the clipboard |
| **Paste friend's answer** | When a friend's game shows an answer code: copy it from their message, then choose this |
| **Same network or VPN** | For friends in the same house or on a VPN: copies one line with this PC's address and the join password, which they paste into Join ([play-lan.md](play-lan.md#on-hookshots-title-screen)) |

One invite code works for the whole time you host.

**Join**

1. Copy the code from your friend's message.
2. Choose **Join**. The code is already filled in when it was on the clipboard. Otherwise press **A** on "Your friend's code" (or Ctrl+V) to paste it.
3. Choose **OK**. The screen says "Joining your friend...", then "Connected! Choose your file and play."
4. The screen says "Press Continue to play (the host's name)'s save." Choose **Continue**.

Dropped out? Choose **Join** again: the first row is **Join (the host's name) again**. No new code is needed while your friend keeps hosting.

**What the screen may say:**

| Message | What to do |
|---|---|
| "That invite code is not right; ask the host to copy it again." | Ask for the code again and paste the whole of it |
| "Your friend's router blocks the way in." with an answer code | Send the answer code to the host. The host chooses **Paste friend's answer**. |
| "Your friend could not be reached." | Check that they are hosting right now and that the code is their newest one. Same house or VPN? Ask for the line from **Same network or VPN**. |
| "No code could be made: your internet connection blocks it." (host) | Friends on the same home network or a VPN can still join with **Same network or VPN** |
| "Your router may block friends..." (host) | If friends cannot join, turn on UPnP in the router's settings, or use the answer code |
| "Another co-op window is already open on this PC." | Close the other co-op game or server window (or restart the PC), then try again |
| "Co-op is on already in this game: quit the game to host or join another." | This game is in a session already (it was started by `Host-Hookshot.bat` or `Join-Hookshot.bat`, or you already hosted or joined). Quit the game and start it with `Play-OoT-Coop.bat`. |

## When the invite code does not work: a free VPN network

1. **Everyone** installs [Radmin VPN](https://www.radmin-vpn.com) (free, no account). The host clicks **Network > Create network**, picks a name and a password, and sends both to the others; the others click **Network > Join network** and type them in. ([Hamachi](#hamachi) works the same way.)
2. **The host** double-clicks `Host.bat`. The server window shows **Give your friends ONE of these**, with the **Over Radmin VPN** address (`26.x.x.x`) first, and has already copied it (the invite code replaces that copy a few seconds later; copy the address from the window then). Send it with the join password: the window shows it as `Join password for this session: ...` (only in the window, never in the log), or set your own `server_password`.
3. **The others** double-click `Join.bat`, paste that address and press Enter. Join.bat says when you are connected.

Nobody touches a router. Only people with the VPN network's password can reach the server.

**If the others cannot connect,** the host runs `check-hosting.cmd` in the kit folder while the server window is open. It prints exactly what is wrong and the command that fixes it (usually one firewall line for Radmin addresses). It changes nothing by itself.

**Hosting from Hookshot's title screen?** The Host screen's **Same network or VPN** row copies one line with your VPN address and the join password; friends paste it into **Join**. Steps: [play-lan.md](play-lan.md#on-hookshots-title-screen).

## Without a VPN: port forwarding

**Advanced.** Most players do not need this: the invite code or a VPN network is easier and safer. Only if the host can change their router and has a public IP address (no CGNAT: your router's WAN address is the one [api.ipify.org](https://api.ipify.org) shows and does not start with `100.64.` to `100.127.`). Anyone on the internet who learns your address can try to connect, so do the safety steps too.

**Host:**

1. Start `Host.bat` once and read the server window: the **Same house** line is your PC's address (for example `192.168.1.20`), the **Over the internet** line your public address.
2. On your router's settings page (often `192.168.1.1`; the address and password are usually on a sticker on the router), under **Port forwarding**, add: protocol **UDP**, port **27510**, to your PC's Same-house address.
3. **Safety:** ask the others for their public address (they open [api.ipify.org](https://api.ipify.org)) and put them in `coop-options.ini` beside `Host.bat`: remove the `#` in front of `server_allow` and write, for example, `server_allow = 81.2.3.4,82.5.6.7`. Then nobody else can join ([Who can join](#who-can-join)). Keep the **join password** on (Host.bat makes one each time unless you set `server_password`; the server window shows it): give it to the others with the address, or send the invite code. Their addresses can change; the password still keeps strangers out when you have to widen `server_allow`.
4. Run `check-hosting.cmd` while the server window is open and run the firewall command it prints under **Over the internet with port forwarding** (in Command Prompt as administrator). It allows only `coop_server.exe`, and with `server_allow` set only those addresses. It applies on networks Windows calls Private: if the check says your home network is Public, switch it to Private as it says.
5. Close and start `Host.bat` again, and send the **Over the internet** address and the join password to the others.

**Others:** double-click `Join.bat` and paste the host's address.

**When you stop playing:** delete the port forward on the router and the firewall rule ([When you are done](#when-you-are-done)). Your public address can change from time to time: read it again in the server window before each session.

### Let the router forward the port (UPnP)

Instead of step 2, the server asks the router itself: Host.bat does this by default (`upnp = no` in `coop-options.ini` turns it off). It maps only the game's UDP port, only to this PC, with a 10-minute lease renewed while the server runs, and removes it when the server stops or its window closes. Every session has a join password, which is what keeps strangers out of the open port (`server_allow` alone would not be enough: it opens your PC's port to the whole internet while the server runs). The server window then says in plain words what happened, and the server log keeps the line:

- `UPnP: the router now forwards UDP port 27510 to this PC (192.168.1.20) for 10 minutes at a time, renewed while the server runs` and your internet address for the others. The forward is removed when the server stops or its window closes; after a crash the router drops it within 10 minutes. Some routers only take a permanent forward: then the window says `until the server stops` and, because after a crash such a forward stays until you remove it by hand, to remove it in the router's page if the server crashes. A renewal asks the router while the server runs; a router that does not answer holds the server up for at most a second each time (then it tries again a minute later).
- `no router answered`: UPnP is off on the router (many have it off, some players turn it off on purpose: Host.bat never turns it on for you). Forward the port by hand (step 2) or use a VPN.
- `the router's own internet address is 100.x / 192.168.x / 10.x`: your router is behind your provider's (CGNAT) or another router; no forward on it can let anyone in. The invite code may still get through with an answer code; otherwise use a VPN.
- `the router already forwards UDP port 27510 to another device`: an old forward by hand; remove it on the router's page.

The firewall rule (step 4) is still needed, and stays your choice: the kit prints it and never adds it.

## Advanced: relay with join codes

Off, and not needed for most players: the invite code above does the same without a third PC. For hosts who cannot open a port at all (CGNAT, a dorm or a phone hotspot) and players who do not want a VPN. Someone whose UDP port is reachable from the internet runs a **relay**: a friend who has forwarded a port, or a small rented server. Nothing is hosted by this project and no other service is used.

1. **Relay owner:** run `start-relay.cmd` in a kit (or `coop_server --relay` on any PC with the server). It listens on UDP port **27530**: forward that port and allow `coop_server.exe` in the firewall as in [Without a VPN](#without-a-vpn-port-forwarding). Give the hosts your address. Optional: `start-relay.cmd --relay-key-env NAME` with a host key in that environment variable, so only hosts with the key can use it.
2. **Host:** in `coop-options.ini` remove the `#` in front of `relay =` and write the relay's address (`relay = relay.example.net:27530`; `relay_key =` if the owner gave you one). Start `Host.bat`. A few seconds later the server window shows `relay: join code ABCD-EFGH. Give your friends ABCD-EFGH@relay.example.net:27530` and copies it. Your router needs no change: the server connects out to the relay. A **join password is required** (`server_password`): without one Host.bat does not use the relay and says so. The join code only finds your server; the password is the lock. `server_allow` does not help here, because every guest through the relay reaches your server from your own PC (127.0.0.1).
3. **Others:** double-click `Join.bat` and paste the whole `ABCD-EFGH@relay...` text. Join.bat sends the code to the relay, says `Join code accepted`, and the game connects through the relay. Next time Enter offers it again; the code changes each time the host starts the server.

**Trade-offs:** the relay owner sees every player's address and when and how much you send. The game traffic itself is encrypted with the join password, so the relay cannot read or change it (docs/protocol.md, "Encrypted traffic"). Each packet takes the detour through the relay, so the delay is the host-relay plus relay-player delay. Pick a relay near all of you, run by someone you trust. All relayed players share one address on the host's server (127.0.0.1), so its per-address limits are shared too: when one of them types a wrong join password 5 times, every new relayed player is turned away for 60 seconds (players already in stay in), and at most 4 can be connecting at the same moment.

How the relay works inside: [invite-design.md](invite-design.md#how-the-relay-works).

## Radmin VPN step by step (and ZeroTier, Tailscale)


Radmin VPN is free and needs no account. ZeroTier and Tailscale work the same way (see below).

**Everyone:**

1. Install Radmin VPN from [radmin-vpn.com](https://www.radmin-vpn.com) and start it.
2. **The host:** click **Network > Create network**, choose a network name and a password, and send both to the others.
3. **The others:** click **Network > Join network** and enter that name and password.
4. Everyone now sees each other in the Radmin VPN window. Each PC has a Radmin address that starts with `26.` (shown at the top of the window, for example `26.12.34.56`).

**Host:** double-click `Host.bat` and send the others the **Over Radmin VPN** address (`26.x.x.x`) from the server window; it is already copied. If they cannot connect, run `check-hosting.cmd`, or allow the server in the firewall for Radmin addresses only by hand. Windows treats the Radmin VPN adapter as a Public network, so this rule uses `profile=any`, kept narrow by `program=` (only the co-op server) and `remoteip=` (only Radmin addresses). In **Command Prompt as administrator**, in your kit folder (put in your own path):

```
cd /d "C:\Games\oot-coop"
netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow program="%CD%\server\coop_server.exe" protocol=UDP localport=27510 profile=any remoteip=26.0.0.0/8
```

For ZeroTier use your network's range instead of `26.0.0.0/8` (my.zerotier.com shows it), for Tailscale `100.64.0.0/10`.

**Others:** double-click `Join.bat` and paste the host's `26.x.x.x` address.

**ZeroTier:** everyone installs ZeroTier and joins the same network ID; the network's owner approves each PC on my.zerotier.com. Use the host's ZeroTier address (the one in the network's member list).

**Tailscale:** everyone signs in to the same tailnet (or the host shares their PC with the others). Use the host's Tailscale address, `100.x.y.z` (the Tailscale window, or `tailscale ip -4`).

### Hamachi

Hamachi (LogMeIn) works the same way as Radmin VPN. Its addresses start with `25.`.

1. Everyone installs Hamachi and joins the same network. The host creates the network and sends its ID and password to the others.
2. **The host:** your Hamachi IPv4 address is at the top of the Hamachi window (for example `25.12.34.56`). Use the IPv4 address, not the IPv6 one. Double-click `Host.bat`, keep the server window open, and send that `25.x.x.x` address to the others.
3. **The others:** double-click `Join.bat` and paste the host's `25.x.x.x` address.

**Firewall (host only).** Windows treats the Hamachi adapter as a **Public** network. The server listens on **UDP port 27510** (or the port in `emulator\user\config\coop.ini`), so `coop_server.exe` must be allowed on Public networks, for Hamachi addresses only. Only the server has to be allowed: the emulator only connects out, on the host's PC too. Either:

- Open **Command Prompt as administrator** in your kit folder (put in your own path) and allow only the co-op server, only from Hamachi addresses (`25.x.x.x`):

  ```
  cd /d "C:\Games\oot-coop"
  netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow program="%CD%\server\coop_server.exe" protocol=UDP localport=27510 profile=any remoteip=25.0.0.0/8
  ```

- Ticking **Public networks** when Windows Firewall asks about `coop_server.exe` also works, but then the server can be reached from any Public network your PC is on (a café or hotel Wi-Fi too). Prefer the rule above.

Only join Hamachi networks of people you know: everyone in the network can reach your server.

**Check that it works.** When someone joins, the host's server window shows `player 2 joined: <name>`. The joining player's log (`emulator\user\log\eden_log.txt`) has `coop: welcomed as player 2 on 25.x.x.x:27510`. If the joiner's panel stays on `connecting`, see Troubleshooting below. Check first that both of you are shown as online (green) in the Hamachi window.

A virtual LAN adds a little delay when it cannot connect the PCs directly; the ping in the game's co-op panel shows how much.

**Dropped every 15-20 seconds?** Older kits sent packets too big for some VPNs and tunnels (Hamachi, Tailscale, DS-Lite internet lines, a VPN inside another VPN): small things got through, but the big ones (the shared flags sent when you join) were lost, and the game was cut after 10 s and joined again, over and over. The packets now fit Hamachi and lines down to 1200 bytes, and a game whose line still loses them switches by itself to the smallest size once (the host's server window shows `its path lost our 1100-byte datagrams ...; told it to connect again with 576-byte ones`, one short reconnect) and then stays in. Both players need the same, current kit. Still dropped? Run `collect-logs.cmd` on both PCs and send the zips.

## Who can join

By default, anyone who has the join password (it is inside the invite code) and can reach the server's port may join. Three lines in the host's `coop-options.ini` (beside `Host.bat`) narrow that:

- `server_password = ...`: your own **join password**. Without this line (or with nothing after the `=`), `Host.bat` makes a new random one each time and puts it in the invite code; the server window shows it too.
  - Set your own to keep the same one every time. No spaces, at most 128 characters. Pick a long one (12 characters or more; three random words joined by dashes work) that you use nowhere else.
  - Friends who join by address (not by invite code) type it when `Join.bat` asks. Enter keeps the last one.
  - A wrong or missing password is turned away, and the `Join.bat` window says `wrong join password`. An address that keeps guessing wrong is shut out for a minute.
  - The password itself never goes over the network, only a proof made from it.
  - To change it, change the line and start `Host.bat` again.
- `server_allow = 25.1.2.3,25.4.5.6`: only these addresses may connect; anyone else is turned away at once. Use the others' Hamachi (`25.`), Radmin (`26.`) or Tailscale (`100.`) addresses, or with port forwarding their public addresses (api.ipify.org on their PC). A whole range: `26.0.0.0/8`.
- `server_bind = 25.1.2.3`: the server listens only on this address of your own PC (your Hamachi or Radmin address), so it cannot be reached through your home network or Wi-Fi at all.

**A password is not enough on its own.** Use it together with a VPN (Radmin, Hamachi, Tailscale: only people in your network can reach the port at all) or, with port forwarding, together with `server_allow`. With port forwarding the port is open to the whole internet, and the password is then what keeps strangers out.

For `server_allow` and `server_bind`: remove the `#` in front of the line, put in the addresses and start `Host.bat` again. The server window then says `listening on port 27510 (only 25.1.2.3, ...)` and `only the N address range(s) given with --allow may connect`. A player who is turned away is counted as `not allowed` when the server stops. More in [SECURITY.md](../SECURITY.md).

## PvP: players hurting each other

Off unless the host turns it on. With PvP on, your sword, arrows, seeds and bombs hurt the other players' Links as they hurt enemies, and theirs hurt you; with it off, nothing changes.

- **The host decides, for everyone.** In the host's coop-options.ini: `pvp = on` (the line is `pvp = off` in a new kit; Host.bat passes it to the server). A guest's own `pvp` line does nothing, so nobody is hurt in a session whose host did not choose it. While playing, the host can type `/pvp on` or `/pvp off` in the chat; anyone can type `/pvp` to see it. The co-op panel shows "PvP on" while it is on, and the chat says when it changes.
- **Your own game decides whether you are hurt.** A hit that reached you on the other player's screen is checked by your game: nothing while you or the attacker are in a cutscene, a talk, a menu, paused or loading, nothing from a player in another area, and at most one hit every 0.8 s (Link blinks after a hit). So a player who lags cannot hurt you when you got away on your screen.
- **How much:** a sword hit takes half a heart, a seed or the boomerang a quarter, an arrow (fire and ice too) a heart, a light arrow or a bomb two hearts; a Deku nut stuns nobody and hurts nobody. The chat says who hit whom ("Saria hit you (-1/2 heart)", "you defeated Mido").
- **Not yet:** the sword is read where the area has the game's grass (outdoor areas); a sword swing indoors is not seen yet. A shot hits only when it ended (hit something or went off) beside the other player. Shown in the debug log as lines with `coop: pvp:`.

## Your colour

Every player's Link wears a tunic and cap in that player's colour, so you can tell each other apart. Your own Link wears your colour on your screen too.

**Without a choice:** the host is blue and everyone who joins is red. With three or more players, pick colours, or all the joiners wear red.

**To pick your colour:**

- **On Hookshot:** **Co-op Settings** on the title screen, **Your colour**, then **Save**.
- **In the file:** open `coop-options.ini` in the kit folder with Notepad. Find the line `# color = blue`, remove the `#` and write your colour. Close the game and start it again.

The colours: red, orange, yellow, green, cyan, blue, purple, pink, white, black. You can also write brown, gray, or a colour code such as `color = #FF8020`; you then get the nearest of the ten (brown looks black, gray looks white).

`Host.bat` and `Join.bat` say "Your Link: the (colour) tunic" when they start. A word the kit does not know is said in the window, and you keep your player number's colour. Two players may pick the same colour, so agree on yours.

**Good to know:**

- Only the green Kokiri tunic changes colour. The Goron and Zora tunics keep their own colours.
- Child Link is in full colour. **Adult Link keeps some green** ([issue 202](https://github.com/daurosk/oot-remake-multiplayer/issues/202)): others can see a touch of it at the back of the hat, and your own adult Link's hat back and skirt look green up close.
- Your colour may also tint a few things that share Link's textures (Mido, the bed in Link's house, the ocarina holder).
- The colours are in the puppet pack, which is made from your own game ([setup.md, step 4](setup.md#4-the-puppet-pack)). With a pack from before the colours, everyone is green: run `make-puppet-pack.cmd` again.
- `LICHII_COOP_TUNICS=0` in `coop-options.ini` turns the colours off for what you see.

## On Hookshot (experimental)

[Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0) runs the game as a normal Windows program instead of in the emulator. The kit builds it on your PC from your own game files. Setup, step by step: [setup.md, "Play on Hookshot"](setup.md#play-on-hookshot-experimental).

Online it works as the emulator kit does:

- Start the game with `Play-OoT-Coop.bat`, then choose **Host** or **Join** on the title screen ([steps](#host-and-join-on-hookshots-title-screen)).
- `Host-Hookshot.bat` and `Join-Hookshot.bat` host or join from a window instead, as `Host.bat` and `Join.bat` do.
- Hookshot and emulator players can join the same session and see each other as Link in their colours (checked by hand).
- The keys differ: chat is **T**, the player list **F6** ([Co-op on Hookshot](setup.md#co-op-on-hookshot)).
- To check the connection, the game's `hookshot_log.txt` (beside `hookshot.exe`) says `coop: module loaded`, then `coop: connected`.
- If the other players show as a fairy instead of Link, the game was built by an older release: run `Build-Hookshot-Coop.bat` again.

Hookshot's own problems (crashes outside Kokiri Forest on some PCs, crashes on some AMD cards) are listed on its page.

## When you are done

Our programs change no firewall rule. The router forward Host.bat asks for (UPnP) ends when the server window closes (after a crash, within 10 minutes; a router that only takes permanent forwards keeps it until you remove it on its page, and the server window says so). The invite code stops working when the window closes, and nothing listens then. What you opened by hand stays open until you close it:

- **The firewall rule:** Command Prompt as administrator, `netsh advfirewall firewall delete rule name="OoT co-op server UDP 27510"`.
- **The router's port forward** (if you made one): delete the UDP 27510 rule on the router's Port forwarding page.
- **The virtual LAN:** leave or close the Radmin VPN or Hamachi network when you are not playing.

Add them again the next time you host.

## Does shared progress stay in my save after I disconnect?

Yes, once your game has saved. Shared progress means the items, songs, chests, keys and pickups that other players shared with you. Co-op writes them into your game the same way your own pickups are written: through the game's own flag store, into the copy the game saves. So they behave like progress you made yourself:

- **What is written into your save:** shared key items and upgrades, songs, bottles (empty), maps, compasses, boss keys, small keys, Gold Skulltula tokens, heart pieces, and the chests and one-time pickups that are shared ([host.md](host.md), [items.md](items.md)). They go into the save file the next time **the game** saves (an autosave, for example after you get an item yourself, or when you save from the menu). Co-op never saves the game itself: receiving a shared item does not save by itself. If you close the emulator before the game has saved, the newest changes are lost, as in normal play.
- **Nothing is undone when you disconnect.** Leaving, losing the connection or the server closing takes nothing out of your game. You keep playing alone with everything you had, and the game saves it as usual.
- **What is not shared, so it is only ever yours:** rupees, ammo, life and magic right now, what is in your bottles, your equipped gear and item buttons ([items.md](items.md), "Rules").
- **While you are away**, you get nothing from the others. When you join again while the same server is still running, you get what they found in the meantime. You also keep what you found alone: the server merges both, and an item you have is never taken away.
- **The server keeps nothing on disk.** Its list of shared progress lives only while its window is open. When it is started again, it fills again from the running game of the first player who joins (the host), so progress lasts in the players' saves, not on the server.

## Troubleshooting

| What you see | What to check |
|---|---|
| The panel stays on `connecting` (Join.bat says "Not connected yet") | Wrong address: the host reads it again from the server window's "Give your friends ONE of these" box. Is the server window still open? The host runs `check-hosting.cmd` and does what it prints. With a VPN: are you both online in the same VPN network? With port forwarding: the router rule is UDP, not TCP, and points at the host's Same-house address. |
| `refused: name taken` | Another player uses that name, or you restarted after a crash and the server still holds it: wait a few seconds, or run `play.cmd change` and pick another name. |
| `refused: wrong protocol version` (or a game build mismatch) | Not everyone has the same release: everyone runs `Update.bat` in their kit, the host too ([setup.md](setup.md#updating)); the server's window says which side is older and which release that is (the host's kit names it). Everyone also needs the same game version (`coop: game build ...` in the log). |
| `connection lost; reconnecting` | The host's connection dropped or the server window was closed. It reconnects on its own when the server is back. |
| High ping (over about 150 ms) or the others jump around | Distance or a slow connection. With a virtual LAN, try port forwarding instead. Close downloads and streams. |
| The others show as a dot, not as Link | You have no puppet pack yet ([setup.md, step 4](setup.md#4-the-puppet-pack)), or they are in another area. |
| Everyone is green | Your puppet pack is from before the tunic colours: run `make-puppet-pack.cmd` again ([Your colour](#your-colour)). |
| A locked dungeon door another player opened stays shut for you | Type `/teleport <their name>` in the chat to go to them, or step a few metres back from the door and come again ([known-issues.md](known-issues.md)). |
| The screen goes black while another player is in a cutscene, or after a teleport | Wait a few seconds for the cutscene to end. If it stays black, close the game and start it again, and tell us what you and the other player were doing ([known-issues.md](known-issues.md)). |
| Windows Firewall asks about `coop_server.exe` or the game | Click **Allow access** (Private networks). Missed it? The host runs `check-hosting.cmd`. |
| Something else | Close the emulator, run `collect-logs.cmd`, and send the zip privately (Discord DM) with a short description, never on a public issue. Addresses in it are masked. |

The log is `emulator\user\log\eden_log.txt`; the co-op lines start with `coop:`. The server's log is in the host's `logs\` folder.
