# Documents

Every document in this folder, what it is, and who it is for. New here? Start with the [main README](../README.md).

## For players

| Document | What it is |
|---|---|
| [setup.md](setup.md) | From the downloaded zip to a running game: the emulator kit, Hookshot, updating, settings, chat commands, saves, Linux, troubleshooting, low frame rate, reporting a problem |
| [play-online.md](play-online.md) | Playing with friends over the internet: the invite code, what to do when it does not connect, a free VPN network, your colour, PvP, who can join |
| [play-lan.md](play-lan.md) | Playing with someone in the same house |
| [known-issues.md](known-issues.md) | What works in the newest release, what does not yet, and what is not checked yet |
| [faq.md](faq.md) | Short answers to the questions players ask most |

The kit zip has copies of the first four as `SETUP.md`, `PLAY-ONLINE.md`, `PLAY-LAN.md` and `KNOWN-ISSUES.md`.

## For hosts and testers

| Document | What it is |
|---|---|
| [play-online.md](play-online.md) | Also for hosts: port forwarding, UPnP, a relay, `server_allow` and the firewall rules |
| [testing.md](testing.md) | A tester's checklist for two PCs (in the kit as `TESTING.md`), then how the kit is made and how the automatic two-game tests run |
| [soak.md](soak.md) | Running the server with test clients for a long time, to check the network layer |
| [../SECURITY.md](../SECURITY.md) | What the server and the kit protect against, and how to report a security problem |

## For contributors

Start with [../CONTRIBUTING.md](../CONTRIBUTING.md). These pages are design and research notes: they say how a part works and how it was found in the game, and they are long.

**The plan and the rules**

| Document | What it is |
|---|---|
| [plan.md](plan.md) | The two stages (emulator, then a native PC build) and the steps of each |
| [protocol.md](protocol.md) | The wire protocol: every message the games and the server send, and the rules for them (version 11) |
| [sync-coverage.md](sync-coverage.md) | The map of what is shared between games and what is not yet, area by area |
| [workflow.md](workflow.md) | How the maintainer's AI sessions are organised: one task, one branch, one review |
| [invite-design.md](invite-design.md) | How invite codes, answer codes, hole punching and the relay work inside |

**The two ways the game runs**

| Document | What it is |
|---|---|
| [lichii.md](lichii.md) | How co-op runs inside the patched emulator (Fritchii); building it is in [../lichii/README.md](../lichii/README.md) |
| [hookshot.md](hookshot.md) | How co-op runs on Hookshot, the native PC build, with its title menu; the files are in [../hookshot/README.md](../hookshot/README.md) |
| [fritchii-autobuild.md](fritchii-autobuild.md) | The workflow that builds new Fritchii releases with our patches |
| [game-builds.md](game-builds.md) | Which build of the game co-op supports, and what ties co-op to it |

**Parts of co-op**

| Document | What it is |
|---|---|
| [presence.md](presence.md) | Working out where the other players are from the messages they send |
| [host.md](host.md) | Reading the local player and the shared flags from the game, and what is sent |
| [overlay.md](overlay.md) | The markers and name tags drawn over the game |
| [puppet.md](puppet.md) | The other players as a second Link: how the puppet is spawned, posed and dressed |
| [scenes.md](scenes.md) | Showing only players in your area, and what happens in cutscenes |
| [flags.md](flags.md) | Where the game keeps its world flags, and how co-op reads and writes them |
| [items.md](items.md) | Where the game keeps items, and which of them are shared |
| [dungeon.md](dungeon.md) | Dungeon switches, doors, keys and cleared rooms the same for every player |
| [dungeons-boss.md](dungeons-boss.md) | Boss rooms entered and fought together |
| [objects.md](objects.md) | Shared enemies, townspeople and objects: one owner each, hits sent to the owner |
| [spawns.md](spawns.md) | Things a player makes (bombs, seeds, arrows) shown in every game |
| [world.md](world.md) | Pots, crates, switches and other world objects the same for every player |
| [time.md](time.md) | The host's time of day in every game |
