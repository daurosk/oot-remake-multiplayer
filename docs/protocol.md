# Wire protocol, version 3

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
- Player numbers are 1 to 4. `PLAYER_STATUS`, `CHAT`, `PLAYER_SAMPLE`, `PLAYER_EQUIP` and the object messages a client sends start with a `u8` player field: a client always sends 0 ("me"), and the server writes in the sender's number (1 to 4), which it knows from the connection, before relaying to the others. A client message with any other value there, or a server one with 0, is refused.
- Times are `u64` microseconds. `PING` and `PONG` carry the sender's own clock, which is how the offset is found (below). Every other time is on the server clock: `WELCOME` carries the server's own, and a sample (`PLAYER_SAMPLE`, and later `OBJECT_SAMPLE`) carries the sender's estimate of the server clock when the state was taken, worked out from its own clock and its offset. The server relays samples with their time unchanged, so a receiver places another player's sample on the same time line as its own estimate of the server clock. A client sends no sample before it has an offset; the `WELCOME` time gives a first, rough one.

## Handshake

1. The client connects and sends `HELLO`: protocol version (`u16`), build id (string, up to 64 characters: emulator or recomp, and its version), game build id (string, up to 32, for example `e0f3ccfaf50065ae`), game version (string, up to 16, for example `1.1.1`), nickname (string, 1 to 16 characters). The protocol version is the first 2 bytes of the body in every version of the protocol, so a server can read it, and refuse with the right reason, even from a `HELLO` whose rest it cannot read.
2. The server answers `WELCOME` or `REFUSED`.
   - `WELCOME`: `u8` the player's number (1 to 4), `u64` server time, `u16` tick rate (not 0), `u8` count (at most 4), then for each player already in: `u8` number, nickname. Every number in it is different, and the new player's own is not in the list.
   - `REFUSED`: `u8` reason, which the client shows: 1 wrong protocol version, 2 wrong game build, 3 server full, 4 name taken, 5 bad name (a nickname of nothing but spaces). A reason the client does not know is kept and shown as unknown.
3. No `HELLO` within 10 seconds: the server drops the connection.
4. One address may hold at most 4 connections that have not sent `HELLO` yet (a server option; 0 for no limit). The server drops one more at once, without `REFUSED`.

### Rejoin after a crash

A killed emulator stays in the session until the server times it out, so its restart would be refused as name taken. A `HELLO` whose nickname (ignoring ASCII case) a player already holds takes that player's place only when both hold:

- it comes from the same IP address as that player, and
- the server has heard nothing from that player for 1 second (a server option; 0 turns the takeover off). Every build answers the server's `PING` every 250 ms, so a live player is never this quiet.

Then the old player is dropped and reported as `PLAYER_LEFT` timed out, with the host handed on as on any leave, and the new connection is welcomed with the old player's number. Otherwise the `HELLO` is refused as name taken as before: someone on another PC, or anyone while the player is still talking, cannot push a player out by using their name. The client does not retry a refusal, so a build restarted within a second of the crash is refused once and must connect again.

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
| 12 | `JOIN_PARTNER` | 0 | both | `u8` player field, `u8` other player, `u16` request id, `u8` kind, then by kind (below) |
| 13 | `FLAG_SET` | 1 | both | `u8` player field, a flag list of 1 to 3600 entries (below) |
| 14 | `FLAG_STATE` | 1 | both | `u8` player field, a flag list of 1 to 3600 entries |
| 15 | `FLAG_SNAPSHOT` | 1 | both | `u16` part, `u16` part count (at least 1, more than part), a flag list of 0 to 3600 entries |
| 16 | `HOST` | 1 | server | `u8` the hosting player's number (1 to 4) |
| 17 | `PLAYER_EQUIP` | 0 | both | `u8` player field, then the components below |

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

The player field of `JOIN_PARTNER`, `FLAG_SET` and `FLAG_STATE` works as in `CHAT`: 0 from a client, the sender's number from the server.

### Join partner (stage 1 step 3)

A warp to another player's scene, in three steps, all on channel 0:

1. The asker sends `JOIN_PARTNER` kind **request**, other player = the one to join, and a request id of its own choosing.
2. The server relays it to that player, with the asker's number in the player field.
3. That player's build answers kind **accept** (where it stands) or **refuse**, other player = the asker, the same request id. The server relays it back the same way.

