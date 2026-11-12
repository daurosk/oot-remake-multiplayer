# Playing over the internet

For players in different places. Every player has done [setup.md](setup.md) first, with the same release. One player is the **host** (their PC runs the server); the others **join**.

## The easy way: Host.bat and an invite code

No router settings, no VPN, no server of ours.

1. **The host** double-clicks `Host.bat`. A few seconds later the server window shows `invite code: XXXXX-XXXXX-...` and has copied it to the clipboard. Paste it to your friends (a private message, not a public channel).
2. **The others** double-click `Join.bat`, paste the invite code and press Enter. A second window opens, the **join helper**: keep it open while you play. Nobody types an address or a password.
3. If the helper says `the host's server answered directly`, the game connects by itself. If instead it shows an **ANSWER CODE** (also copied to the clipboard), send it back to the host. **The host** pastes it into the server window (right-click pastes) and presses Enter. Within a few seconds the helper says `the way through both routers is open` and the game connects.

What Host.bat does for this, by itself:

- It makes a **new random join password** each time it starts (20 letters, 100 random bits) and puts it in the invite code, so nobody has to type it. If you set your own `server_password` in `coop-options.ini`, that one is used instead (an empty `server_password =` line means no password, and then there is no invite code and no router forward).
- It asks your router to **forward the game's UDP port** (UPnP) while the server runs; see [Let the router forward the port](#let-the-router-forward-the-port-upnp). Many routers have UPnP off; the invite code then still works for most friends through the answer code.
- It asks a free public **STUN server** (Google's `stun.l.google.com`, then Cloudflare's `stun.cloudflare.com`) which internet address and port your router gives the game's port. That address goes into the invite code.

**Keep the invite code among friends.** It contains your internet address and the join password: anyone who has it can join while your server runs. It stops working when the server window closes, and Host.bat makes a new one each time. It is never written to the server's log or into `collect-logs` output.

**Privacy:** the STUN server sees your internet address (as every website you visit does), nothing of the game. To not use it, write `invite = no` in `coop-options.ini`; `stun_servers = name:port, ...` picks other ones. The friend's join helper asks the same servers.

**Windows Firewall:** the first time the server runs, Windows may ask whether `coop_server.exe` may use the network. For the answer-code way nothing more is needed (Windows lets in the answers to what the server sent). If friends should connect directly through a UPnP forward, allow it for the network you are on, or run the rule `check-hosting.cmd` prints.

### When the invite code does not connect

