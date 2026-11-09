# Host

`coop/host/` (library `coop_host`) is the sending side of presence: it reads the local player from guest memory and builds the `PLAYER_SAMPLE` and `PLAYER_STATUS` messages of [protocol.md](protocol.md). It has no sockets and no emulator code. The emulator (Lichii, later the recomp) implements one small interface, `GameMemory`, and sends what the sampler returns. Plan stage 1 step 1; the receiving side is [presence.md](presence.md).

## Use

```cpp
class LichiiMemory : public coop::host::GameMemory {
  std::uint64_t main_base() const override;                          // where main is mapped
  bool read(std::uint64_t address, std::span<std::uint8_t> out) override;  // false if unmapped
};

LichiiMemory memory;
coop::host::PlayerReader reader(memory);
coop::host::Sampler sampler;            // default settings

// once per emulated frame, once the client is welcomed:
for (auto& m : sampler.frame(reader, client.server_now_us())) send(m.channel, m.bytes);
// after a reconnect:
sampler.reset();
```

## What it reads

`PlayerReader::read()` gives one `PlayerReading` a frame, or nothing.

- **Position** (required): the GameUI copy while its valid bit is set, otherwise the player actor's own position. Without a position there is no reading and no sample.
- **Rotation**: the actor's 3x3 matrix, turned into a quaternion (Shepperd's method, accurate near 180 degrees; w >= 0).
- **Age**: GameUI is-child, 1 child, 0 adult.
- **Scene**: the Banc path's file name, cut at the first `.` (as the game's 0x58aac0 does).
- **Event running**: event manager +0x68 not zero.

The player actor is found as the game does (0x37cca0): Scene module, table, player manager (index 15), then the first link whose holder id matches and whose actor is in state 4, else the second link.

Every read is checked, and a bad one gives nothing rather than garbage:

| Read | Refused when |
|---|---|
| any pointer | null, or any byte unreadable |
| position | NaN, infinite, or a coordinate beyond 1e6 game units |
| rotation | a NaN or infinity, or not a rotation: rows not of length 1 or not at right angles, or determinant not +1 (all within 0.05), so mirrors and scaled matrices are refused |
| age | a byte other than 0 or 1 |
| scene | Banc +0x38 null, capacity 0, no terminator within the capacity (at most 256 bytes read), an empty name, more than 64 characters, or anything but printable ASCII |
| actor | table count under 0x10, manager +0x30 not set, link id empty (~0), holder id not the link's, state not 4 |

A field other than the position that fails is left out of the reading; the rest is still sent.

## What it sends

`Sampler::frame` returns encoded messages, the status first:

- `PLAYER_STATUS` on the first frame with a reading, on every change, and again after `reset()`. Only bit 2 (in cutscene) is set from the game so far, from "event running". When the event state cannot be read, the last flags stand. The other bits are left for later steps.
- `PLAYER_SAMPLE` every frame the player moves (more than `still_distance` from the last sample sent), turns, or changes scene or age; once more on the first still frame after moving, so receivers stop extrapolating; and otherwise every `keepalive_us` (200 ms, so at least 4 a second even with late frames). Each carries time (the server time given), a sequence that counts up, position, rotation, scene and age when known, and velocity.
- Velocity is the change since the last frame's reading, in metres a second (`units_per_metre`, 1 until a run measures the game's unit). Movement under `still_distance` is a velocity of exactly 0. No velocity is sent for the first reading, after a frame with no reading, across a gap over `max_velocity_gap_us` (250 ms), across a scene change, when the server time did not move on, or above `max_speed` (200 m/s: a warp).

No reading this frame means no messages, and velocity starts afresh at the next reading.

## Address table

All addresses and offsets are in [`coop/host/src/addresses.hpp`](../coop/host/src/addresses.hpp), each with its source. They are offsets into `main` of build e0f3ccfaf50065ae (1.1.1) and come from two places, read only:

- `docs/plan.md`, "Game facts we start from".
- `..\integration\docs\player-state-map.md` (the stage 2 project's map). Most of it was read from the code and not checked in a run; Run 1 there saw the event state and the GameUI and first-link positions agree.

Not confirmed yet, to check in the first run:

- That the GameUI position updates every frame and follows a moving player (Run 1 saw it change once).
- That the matrix at actor +0x328 is stored by rows of the local-to-world matrix (`kActorRotationRowMajor`). If puppets turn the wrong way, flip it.
- The widths of the table count (+0x50) and the two link ids (+0x10, +0x48): 4 bytes assumed.
- That Banc +0x40 is the scene in every state: the game's 0x58aac0 has an earlier branch that was not read.
- What the two player links stand for; the second was unreadable in Run 1.
- The type check of the player manager against 0x4c7f8a0 is not done here.
- The game's unit of length (`units_per_metre`).
