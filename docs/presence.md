# Presence

`coop/presence/` (library `coop_presence`) works out where to draw the other players from the messages they send. It has no sockets, no ENet and no game addresses: the emulator (and later the recomp) decodes messages with `coop/protocol`, hands them over, and asks each frame what to draw. Plan stage 1 step 1; the wire side is in [protocol.md](protocol.md) under `PLAYER_SAMPLE`.

## Use

```cpp
coop::presence::Presence presence;          // default settings
presence.apply(message);                    // every decoded message; the rest are ignored
for (auto n : presence.players_in_scene(local_scene)) {
  if (auto pose = presence.shown(n, client.server_now_us())) draw(n, *pose);
}
```

- `WELCOME` sets this player's number and the players already in. `PLAYER_JOINED` adds a player (a number already in starts afresh); `PLAYER_LEFT` removes one. A sample for a number with no `PLAYER_JOINED` is dropped, so a late sample after a leave does not bring the player back.
- Each player keeps its last 16 samples that have a position, ordered by server time, then sequence. A sample whose sequence is already buffered is a duplicate and dropped. A sample older than the time last shown for that player is dropped as too old (it would rewrite what was already drawn), unless it is newer than every buffered sample: then it is kept and extrapolated from, so a player whose samples all arrive later than the show delay is still shown. A sample older than all 16 of a full buffer is dropped too.
- A sample with a NaN or infinite float, or a rotation of length 0, is dropped as invalid (the decoder refuses these already; this is a second guard). Rotations are normalised when buffered, and shown positions and velocities are kept within the float range, so even huge finite values from a sender give a finite pose.
- Scene and age come from the newest sample that carried them, by sequence (allowing for the u32 wrap), so a late, older sample does not overwrite them and a jump in the sender's clock estimate cannot freeze them. They are taken even from a sample too late to be drawn. `same_scene` is false while the scene is not known.
- `PLAYER_STATUS` flags are kept per player as received (unknown bits too).
- **Each player's own delay** (player report #37, `show_delay.hpp`): a sample from another game is its trip to the server plus the server's trip to us old when it comes (150-300 ms at internet pings), so a fixed 100 ms put the shown time past the newest sample: every frame extrapolated, then held, a jump at each sample. `seen(player, sample_time, server_now)` (the host session calls it for each `PLAYER_SAMPLE`) keeps that player's last 32 ages; its delay aims at their 90th percentile plus 60 ms (one send interval), never under `show_delay_us` (a LAN is unchanged) nor over 600 ms, grows at half speed and shrinks at 5 % (the shown time never goes back). `show_delay_us(player)` reads it; the hook gives the same delay to that player's pose copy and to the enemies it owns (`coop_show_delay.h`). Measured (tests/net/lag_path_test.cpp, 62 ms + 0-60 ms jitter each way): the other player drawn between two samples 0 % of frames at the fixed 100 ms, 99.6 % at the aged delay (about 260 ms).
- `shown(player, server_now)` gives the pose at `server_now - ` that player's delay (`Pose::at_us`), and `Pose::how` says how it was made:
  - **interpolated** between the two samples around that time: cubic Hermite using both velocities, or a straight line if either has none. Rotation is a shortest-path slerp.
  - **extrapolated** past the last sample with its velocity, up to `extrapolate_us`.
  - **held** after that, at the extrapolation's end, with `fade` going from 1 to 0 over `fade_us`. Draw with `fade` as opacity; 0 means hide.
- Two samples further apart than `max_gap_us` are not joined (the player paused or stalled): the pose holds and fades at the first, then jumps to the second.
- A sample without a rotation keeps the newest rotation before it. There is no spin on the wire, so rotation is held, not extrapolated.

## Defaults and tuning

| Setting | Default | Raise it when | Lower it when |
|---|---|---|---|
| `show_delay_us` | 100 ms | players stutter or extrapolate often on a LAN (jitter or loss); over the internet each player's delay rises above it by itself | players look late next to voice or video |
| `extrapolate_us` | 150 ms | short drops make players freeze | players overshoot corners after a drop |
| `fade_us` | 250 ms | a held player vanishes too abruptly | a stalled player lingers too long |
| `max_gap_us` | 500 ms | normal keep-alive gaps (4 a second while still) are being cut | players slide across a pause |
| `hermite` | on | | velocities are wrong or noisy (overshoot between samples) |
| `velocity_scale` | 1 | positions use a smaller unit than velocity | positions use a bigger unit |

`velocity_scale` is position units per velocity unit. The protocol gives velocity in metres a second; if the sender's positions are in game units, set it to game units per metre. Keep `max_gap_us` above 250 ms, the longest normal gap between keep-alive samples.

The show delay should be tuned by measuring: `tools/coop_test` reports the shown error of its fake path, and the delay that keeps "extrapolated" rare on the target network is the one to use.