If the player to join is not in the session, the server itself answers kind refuse, reason 4, with that player's number in the player field. The asker drops an answer whose request id is not its latest request, and gives up on its own after a while with no answer.

| Kind | Value | Then |
|---|---|---|
| request | 0 | nothing |
| accept | 1 | scene (string, 1 to 64 characters), position (3 x f32), rotation (4 x f32 quaternion, x y z w, as in `PLAYER_SAMPLE`) |
| refuse | 2 | `u8` reason: 1 busy (loading, cutscene, menu, dead), 2 declined, 3 cannot warp here (a boss fight, for example), 4 not there (from the server); an unknown reason is kept |

Any other kind is refused, since the rest of the body cannot be read. The other player is 1 to 4 and never the sender itself. Floats must be finite.

### Who hosts

The first player in hosts. When the host leaves, the remaining player with the lowest number takes over; a player who joins later never takes over by joining, even with a lower number. The server tells a joining player who hosts with `HOST` (after `WELCOME`, before its `FLAG_SNAPSHOT`), and tells everyone with `HOST` when the host changes. `HOST` is on channel 1, so a new host learns that it hosts before any `FLAG_SET` relayed to it. Channels are not ordered against each other, so a build may get `HOST` and its snapshot before `WELCOME`; it keeps them until `WELCOME` comes.

### Shared flags (stage 1 step 4)

Only world flags are shared (chests, small keys, doors, heart pieces, songs, dungeon state). Inventory, hearts and magic stay per player, the suggested answer in the plan's open decisions.