- `answer code not used: ...` in the server window: the line says why (a letter wrong or missing, an answer older than 10 minutes, or one made from an old invite code). Ask for a new answer code: closing the join helper and starting Join.bat again makes one.
- The helper keeps waiting and after 3 minutes says **No way through the routers was found**: both routers change the port for every address (symmetric NAT), or a provider shares one internet address between many homes (CGNAT). That happens on some mobile and dorm connections. Use a [free VPN network](#when-the-invite-code-does-not-work-a-free-vpn-network) (always works), or the host forwards the port by hand ([port forwarding](#without-a-vpn-port-forwarding)).
- The server window says `no invite code: no STUN server answered`: the PC has no internet, or a firewall blocks UDP. Same help as above.
- The server window says `your router gives every address its own port (symmetric NAT)`: the answer-code way will probably not work for this host; a VPN will.

### How the invite code works (design note)

- **Invite code** (`server/nat_codes.hpp`): a kind and version byte, the host's internet IPv4 address and port, the join password (a made password packed in 13 bytes, or your own as text) and the first 4 bytes of its SHA-256, written as letters from 32 that cannot be mixed up (no I, O, 0, 1) in groups of five. A mistyped or missing letter is caught by the checksum; a code from a newer version says so. It is made only with a join password, and only for a real internet address. Join.bat reads it in `tools/release/kit/scripts/invite.ps1`; both sides are tested against the same codes.
- **STUN** (`server/stun.hpp`, RFC 5389): one Binding request from the game server's own UDP socket (the join helper uses its own socket the same way), so the address in the answer is the one friends' packets reach. Only an answer from the server we asked, with our 96-bit transaction id, is used, and every length in it is checked before it is read (unit tests with every shortened copy and 20000 random answers). When two STUN servers see different ports, the window warns about symmetric NAT.
- **Direct try:** the join helper sends signed probes to the invite address for 3 seconds. The server answers a probe only when it is signed with the join password, and its answer is no bigger than the probe.
- **Answer code:** the helper's internet address and port (from STUN), a random 8-byte nonce and the time, signed with HMAC-SHA256 under a key made from the join password (truncated to 8 bytes), plus a 2-byte checksum. The server takes it only when the signature is right, it is at most 10 minutes old, and the address is an internet address. A stranger cannot make the server send packets anywhere: it only punches toward an address in a signed answer code that the host pasted.
- **Punches** (`server/punch.hpp`): 72-byte datagrams starting with `OCP` and version 1 (ENet never sends that), with the type, the nonce, the time, both addresses and an HMAC-SHA256 over all of it. The key is HMAC-SHA256(password, "oot-coop nat punch v1"), so it is never the same as a join proof. The server drops a punch that is unsigned, more than 60 seconds off its clock, or for a nonce it was not given; it checks at most 10 datagrams a second from one address and 100 from all before it computes any signature. After the host pastes an answer code, the server punches toward that address 5 times a second for 10 seconds, and answers that helper's punches (toward the answer code's address, never toward the sender) for 10 minutes. Both routers then let the other side's packets in, and ENet connects over the same ports; the helper forwards the game's datagrams between 127.0.0.1 and that socket.
- The game, the join password check, the 5-wrong-passwords block, the half-open limit and `server_allow` are unchanged: the punch only opens the way. A port scan finds nothing new: without the password the server answers no punch.
- Tests: `tests/server/nat_test.cpp` (codes, STUN, punches, the host's rules: every mistyped letter, forged and stale codes, a stranger without the password gets nothing, rate limit, punches only toward the answer address), `tests/server/nat_loopback_test.cpp` (a real server, the join helper, a fake STUN server and an ENet game over 127.0.0.1: answer code, punches, the game reaches the server; a helper with another password never gets a punch back; the direct way), `tools/release/tests/hosting_test.ps1` (Join.bat's reader, the defaults, UPnP and the invite need a password). 127.0.0.1 has no routers: a real two-network test is still to do.

## When the invite code does not work: a free VPN network

