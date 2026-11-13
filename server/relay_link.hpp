#pragma once

// coop_server --relay-via HOST[:PORT]: the host's side of a relay
// (relay.hpp). It connects out to the relay, gets a join code and keeps it
// with a keepalive every 5 seconds (which also keeps the host's router
// letting the relay's answers in). Each guest the relay forwards gets its own
// UDP socket on 127.0.0.1, so to this PC's server every guest is a separate
// player at 127.0.0.1.

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <enet/enet.h>

#include "relay_wire.hpp"

namespace coop::server::relay {

struct RelayLinkConfig {
  std::string relay_host;  // a name or an IPv4 address
  std::uint16_t relay_port = kDefaultRelayPort;
  std::uint16_t server_port = 0;  // this PC's coop_server
  std::string key;                // the relay's host key, if it wants one
  std::size_t max_flows = 32;
  std::function<void(const std::string&)> log;
  // Called when the relay gives a join code (again after a relay restart).
  std::function<void(const std::string& code)> on_code;
  // The session token; std::random_device when empty.
  std::function<std::uint64_t()> random;
};

// "relay.example.net:27530" or "1.2.3.4" (default port) as host and port;
// false when it is not one.
bool parse_relay_address(const std::string& text, std::string& host, std::uint16_t& port);

class RelayLink {
 public:
  explicit RelayLink(RelayLinkConfig config);
  ~RelayLink();
  RelayLink(const RelayLink&) = delete;
  RelayLink& operator=(const RelayLink&) = delete;

  // Looks the relay up and opens the socket. False (and why) when it cannot.
  bool start(std::string& error);
  // Handles what arrived (does not wait) and sends what is due.
  void service();
  // Tells the relay the host stopped, so the code ends at once.
  void stop();

  // The join code while the relay has given one; empty before that.
  const std::string& code() const { return code_; }
  std::size_t flows() const { return flows_.size(); }

 private:
  struct Flow {
    ENetSocket socket = ENET_SOCKET_NULL;
    std::uint64_t last_ms = 0;
  };

  std::uint64_t now_ms() const;
  void send_relay(const Message& m);
  void handle(const Message& m, std::uint64_t now);
  void close_flow(std::uint16_t flow, bool tell_relay);
  void say(const std::string& text) const;

  RelayLinkConfig config_;
  ENetSocket socket_ = ENET_SOCKET_NULL;
  ENetAddress relay_{};
  ENetAddress server_{};
  std::uint64_t token_ = 0;
  std::string code_;
  bool registered_ = false;
  bool refused_hard_ = false;
  std::uint64_t started_ms_ = 0;
  std::uint64_t last_sent_ms_ = 0;
  std::uint64_t last_register_ms_ = 0;
  std::uint64_t last_keepalive_ms_ = 0;
  std::uint64_t last_heard_ms_ = 0;
  std::uint64_t first_try_ms_ = 0;
  std::uint64_t last_waiting_note_ms_ = 0;
  std::map<std::uint16_t, Flow> flows_;
};

}  // namespace coop::server::relay
