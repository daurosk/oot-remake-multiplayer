#pragma once

// The co-op client: one connection to the server over ENet, used by the test
// tool, Lichii and the recomp alike. No game code and no threads: the host
// loop calls service() every frame (or more often), and reads what happened
// with next_event(). It also keeps the list of players in the room and
// sends chat (send_chat, players()).
//
// What it does by itself, inside service():
// - connects, sends HELLO, waits for WELCOME or REFUSED;
// - sends PING every 250 ms once welcomed, answers the server's PINGs with
//   PONG, and keeps the round trip and clock offset (clock.hpp);
// - checks every received message with check_received(msg, Sender::server,
//   channel) and drops the ones that fail (counted in rejected_count());
// - after a drop, tries again with a growing delay and sends HELLO again.
//
// Against a hostile or broken server:
// - ENet refuses any packet longer than the longest message (4 + 65535
//   bytes) before assembling it;
// - one service() handles at most max_events_per_service ENet events, so a
//   flood cannot keep it from returning; the rest wait for the next call;
// - PINGs are answered at most 4 at once and then one per 125 ms; the others
//   are dropped and counted;
// - the retry delay goes back to the first one only after a connection that
//   lasted stable_after_ms, so a server that welcomes and drops at once is
//   retried ever more slowly;
// - the client retries for as long as it runs; the host decides when to give
//   up (failed_attempts(), then stop()).
// The event queue grows only by what one service() handles, so a host that
// drains next_event() after every service() keeps it small.
//
// Shared flags (stage 1 step 4, docs/protocol.md):
// - set_flags() sends one FLAG_SET for the call (FLAG_STATE when this client
//   is the host, which is the truth). The game keeps its own change until a
//   FLAG_STATE says otherwise. Each flag sent in a FLAG_SET stays pending
//   until a FLAG_STATE or snapshot carries it (pending_flags()). When the
//   host changes, every pending flag is sent again: to the new host as a
//   FLAG_SET, or as FLAG_STATE when the new host is us. The server loses a
//   FLAG_SET in flight to a host that leaves; this is what keeps it.
// - Every FLAG_STATE from the host goes into flags() and gives a
//   `flag_state` event; the game applies those values, even over its own
//   change. As host we get FLAG_STATE too, with our own number: the
//   server's value where our snapshot clashed with its table.
// - FLAG_SNAPSHOT parts are collected; when the last one is in they replace
//   flags() and give one `flag_snapshot` event. Until then synced() is false,
//   so the game can hold. A part out of order is refused and the collection
//   waits for the next part 0.
// - The host answers each FLAG_SET it receives with a FLAG_STATE, through
//   set_flag_answer() (default: accept every value). That FLAG_STATE goes
//   into flags() too and gives a `flag_state` event for the host's game.
//   The server sends the host its own FLAG_SETs back (ones sent before we
//   knew we host). Those are answered with the table's value where it has
//   one (we have stated it since), else the asked value, and set_flag_answer()
//   is not asked: our own changes as host are the truth.
// - Count deltas (FlagKind::count_delta, docs/protocol.md "Counts"): a
//   player's are sent in FLAG_SET; unanswered ones for the same flag add up
//   in pending_flags() for a resend, and are dropped when we become host
//   (our game holds them). The host does not answer a delta: it is added to
//   the end of the `flag_state` event's flags for our game, which adds it
//   and states the total; set_flag_answer() is not asked, flags() never
//   holds one, and our own echoed delta is not handed over. As host,
//   set_flags() drops deltas (our count goes out as its total).
// - Who hosts: the server's HOST message. host_number() and is_host() follow
//   it, and every change gives a `host_changed` event, the first HOST
//   included. A client that becomes host sends its table with
//   send_flag_snapshot().
// - Channels are not ordered against each other, so HOST and FLAG_* (channel
//   1) can come before WELCOME. Up to max_early_messages of them are kept
//   while waiting for WELCOME and handled right after it.
//
// Equipment: a PLAYER_EQUIP from another player updates its players() entry
// and gives `player_equip`. The last one we sent is sent again to the server
// whenever another player joins, so the newcomer's puppet of us is dressed.
//
// Join partner (stage 1 step 3): request_join() asks to warp to a player and
// returns the request id; its answer gives `join_accepted` or `join_refused`,
// and no answer in join_timeout_ms gives `join_timed_out`. Only the latest
// request is open: a newer one, a drop or stop() ends it, and an answer that
// is not for it is dropped (stale_join_answers()). A request from another
// player gives `join_request`; answer it with accept_join() or refuse_join()
// within join_timeout_ms (the asker gives up then).
//
// A drop (the `disconnected` event) clears the flags, synced(), snapshot
// parts and every join request, ours and theirs, with no further event.
//
// The host name is resolved by the first attempt that succeeds and kept for
// later ones. A DNS name makes that attempt block inside service() for as
// long as the lookup takes; pass an IP address to never block.