1. **Everyone** installs [Radmin VPN](https://www.radmin-vpn.com) (free, no account). The host clicks **Network > Create network**, picks a name and a password, and sends both to the others; the others click **Network > Join network** and type them in. ([Hamachi](#hamachi) works the same way.)
2. **The host** double-clicks `Host.bat`. The server window shows **Give your friends ONE of these**, with the **Over Radmin VPN** address (`26.x.x.x`) first, and has already copied it (the invite code replaces that copy a few seconds later; copy the address from the window then). Send it with the join password: the window shows it as `Join password for this session: ...` (only in the window, never in the log), or set your own `server_password`.
3. **The others** double-click `Join.bat`, paste that address and press Enter. Join.bat says when you are connected.

Nobody touches a router. Only people with the network password can reach the server. If the others cannot connect, the host runs `check-hosting.cmd` in the kit folder while the server window is open: it prints exactly what is wrong and the command that fixes it (usually one firewall line for Radmin addresses; it changes nothing by itself).

## Without a VPN: port forwarding

Only if the host can change their router and has a public IP address (no CGNAT: your router's WAN address is the one [api.ipify.org](https://api.ipify.org) shows and does not start with `100.64.` to `100.127.`). Anyone on the internet who learns your address can try to connect, so do the safety steps too.

**Host:**

1. Start `Host.bat` once and read the server window: the **Same house** line is your PC's address (for example `192.168.1.20`), the **Over the internet** line your public address.
2. On your router's settings page (often `192.168.1.1`; the address and password are usually on a sticker on the router), under **Port forwarding**, add: protocol **UDP**, port **27510**, to your PC's Same-house address.
3. **Safety:** ask the others for their public address (they open [api.ipify.org](https://api.ipify.org)) and put them in `coop-options.ini` beside `Host.bat`: remove the `#` in front of `server_allow` and write, for example, `server_allow = 81.2.3.4,82.5.6.7`. Then nobody else can join ([Who can join](#who-can-join)). Keep the **join password** on (Host.bat makes one each time unless you set `server_password`; the server window shows it): give it to the others with the address, or send the invite code. Their addresses can change; the password still keeps strangers out when you have to widen `server_allow`.
4. Run `check-hosting.cmd` while the server window is open and run the firewall command it prints under **Over the internet with port forwarding** (in Command Prompt as administrator). It allows only `coop_server.exe`, and with `server_allow` set only those addresses. It applies on networks Windows calls Private: if the check says your home network is Public, switch it to Private as it says.
5. Close and start `Host.bat` again, and send the **Over the internet** address and the join password to the others.

**Others:** double-click `Join.bat` and paste the host's address.

**When you stop playing:** delete the port forward on the router and the firewall rule ([When you are done](#when-you-are-done)). Your public address can change from time to time: read it again in the server window before each session.

### Let the router forward the port (UPnP)

Instead of step 2, the server asks the router itself: Host.bat does this by default (`upnp = no` in `coop-options.ini` turns it off). It maps only the game's UDP port, only to this PC, with a 10-minute lease renewed while the server runs, and removes it when the server stops or its window closes. It is never used without a join password (`server_allow` alone is not enough: it opens your PC's port to the whole internet while the server runs). The server window then says in plain words what happened, and the server log keeps the line:

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

**Trade-offs:** the relay owner sees every player's address and when and how much you send. With a join password the game traffic itself is encrypted, so the relay cannot read or change it (docs/protocol.md, "Encrypted traffic"); without one it can. A player who has a join password typed in does not join a server without one ("the server did not ask for the join password"): start Join.bat again with the password empty. Each packet takes the detour through the relay, so the delay is the host-relay plus relay-player delay. Pick a relay near all of you, run by someone you trust. All relayed players share one address on the host's server (127.0.0.1), so its per-address limits are shared too: when one of them types a wrong join password 5 times, every new relayed player is turned away for 60 seconds (players already in stay in), and at most 4 can be connecting at the same moment.

### How the relay works (design note)

- The relay is `coop_server --relay` (`server/relay.hpp`): one UDP port, its own small messages (`server/relay_wire.hpp`, all starting with `OCR` and version 1) next to the game's plain ENet datagrams. No game code changes: the game talks ENet to the relay's address as to any server.
- The host's server with `--relay-via` (`server/relay_link.hpp`) sends **REGISTER** with a random 64-bit session token and gets a **join code**: 8 letters from 32 that cannot be mixed up (no I, O, 0, 1), 40 bits. A **KEEPALIVE** every 5 seconds keeps the code and the host's router path open; 30 seconds of silence ends the code. The token, not the address, says it is the same host, so a router that renumbers the host's port does not break it. A **BYE** on stop ends the code at once.
- A guest's Join.bat sends **LOOKUP** with the code. A right code lets that guest's internet address send to that host for 30 minutes after its last packet; datagrams from addresses without a code are dropped unread. Each guest port is a **flow**: the relay wraps its datagrams for the host's link, and the link gives each flow its own UDP socket on 127.0.0.1, so the server sees every guest as a separate player at 127.0.0.1 (with `server_allow` set, 127.0.0.1 is let in). The relay lets in an **address**, not a person: anyone else behind the same internet address (CGNAT, a dorm, a hotspot, an office) can reach the host for those 30 minutes without the code. So the code only finds the host, and `--relay-via` refuses to start without a join password: the password is the lock.
- Limits: 10 wrong codes per address per 10 minutes (then even the right one waits), 6 registrations per address per 10 minutes, 2 hosts per address, 64 hosts, 32 flows and 16 guest addresses per host, 1400-byte datagrams (ENet's are 1100, so a relayed one stays under a 1200-byte path, issue 143). The code is not a lock: the join password (required) and the server's own checks are.
- Tests: `tests/server/relay_test.cpp` (the rules, and a host's server + link + relay + a guest joining over 127.0.0.1; a guest without the code never connects), `tests/server/upnp_test.cpp` (a fake router on 127.0.0.1), `tools/release/tests/hosting_test.ps1` (Join.bat's part).

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

By default anyone who can reach the server's port may join. Three lines in the host's `coop-options.ini` (beside `Host.bat`) narrow that:

- `server_password = ...`: the **join password** (protocol 9). Without this line `Host.bat` makes a new random one each time and puts it in the invite code (the server window shows it too, never the log); set your own here to keep the same one, or `server_password =` (empty) for none, which also turns off the invite code and the router forward. Without an invite code, send it to your friends with the address. `Join.bat` asks for it each time (Enter keeps the last one, `-` for none) and keeps it in `emulator\user\config\coop.ini`. A player with a wrong or no password is turned away and their `Join.bat` window says `wrong join password`. The password itself never goes over the network: the server sends a random number, and the game answers with a proof made from the password and that number (HMAC-SHA256), which is no good for any other connection. No spaces, at most 128 characters; pick a long one (12 characters or more; three random words joined by dashes work) that you use nowhere else, since a short or common word can be guessed. To change it, change the line and start `Host.bat` again; delete the line to get a new random one each time. `check-hosting.cmd` says whether one is set.
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

Every player's Link wears a tunic (and cap) in that player's colour in the other games, so you can tell each other apart. Without a choice you get your player number's colour: 1 blue, 2 red, 3 green, 4 yellow.

To pick your own: open `coop-options.ini` beside `Host.bat` and `Join.bat`, remove the `#` in front of `# color = blue` and put in one of red, orange, yellow, green, cyan, blue, purple, pink, white, black, brown, gray, or any colour as `#RRGGBB` (for example `color = #FF8020`). Close the game and start `Host.bat` or `Join.bat` again. A word the kit does not know is said in the window and you keep your number's colour. Two players may pick the same colour; nothing stops it, so agree on yours.

The tunics come in ten colours (green, red, orange, yellow, cyan, blue, purple, pink, white, black); another colour gets the nearest of them (brown looks black, gray looks white). They are made by `make-puppet-pack.cmd` from your own game, so run it again after an update that brings them. A puppet pack from before the colours still works: everyone then wears green (after about 30 s the first time). `LICHII_COOP_TUNICS=0` in `coop-options.ini` turns the colours off for what you see.

## On Hookshot (experimental)

[Hookshot](https://github.com/NotRexed/Hookshot) (NotRexed and contributors, GPL-3.0) runs the game as a native PC program instead of in the emulator. The kit can build a co-op Hookshot game for you from your own game files. Emulator players and Hookshot players can be in the same session: same game build, same server.

**You need:** a graphics card with Vulkan 1.3 and mesh shaders (NVIDIA RTX 20 or newer, AMD RX 6000 or newer), 16 GB of RAM, about 60 GB free while it builds, and your game as a `.nup` file (or its `romfs` and `exefs` folders), the same version as for the emulator. [setup.md](setup.md#hookshot-instead-of-the-emulator-experimental) has the details.

1. Double-click `Build-Hookshot-Coop.bat` in the kit. It finds your `.nup` as `make-puppet-pack.cmd` does, or asks you to drag it into the window. The first time it downloads Hookshot's build tools (about 400 MB, checked against what Hookshot published), then builds the game: 10 to 45 minutes. When it finishes, the game is in `hookshot\game` in the kit, with co-op and the puppet pack already in it.
2. The host double-clicks `Host-Hookshot.bat`, the others `Join-Hookshot.bat`. They work like `Host.bat` and `Join.bat` (same questions, same server; invite codes too, not yet tested on Hookshot) but start the Hookshot game instead of the emulator; they write its `coop.ini` beside its `hookshot.exe`. Hookshot and emulator players can join each other's games.
3. The game's `hookshot_log.txt` (beside it) says `coop: module loaded`, then `coop: connected`. `Join-Hookshot.bat` does not yet watch for the connection as `Join.bat` does: look in the log, or for the other players in the game.

**After `Update.bat`:** run `Build-Hookshot-Coop.bat` again. It copies the new co-op files into the game in a few minutes, and builds the game again only when the update moved to a newer Hookshot.

**What works on Hookshot so far:** seeing each other (the puppets) and the session itself. The rest of co-op (shared items, flags, enemies, the co-op window and chat) is emulator-only for now; [known-issues.md](known-issues.md) says what changes. `HOS_COOP=0` starts the Hookshot game without co-op. Hookshot's own problems (crashes outside Kokiri Forest on some PCs, AMD device losses) are listed in its readme.

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
| Something else | Close the emulator, run `collect-logs.cmd`, and send the zip privately (Discord DM) with a short description, never on a public issue. Addresses in it are masked. |

The log is `emulator\user\log\eden_log.txt`; the co-op lines start with `coop:`. The server's log is in the host's `logs\` folder.
