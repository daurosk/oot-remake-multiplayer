# Playing over the internet

For players in different places. Every player has done [setup.md](setup.md) first, with the same release. One player is the **host** (their PC runs the server); the others **join**.

The others must be able to reach the host's PC on **UDP port 27510** (or the port in `emulator\user\config\coop.ini`). There are two ways:

- **(a) Port forwarding**: the host opens the port on their router. Fastest, nothing to install, but the host needs access to the router and a public IP address.
- **(b) A virtual LAN** such as Radmin VPN, Hamachi, ZeroTier or Tailscale: everyone installs the same free program and joins one private network, then it works as on a LAN. Nothing to change on any router.

**Which is safer?** (b). With a virtual LAN only the people in your private network can reach the server. With port forwarding anyone on the internet who learns your address can try to connect: keep the address to the people you play with and list them in `server_allow` (see [Who can join](#who-can-join)).

**When do you need (b)?** When the host cannot forward a port: no access to the router, a shared network (dorm, work, school, hotel), mobile internet, or an internet provider that uses CGNAT (your router's WAN address is not the address that [whatismyip](https://www.whatismyip.com) shows, or starts with `100.64.` to `100.127.`). If you are not sure, try (b): it always works.

## (a) Port forwarding

**Host:**

1. Find your PC's LAN address: `ipconfig` in a Command Prompt, **IPv4 Address** (for example `192.168.1.20`). Giving the PC a fixed address in the router (a "DHCP reservation") keeps it from changing.
2. Open your router's settings page (often `192.168.1.1` or `192.168.0.1`; the address and password are usually on a sticker on the router). Find **Port forwarding** (sometimes under NAT, Virtual server or Games).
3. Add a rule: protocol **UDP**, external port **27510**, internal port **27510**, internal address = your PC's LAN address from step 1. Save.
4. Allow the port in Windows Firewall ([play-lan.md, Firewall](play-lan.md#firewall); use `profile=any` if Windows calls your network Public).
5. Find your public address: open [whatismyip.com](https://www.whatismyip.com) and use the **IPv4** address.
6. Double-click `Host.bat`, keep the server window open, and send your public address to the others.

**Others:** double-click `Join.bat` and type the host's public address.

Your public address can change from time to time (most home connections); check it again before each session. Only share it with people you play with, and list theirs in `server_allow` ([Who can join](#who-can-join)). Remove the forwarding rule when you stop playing for a while.

## (b) Radmin VPN (or another virtual LAN)

Radmin VPN is free and needs no account. ZeroTier and Tailscale work the same way (see below).

**Everyone:**

1. Install Radmin VPN from [radmin-vpn.com](https://www.radmin-vpn.com) and start it.
2. **The host:** click **Network > Create network**, choose a network name and a password, and send both to the others.
3. **The others:** click **Network > Join network** and enter that name and password.
4. Everyone now sees each other in the Radmin VPN window. Each PC has a Radmin address that starts with `26.` (shown at the top of the window, for example `26.12.34.56`).

**Host:** double-click `Host.bat` and send your `26.x.x.x` address to the others. If they cannot connect, allow the port in the firewall for Radmin addresses only: Windows often treats the Radmin VPN adapter as a Public network. In **Command Prompt as administrator**:

```
netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow protocol=UDP localport=27510 profile=any remoteip=26.0.0.0/8
```

For ZeroTier use your network's range instead of `26.0.0.0/8` (my.zerotier.com shows it), for Tailscale `100.64.0.0/10`.

**Others:** double-click `Join.bat` and type the host's `26.x.x.x` address.

**ZeroTier:** everyone installs ZeroTier and joins the same network ID; the network's owner approves each PC on my.zerotier.com. Use the host's ZeroTier address (the one in the network's member list).

**Tailscale:** everyone signs in to the same tailnet (or the host shares their PC with the others). Use the host's Tailscale address, `100.x.y.z` (the Tailscale window, or `tailscale ip -4`).

### Hamachi

Hamachi (LogMeIn) works the same way as Radmin VPN. Its addresses start with `25.`.

1. Everyone installs Hamachi and joins the same network. The host creates the network and sends its ID and password to the others.
2. **The host:** your Hamachi IPv4 address is at the top of the Hamachi window (for example `25.12.34.56`). Use the IPv4 address, not the IPv6 one. Double-click `Host.bat`, keep the server window open, and send that `25.x.x.x` address to the others.
3. **The others:** double-click `Join.bat` and type the host's `25.x.x.x` address.

**Firewall (host only).** Windows treats the Hamachi adapter as a **Public** network. The server listens on **UDP port 27510** (or the port in `emulator\user\config\coop.ini`), so `coop_server.exe` must be allowed on Public networks. Only the server has to be allowed: the emulator only connects out, on the host's PC too. Either:

- Open **Command Prompt as administrator** and allow the port for Hamachi addresses only (`25.x.x.x`):

  ```
  netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow protocol=UDP localport=27510 profile=any remoteip=25.0.0.0/8
  ```

- Ticking **Public networks** when Windows Firewall asks about `coop_server.exe` also works, but then the server can be reached from any Public network your PC is on (a café or hotel Wi-Fi too). Prefer the rule above.

Only join Hamachi networks of people you know: everyone in the network can reach your server.

**Check that it works.** When someone joins, the host's server window shows `player 2 joined: <name>`. The joining player's log (`emulator\user\log\eden_log.txt`) has `coop: welcomed as player 2 on 25.x.x.x:27510`. If the joiner's panel stays on `connecting`, see Troubleshooting below. Check first that both of you are shown as online (green) in the Hamachi window.

A virtual LAN adds a little delay when it cannot connect the PCs directly; the ping in the game's co-op panel shows how much.

## Who can join

By default anyone who can reach the server's port may join. Two lines in the host's `coop-options.ini` (beside `Host.bat`) narrow that:

- `server_allow = 25.1.2.3,25.4.5.6`: only these addresses may connect; anyone else is turned away at once. Use the others' Hamachi (`25.`), Radmin (`26.`) or Tailscale (`100.`) addresses, or with port forwarding their public addresses (whatismyip.com on their PC). A whole range: `26.0.0.0/8`.
- `server_bind = 25.1.2.3`: the server listens only on this address of your own PC (your Hamachi or Radmin address), so it cannot be reached through your home network or Wi-Fi at all.

Remove the `#` in front of the line, put in the addresses and start `Host.bat` again. The server window then says `listening on port 27510 (only 25.1.2.3, ...)` and `only the N address range(s) given with --allow may connect`. A player who is turned away is counted as `not allowed` when the server stops. More in [SECURITY.md](../SECURITY.md).

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
| The panel stays on `connecting` | Wrong address or port: check with the host (the server window lists the addresses, `coop.ini` the port). Is the server window still open? Host's firewall ([play-lan.md](play-lan.md#firewall)). For (a): the router rule is UDP, not TCP, and points at the right PC. For (b): are you both shown as online in the VPN program? |
| `refused: name taken` | Another player uses that name, or you restarted after a crash and the server still holds it: wait a few seconds, or run `play.cmd change` and pick another name. |
| `refused: wrong protocol version` (or a game build mismatch) | Not everyone has the same release: everyone runs `Update.bat` in their kit, the host too ([setup.md](setup.md#updating)); the server's window says which side is older and which release that is (the host's kit names it). Everyone also needs the same game version (`coop: game build ...` in the log). |
| `connection lost; reconnecting` | The host's connection dropped or the server window was closed. It reconnects on its own when the server is back. |
| High ping (over about 150 ms) or the others jump around | Distance or a slow connection. With a virtual LAN, try port forwarding instead. Close downloads and streams. |
| The others show as a dot, not as Link | You have no puppet pack yet ([setup.md, step 4](setup.md#4-the-puppet-pack)), or they are in another area. |
| Something else | Close the emulator, run `collect-logs.cmd`, and send the zip with a short description. |

The log is `emulator\user\log\eden_log.txt`; the co-op lines start with `coop:`. The server's log is in the host's `logs\` folder.
