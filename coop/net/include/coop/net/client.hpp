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
  std::string host = "127.0.0.1";  // a name is resolved once per attempt
  std::uint16_t port = protocol::kDefaultPort;
  protocol::Hello hello;  // sent as is on every connection

  std::uint32_t ping_interval_ms = 250;
  // ENet drops a peer after this long with nothing acknowledged.
  std::uint32_t timeout_ms = 10000;
  // Give up on an attempt with no WELCOME or REFUSED by then.
  std::uint32_t handshake_timeout_ms = 10000;
  // Delay before the next attempt: doubles from the first to the most.
  std::uint32_t retry_first_ms = 250;
  std::uint32_t retry_max_ms = 5000;

  // The client's clock in microseconds. Empty means std::chrono::steady_clock.
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

  std::uint64_t attempt_started_us_ = 0;
  std::uint64_t hello_sent_us_ = 0;
  std::uint64_t next_ping_us_ = 0;
  std::uint64_t retry_at_us_ = 0;
  std::uint32_t retry_delay_ms_ = 0;
  std::uint32_t failed_attempts_ = 0;

  ClockEstimator clock_;
  std::deque<Event> events_;
  std::size_t rejected_ = 0;
};

}  // namespace coop::net
