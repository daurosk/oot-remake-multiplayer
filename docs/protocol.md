# Wire protocol, version 0

The one protocol that the emulator build, the recomp build and the server all speak. The messages of stages 0 and 1 are coded in `coop/protocol/` (`messages.hpp`, `codec.hpp`); the later stages are still a design. The code and this page change together in the same pull request.

## Rules it is built on

Each rule answers a way the TotK online sync was seen to break.

| Rule | What it prevents |
|---|---|
| Every state sample carries its time on the server clock and a sequence number. | Receivers timing samples by arrival, so network jitter becomes motion jitter. |
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

- Each message type has its channel, and each is sent by the client, the server or both (table below). A receiver refuses a message that came on the wrong channel or from a side that may not send it (`check_received` in the code).
- The server relays and decides; it never runs the game. The host player's game is the source of truth for shared progress (stage 4).
- The server holds each player to at most 60 messages a second on channels 0 to 2 and 240 on channels 3 and 4, with bursts of twice that, and drops the rest without a word. A packet longer than any message can be (4 + 65535 bytes) is thrown away unread.

## Encoding

- Little-endian fixed-size integers and IEEE-754 floats. No variable-length integer trick: messages are small and fixed fields keep the code simple.
- A message is: `u16 type`, `u16 length` (bytes after this header), then the body.
- One ENet packet holds exactly one message. A packet with bytes after the message, or a fixed-layout body with bytes after its last field, is refused.
- A message type the reader does not know is reported as unknown and ignored, not treated as an error in the connection.
- A body made of components lists them as `u16 component_id`, `u16 length`, data. A reader skips any id it does not know. A known component must have exactly its length (below) and may appear only once.
- Strings are `u16 length` + UTF-8 bytes, no terminator, with a maximum per field counted in characters (code points). The UTF-8 must be valid (no overlong forms, no surrogates, nothing past U+10FFFF) and hold no control characters (U+0000 to U+001F, U+007F to U+009F). A string is also never more than 4 bytes a character.
- Floats must be finite: NaN and infinity are refused.
- Player numbers are 1 to 4. `PLAYER_STATUS`, `CHAT` and `PLAYER_SAMPLE` start with a `u8` player field: a client always sends 0 ("me"), and the server writes in the sender's number (1 to 4), which it knows from the connection, before relaying to the others. A client message with any other value there, or a server one with 0, is refused.
- Times are `u64` microseconds. `PING` and `PONG` carry the sender's own clock, which is how the offset is found (below). Every other time is on the server clock: `WELCOME` carries the server's own, and a sample (`PLAYER_SAMPLE`, and later `OBJECT_SAMPLE`) carries the sender's estimate of the server clock when the state was taken, worked out from its own clock and its offset. The server relays samples with their time unchanged, so a receiver places another player's sample on the same time line as its own estimate of the server clock. A client sends no sample before it has an offset; the `WELCOME` time gives a first, rough one.

## Handshake

1. The client connects and sends `HELLO`: protocol version (`u16`), build id (string, up to 64 characters: emulator or recomp, and its version), game build id (string, up to 32, for example `e0f3ccfaf50065ae`), game version (string, up to 16, for example `1.1.1`), nickname (string, 1 to 16 characters). The protocol version is the first 2 bytes of the body in every version of the protocol, so a server can read it, and refuse with the right reason, even from a `HELLO` whose rest it cannot read.
2. The server answers `WELCOME` or `REFUSED`.
   - `WELCOME`: `u8` the player's number (1 to 4), `u64` server time, `u16` tick rate (not 0), `u8` count (at most 4), then for each player already in: `u8` number, nickname. Every number in it is different, and the new player's own is not in the list.
   - `REFUSED`: `u8` reason, which the client shows: 1 wrong protocol version, 2 wrong game build, 3 server full, 4 name taken, 5 bad name (a nickname of nothing but spaces). A reason the client does not know is kept and shown as unknown.