#include <cstddef>
#include <compare>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/net/clock.hpp"
#include "coop/net/path_mark.hpp"
#include "coop/protocol/messages.hpp"

struct _ENetHost;
struct _ENetPeer;

namespace coop::net {

struct ClientConfig {
  std::string host = "127.0.0.1";  // an IP, or a name (see above)
  std::uint16_t port = protocol::kDefaultPort;
  protocol::Hello hello;  // sent as is on every connection
  // The join password, when the server asks for one (JOIN_CHALLENGE). Empty:
  // none; a server with a password then refuses with bad_password.
  std::string join_password;

  std::uint32_t ping_interval_ms = 250;
  // ENet drops a peer after this long with nothing acknowledged.
  std::uint32_t timeout_ms = 10000;
  // ENet's datagram size (its MTU), see protocol::kDatagramBytes.
  std::uint32_t mtu = protocol::kDatagramBytes;
  // Most bytes ENet may hold for us between services (fragments of messages
  // not yet whole, and whole ones not yet taken); a server cannot make us
  // hold more. 8 MB is twice a whole flag snapshot at the server's max_flags.
  // The count is kept true after each service (enet_waiting.hpp).
  std::size_t max_waiting_bytes = std::size_t{8} << 20;
  // Give up on an attempt with no WELCOME or REFUSED by then.
  std::uint32_t handshake_timeout_ms = 10000;
  // Delay before the next attempt: doubles from the first to the most.
  // The first is at least 1 ms and the most at least the first.
  std::uint32_t retry_first_ms = 250;
  std::uint32_t retry_max_ms = 5000;
  // A welcomed connection that lasted this long sets the delay back to the
  // first one when it drops; a shorter one keeps doubling it.
  std::uint32_t stable_after_ms = 10000;
  // REFUSED "name taken" is tried again every name_taken_retry_ms for
  // name_taken_retry_for_ms before the client gives up and reports it: a
  // build restarted after a crash finds its old self still in, until the
  // server lets it take that place (1 s of silence from the same address)
  // or times the old one out (10 s). Either one 0: no retry, the name taken
  // is reported at once. The interval is at least 100 ms. While it retries,
  // the state is waiting_retry, retrying_name_taken() is true and no refused
  // event is pushed. An attempt that ends another way (timeout, lost
  // connection) ends the retry window: the next name taken starts a new one.
  std::uint32_t name_taken_retry_ms = 1500;
  std::uint32_t name_taken_retry_for_ms = 12000;
  // ENet events handled in one service() at most (at least 1).
  std::uint32_t max_events_per_service = 256;
  // Our join request ends with `join_timed_out` after this long with no
  // answer; a request from another player can be answered for as long.
  std::uint32_t join_timeout_ms = 5000;
  // Progress-channel messages (HOST, FLAG_*) kept while waiting for WELCOME;
  // more are dropped and counted in rejected_count(). The server's largest
  // table (262,144 flags) is 73 snapshot parts.
  std::uint32_t max_early_messages = 1024;

