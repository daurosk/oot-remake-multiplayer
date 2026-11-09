# Host

`coop/host/` (library `coop_host`) is the sending side of presence and the game side of shared flags (below). For presence it reads the local player from guest memory and builds the `PLAYER_SAMPLE` and `PLAYER_STATUS` messages of [protocol.md](protocol.md). It has no sockets and no emulator code. The emulator (Lichii, later the recomp) implements one small interface, `GameMemory`, and sends what the sampler returns. Plan stage 1 step 1; the receiving side is [presence.md](presence.md).

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

## Shared flags

The game side of shared progress (plan stage 1 step 4, protocol.md "Shared flags"). Like the sampler it has no sockets: it takes received flag messages and returns encoded ones. The emulator implements one more interface, `FlagStore`.

```cpp
class LichiiFlags : public coop::host::FlagStore {
  std::optional<coop::protocol::FlagEntry> read(coop::host::FlagKey key) override;  // nothing if it cannot
  bool write(const coop::protocol::FlagEntry& entry) override;                       // false if it cannot
};

std::string error;
auto shared = coop::host::SharedFlags::load("shared_flags.txt", &error);  // the user's list
coop::host::FlagWatcher flags(*shared, is_host ? coop::host::FlagRole::host
                                               : coop::host::FlagRole::player);

// for each FLAG_SET, FLAG_STATE or FLAG_SNAPSHOT the client receives:
flags.receive(message);
// once per emulated frame, on the game's thread:
for (auto& m : flags.frame(lichii_flags)) send(m.channel, m.bytes);
// host: when starting a session or taking over
flags.set_role(coop::host::FlagRole::host);
flags.request_snapshot();
// after a reconnect:
flags.reset();
```

While `holding()` is true a snapshot is coming in parts; the emulator should hold the game (protocol.md: the joiner holds until the last part).

**Not located yet:** the game's own flag get and set functions. `main-map.md` ("Flags") names the `GameDataMgr` and the flag table file but not the functions. `player-state-map.md` reads one bool flag through the tables in memory (value at byte +0x16 of the entry at manager +0x18 + index x 0x18, the index from the hash table at manager +0x20), but it is not known that the in-memory index is the file position, and the second table at +0x80 was not read. Until the functions are found, a `FlagStore` would go through those tables, bools only, and any write needs a run to check it.

### The flag list

Which flags are shared is a plain text file the user supplies; no list from the game is in this repository. One flag a line: a name (hashed with `flag_hash`, Murmur3 x86 32-bit, seed 0) or a hash as `0x` and up to 8 hex digits, then for an array flag an index or a range (`0-7`). `#` starts a comment. A made-up example is [`tests/host/shared_flags.example.txt`](../tests/host/shared_flags.example.txt). A bad line refuses the whole file, with its line number.

**Choice:** the list holds world flags only, the suggested answer in the plan's open decisions: chests, small keys, doors, heart pieces collected, songs learned, dungeon state, events seen. Inventory, rupees, hearts, magic and equipment stay per player and are left out of the list. A flag that comes in but is not in the local list is not written (`stats().not_shared`), so a host with a longer list cannot change a player's per-player flags. Caleb has not decided this yet; changing it is a change to the list, not to the code.

### What it does each frame

1. Writes again any incoming value whose write failed last time, unless the game changed that flag by itself since (its change is newer and is sent).
2. Takes the queued messages in order:
   - Player: a `FLAG_STATE` is written into the game and clears the local change waiting for that flag (the host's answer wins, even "no"). `FLAG_SNAPSHOT` parts are kept until the last one (part = part count - 1), then written all together; a flag with a local change still waiting is not overwritten. A `FLAG_STATE` that comes while a snapshot is held is written after it. A part out of order or missing drops the snapshot (`stats().bad_snapshots`).
   - Host: each `FLAG_SET` is written, read back, and answered with the value that came out, so a refused write is answered with the game's value.
3. Unless a snapshot is held, reads every shared flag and compares it with last frame. The first good read of a flag is its baseline and sends nothing. A failed read is skipped and the flag keeps its last value. What the watcher wrote itself is already its last value, so it is never sent back.
4. Sends the changes: a player as `FLAG_SET` (kept as waiting for an answer), the host as `FLAG_STATE` together with its answers. At most 4096 entries a message; more go in several.
5. Host, when asked: reads the whole list and sends it as `FLAG_SNAPSHOT` parts (`split_snapshot`). Flags that cannot be read are left out.

A player takes no `FLAG_SET` and a host no `FLAG_STATE` or `FLAG_SNAPSHOT`; those are ignored. After `reset()` changes still waiting for an answer are sent again.

Not proven yet: nothing has run against the game. A late answer to an earlier `FLAG_SET` can briefly undo a newer local change of the same flag until the next answer comes; this is rare (two changes of one flag within a round trip) and was left as protocol.md says (every `FLAG_STATE` is applied).
