# Wire protocol, version 9

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
- Datagrams of at most **1100 bytes** (ENet's MTU, `kDatagramBytes`), set on the server and in every game; ENet takes the smaller of the two sides at connect, so this is not part of the protocol version. ENet's default 1392 made 1420-byte IP packets, more than a VPN carries (Hamachi about 1404, Tailscale 1280; issue 143). Larger messages are split by ENet into several datagrams.
- Smaller paths (issue 143, `coop/net/enet_path.hpp`): when a reliable command too big for a 576-byte datagram has been sent 3 times and then once more without an ACK, each time while packets sent after it were acknowledged, the path loses full datagrams and no full-size command has got through on that connection yet (plain loss and short outages do not count: small commands are not looked at, one full-size command acknowledged (within 32 bytes of the datagram size), as the join snapshot is at once on a good line, ends the check for that connection, and a big one resent after the line is back gets through). The server then ends that connection with ENet disconnect data `0x534D4C4C` ("SMLL") and remembers the address for 60 s; the game connects again at once with **576-byte** datagrams (ENet's smallest, 604 bytes on the wire) and keeps them until it is restarted. A game that sees it on its own sends does the same. A remembered address that connects again with bigger datagrams within those 60 s gets the same disconnect at once, before anything big is sent, and is then forgotten. The game never switches back until it is restarted. No message type or version change: the size is agreed by ENet at connect.
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
2. The server answers `WELCOME` or `REFUSED`; a server with a join password first answers `JOIN_CHALLENGE` (see "Join password" below), and `WELCOME` or `REFUSED` follows the client's `JOIN_PROOF`.
   - `WELCOME`: `u8` the player's number (1 to 4), `u64` server time, `u16` tick rate (not 0), `u8` count (at most 4), then for each player already in: `u8` number, nickname. Every number in it is different, and the new player's own is not in the list.
   - `REFUSED`: `u8` reason, which the client shows: 1 wrong protocol version, 2 wrong game build, 3 server full, 4 name taken, 5 bad name (a nickname of nothing but spaces), 6 bad password (the join password did not match, protocol 9). A reason the client does not know is kept and shown as unknown.
3. No `HELLO` within 10 seconds: the server drops the connection.
4. One address may hold at most 4 connections that have not sent `HELLO` yet (a server option; 0 for no limit). The server drops one more at once, without `REFUSED`.

### Join password (protocol 9, types 191 to 195)

The host may set a join password (`coop_server --password TEXT`, or `--password-env NAME` to read it from an environment variable so it is not on the command line; the kit's `server_password` in `coop-options.ini`). Empty means none, and the handshake is as above.

1. With a password, the server checks the `HELLO`'s protocol version (a wrong one is refused as before) and then, before any other check, answers `JOIN_CHALLENGE` (191, channel 0, server only): 32 bytes, a fresh random nonce for this connection (the OS's random source).
2. The client answers `JOIN_PROOF` (192, channel 0, client only): 32 bytes, HMAC-SHA256 keyed with the password's UTF-8 bytes over the nonce. A client with no password answers with the empty password's proof. It answers one challenge per connection.
3. The server compares in constant time. Right: the usual checks (game build, name, full) and `WELCOME` or `REFUSED`. Wrong: `REFUSED` 6, bad password, and the connection is closed; the client shows "wrong join password" and does not retry.

The password never goes on the wire, and a proof is good for one nonce only, so a recorded proof is refused on any other connection. The server sends one challenge per connection (a second `HELLO` gets none), takes one proof, and the 10 s `HELLO` timeout covers the proof too; a connection waiting for its proof counts as half-open. A `JOIN_PROOF` from a joined player is dropped, never relayed. Guessing is slowed down: 5 wrong proofs from one address within 60 s close that address at connect for 60 s (`SessionConfig::max_wrong_passwords`, stat `password_blocked`; the log line names no address). Someone who records a challenge and its proof can still try passwords offline against it, as with any password check without a key exchange: use a long password used nowhere else. Passwords are at most 128 bytes with no control characters; the kit also refuses spaces.

Other datagrams on the server's port, outside the protocol: STUN answers and NAT punches for the invite code (`coop_server --invite`; datagrams starting with `OCP` and version 1, signed with a key made from the join password). They are handled before ENet sees them, use no message types and change nothing in the session; the punch only opens the way through the routers, and the join password check above still decides who joins. [play-online.md](play-online.md), "How the invite code works".

### Rejoin after a crash

A dropped or killed emulator stays in the session until the server times it out, so its new connection would be refused as name taken. A `HELLO` whose nickname (ignoring ASCII case) a player already holds takes that player's place only when all three hold:

- the new connection showed that player's rejoin token: after each `WELCOME` the server sends `REJOIN_TOKEN` (345), 32 fresh random bytes for that place; the client keeps it and sends `REJOIN_CLAIM` (346) with it before its `HELLO` on every later connection (the first claim on a connection counts; one after `HELLO` is dropped, stat `stray_rejoin_claim`). It works with a join password too: claim, `HELLO`, challenge, proof. The server compares in constant time, and each `WELCOME` brings a new token, so an old one is spent. The token goes in the clear, as everything on ENet does: someone who can read the link and is behind the same address could still take the place. That is outside issue 163 (two honest players behind one router);
- it comes from the same IP address as that player, and
- the server has heard nothing from that player for 1 second (a server option; 0 turns the takeover off). Every build answers the server's `PING` every 250 ms, so a live player is never this quiet.

Issue 163: before protocol 10 the token was not needed, so two players behind one router (one public address) could push each other out by name during a stall of over a second. A `HELLO` that is quiet and from the same address but shows no token, or a wrong one, is now refused as name taken (stat `rejoin_no_token`). The token lives in the client only, not on disk: a reconnect of a running game (connection lost) takes its place at once; a restarted emulator has no token and waits, through the name taken retries below, for the server to time the old player out (10 s).

Then the old player is dropped and reported as `PLAYER_LEFT` timed out, with the host handed on as on any leave, and the new connection is welcomed with the old player's number. Otherwise the `HELLO` is refused as name taken as before: someone on another PC, or anyone while the player is still talking, cannot push a player out by using their name. A build restarted within that second is refused, so the client asks again: a `REFUSED` name taken is tried again every 1.5 s for 12 s (`name_taken_retry_ms`, `name_taken_retry_for_ms` in `ClientConfig`; either one 0 turns it off, and the interval is at least 100 ms) before it is reported. The 12 s count from the first name taken in a row: an attempt that ends any other way (timeout, lost connection) closes the window, so a later name taken gets the full 12 s again. That covers the second of silence on the same PC, and also a restart that comes from another address (the same PC joining by its LAN address after `127.0.0.1`), which waits for the server to time the old player out (10 s). Any other refusal is reported at once and not retried. While it retries the client is in `waiting_retry` and reports nothing (`retrying_name_taken()` is true, and the player list shows "name in use, asking again"); a name that someone else is really using is reported after the 12 s.

## Keep-alive and clock

- `PING` every 250 ms from each side with the sender time; `PONG` echoes it with the receiver time. This gives the round trip and the clock offset.
- `PING` is `u64` sender time. `PONG` is `u64` the time echoed from the `PING`, then `u64` the `PONG` sender's time.
- ENet's own timeout drops a peer after 10 seconds of nothing. A drop is reported to the others as `PLAYER_LEFT` with the reason (left, timed out, kicked).
- Waiting data: each side lets a peer's received data wait up to a cap before ENet refuses more (server 1 MB a player, game 8 MB). ENet 1.3.18 does not take an unreliable message off that count when it throws it away unassembled (one of its fragments was lost), so on a line that loses packets the count grew with every lost pose until ENet refused everything from that peer, reliable packets too, without acknowledging them, and the other side dropped the player after 10 s (issue 175: reconnects every few minutes over the internet, sooner once the game's cap went from 32 MB to 8 MB). Both sides now set the count to what really waits after each service round (`coop/net/enet_waiting.hpp`). The caps stay.
- Every drop is logged with its reason. Server: `player N left: NAME (timed out: the connection was lost, nothing from it for N ms; round trip, loss, waiting bytes)`, `(timed out: nothing heard for N ms)`, `(the connection closed, last packet N ms ago; ...)` or `(replaced: the same name came back from the same address)`; at stop, `lost fragment bytes taken off ENet's count N`. Game: `coop: connection lost; reconnecting (...)` with the link, the waiting bytes and the lost fragment bytes taken off the count on that connection.

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
| 135 | `FLAG_RESET` | 1 | both | `u8` player field, a flag list of 1 to 3600 entries, no count deltas (protocol 7; types 135 to 139 are for saves) |
| 150 | `FLAG_ORIGIN` | 1 | server | `u8` player (1 to 4: who asked), a flag list of 1 to 3600 entries, count deltas allowed: the FLAG_SET the server just gave the host, sent to every other player but the host and the asker, before the host's answer (protocol 8; types 150 to 154 are for "who did it", the chat notices in [setup.md](setup.md)) |
| 191 | `JOIN_CHALLENGE` | 0 | server | 32-byte nonce (protocol 9, "Join password"; 191 to 195 kept for it) |
| 192 | `JOIN_PROOF` | 0 | client | 32-byte HMAC-SHA256(password, nonce) |
| 206 | `SAVE_ASK` | 0 | both | `u8` player field: a guest asks the host for its save (protocol 9; types 206 to 215 are for the save transfer, below) |
| 207 | `SAVE_PART` | 0 | both | `u8` player field, `u8` to player (0 every guest), `u32` transfer id, `u8` reason (1 join, 2 host load), `u32` total size (1 to 1 MiB), `u32` CRC-32 of all of it, `u16` index, `u16` count, then the part's bytes (below) |
| 320 | `FISH_LURE` | 2 | client | `u8` player field, `u16` count (0 to 256), then per fish: object id (10 bytes), `u16` centimetres from the sender's nearest cast lure (`0xffff`: none within 6 m), `u8` flags (bit 0 hooked, bit 1 drawn; both 0 with `0xffff`); no fish twice. Not relayed (protocol 10; types 320 to 324 are for fishing, "Fish and lures" below) |
| 345 | `REJOIN_TOKEN` | 0 | server | 32-byte random token, sent right after `WELCOME` (protocol 10, "Rejoin after a crash"; 345 to 349 kept for it) |
| 346 | `REJOIN_CLAIM` | 0 | client | 32-byte token from the last `REJOIN_TOKEN`, sent before `HELLO` on a new connection |
| 355 | `DUNGEON_EVENT` | 0 | both | `u8` player field, scene (string, 1 to 64), event name (string, 1 to 96, no `HavePlayedEvent.`): a room gimmick's event started in the sender's game. Relayed to the others in that scene (only while the sender is in it), never kept (protocol 10, branch `deku-dungeon-2`; 356 to 359 kept; [dungeon.md](dungeon.md), "Live events") |
| 385 | `PLAYER_RIDE` | 3 | both | `u8` player field, `u8` flags (bit 0 riding; other bits refused), horse actor name (string, up to 64 characters; not empty when riding), offset 3 x f32 (each within 10 m), rotation 4 x f32 quaternion, `u8` 1 when an animation follows (string name up to 64 characters, f32 frame, f32 rate) else 0. Relayed (protocol 10, issue 199; 385 to 389 are for riding, [Riding](#riding-player_ride)) |
| 16 | `HOST` | 1 | server | `u8` the hosting player's number (1 to 4) |
| 17 | `PLAYER_EQUIP` | 0 | both | `u8` player field, then the components below |
| 251 | `PLAYER_LIFE` | 0 | both | `u8` player field, `u16` life, `u16` max life, quarter hearts (protocol 10; 251 to 255 are the co-op HUD's, [Hearts](#hearts-player_life)) |

Type numbers are never reused or renumbered; new types are added at the end.

Protocol 10 also keeps 360 to 364 for `cutscene-snap` (issue 194); none is used yet (the fix needs no message).

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

A player sends `PLAYER_SAMPLE` every frame it moves (up to 60 a second: samples are at least 14 ms apart, so high refresh (Ctrl+U, up to 165 presents a second) sends no more; a scene or age change goes at once) and at least 4 a second while still. Receivers keep the last 16 samples per player, show them about 100 ms behind (tuned by measuring), interpolate between samples and extrapolate up to 150 ms from the last one; beyond that the puppet holds and fades.

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

### Saves: the host's save decides (protocol 7, `FLAG_RESET`)

Caleb (2026-11-11): "Let's make it follow the hosts save." A host's build sends `FLAG_RESET` after its own save load, with every shared flag its load changed, as its game has them now, 0s included. The server takes it from the host only, keeps each value (replacing what it had, as with `FLAG_STATE`) and relays it to everyone else with the host's number. Every player writes each value exactly: no merging (max, OR) and no one-way rule, so items, keys, chests and switches go back to the host's save. A player's own save load sends nothing and changes nothing for the group: that game takes the group's values back. Progress made while playing still merges as before. When the host leaves, the new host's state is the truth from then on, with no reset. How a build tells a save load from a door, and when it sends, is in [flags.md](flags.md), "Saves". A build without protocol 7 does not know type 135; all players need the same release.

### Save transfer: the host's save file (protocol 9, `SAVE_ASK` and `SAVE_PART`)

Issue 167: a guest plays from the host's save file, not its own. The data is one save slot of the game's save folder (`slot_NN\progress.sav` and `caption.sav`, about 170 KB), packed by `coop/host/save_transfer.hpp` (`OOTSAVE1`, then each file's name and bytes; only those two names are allowed, never `option.sav`, `account.sav` or a path).

- A guest's game (one that uses the co-op save folder, [flags.md](flags.md), "The host's save file") sends `SAVE_ASK` once a run, when it is connected and knows the host. The server passes it to the host only, with the asker's number; an ask from the host itself or with no host is dropped.
- The host takes one ask per player while it stays (a repeated ask is ignored and logged; a player who left may ask again with its number), and keeps at most four slots' worth of parts waiting (a send past that is dropped and logged).
- The host answers with its newest slot (by `progress.sav`'s write time) as `SAVE_PART`s to the asker (`to player` = the asker, reason 1). After its own save load the host waits until the game writes a save after it (the game saves at every door), then sends that slot to every guest (`to player` 0, reason 2); an ask that came meanwhile gets that one.
- Parts: the packed slot cut into 16384-byte parts (the last one the rest), `count` = ceil(total / 16384), at most 64 parts for 1 MiB. Every part carries the whole header, so each is checked alone: the codec refuses a wrong count, index, part size, reason, a size over 1 MiB, and reason 1 to player 0. The host sends a part a frame; the server takes `SAVE_PART` from the host only (outside the host's byte and message budgets: a dropped part would leave the transfer unfinished) and passes it to the named player, or to every other player for 0; a part for a player who is not there is dropped.
- A guest keeps the parts of one transfer id (a newer id starts again), takes them out of order or twice, refuses a part not from the host or whose header differs from the transfer's, and checks the CRC-32 (zlib's) when all are in: a damaged save is dropped and the next one waited for.
- A build without these messages refuses them as unknown types, so the server and every game must be protocol 9.

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

### Riding (`PLAYER_RIDE`)

Issue 199 (protocol 10): a player riding Epona stays seen by the others, on a copy of the horse ([puppet.md](puppet.md#riding-issue-199)). While its Link's command names a horse, a build sends `PLAYER_RIDE` about 30 times a second on the samples channel: the horse it sits on (`Animal_Epona_Adult`), the horse's place minus its Link's and the horse's turn, both in its Link's own frame (turned back by its rotation), and the horse's AS slot 0. Getting off sends riding off (flags 0, no name) three times. The others put the copy at the rider's puppet plus the offset turned by the puppet's turn, so horse and puppet move together whatever the delay; a ride not heard of for 0.6 s is over (the channel is unreliable). The server relays it to everyone else with the sender's number. A build without it ignores the type; its riders show on foot in their riding pose.

### Hearts (`PLAYER_LIFE`)

Protocol 10, type 251 (251 to 255 are the co-op HUD's). Each player's hearts, for the HUD's player blocks ([lichii.md](lichii.md#the-game-look)) and PvP. On channel 0, reliable; the server relays it to the others like `PLAYER_STATUS` and keeps nothing.

| Bytes | Field |
|---|---|
| u8 | player: 0 from a client, 1 to 4 relayed |
| u16 | life now, in quarter hearts (4 = one heart), as the game keeps it |
| u16 | the most life can be (heart containers), same unit; 0: not known |

Both at most 160 (`kMaxLifeQuarters`, 40 hearts); more is refused. A game sends it when its Link's hearts change (at most ten a second), every 5 s anyway, and again when a player joins (`CoopHud::Frame`, `lichii/hook/coop_hud.cpp`). The values are Link's own Life component as the object walk reads it (`CoopObjects::OurLife`, the one source; `LICHII_COOP_OBJECTS=1`, on in the kit), not the `PlayerStatus.Life` flag, which is a copy ([items.md](items.md), live check 2). A receiver forgets a player's hearts when they leave or the connection drops. An older game skips the type as unknown.

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
| 10 | `held` | string: the item in equipment slot 3, the slot of the slingshot and the bow (for example `Weapon_Range_SlingShot`); in hand when bit 3 of `drawn` is set | the name of the actor in that slot |
| 11 | `weapon_slot` | string: the actor in equipment slot 0 (Weapon), which is not always the equipped sword (a Deku stick); in hand when bit 0 of `drawn` is set | the name of the actor in that slot |
| 12 | `sub_weapon` | string: the actor in slot 1 (SubWeapon); bit 1 of `drawn` | the same |
| 13 | `third_weapon` | string: the actor in slot 2 (ThirdWeapon); bit 2 | the same |
| 14 | `tool` | string: the actor in slot 5 (Tool); bit 5 | the same |
| 15 | `sub_tool` | string: the actor in slot 6 (SubTool); bit 6 | the same |

Each string is the game's actor name of the piece, for example `Weapon_Sword_Kokiri` or `Armor_Adult_Goron`; an empty string means nothing in that slot. A component that is left out is unknown (the sender could not read it): the receiver keeps what it had for it (`merge_equipment` in the code). Age is not here: it is in `PLAYER_SAMPLE`.

How it moves:

- A build sends `PLAYER_EQUIP` with every component it can read on its first frame once welcomed, then whenever any of them changes (a field that fails to read for a frame is not a change), and again after a reconnect.
- The server relays it to everyone else like `CHAT`, with the sender's number in the player field. It keeps nothing.
- Every build keeps the last one it sent, and sends it again when a `PLAYER_JOINED` arrives, so a newcomer sees what everyone already wears.

### Pose (`PLAYER_POSE`)

The local Link's skeleton after its animation, so the puppet can copy it bone by bone ([puppet.md](puppet.md), "Pose"). On channel 3 with `PLAYER_SAMPLE`, unreliable: a lost pose is replaced by the next one. Since protocol 8 a player also sends a `PLAYER_POSE` for each piece out in Link's hands (the slingshot, the bow), with that piece's own skeleton id and every bone ([puppet.md](puppet.md), "Held pieces"); the receiver keeps one track per skeleton id and writes each onto the piece of that skeleton. The layout is unchanged.

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

### Field grass (`GRASS_CUT`)

One cut of the field grass the foliage module draws (no actor; [world.md](world.md), "Field grass"), as the sender's game queued it.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 28 | `GRASS_CUT` | 0 | both | `u8` player field, `str` scene (1 to 64 chars), 3 x `f32` position (finite), `f32` radius (above 0, at most 100), `u8` flags (the game's, passed on unread), 9 x `f32`: three points of the swing (each finite or NaN) |

- Sent once for each cut the sender's game makes; a cut it made for another player is not sent back.
- A game in the same scene makes the same cut; any other drops it.
- The server takes the cut's scene as the sender's scene, keeps the cut in that scene's list (at most 512, the oldest go first) and relays it to the other players in that scene, with the sender's number in the player field. A cut the scene's list already has (position and radius the same, bit for bit) is neither kept again nor relayed.
- A player whose scene changes to one with a list (its `PLAYER_SAMPLE` scene, `OBJECT_PRESENT` or a cut of its own), a late joiner too, gets every cut in it, oldest first. A scene's list is forgotten when no player is in it any more: the grass grows back in a game that loads the scene again, so the cuts end with the last game that still shows them.

### Shared world objects (`WORLD_BREAK`, `WORLD_MOVE`, `WORLD_PLACE`)

Placed breakables and movables, by placement hash ([world.md](world.md), "Broken for everyone, moved for everyone"). All three start with `u8` player field, `str` scene (1 to 64 chars), `u64` placement hash (not 0, not all ones), and go to the players in the sender's scene.

| Type | Name | Channel | From | Body after the head |
|---|---|---|---|---|
| 32 | `WORLD_BREAK` | 0 | both | 3 x `f32` position (finite), `u8` flags (bit 0: restored; bit 1: kept, a small-key door's lock, [dungeon.md](dungeon.md) "Locked doors"; not both; other bits refused) |
| 33 | `WORLD_MOVE` | 4 | both | `u64` server time, 3 x `f32` position, 4 x `f32` rotation (unit quaternion x y z w), `u8` flags (bit 0: carried) |
| 34 | `WORLD_PLACE` | 0 | both | 3 x `f32` position, 4 x `f32` rotation |
| 35 | `WORLD_HIT` | 0 | both | 3 x `f32` where the sword was, 3 x `f32` where the player stood (a cut sign) |
| 80 | `SIGN_CUT` | 0 | both | `u8` player, scene, `u64` hash, `u16` count (1 to 48), then count x (`u64` key, `u64` value): the words of the sign's components its cut changed. Key = (2 + component table entry, below 0x60) << 32, ored with the byte offset in that component (below 0x200, a multiple of 8) |

The server keeps a scene's breaks (at most 1,024, a repeated one is not passed on, `restored` removes one; a `kept` break, at most 256 a scene, is kept for the whole session, also while nobody is in the scene, and a second player's `kept` break of the same object is passed on though not kept again; at most 2,048 kept breaks in all; they are all forgotten when the host loads a save, `FLAG_RESET`). Only a lock's break carries `kept` (the hook sets it for `Obj_DoorChainLock` actors only); the server cannot check it, so a game sending `kept` for anything else keeps that object gone for the session and each object's last `WORLD_PLACE` (at most 256 objects) while a player is in the scene, and sends them to a player who enters it. `WORLD_MOVE` is passed on only, and dropped when the sender is not in that scene. The last 4 `WORLD_HIT` of each object (at most 128 objects a scene) are kept and sent the same way, and the latest `SIGN_CUT` of each sign (at most 128 a scene) after them. Types 29 to 31 are the enemy ownership messages.

### Objects (stage 5)

Shared enemies and projectiles; the design and the reasons are in [objects.md](objects.md). The server keeps an object table (below); the hook does not send these yet.

The server's rules today (`server/session.cpp`):

- A player's scene is the one of its latest `OBJECT_PRESENT` or `PLAYER_SAMPLE` scene. Changing scene lets go of everything it had loaded or made in the old one.
- `OBJECT_PRESENT` is not relayed. The first game to list a placed object owns it (epoch 0) and everyone in that scene gets `OBJECT_OWNER`; a later game listing it gets `OBJECT_OWNER` alone. A made id in the list counts only for a made enemy the server knows (the lister has its copy and may be given it); unknown made ids and projectiles are ignored. Every 10 s the answer to a list is `OBJECT_OWNER` for everything in it.
- `PLAYER_SAMPLE` and `OBJECT_PRESENT` both say the sender's scene. One that names a new scene for its own kind changes the player's scene at once; one that repeats the scene its kind said before, after the other kind moved the player on, is late and is ignored.
- At a player's first `OBJECT_PRESENT` in a scene the server sends it every `OBJECT_SPAWN` still alive there (player field: the owner; the last relayed state), each followed by `OBJECT_OWNER` when the owner is not the maker or the epoch is not 0.
- `OBJECT_SAMPLE` and `OBJECT_REMOVE` are relayed only from the owner, with the object's current epoch, to the others in its scene; `OBJECT_SPAWN` of a made id only from the maker named in the id, once; of a placed id only from its current owner, in its scene (relayed, nothing registered). Anything else is dropped.
- `DAMAGE` goes to the owner alone, whatever its epoch; dropped when there is no owner or it comes from the owner. On a boss or a part of its group, since protocol 10 (gohma-sync) only one of amount 0 (a stun, below) from a non-owner asks the boss over, at most every 8 s; a hit that takes life is the owner's to apply and the boss stays (`boss_hit_handover` true: any hit asks it over, as before; issue 150, [dungeons-boss.md](dungeons-boss.md)). It starts the object's fight (12 s, 60 s for a boss).
- A `DAMAGE` of amount 0 is a stun with no life lost: a boss copy whose own game went into a stun the owner's boss is not in (a seed in Gohma's eye) sends one, at most every 2 s, so the server hands the boss to that player. The owner's game takes it as no hit (issue 150).
- Off by default since protocol 10 (gohma-sync; `boss_near_hold_us` 4000000 turns it on): a boss in its fight also moves to a fit player within 15 m of it and 4 m nearer than its owner for 4 s in a row (not while the owner hits it, not within 8 s of the last change), since the boss's AI chases only its owner's Link (issue 150).
- The server moves an object by the rules in [objects.md](objects.md) ("Enemies": the player near it out of a fight, a hit from the player near it, an owner that is loading, paused 6 s or dead 10 s, a group member after its root). It sends the owner `HANDOVER_ASK`; the owner answers `HANDOVER_STATE` with the ask's epoch (anything else is dropped); the server then sends the new owner `HANDOVER_STATE` with epoch + 1 (player field: the old owner) and everyone in the scene `OBJECT_OWNER`. No answer in 500 ms: the same with the last relayed sample. Until then the old owner's samples are still relayed.
- `OBJECT_REMOVE` with reason dead from the owner before the last change, with the epoch before, is taken for 2 s after it and passed on with the current epoch.
- An owner also sends `OBJECT_REMOVE` reason dead for a placed enemy that died in its game (issue 133, [objects.md](objects.md) "Kills and woken enemies"); the server keeps the id, owned by nobody, until no game lists it. Receivers end their copy. No new message: the server took this from protocol 4 on. The server also remembers it per scene, and a game that lists it later without having seen it die (a late entrant or a rejoin) gets the same `OBJECT_REMOVE` dead from the server, with no owner, until a game that saw it die lists it again (respawned) or the scene is empty ([objects.md](objects.md) "Kills for late entrants").
- `OBJECT_GROUP` is not relayed. An entry is taken when both ids are known in the sender's scene and the sender has the member loaded; a root that follows another root gives its root; an entry naming itself leaves its group. `OBJECT_OWNER` then carries the group (the same number for the root and its members) and is sent to the scene when it changes.
- When the owner of a placed object unloads it, changes scene or leaves, the nearest game that still has it (the lowest number when places are not known) takes it with epoch + 1: it gets `HANDOVER_STATE` with the last relayed sample (player field: the old owner) when there was one, then everyone in the scene gets `OBJECT_OWNER`. Nobody left: the object is forgotten.
- A made `OBJECT_SPAWN` is sent for a rolled enemy (the night Stalchild) and, from gohma-larva on, for a larva that hatched from an egg the sender owns; that sender then ends the egg with `OBJECT_REMOVE` reason gone, and receivers delete their egg copy ([objects.md](objects.md), "Hatched enemies"). No new message and no change on the server: it took both from protocol 2 on.
- A made object goes when its owner lets go of it: the others in the scene get `OBJECT_REMOVE` (gone, at its last position) from the server. A made enemy another game has listed goes to that game instead, like a placed one. An owner that stops listing its made enemy without an `OBJECT_REMOVE` has let go of it after 1 s.
- A removed placed object stays, owned by nobody, until no game lists it, so a copy that is still loaded cannot bring it back.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 18 | `OBJECT_PRESENT` | 2 | both | `u8` player field, scene (string, up to 64), `u16` count (up to 1024), then per object: object id, `u8` flags (bit 0 awake, bit 1 in a fight, bit 2 targeting our player: from the owner a fight; from another game its copy chases that game's Link, and out of a fight the server hands the object to it after 500 ms, issue 101). No id twice |
| 19 | `OBJECT_OWNER` | 2 | server | object id, `u8` owner (0 none, else 1 to 4), `u16` epoch, `u32` group (0 none) |
| 20 | `HANDOVER_ASK` | 2 | server | object id, `u16` epoch, `u8` to player (1 to 4) |
| 21 | `HANDOVER_STATE` | 2 | both | `u8` player field, object id, `u16` epoch, then object components |
| 22 | `OBJECT_SPAWN` | 2 | both | `u8` player field, object id (kind made; or kind placed, from its owner: the actor of a placed object that other games may not have loaded, such as a Gold Skulltula one game shook out of a tree), actor name (string, 1 to 64), `u8` flags (bit 0 projectile), then object components |
| 23 | `OBJECT_SAMPLE` | 4 | both | `u8` player field, object id, `u16` epoch, then object components |
| 24 | `OBJECT_REMOVE` | 2 | both | `u8` player field, object id, `u16` epoch, `u8` reason (0 gone, 1 dead, 2 exploded, 3 hit, 4 picked up), position (3 x f32) |
| 29 | `OBJECT_GROUP` | 2 | client | `u8` player field, `u16` count (1 to 256), then per entry: member object id, root object id, `u8` flags (bit 0 the member is a boss; other bits refused). No member twice |
| 25 | `DAMAGE` | 2 | both | `u8` player field, object id, `u16` epoch, `s32` amount (0 or more), `u8` cause (0 unknown, 1 sword, 2 arrow, 3 fire arrow, 4 ice arrow, 5 light arrow, 6 seed, 7 bomb, 8 nut, 9 boomerang, 10 hookshot, 11 hammer, 12 magic; others kept), position (3 x f32) |

An **object id** is 10 bytes: `u8` kind (0 placed, 1 made), `u8` maker (0 for placed, 1 to 4 for made), `u64` value (the placement hash, or the maker's own counter). Any other kind, or a maker that does not fit the kind, is refused.

Object components use the same scheme as `PLAYER_SAMPLE` (`u16` id, `u16` length, unknown ids skipped, each known id at most once, `time` required): 1 `time` (12), 2 `position` (12), 3 `velocity` (12), 4 `rotation` (16), 7 `animation` (as in `PLAYER_SAMPLE`), 16 `life` (`s32` life, `s32` max, 8), 17 `fuse` (f32 seconds left, 4), 18 `state` (`u8`: bit 0 in a fight, bit 1 held, bit 2 asleep, 1), 19 `slots` (protocol 10, branch gohma-sync, issue 150: a boss's AS slots after slot 0, such as Gohma's eye guard and eyelids: `u8` count up to 8, then per slot `u8` slot 1 to 31, name as in `animation` (empty: the slot plays nothing), `f32` frame, `f32` rate; no slot twice; sent only for bosses, older builds skip it). Floats must be finite. Types 370 to 374 were kept for gohma-sync and are not used: the slots ride in the samples, so no new message was needed.

The player field follows the `PLAYER_SAMPLE` rule (0 from a client, the sender's number when relayed).

Fish and lures (issue 151). A landed fish is `OBJECT_REMOVE` reason dead, and lures are `OBJECT_SPAWN`/`OBJECT_SAMPLE`/`OBJECT_REMOVE` made objects ([objects.md](objects.md), "Fish and lures"). Since protocol 10 (branch fishing-3) each game sends `FISH_LURE` (320) after its `OBJECT_PRESENT`: every fish it has loaded, how near its own cast lure is, and whether the fish is hooked (the sender owns it, within 1 m of the lure) or drawn (it swims at the lure). It goes on a change (at most every 250 ms, a change of hooked or drawn at once), every second while a fish is loaded, and once empty when the last fish left. The server marks each listed object a fish: a fish is never moved by the distance rule, never held by a fight lock, and a hit does not move it; it goes to the game whose lure beats its owner's (the same rank and 1 m nearer, or drawn against the owner's plain distance and not over 1 m farther), held 300 ms, not within 1 s of its last change. Only the owner's hooked counts, and the owner keeps a hooked fish: on its last list for 10 s, and while its game stands still (paused, dead). Any other list older than 2.5 s counts as no lure; a fish whose owner stands still with no hooked fish goes to the best lure, else as any object. Types 186 to 190 (protocol 8) were never used; 321 to 324 are kept.

### Drops

The rupees, hearts and ammo that an enemy, a pot, grass or a rock leaves ([objects.md](objects.md), "Drops"). All on channel 2. The server relays each to everyone else, with the sender's number in the player field, and keeps nothing.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 36 | `DROP_SPAWN` | 2 | both | `u8` player field, object id (kind made: the game that rolled it and its counter), scene (string, 1 to 64), actor name (string, 1 to 64), position (3 x f32), `u64` server time when its maker first saw it, `u8` flags (bit 0 the sender owns what dropped it, bit 1 told again for a player who came later; other bits refused) |
| 37 | `DROP_TAKE` | 2 | both | `u8` player field, object id (kind made, or since version 10 kind placed: a bush's or grass tuft's fixed loot by its placement hash), `u8` reason (0 picked up, 1 blinked out; others refused) |
| 38 | `DROP_ASK` | 2 | both | `u8` player field, scene (string, 1 to 64): the sender came into this scene and asks for its drops |

Type 39 is kept for drops and not used. A `DROP_SPAWN` told again may come from any game that still shows the drop, so the maker in its id need not be the sender.

### Gimmick events (`DUNGEON_EVENT`)

Protocol 10, type 355 (355 to 359 kept; branch `deku-dungeon-2`, issues 176 and 196). When a game starts one of the room gimmick events whose played flag the others hold ([dungeon.md](dungeon.md), "Live events"), it sends the event's name at once. The others in that scene start the same event as soon as their Link is near its place, instead of waiting for the played flag, which comes only at the end of the sender's event. On channel 0, reliable; the server writes the sender's number, relays it to the others in the sender's scene and keeps nothing (a game that comes later gets the played flag). A version 9 server refuses it as unknown. Story cutscenes use the same message (branch `shared-cutscenes`): the name is one of the story cutscenes in `coop_shared_scenes.cpp`, and the others in the room play it at once ([scenes.md](scenes.md), "Shared story cutscenes"); a game without that branch ignores such a name.

### Boss rooms (`BOSS_ROOM`)

Where a player stands with a dungeon's boss ([dungeons-boss.md](dungeons-boss.md)). On channel 0; the server relays it to everyone else with the sender's number in the player field and keeps nothing.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 50 | `BOSS_ROOM` | 0 | both | `u8` player field, scene (string, 1 to 64: the dungeon), `u8` room (0 to 15, the index in `coop_boss.h`'s table), `u8` state: 1 in the room (sent on entering and every 2 s while there), 2 intro started, 3 intro over (the fight has begun), 4 left the room, 5 the boss is beaten; others refused |

Types 51 to 54 are kept for boss rooms and not used.

### PvP (protocol 10, `PVP_MODE`, `PVP_HIT`, `PVP_HURT`)

Players hurting each other ([play-online.md](play-online.md), "PvP"; `lichii/hook/coop_pvp.h`). Off unless the host turns it on. Types 241 to 250 are kept for it.

| Type | Name | Channel | Sent by | Body |
|---|---|---|---|---|
| 241 | `PVP_MODE` | 0 | both | `u8` set by (0 the server's start, 1 to 4 the player who changed it), `u8` mode (0 off, 1 on; others refused). No player field |
| 242 | `PVP_HIT` | 2 | both | `u8` player field (the attacker), `u8` victim (1 to 4, not the attacker), `u8` cause (as in `DAMAGE`; others kept), `s32` amount (quarter hearts, 1 to 80), 3 x `f32` where the blow landed, 3 x `f32` where the attacker stood (all finite) |
| 243 | `PVP_HURT` | 2 | both | `u8` player field (the victim), `u8` attacker (1 to 4, not the victim), `s32` amount taken (0 to 80), `s32` life left (0 to 80), `u8` flags: bit 0 the hit took the last life, bit 1 kept off (amount 0); other bits refused |

- **The session's value is the server's.** `coop_server --pvp on|off` (the host's `pvp` line in coop-options.ini; off by default) sets it. A game starts every connection with PvP off; while it is on, the server sends `PVP_MODE` to each player at join, after `HOST`. The host may change it with its own `PVP_MODE` (the `/pvp` chat command): the server sends the new value, with the host's number, to everyone. A `PVP_MODE` from anyone else is not taken; the server answers that player alone with the value as it is. When the session empties, the value goes back to the server's start value.
- **Hits go to their victim only, and only while PvP is on.** The server passes a `PVP_HIT` to the victim alone, with the attacker's number written in, and drops it while PvP is off (counted in `pvp_hits_dropped`), so a game with PvP off never gets one, whatever an attacker's game believes.
- **The victim decides.** Its game takes the hit (or not, by the rules in play-online.md) and sends `PVP_HURT`, which the server relays to everyone else. A `PVP_HURT` naming its own sender as the attacker is dropped.

## Later stages (outline only)

- **Stage 3:** `PLAYER_STATUS` decides whether puppets show during cutscenes. `JOIN_PARTNER` (above) asks for a warp to another player's scene.
- **Stage 4:** `FLAG_SET`, `FLAG_STATE` and `FLAG_SNAPSHOT` (above) share world flags through the host.
- **Stage 5:** the object messages (above). The server's ownership rules are built; the hook's side of the hand-over and of groups is not.

## Versioning

- The protocol version goes up whenever a message layout changes in a way an older reader cannot skip.
- Adding a component or a message type that old readers may ignore does not change the version.
- Types 12 to 27 (`JOIN_PARTNER`, the flag messages, `HOST`, `PLAYER_EQUIP`, the object messages, `PLAYER_POSE` and `TIME_OF_DAY`) were added this way and the version stayed 0: no earlier layout changed, and a build without them reports them as unknown and ignores them. A session with such a build still runs, but that player cannot warp to a partner or share flags, and its puppets wear the default gear.
- The key kind at the start of each flag entry (64-bit keys) changed the flag list layout. Nothing with the old layout was released, so the version stayed 0.
- Kind 4, count delta, was added to the flag entry the same way. A build without it refuses a `FLAG_SET` that carries one (an unknown kind), so a host must have it; a player without it never sends one and takes totals as it always did.
- `PLAYER_EQUIP` components 11 to 15 (the other hand slots, branch `held-items-2`) were added the same way and need no version of their own: a build without them skips them (unknown components are skipped), and its puppets show only the sword, shield and slot 3. As for 10, a server without them drops them when it relays.
- `PLAYER_EQUIP` component 10, `held`, was added this way (2026-11-09) and the version stayed 3: a build without it skips it, and its puppets show no held item. The server reads and writes the message again when it relays, so a server without it drops the component: the held item needs a server that has it.
- Version 1 (2026-11-09, the first test release): no layout changed, but the version went up so that the release and older development builds, which lack those messages, refuse each other at `HELLO` instead of playing a half-working session.
- Version 2 (2026-11-09): `OBJECT_SPAWN` may carry a placed id, sent by that object's owner to name its actor for games that lack it. Version 1 readers refuse such a spawn, so the version went up.
- Version 3 (2026-11-09): `TIME_OF_DAY` (type 27). No layout changed; the version went up so that the first release, which never sends the host's clock, and this build refuse each other at `HELLO` instead of showing different skies.
- Version 4 (2026-11-09): one bump for all the messages of types 28 to 38: `GRASS_CUT` (28), `OBJECT_GROUP` (29), `WORLD_BREAK`, `WORLD_MOVE`, `WORLD_PLACE`, `WORLD_HIT` (32 to 35), `DROP_SPAWN`, `DROP_TAKE`, `DROP_ASK` (36 to 38); 30, 31 and 39 are kept free. No earlier layout changed. The version went up because the server now sends `HANDOVER_ASK` and moves objects between players, which a version 3 game does not answer, and so that a version 3 server, which would not relay the new types, is refused at `HELLO`: every game in a session shares grass, breaks and drops, or none does.
- Version 5 (chunk 2): `BOSS_ROOM` (50). No earlier layout changed. The version went up so that a version 4 game, which would never move into the boss room or leave the intro with the others, is refused at `HELLO`.
- Version 6 (chunk 4): `SIGN_CUT` (80; 80 to 84 kept for signs). No earlier layout changed. The version went up so that a version 5 server, which would not relay it, is refused at `HELLO`.
- Version 7 (chunk 5): `WORLD_BREAK` flags bit 1, `kept` (branch `locked-doors`). A version 6 game or server refuses a break with that bit, and a version 6 server would forget a lock's break when its scene empties, so every game and the server must be 7.
- Version 7 also brings `FLAG_RESET` (135; 135 to 139 kept for saves, branch `host-save`): the host's save load sets every game's shared flags to the save's values. A version 6 server would not relay it.
- Version 8: `FLAG_ORIGIN` (150; 150 to 154 kept), from the server only. A version 7 game would refuse it as unknown, so every game and the server must be 8.
- Version 9 (chunk 8) also brings `SAVE_ASK` (206) and `SAVE_PART` (207); 206 to 215 kept for the save transfer (branch `host-save-load`, issue 167). A version 8 server would not relay them.
- Version 8 also brings held pieces in `PLAYER_POSE` (branch `sling-fire`, issue 158): each piece in Link's hands goes as a pose of its own skeleton. The layout is unchanged; a build before it keeps one pose track per player and would mix Link's poses with the slingshot's, so every game of protocol 8 must have it. Types 156 to 160 are kept for projectiles (none used yet).
- Version 8 also (chunk 7, world-objects): no new message (171 to 175 kept). The placed rolling boulders now go as `WORLD_MOVE` ([world.md](world.md), "Rolling boulders").
- Version 8 also (items-progress): adds no message type (176 to 180 kept, unused). The Deku Shield (`PouchData.Weapon_Shield_Deku.StockNum`, `IsGet.Weapon_Shield_Deku`) is each player's own, and the Gold Skulltula token count is guarded by the 100 Skulltula keys ([items.md](items.md), issues 130 and 89).
- Version 9 (chunk 8, join-password, issue 160): `JOIN_CHALLENGE` (191), `JOIN_PROOF` (192) and `REFUSED` reason 6, bad password; 191 to 195 kept. A version 8 game cannot answer the challenge, so every game and the server must be 9. Other chunk 8 branches add their types under protocol 9 without a bump of their own.
- Version 10 (chunk 11): one bump for all of these. New types: `PVP_MODE` (241), `PVP_HIT` (242), `PVP_HURT` (243), 241 to 250 kept for PvP (branch `pvp`); `PLAYER_LIFE` (251), 251 to 255 kept for the co-op HUD (branch `game-hud`). Changed: `DROP_TAKE` may carry a placed id (kind placed, maker 0, the placement hash), so the loot a bush or grass tuft lays out in every game is taken from all when one player picks it up (branch `enemy-drops`, issue 102); a version 9 game refuses such a take. Enemy shots (branch `enemy-shots`) go as `OBJECT_SPAWN` with the projectile flag and add no type. A version 9 server would not relay or check the new types, so every game and the server must be 10. The Deku Shield's two flags are shared again (max / any, branch `shop-shield-2`, issue 130); a version 9 game keeps them per player, so it would not take a shield bought elsewhere.
- Protocol 10 (items-sync, issues 89 and 130): adds no message type (325 to 329 kept, unused). A Gold Skulltula token that appears within 5 s of its Skulltula leaving the walk, up to 12 m away, is now known as its token ([world.md](world.md), tokens).
- Version 10 also brings `FISH_LURE` (320; 320 to 324 kept, branch `fishing-3`, issue 151), from a client only and not relayed. A version 9 server refuses it as unknown.
- Version 10 also brings `DUNGEON_EVENT` (355; 355 to 359 kept, branch `deku-dungeon-2`, issues 176 and 196), from either side, relayed to the sender's scene, never kept.
- Version 10 (host-fixes, issue 163): `REJOIN_TOKEN` (345) and `REJOIN_CLAIM` (346); 345 to 349 kept. A build without them is never given a takeover (it shows no token), which is only slower, so no bump of its own.
- Version 10 also brings `PLAYER_RIDE` (385; 385 to 389 kept, branch `epona-ride`, issue 199), on the samples channel, relayed to everyone else. A build without it ignores the type and its riders show on foot.