  // The client's clock in microseconds. Empty means std::chrono::steady_clock.
  // It must not go backwards by much: a step back only pauses the timers.
  std::function<std::uint64_t()> now_us;
};

// ENet flags for a packet on `channel`: 0 to 2 reliable and ordered, 3 and 4
// unreliable and sequenced. A packet on 3 or 4 longer than one datagram
// (protocol::kDatagramBytes) is sent as unreliable fragments: without the
// flag ENet sends it reliably, and on a lossy VPN one lost fragment held
// the channel until the timeout (issue 143).
std::uint32_t packet_flags(std::uint8_t channel);

enum class State : std::uint8_t {
  idle,           // start() not called yet
  connecting,     // ENet connecting
  handshaking,    // HELLO sent, waiting for the answer
  connected,      // WELCOME received
  waiting_retry,  // between attempts
  refused,        // the server sent REFUSED; the client does not retry (name taken: see name_taken_retry_ms)
  stopped,        // stop() was called
};

std::string_view to_string(State state);

enum class EventKind : std::uint8_t {
  welcomed,        // the first WELCOME: `welcome` is set
  refused,         // REFUSED: `refuse_reason` is set; the client stops
  connect_failed,  // an attempt failed before WELCOME; it will try again
  disconnected,    // a welcomed connection was lost; reconnecting starts
  reconnected,     // WELCOME again after a disconnected: `welcome` is set
  message,         // anything else from the server: `message`, `channel`
  // These four follow the `message` event of the same CHAT, PLAYER_JOINED,
  // PLAYER_LEFT or PLAYER_STATUS, with players() already updated. A host
  // that feeds every `message` to coop/presence can ignore them.
  chat,            // `player`, `text`
  player_joined,   // `player`, `nickname`
  player_left,     // `player`, `nickname` (as it was), `leave_reason`
  player_status,   // `player`, `status_flags`
  // Shared flags. flag_state follows the `message` event of a FLAG_STATE
  // (`player` = the host), or of a FLAG_SET this host answered (`player` =
  // our own number); `flags` holds the values to apply, flags() is updated.
  flag_state,      // `player`, `flags`
  flag_snapshot,   // the last FLAG_SNAPSHOT part is in: `flags` = the table
  flag_reset,      // the host loaded a save: `player` = the host, `flags` to write exactly
  // Join partner. The first three follow the `message` event of their
  // JOIN_PARTNER; join_timed_out has none.
  join_request,    // `player` asks to join us: `request_id`
  join_accepted,   // `player` accepted our `request_id`: `join_target`
  join_refused,    // `player` refused our `request_id`: `join_reason`
  join_timed_out,  // no answer from `player` to our `request_id` in time
  // HOST named a new host, `player`. Follows HOST's `message` event, after
  // our pending flags went out again.
  host_changed,
  // PLAYER_EQUIP from `player`: `equipment` is what that player now wears,
  // the fields the message left out kept from before (also in players()).
  player_equip,
};

std::string_view to_string(EventKind kind);

// Why an attempt or a connection ended.
enum class DropReason : std::uint8_t {
  none,
  connection_lost,    // ENet disconnect or timeout
  handshake_timeout,  // no answer to HELLO in time
  no_route,           // the address could not be resolved or used
  // Full datagrams did not get through while small ones did (we saw it, or
  // the server said so): connecting again with small ones (enet_path.hpp).
  small_path,
};

// How the link to the server looked (issue 143), for the log. Read from
// ENet while a connection is up; a lost connection reports the last values.
struct LinkInfo {
  std::uint32_t quiet_ms = 0;       // since the last packet from the server
  std::uint32_t round_trip_ms = 0;  // ENet's own estimate
  std::uint32_t loss_percent = 0;   // ENet's packet loss estimate
  std::uint32_t mtu = 0;            // the datagram size both sides agreed
  std::uint32_t largest_sent = 0;   // our largest message on this connection, bytes
  std::uint32_t waiting_bytes = 0;  // what ENet holds for us now (enet_waiting.hpp)
  // What ENet counted for unassembled messages it threw away and we took off
  // again on this connection (lost fragments; issue 175).
  std::uint64_t mended_bytes = 0;
};

struct Event {
  EventKind kind = EventKind::message;
  protocol::Welcome welcome;          // welcomed, reconnected
  protocol::RefuseReason refuse_reason{};  // refused; raw value kept
  DropReason drop_reason = DropReason::none;  // connect_failed, disconnected
  protocol::Message message;          // message
  std::uint8_t channel = 0;           // message, and the four after it
  std::uint8_t player = 0;            // chat, player_*: 1 to 4
  std::string text;                   // chat
  std::string nickname;               // player_joined, player_left
  protocol::LeaveReason leave_reason{};  // player_left; raw value kept
  std::uint8_t status_flags = 0;      // player_status; unknown bits kept
  std::vector<protocol::FlagEntry> flags;    // flag_state, flag_snapshot
  std::uint16_t request_id = 0;              // join_*
  protocol::JoinTarget join_target;          // join_accepted
  protocol::JoinRefuseReason join_reason{};  // join_refused; raw value kept
  protocol::Equipment equipment;             // player_equip
  LinkInfo link;  // disconnected, connect_failed: the link just before
};

// A shared flag's place in the table: a name's hash and array index, or a
// 64-bit key (protocol::FlagKey).
using FlagKey = protocol::FlagKey;

// The host's answer to one flag of a FLAG_SET from `player`: the value to
// keep (as asked or changed; the key stays as asked), or nothing to
// refuse it. A refused flag already in flags() is sent back with that value,
// so the asker's game goes back to it.
using FlagAnswer = std::function<std::optional<protocol::FlagEntry>(
    std::uint8_t player, const protocol::FlagEntry& asked)>;

// One player in the room, this client included.
struct Player {
  std::uint8_t number = 1;  // 1 to 4
  std::string nickname;     // our own is the one from our HELLO
  // The last PLAYER_STATUS flags: the ones received for the others, the ones
  // sent for ourselves. Nothing until the first.
  std::optional<std::uint8_t> status;
  bool self = false;
  // What the player wears: the last PLAYER_EQUIP, received or (ours) sent,
  // each field kept until a message carries it again. Nothing until the
  // first. Ours is sent again to every player who joins, so they see it.
  std::optional<protocol::Equipment> equipment;
  bool operator==(const Player&) const = default;
};

// Longest chat text in characters (code points), as in the protocol.
inline constexpr std::size_t kMaxChatChars = protocol::kMaxChatChars;

// Text to show for a refusal, "unknown reason" for one this build does not know.
std::string_view refuse_reason_text(protocol::RefuseReason reason);

class Client {
 public:
  explicit Client(ClientConfig config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Starts the first attempt. False when ENet could not start.
  bool start();
  // Says goodbye to the server if connected and stops for good.
  void stop();

  // Runs ENet and the timers. Waits at most `wait_ms` for network traffic;
  // 0 does not wait.
  void service(std::uint32_t wait_ms = 0);

  // The next thing that happened, oldest first.
  std::optional<Event> next_event();

  // Sends on the message's own channel (channel_of). Only while connected,
  // and only messages a client may send; false otherwise.
  bool send(const protocol::Message& message);
  // Same, on a channel the message type allows (TEST_PAYLOAD on 0 or 3).
  bool send_on(const protocol::Message& message, std::uint8_t channel);

  // Sends CHAT. False for empty text, more than kMaxChatChars characters,
  // text the protocol refuses (bad UTF-8, control characters), or when not
  // connected. The server does not echo it back; show it locally.
  bool send_chat(std::string_view text);

  // Sends the flags our game changed: FLAG_SET for the call (one message per
  // kMaxFlagEntries), or FLAG_STATE as the host, which also goes into
  // flags() with no event (our game already has it). A flag given twice
  // keeps its last value. False when not connected, for none, or for a value
  // the protocol refuses (a bool not 0 or 1, a non-finite float, a bad kind);
  // then nothing is sent.
  bool set_flags(std::span<const protocol::FlagEntry> flags);
  bool set_flag(const protocol::FlagEntry& flag) { return set_flags({&flag, 1}); }
  // The host's whole table to the server, as FLAG_SNAPSHOT parts, when it
  // starts a session or takes over. flags() is not changed. False when not
  // connected, not the host, or the table cannot be split or sent.
  bool send_flag_snapshot(std::span<const protocol::FlagEntry> flags);
  // The host loaded a save: the flags its load changed, as FLAG_RESET
  // messages (kMaxFlagEntries each). flags() takes them. False when not
  // connected, not the host, or a part cannot be sent.
  bool send_flag_reset(std::span<const protocol::FlagEntry> flags);
  // How the host answers a FLAG_SET; empty means accept every value.
  void set_flag_answer(FlagAnswer answer) { flag_answer_ = std::move(answer); }

  // The shared flags as the host last gave them: snapshot, then FLAG_STATEs.
  const std::map<FlagKey, protocol::FlagEntry>& flags() const { return flags_; }
  std::optional<protocol::FlagEntry> find_flag(const FlagKey& key) const;
  std::optional<protocol::FlagEntry> find_flag(std::uint32_t hash,
                                               std::uint16_t index = 0) const {
    return find_flag(FlagKey{hash, index});
  }
  // True once the last FLAG_SNAPSHOT part of this connection is in.
  bool synced() const { return synced_; }
  // Whether this client hosts: connected and named by the last HOST.
  bool is_host() const;
  // The player named by the last HOST; 0 before one (and after a drop).
  std::uint8_t host_number() const { return host_number_; }
  // Flags sent in a FLAG_SET that no FLAG_STATE or snapshot has carried yet.
  const std::map<FlagKey, protocol::FlagEntry>& pending_flags() const { return pending_; }

  // Asks to warp to `player`'s scene. The request id, or nothing when not
  // connected, `player` is not in the room or is us, or it could not be sent.
  // Ends any request still open, whose answer is then stale.
  std::optional<std::uint16_t> request_join(std::uint8_t player);
  // Our open request's id, if any.
  std::optional<std::uint16_t> join_request_id() const;
  // Answers the latest request from `player` (a `join_request` event) with
  // its id: accept with where we stand, or refuse. False when no such
  // request is open, the target is one the protocol refuses (a scene of 0 or
  // more than 64 characters, non-finite numbers), or it could not be sent.
  bool accept_join(std::uint8_t player, std::uint16_t request_id,
                   const protocol::JoinTarget& target);
  bool refuse_join(std::uint8_t player, std::uint16_t request_id,
                   protocol::JoinRefuseReason reason);
  // Answers dropped because they were not for our open request.
  std::size_t stale_join_answers() const { return stale_join_; }

  // Who is in the room, this client included, by player number. Built from
  // WELCOME and kept current by PLAYER_JOINED, PLAYER_LEFT and
  // PLAYER_STATUS. Empty until WELCOME and again after a drop (the
  // `disconnected` event); the next WELCOME rebuilds it, with no status for
  // anyone, so a host resends its own PLAYER_STATUS after `reconnected`.
  // From the server, these are refused and counted in rejected_count(): a
  // PLAYER_LEFT, PLAYER_STATUS or CHAT for a number not in the list or for
  // our own, and a PLAYER_JOINED for our own. A PLAYER_JOINED for a number
  // already in the list replaces that player.
  const std::vector<Player>& players() const { return players_; }
  // The player with this number, or null.
  const Player* find_player(std::uint8_t number) const;

  State state() const { return state_; }
  // The player number from the last WELCOME, 0 before.
  std::uint8_t player_number() const { return player_number_; }
  // From the PINGs since the last WELCOME; nothing before the first sample.
  std::optional<std::uint64_t> round_trip_us() const { return clock_.round_trip_us(); }
  std::optional<std::int64_t> clock_offset_us() const { return clock_.offset_us(); }

  std::uint64_t now_us() const;
  // The server's clock now, estimated; the client's clock before any sample.
  std::uint64_t server_now_us() const;
  std::uint64_t to_server_time(std::uint64_t client_time_us) const;
  std::uint64_t to_client_time(std::uint64_t server_time_us) const;

  // Messages dropped: not decodable, unknown type, or refused by
  // check_received, or not expected in the current state.
  std::size_t rejected_count() const { return rejected_; }
  // This connection's traffic is encrypted (a server with a join password;
  // docs/protocol.md "Encrypted traffic"). False before the JOIN_CHALLENGE
  // and on a server without a password, whose traffic stays plain.
  bool encrypted() const { return cipher_ != nullptr; }
  // Attempts since the last WELCOME.
  std::uint32_t failed_attempts() const { return failed_attempts_; }
  // REFUSED "name taken" answers that were tried again (name_taken_retry_ms).
  std::uint32_t name_taken_retries() const { return name_taken_retries_; }
  // True while a REFUSED "name taken" is being asked again (state
  // waiting_retry, connecting or handshaking within name_taken_retry_for_ms).
  bool retrying_name_taken() const { return name_taken_since_us_ != 0; }
  // Whether the server gave this client a rejoin token (REJOIN_TOKEN).
  bool has_rejoin_token() const { return rejoin_token_.has_value(); }
  // The link now (issue 143). quiet_ms counts while connecting, handshaking
  // or connected; the server PINGs every 250 ms, so a few seconds of quiet
  // means its packets do not reach us.
  LinkInfo link() const;
  // True once the path to the server lost full datagrams (DropReason
  // small_path): every connection from then on uses ENet's smallest, 576.
  bool small_datagrams() const { return small_datagrams_; }

 private:
  void use_small_datagrams();
  void begin_attempt();
  void end_attempt(DropReason reason);
  void on_receive(const std::uint8_t* data, std::size_t size, std::uint8_t channel);
  void on_welcome(const protocol::Welcome& welcome);
  void on_connected_message(protocol::Message message, std::uint8_t channel);
  bool on_host(std::uint8_t number);  // false when it names the same host
  void settle_pending(std::span<const protocol::FlagEntry> flags);
  bool send_raw(const protocol::Message& message, std::uint8_t channel);
  // One encoded message to the server, sealed once the keys are known.
  bool send_packet(std::span<const std::uint8_t> bytes, std::uint8_t channel);
  // A JOIN_CHALLENGE: checks it offers a key share, answers it, and makes
  // this connection's keys from it. False when it did not work out.
  bool answer_challenge(const protocol::JoinChallenge& challenge,
                        std::span<const std::uint8_t> packet);
  void push(Event event);
  // Checks a room message against players_ and updates it; sets `out` to
  // the event that follows its `message` event, if any. False to refuse.
  bool apply_room(const protocol::Message& message, std::uint8_t channel,
                  std::optional<Event>& out);
  Player* player_entry(std::uint8_t number);
  // FLAG_* and JOIN_PARTNER: checks and updates, as apply_room.
  bool apply_shared(const protocol::Message& message, std::uint8_t channel,
                    std::optional<Event>& out);
  bool is_other_player(std::uint8_t number) const;
  void store_flags(std::span<const protocol::FlagEntry> flags);
  bool send_flags(std::span<const protocol::FlagEntry> flags, bool as_state);
  bool answer_join(protocol::JoinPartner answer);
  void clear_shared();
  void check_join_timeouts(std::uint64_t now);

  ClientConfig config_;
  bool enet_started_ = false;
  _ENetHost* host_ = nullptr;
  _ENetPeer* peer_ = nullptr;
  State state_ = State::idle;
  bool ever_welcomed_ = false;
  std::uint8_t player_number_ = 0;

  bool address_known_ = false;
  std::uint32_t address_host_ = 0;  // ENet's form, network byte order

  std::uint64_t attempt_started_us_ = 0;
  std::uint64_t hello_sent_us_ = 0;
  // This attempt has answered its JOIN_CHALLENGE: a second one is refused.
  bool challenge_answered_ = false;
  // This attempt's X25519 key pair and HELLO as sent, until the challenge;
  // then the keys (traffic_crypto.hpp) for the rest of the connection.
  std::optional<protocol::KeyPair> key_pair_;
  std::vector<std::uint8_t> hello_packet_;
  std::unique_ptr<protocol::TrafficCipher> cipher_;
  // From REJOIN_TOKEN after our last WELCOME; shown (REJOIN_CLAIM) before
  // each later HELLO to this server.
  std::optional<protocol::RejoinTokenBytes> rejoin_token_;
  std::uint64_t welcomed_us_ = 0;
  std::uint64_t pong_due_us_ = 0;  // PONG budget: the next free slot's time
  std::uint64_t next_ping_us_ = 0;
  std::uint64_t retry_at_us_ = 0;
  std::uint32_t retry_delay_ms_ = 0;
  std::uint32_t failed_attempts_ = 0;
  std::uint64_t name_taken_since_us_ = 0;  // the first "name taken" in a row, 0: none
  std::uint64_t last_receive_us_ = 0;  // last packet from the server, 0: none this attempt
  LinkInfo link_;  // ENet's values, read after each service() with a peer
  std::uint32_t name_taken_retries_ = 0;
  bool small_datagrams_ = false;
  PathMark path_mark_;  // enet_path.hpp

  ClockEstimator clock_;
  std::vector<Player> players_;  // sorted by number
  std::deque<Event> events_;
  std::size_t rejected_ = 0;

  std::map<FlagKey, protocol::FlagEntry> flags_;
  std::map<FlagKey, protocol::FlagEntry> pending_;  // our unanswered FLAG_SETs
  std::uint8_t host_number_ = 0;
  struct EarlyMessage {
    protocol::Message message;
    std::uint8_t channel = 0;
  };
  std::vector<EarlyMessage> early_;  // progress channel before WELCOME
  bool synced_ = false;
  std::vector<protocol::FlagEntry> snapshot_flags_;  // parts collected so far
  std::uint16_t snapshot_next_ = 0;   // the part expected next
  std::uint16_t snapshot_count_ = 0;  // 0 while no collection is open
  FlagAnswer flag_answer_;

  struct OpenJoin {
    std::uint8_t player = 0;
    std::uint16_t request_id = 0;
    std::uint64_t at_us = 0;
  };
  std::optional<OpenJoin> our_join_;
  std::vector<OpenJoin> their_joins_;  // at most one per asker
  std::uint16_t next_request_id_ = 1;
  std::size_t stale_join_ = 0;
};

}  // namespace coop::net