The game keeps world flags under two kinds of key (`docs/flags.md`): named flags, by the hash of their name, and 64-bit-key flags (the game's `Bool64bitKey`), by the 64-bit `Hash` of an actor's placement. Chests, pots, rupees, Gold Skulltula tokens and enemies are 64-bit keys.

A flag list is `u16` count, then that many entries. An entry is its key, then its value:

| Field | Type | What |
|---|---|---|
| key kind | `u8` | 0 named, 1 64-bit key. Any other is refused, since the rest of the list cannot be read |
| key | 6 or 12 bytes | by key kind, below |
| kind | `u8` | 0 bool, 1 s32, 2 f32, 3 u32 (an enum flag's hash, or any unsigned value), 4 count delta (an s32 change, `FLAG_SET` only, below); other kinds are refused |
| value | 4 bytes | the value as that kind: a bool is 0 or 1, an f32 is finite |

| Key kind | Fields | Entry size |
|---|---|---|
| 0 named | `u32` hash: Murmur3 x86 32-bit, seed 0, of the flag's name in the game (`flag_hash` in the code); `u16` index: the element of an array flag, 0 for a plain flag | 12 bytes |
| 1 64-bit key | `u64` key: the placement's `Hash`; `u32` table: the hash naming the `Bool64bitKey` table, 0 for the game's one table (the only one found so far) | 18 bytes |

A 64-bit-key flag is always a bool; any other kind with key kind 1 is refused. A list holds the same key at most once: a named flag and a 64-bit key are different flags even with the same number, and so are the same 64-bit key in two tables. A list of more than 3600 entries is refused (that keeps a message of 64-bit keys, 3600 × 18 + 5 bytes, within the `u16` body length).

Only persistent 64-bit keys are shared: `ResetTypeValue` 24 in the game's flag list (chests). Pots and enemies (88, 280) come back on a scene change; shared, a pot one player broke would break for everyone. The list of shared flags is the user's (`SharedFlags` in `coop_host`), so this is a rule for that list, not a check in the codec.

How flags move:

- A player's game changes shared flags: its build sends `FLAG_SET` with every flag changed that frame. The server relays it to the host only; a `FLAG_SET` from the host itself comes back to the host, so the host's build answers every `FLAG_SET` the same way. The game that set them keeps its own change until the answer comes.
- The host's build applies the change to its game, which is the truth, and sends `FLAG_STATE` with the values that came out. The host also sends `FLAG_STATE` for the flags its own game changes. The server keeps every value it relays (so progress stays when the host leaves) and relays it to everyone else. Every player applies a `FLAG_STATE`, even over its own `FLAG_SET`. The server drops a `FLAG_STATE` from anyone but the host.
- On join, the server sends the joining player its whole table as `FLAG_SNAPSHOT`, before any `FLAG_STATE` that comes after: the channel is ordered, so nothing is missed between them. The joiner holds its game until the last part has come (part = part count − 1) and then applies them all.
- A host that starts a session, or takes over from one who left, sends its table to the server as `FLAG_SNAPSHOT` the same way. The server merges it in. A flag the server lacks is kept and sent to everyone else as `FLAG_STATE`. On a clash the server's value stays (it is what every other player already has): the server sends it to the host as `FLAG_STATE`, which the host applies like any other. The server drops a `FLAG_SNAPSHOT` from anyone but the host.
- The server keeps the table while anyone is in and forgets it when the last player leaves. It holds at most 262,144 flags (a server setting); a value for a new flag past that, in a `FLAG_STATE` or a host's `FLAG_SNAPSHOT`, is still relayed but not kept, so a later joiner does not get it.
- A table of more than 3600 flags is split into parts in order, each up to 3600 entries (`split_snapshot` in the code). An empty table is one part with no entries. A table can have at most 65535 parts.
- The server holds a host sending its table to the same limit as any message on channels 0 to 2 (60 a second, bursts of 120), so a host table of up to about 100 parts (over 360,000 flags) fits one burst. The server's own sends have no such limit.

#### Counts

Small keys, Gold Skulltula tokens and heart pieces are counts ([items.md](items.md)): two players can gain or spend at once, and an absolute value would lose one of the two (the host spends a key, 2 → 1, while a player gets one, 2 → 3; the host cannot tell that the 3 started from 2). So a player sends a count's change, not its value:

- A player's build sends a count change as kind 4, count delta: the s32 change since the last value its game had (+1 got, −1 used). Only a named flag carries one, and only in `FLAG_SET`; a `FLAG_STATE` or `FLAG_SNAPSHOT` with kind 4 is refused, so the server's table only ever holds totals. The server relays it to the host like any `FLAG_SET`.
- The host's build adds the delta to its game's value (never below 0) and sends the new total as `FLAG_STATE`, kind 1, which every player applies. It sends the total even when it did not change (the host spent what the player got), since the player waits for it. Its client does not answer a delta itself: it hands it to the game, whose flag watcher sends the total the same frame.
- A player writes a total on top of any change its game made that it has not sent yet, and still sends that change. So every gain and every spend is counted once, whatever the order.
- A count change does not wait for an answer of its own. A delta not answered when the host changes is sent again to the new host (the sum, if there were several); when the new host is us, it is dropped, since our game holds it and our snapshot carries it. A host's own count change goes out as its total. Our own delta that comes back to us as host (sent before we knew we host) is not added again.
- Not covered: a delta that reached a host who left before its `FLAG_STATE` went out is lost with that host's game; one answered by a `FLAG_STATE` for the same flag from another player's change is no longer sent again on a host change.

### Equipment (`PLAYER_EQUIP`)

What a player wears and holds, so the puppet of that player in everyone else's game wears it too ([puppet.md](puppet.md), "Equipment"). On channel 0, reliable: it changes seldom and must not be lost, unlike a sample.

Components, as in `PLAYER_SAMPLE` (id, length, data; unknown ids skipped, a known one at most once and of exactly its length):

| Id | Name | Data | Read from |
|---|---|---|---|
| 1 | `weapon` | string, up to 64 characters | `PlayerStatus.EquipWeapon` |
| 2 | `shield` | string | `PlayerStatus.EquipShield` |
| 3 | `armor` | string (the tunic) | `PlayerStatus.EquipArmor` |
| 4 | `boots` | string | `PlayerStatus.EquipBoot` |
| 5 | `head` | string (a mask, or head armor) | `PlayerStatus.EquipHeadArmor` |
| 6 | `hand` | string (gauntlets) | `PlayerStatus.EquipHandArmor` |
| 7 | `ocarina` | string | `PlayerStatus.EquipOcarina` |
| 8 | `arrow` | string | `PlayerStatus.EquipArrow` |
| 9 | `drawn` | `u8`: bit *i* set when the player's equipment slot *i* is drawn (in hand), clear when sheathed or empty | the EquipmentUser component |

Each string is the game's actor name of the piece, for example `Weapon_Sword_Kokiri` or `Armor_Adult_Goron`; an empty string means nothing in that slot. A component that is left out is unknown (the sender could not read it): the receiver keeps what it had for it (`merge_equipment` in the code). Age is not here: it is in `PLAYER_SAMPLE`.

How it moves:

- A build sends `PLAYER_EQUIP` with every component it can read on its first frame once welcomed, then whenever any of them changes (a field that fails to read for a frame is not a change), and again after a reconnect.
- The server relays it to everyone else like `CHAT`, with the sender's number in the player field. It keeps nothing.
- Every build keeps the last one it sent, and sends it again when a `PLAYER_JOINED` arrives, so a newcomer sees what everyone already wears.

### Pose (`PLAYER_POSE`)

The local Link's skeleton after its animation, so the puppet can copy it bone by bone ([puppet.md](puppet.md), "Pose"). On channel 3 with `PLAYER_SAMPLE`, unreliable: a lost pose is replaced by the next one.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 26 | `PLAYER_POSE` | 3 | both | `u8` player field, `u64` server time, `u32` sequence, `u32` skeleton, `u16` bone count (at most 512), then per bone: `u16` bone (below 512, each above the one before), `u8` parts (bit 0 rotation, bit 1 translation, bit 2 scale; other bits refused), `u8` flags, then the parts it has in that order: rotation 4 x `s16` (quaternion x y z w scaled by 32767; -32768 refused), translation 3 x `s16`, scale 3 x `s16` (both scaled by 8192) |

Only the bones that move are listed; a bone left out keeps what the puppet has. The rotation, translation and scale are the bone's local transform (relative to its parent) after the sender's animation, and flags is the game's flag byte of that bone, passed on unread ([puppet.md](puppet.md), "Pose"). `skeleton` tells the receiver which skeleton the bones belong to; a puppet whose skeleton differs ignores the message. The time works as in `PLAYER_SAMPLE`. The server relays it like `PLAYER_SAMPLE`, with the sender's number in the player field, and keeps nothing.

### Time of day (`TIME_OF_DAY`)

Every game shows the host's time of day ([time.md](time.md)).

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 27 | `TIME_OF_DAY` | 0 | both | `u8` player field, `u32` seconds since midnight (below 86,400), `u8` flags (bit 0 jump; other bits refused) |

- The host sends it about once a second; a game more than two in-game minutes away from it moves its clock there.
- A game whose clock jumped by itself (the Sun's Song, sleeping, an event) sends it with the jump bit, and every other game, the host's too, takes it at once.
- The server relays it to everyone else like `CHAT`, with the sender's number in the player field.

### Objects (stage 5)

Shared enemies and projectiles; the design and the reasons are in [objects.md](objects.md). The server keeps an object table (below); the hook does not send these yet.

The server's rules today (`server/session.cpp`):

- A player's scene is the one of its latest `OBJECT_PRESENT` or `PLAYER_SAMPLE` scene. Changing scene lets go of everything it had loaded or made in the old one.
- `OBJECT_PRESENT` is not relayed. The first game to list a placed object owns it (epoch 0) and everyone in that scene gets `OBJECT_OWNER`; a later game listing it gets `OBJECT_OWNER` alone. Made ids in the list are ignored.
- `OBJECT_SAMPLE` and `OBJECT_REMOVE` are relayed only from the owner, with the object's current epoch, to the others in its scene; `OBJECT_SPAWN` of a made id only from the maker named in the id, once; of a placed id only from its current owner, in its scene (relayed, nothing registered). Anything else is dropped.
- `DAMAGE` goes to the owner alone, whatever its epoch; dropped when there is no owner or it comes from the owner.
- When the owner of a placed object unloads it, changes scene or leaves, the game with the lowest number that still has it takes it with epoch + 1: it gets `HANDOVER_STATE` with the last relayed sample (player field: the old owner) when there was one, then everyone in the scene gets `OBJECT_OWNER`. Nobody left: the object is forgotten.
- A made object goes when its maker lets go of it: the others in the scene get `OBJECT_REMOVE` (gone, at its last position) from the server.
- A removed placed object stays, owned by nobody, until no game lists it, so a copy that is still loaded cannot bring it back.
- Not yet: `HANDOVER_ASK` (a client's `HANDOVER_STATE` is dropped), the fight lock, the distance hand-over and groups.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 18 | `OBJECT_PRESENT` | 2 | both | `u8` player field, scene (string, up to 64), `u16` count (up to 1024), then per object: object id, `u8` flags (bit 0 awake, bit 1 in a fight, bit 2 targeting our player). No id twice |
| 19 | `OBJECT_OWNER` | 2 | server | object id, `u8` owner (0 none, else 1 to 4), `u16` epoch, `u32` group (0 none) |
| 20 | `HANDOVER_ASK` | 2 | server | object id, `u16` epoch, `u8` to player (1 to 4) |
| 21 | `HANDOVER_STATE` | 2 | both | `u8` player field, object id, `u16` epoch, then object components |
| 22 | `OBJECT_SPAWN` | 2 | both | `u8` player field, object id (kind made; or kind placed, from its owner: the actor of a placed object that other games may not have loaded, such as a Gold Skulltula one game shook out of a tree), actor name (string, 1 to 64), `u8` flags (bit 0 projectile), then object components |
| 23 | `OBJECT_SAMPLE` | 4 | both | `u8` player field, object id, `u16` epoch, then object components |
| 24 | `OBJECT_REMOVE` | 2 | both | `u8` player field, object id, `u16` epoch, `u8` reason (0 gone, 1 dead, 2 exploded, 3 hit, 4 picked up), position (3 x f32) |
| 25 | `DAMAGE` | 2 | both | `u8` player field, object id, `u16` epoch, `s32` amount (0 or more), `u8` cause (0 unknown, 1 sword, 2 arrow, 3 fire arrow, 4 ice arrow, 5 light arrow, 6 seed, 7 bomb, 8 nut, 9 boomerang, 10 hookshot, 11 hammer, 12 magic; others kept), position (3 x f32) |

An **object id** is 10 bytes: `u8` kind (0 placed, 1 made), `u8` maker (0 for placed, 1 to 4 for made), `u64` value (the placement hash, or the maker's own counter). Any other kind, or a maker that does not fit the kind, is refused.

Object components use the same scheme as `PLAYER_SAMPLE` (`u16` id, `u16` length, unknown ids skipped, each known id at most once, `time` required): 1 `time` (12), 2 `position` (12), 3 `velocity` (12), 4 `rotation` (16), 7 `animation` (as in `PLAYER_SAMPLE`), 16 `life` (`s32` life, `s32` max, 8), 17 `fuse` (f32 seconds left, 4), 18 `state` (`u8`: bit 0 in a fight, bit 1 held, bit 2 asleep, 1). Floats must be finite.

The player field follows the `PLAYER_SAMPLE` rule (0 from a client, the sender's number when relayed).

## Later stages (outline only)

- **Stage 3:** `PLAYER_STATUS` decides whether puppets show during cutscenes. `JOIN_PARTNER` (above) asks for a warp to another player's scene.
- **Stage 4:** `FLAG_SET`, `FLAG_STATE` and `FLAG_SNAPSHOT` (above) share world flags through the host.
- **Stage 5:** the object messages (below). The server's ownership rules and the hold times are not in a build yet.

## Versioning

- The protocol version goes up whenever a message layout changes in a way an older reader cannot skip.
- Adding a component or a message type that old readers may ignore does not change the version.
- Types 12 to 27 (`JOIN_PARTNER`, the flag messages, `HOST`, `PLAYER_EQUIP`, the object messages, `PLAYER_POSE` and `TIME_OF_DAY`) were added this way and the version stayed 0: no earlier layout changed, and a build without them reports them as unknown and ignores them. A session with such a build still runs, but that player cannot warp to a partner or share flags, and its puppets wear the default gear.
- The key kind at the start of each flag entry (64-bit keys) changed the flag list layout. Nothing with the old layout was released, so the version stayed 0.
- Kind 4, count delta, was added to the flag entry the same way. A build without it refuses a `FLAG_SET` that carries one (an unknown kind), so a host must have it; a player without it never sends one and takes totals as it always did.
- Version 1 (2026-11-09, the first test release): no layout changed, but the version went up so that the release and older development builds, which lack those messages, refuse each other at `HELLO` instead of playing a half-working session.
- Version 2 (2026-11-09): `OBJECT_SPAWN` may carry a placed id, sent by that object's owner to name its actor for games that lack it. Version 1 readers refuse such a spawn, so the version went up.
- Version 3 (2026-11-09): `TIME_OF_DAY` (type 27). No layout changed; the version went up so that the 0.1.0-test1 release, which never sends the host's clock, and this build refuse each other at `HELLO` instead of showing different skies.