3. No `HELLO` within 10 seconds: the server drops the connection.

## Keep-alive and clock

- `PING` every 250 ms from each side with the sender time; `PONG` echoes it with the receiver time. This gives the round trip and the clock offset.
- `PING` is `u64` sender time. `PONG` is `u64` the time echoed from the `PING`, then `u64` the `PONG` sender's time.
- ENet's own timeout drops a peer after 10 seconds of nothing. A drop is reported to the others as `PLAYER_LEFT` with the reason (left, timed out, kicked).

## Messages for stages 0 and 1

| Type number | Type | Channel | Sent by | Body |
|---|---|---|---|---|
| 1 | `HELLO` | 0 | client | above |
| 2 | `WELCOME` | 0 | server | above |
| 3 | `REFUSED` | 0 | server | above |
| 4 | `PLAYER_JOINED` | 0 | server | `u8` player number, nickname |
| 5 | `PLAYER_LEFT` | 0 | server | `u8` player number, `u8` reason: 0 left, 1 timed out, 2 kicked (an unknown reason is kept) |
| 6 | `PING` | 0 | both | above |
| 7 | `PONG` | 0 | both | above |
| 8 | `PLAYER_STATUS` | 0 | both | `u8` player field, `u8` flags: bit 0 paused, 1 loading, 2 in cutscene, 3 in menu, 4 dead (unknown bits are kept); sent on every change |
| 9 | `CHAT` | 0 | both | `u8` player field, text up to 200 characters |
| 10 | `PLAYER_SAMPLE` | 3 | both | `u8` player field, then the components below |
| 11 | `TEST_PAYLOAD` | 0 or 3 | both | `u32` sequence number, then the rest of the body is filler; used only by the stage 0 test tool |

Type numbers are never reused or renumbered; new types are added at the end.

`PLAYER_SAMPLE` components:

| Id | Name | Data | Length |
|---|---|---|---|
| 1 | `time` | `u64` server time (the sender's estimate, above), `u32` sequence | 12 |
| 2 | `position` | 3 x f32 | 12 |
| 3 | `velocity` | 3 x f32, metres a second | 12 |
| 4 | `rotation` | 4 x f32 quaternion, x y z w (the game's 3x3 matrix converted on send) | 16 |
| 5 | `scene` | string, the game's scene name (for example `spot00`), up to 64 characters | 2 + string bytes |
| 6 | `age` | u8: 0 child, 1 adult (other values refused) | 1 |
| 7 | `animation` | string name up to 64 characters, f32 frame, f32 rate (stage 2) | 2 + string bytes + 8 |

The `time` component is required in every sample; the others are sent only when the sender has them.

A player sends `PLAYER_SAMPLE` every frame it moves (up to 60 a second) and at least 4 a second while still. Receivers keep the last 16 samples per player, show them about 100 ms behind (tuned by measuring), interpolate between samples and extrapolate up to 150 ms from the last one; beyond that the puppet holds and fades.

## Later stages (outline only)

- **Stage 3:** `PLAYER_STATUS` decides whether puppets show during cutscenes. `JOIN_PARTNER` asks for a warp to another player's scene.
- **Stage 4:** `FLAG_SET` (flag hash, value) from any player goes to the host; the host's `FLAG_STATE` is the answer everyone applies. A full flag snapshot is sent on join.
- **Stage 5:** `OWNER_CLAIM`, `OWNER_GRANT` (with the last state, life included), `OBJECT_SAMPLE` (timed on the server clock, as `PLAYER_SAMPLE`), `DAMAGE` (target, amount, kind, source), `OBJECT_REMOVE` (owner only). Groups for bosses. The hold times and margins are measured before they are fixed.

## Versioning

- The protocol version goes up whenever a message layout changes in a way an older reader cannot skip.
- Adding a component or a message type that old readers may ignore does not change the version.
