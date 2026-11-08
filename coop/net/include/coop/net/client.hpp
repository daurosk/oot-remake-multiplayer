#pragma once

// The co-op client: one connection to the server over ENet, used by the test
// tool, Lichii and the recomp alike. No game code and no threads: the host
// loop calls service() every frame (or more often), and reads what happened
// with next_event().
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
// The host name is resolved by the first attempt that succeeds and kept for
// later ones. A DNS name makes that attempt block inside service() for as
// long as the lookup takes; pass an IP address to never block.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "coop/net/clock.hpp"
#include "coop/protocol/messages.hpp"

struct _ENetHost;
struct _ENetPeer;

namespace coop::net {

struct ClientConfig {
  std::string host = "127.0.0.1";  // an IP, or a name (see above)
  std::uint16_t port = protocol::kDefaultPort;
  protocol::Hello hello;  // sent as is on every connection

  std::uint32_t ping_interval_ms = 250;
  // ENet drops a peer after this long with nothing acknowledged.
  std::uint32_t timeout_ms = 10000;
  // Give up on an attempt with no WELCOME or REFUSED by then.
  std::uint32_t handshake_timeout_ms = 10000;
  // Delay before the next attempt: doubles from the first to the most.
  // The first is at least 1 ms and the most at least the first.
  std::uint32_t retry_first_ms = 250;
  std::uint32_t retry_max_ms = 5000;
  // A welcomed connection that lasted this long sets the delay back to the
  // first one when it drops; a shorter one keeps doubling it.
  std::uint32_t stable_after_ms = 10000;
  // ENet events handled in one service() at most (at least 1).
  std::uint32_t max_events_per_service = 256;

  // The client's clock in microseconds. Empty means std::chrono::steady_clock.
  // It must not go backwards by much: a step back only pauses the timers.
  std::function<std::uint64_t()> now_us;
};

enum class State : std::uint8_t {
  idle,           // start() not called yet
  connecting,     // ENet connecting
  handshaking,    // HELLO sent, waiting for the answer
  connected,      // WELCOME received
  waiting_retry,  // between attempts
  refused,        // the server sent REFUSED; the client does not retry
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
};

std::string_view to_string(EventKind kind);

// Why an attempt or a connection ended.
enum class DropReason : std::uint8_t {
  none,
  connection_lost,    // ENet disconnect or timeout
  handshake_timeout,  // no answer to HELLO in time
  no_route,           // the address could not be resolved or used
};

struct Event {
  EventKind kind = EventKind::message;
  protocol::Welcome welcome;          // welcomed, reconnected
  protocol::RefuseReason refuse_reason{};  // refused; raw value kept
  DropReason drop_reason = DropReason::none;  // connect_failed, disconnected
  protocol::Message message;          // message
  std::uint8_t channel = 0;           // message
};

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
  // Attempts since the last WELCOME.
  std::uint32_t failed_attempts() const { return failed_attempts_; }

 private:
  void begin_attempt();
  void end_attempt(DropReason reason);
  void on_receive(const std::uint8_t* data, std::size_t size, std::uint8_t channel);
  void on_welcome(const protocol::Welcome& welcome);
  bool send_raw(const protocol::Message& message, std::uint8_t channel);
  void push(Event event);

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
  std::uint64_t welcomed_us_ = 0;
  std::uint64_t pong_due_us_ = 0;  // PONG budget: the next free slot's time
  std::uint64_t next_ping_us_ = 0;
  std::uint64_t retry_at_us_ = 0;
  std::uint32_t retry_delay_ms_ = 0;
  std::uint32_t failed_attempts_ = 0;

  ClockEstimator clock_;
  std::deque<Event> events_;
  std::size_t rejected_ = 0;
};

}  // namespace coop::net
