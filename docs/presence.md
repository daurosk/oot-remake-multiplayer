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
- Each player keeps its last 16 samples that have a position, ordered by server time, then sequence. A sample whose sequence is already buffered is a duplicate and dropped. A sample older than the time last shown for that player, or older than all 16 when the buffer is full, is dropped as too old.
- Scene and age come from the newest sample that carried them, so a late, older sample does not overwrite them. `same_scene` is false while the scene is not known.
- `PLAYER_STATUS` flags are kept per player as received (unknown bits too).
- `shown(player, server_now)` gives the pose at `server_now - show_delay`, and `Pose::how` says how it was made:
  - **interpolated** between the two samples around that time: cubic Hermite using both velocities, or a straight line if either has none. Rotation is a shortest-path slerp.
  - **extrapolated** past the last sample with its velocity, up to `extrapolate_us`.
  - **held** after that, at the extrapolation's end, with `fade` going from 1 to 0 over `fade_us`. Draw with `fade` as opacity; 0 means hide.
- Two samples further apart than `max_gap_us` are not joined (the player paused or stalled): the pose holds and fades at the first, then jumps to the second.
- A sample without a rotation keeps the newest rotation before it. There is no spin on the wire, so rotation is held, not extrapolated.

## Defaults and tuning

| Setting | Default | Raise it when | Lower it when |
|---|---|---|---|
| `show_delay_us` | 100 ms | players stutter or extrapolate often (jitter or loss) | players look late next to voice or video |
| `extrapolate_us` | 150 ms | short drops make players freeze | players overshoot corners after a drop |
| `fade_us` | 250 ms | a held player vanishes too abruptly | a stalled player lingers too long |
| `max_gap_us` | 500 ms | normal keep-alive gaps (4 a second while still) are being cut | players slide across a pause |
| `hermite` | on | | velocities are wrong or noisy (overshoot between samples) |
| `velocity_scale` | 1 | positions use a smaller unit than velocity | positions use a bigger unit |

`velocity_scale` is position units per velocity unit. The protocol gives velocity in metres a second; if the sender's positions are in game units, set it to game units per metre. Keep `max_gap_us` above 250 ms, the longest normal gap between keep-alive samples.

The show delay should be tuned by measuring: `tools/coop_test` reports the shown error of its fake path, and the delay that keeps "extrapolated" rare on the target network is the one to use.
