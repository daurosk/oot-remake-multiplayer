# Wire protocol, version 0

The one protocol that the emulator build, the recomp build and the server all speak. This is a design: nothing here is built yet. When code exists, the code and this page change together in the same pull request.

## Rules it is built on

Each rule answers a way the TotK online sync was seen to break.

| Rule | What it prevents |
|---|---|
| Every state sample carries the sender's time and a sequence number. | Receivers timing samples by arrival, so network jitter becomes motion jitter. |
| Every moving thing's sample carries a velocity. | Copies that can only snap or slide, never predict. |
| Senders keep sending while still, at a low rate (a keep-alive sample). | Silence that could mean "still" or "late". |
| Every component inside a message carries its own length. | A reader that meets an unknown part cannot skip it. |
| The same message layout in both directions. | Two codecs that drift apart. |
| "Paused", "loading" and "in a cutscene" are messages, not guesses from silence. | The server handing things over because a player's map was open. |
| Ownership changes only by a hand-over message that carries the last state. | Owners that flip with no hand-over, and AI that restarts on the new owner. |
| Ownership has hold times and a distance margin, and can cover a group (a boss and its parts). | Bosses passed back and forth every few seconds, and parts run by a different game than the body. |
| A hit on a copy is sent to the owner as a damage event. Only the owner removes an object. | Hits that reach nobody, and a non-owner deleting an enemy for everyone. |

## Transport

- ENet over UDP, default port **27510** (changeable in settings).
- Channels:

| Channel | Kind | Carries |
|---|---|---|
| 0 | reliable, ordered | handshake, lobby, chat, player state flags (paused, loading, cutscene) |
| 1 | reliable, ordered | shared progress (flags, items, dungeon state) |
| 2 | reliable, ordered | ownership and damage events |
| 3 | unreliable, sequenced | player samples (position, rotation, animation) |
| 4 | unreliable, sequenced | object and enemy samples from their owners |

- The server relays and decides; it never runs the game. The host player's game is the source of truth for shared progress (stage 4).

## Encoding

- Little-endian fixed-size integers and IEEE-754 floats. No variable-length integer trick: messages are small and fixed fields keep the code simple.
- A message is: `u16 type`, `u16 length` (bytes after this header), then the body.
- A body made of components lists them as `u16 component_id`, `u16 length`, data. A reader skips any id it does not know.
- Strings are `u16 length` + UTF-8 bytes, no terminator, with a maximum per field.
- Times are `u64` microseconds on the sender's clock. Each client estimates the offset to the server clock from the heartbeat (below) and converts.

## Handshake

1. The client connects and sends `HELLO`: protocol version (`u16`), build id (string: emulator or recomp, and its version), game build id (string, for example `e0f3ccfaf50065ae`), game version (string, `1.1.1`), nickname (up to 16 characters).
2. The server answers `WELCOME` with the player's number (1 to 4), the server time, the tick rate and the list of players already in, or `REFUSED` with a reason the client shows: wrong protocol version, wrong game build, server full, name taken.
3. No `HELLO` within 10 seconds: the server drops the connection.

## Keep-alive and clock

- `PING` every 250 ms from each side with the sender time; `PONG` echoes it with the receiver time. This gives the round trip and the clock offset.
- ENet's own timeout drops a peer after 10 seconds of nothing. A drop is reported to the others as `PLAYER_LEFT` with the reason (left, timed out, kicked).

## Messages for stages 0 and 1

| Type | Channel | Body |
|---|---|---|
| `HELLO` | 0 | above |
| `WELCOME` / `REFUSED` | 0 | above |
| `PLAYER_JOINED` / `PLAYER_LEFT` | 0 | player number, nickname / reason |
| `PING` / `PONG` | 0 | times |
| `PLAYER_STATUS` | 0 | flags: paused, loading, in cutscene, in menu, dead; sent on every change |
| `CHAT` | 0 | text, up to 200 characters |
| `PLAYER_SAMPLE` | 3 | components below |
| `TEST_PAYLOAD` | 0 or 3 | sequence number and filler, used only by the stage 0 test tool |

`PLAYER_SAMPLE` components:

| Id | Name | Data |
|---|---|---|
| 1 | `time` | `u64` sender time, `u32` sequence |
| 2 | `position` | 3 x f32 |
| 3 | `velocity` | 3 x f32, metres a second |
| 4 | `rotation` | 4 x f32 quaternion (the game's 3x3 matrix converted on send) |
| 5 | `scene` | string, the Banc scene name |
| 6 | `age` | u8: 0 child, 1 adult |
| 7 | `animation` | string name, f32 frame, f32 rate (stage 2) |

A player sends `PLAYER_SAMPLE` every frame it moves (up to 60 a second) and at least 4 a second while still. Receivers keep the last 16 samples per player, show them about 100 ms behind (tuned by measuring), interpolate between samples and extrapolate up to 150 ms from the last one; beyond that the puppet holds and fades.

## Later stages (outline only)

- **Stage 3:** `PLAYER_STATUS` decides whether puppets show during cutscenes. `JOIN_PARTNER` asks for a warp to another player's scene.
- **Stage 4:** `FLAG_SET` (flag hash, value) from any player goes to the host; the host's `FLAG_STATE` is the answer everyone applies. A full flag snapshot is sent on join.
- **Stage 5:** `OWNER_CLAIM`, `OWNER_GRANT` (with the last state, life included), `OBJECT_SAMPLE`, `DAMAGE` (target, amount, kind, source), `OBJECT_REMOVE` (owner only). Groups for bosses. The hold times and margins are measured before they are fixed.

## Versioning

- The protocol version goes up whenever a message layout changes in a way an older reader cannot skip.
- Adding a component or a message type that old readers may ignore does not change the version.
