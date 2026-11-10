# Playing over the internet

For players in different places. Every player has done [setup.md](setup.md) first, with the same release. One player is the **host** (their PC runs the server); the others **join**.

The others must be able to reach the host's PC on **UDP port 27510** (or the port in `emulator\user\config\coop.ini`). There are two ways:

- **(a) Port forwarding**: the host opens the port on their router. Fastest, nothing to install, but the host needs access to the router and a public IP address.
- **(b) A virtual LAN** such as Radmin VPN, ZeroTier or Tailscale: everyone installs the same free program and joins one private network, then it works as on a LAN. Nothing to change on any router.

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

Your public address can change from time to time (most home connections); check it again before each session. Only share it with people you play with.

## (b) Radmin VPN (or another virtual LAN)

Radmin VPN is free and needs no account. ZeroTier and Tailscale work the same way (see below).

**Everyone:**

1. Install Radmin VPN from [radmin-vpn.com](https://www.radmin-vpn.com) and start it.
2. **The host:** click **Network > Create network**, choose a network name and a password, and send both to the others.
3. **The others:** click **Network > Join network** and enter that name and password.
4. Everyone now sees each other in the Radmin VPN window. Each PC has a Radmin address that starts with `26.` (shown at the top of the window, for example `26.12.34.56`).

**Host:** double-click `Host.bat` and send your `26.x.x.x` address to the others. If they cannot connect, allow the port in the firewall ([play-lan.md, Firewall](play-lan.md#firewall) with `profile=any`): Windows often treats the Radmin VPN adapter as a Public network.

**Others:** double-click `Join.bat` and type the host's `26.x.x.x` address.

**ZeroTier:** everyone installs ZeroTier and joins the same network ID; the network's owner approves each PC on my.zerotier.com. Use the host's ZeroTier address (the one in the network's member list).

**Tailscale:** everyone signs in to the same tailnet (or the host shares their PC with the others). Use the host's Tailscale address, `100.x.y.z` (the Tailscale window, or `tailscale ip -4`).

A virtual LAN adds a little delay when it cannot connect the PCs directly; the ping in the game's co-op panel shows how much.

## Troubleshooting

| What you see | What to check |
|---|---|
| The panel stays on `connecting` | Wrong address or port: check with the host (the server window lists the addresses, `coop.ini` the port). Is the server window still open? Host's firewall ([play-lan.md](play-lan.md#firewall)). For (a): the router rule is UDP, not TCP, and points at the right PC. For (b): are you both shown as online in the VPN program? |
| `refused: name taken` | Another player uses that name, or you restarted after a crash and the server still holds it: wait a few seconds, or run `play.cmd change` and pick another name. |
| `refused: wrong protocol version` (or a game build mismatch) | Not everyone has the same release: everyone runs `Update.bat` in their kit, the host too ([setup.md](setup.md#updating)); the server's window says which side is older. Everyone also needs the same game version (`coop: game build ...` in the log). |
| `connection lost; reconnecting` | The host's connection dropped or the server window was closed. It reconnects on its own when the server is back. |
| High ping (over about 150 ms) or the others jump around | Distance or a slow connection. With a virtual LAN, try port forwarding instead. Close downloads and streams. |
| The others show as a dot, not as Link | You have no puppet pack yet ([setup.md, step 4](setup.md#4-the-puppet-pack)), or they are in another area. |
| Something else | Close the emulator, run `collect-logs.cmd`, and send the zip with a short description. |

The log is `emulator\user\log\eden_log.txt`; the co-op lines start with `coop:`. The server's log is in the host's `logs\` folder.
