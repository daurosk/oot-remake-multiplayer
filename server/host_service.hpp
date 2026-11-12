#pragma once
// A hosted co-op session as one object: the game server (EnetServer and its
// Session) plus what the host asked for around it: the router forward
// (upnp.hpp), the relay (relay_link.hpp) and the invite code with its answer
// codes (punch.hpp). coop_server's hosting mode is a loop around it, and the
// Hookshot module runs it on its own thread so a player can host from the
// game's menu without a server window (docs/hookshot.md, "Hosting from the
// game").
//
// One thread owns it: start, service, add_answer and stop are called from
// that thread only. status() is a copy and may be taken from any thread.
//
// The caller starts ENet (enet_initialize) before start() and stops it after
// the object is gone, as for EnetServer.
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "enet_server.hpp"
#include "nat_codes.hpp"

namespace coop::server {

namespace upnp {
class Mapping;
}
namespace relay {
class RelayLink;
}
namespace nat {
class HostRendezvous;
}

struct HostServiceConfig {
  // The game port and how many ports after it to try when it is taken (the
  // Hookshot menu tries a few; coop_server takes only the one it was given).
  std::uint16_t port = protocol::kDefaultPort;
  std::uint16_t port_tries = 1;
  std::optional<std::uint32_t> bind;  // as ServerOptions::bind
  SessionConfig session;              // join_password must not be empty
  // ENet's connection slots: more than max_players, so one more player can
  // connect and be told the server is full.
  std::size_t max_connections = 20;
  bool upnp = false;                  // only when the host said yes
  bool invite = false;                // STUN, then the invite code
  std::vector<nat::Addr> stun_servers;
  bool stun_allow_private = false;    // tests: a STUN server on 127.0.0.1
  std::string relay_via;              // "" for none
  std::string relay_key;
  // Every line the service has to say, without a "coop_server: " head.
  std::function<void(const std::string&)> log;
  // Enemy counts and the like, at most every 5 s (coop_server's window
  // shows them; the game's log need not).
  bool counts = true;
  // The time in ms for the invite code; empty: the steady clock.
  std::function<std::uint64_t()> now_ms;
};

enum class HostState { kOff, kHosting, kStopped, kFailed };
// What `message` is about, for a menu that says it in its own words.
enum class HostProblem { kNone, kNoPassword, kPortTaken, kUpnp, kRelay, kNoInvite, kSymmetric };

// What a menu or a window shows. Strings are empty until known.
struct HostStatus {
  HostState state = HostState::kOff;
  std::uint16_t port = 0;
  std::size_t players = 0;
  std::string invite;       // the invite code, once the STUN round is done
  bool invite_done = false; // the STUN round is over (with a code or not)
  std::string relay_code;   // "<code>@<relay>", once the relay gave one
  bool upnp_open = false;
  std::string external;     // the router's internet address (UPnP), if known
  std::string message;      // the last problem or hint, plain English
  HostProblem problem = HostProblem::kNone;
  // Other players' samples and poses this server sent in the last second,
  // in kilobits (the host's upload for them; docs/protocol.md "Who gets
  // which stream").
  std::uint32_t stream_kbps = 0;
};

class HostService {
 public:
  explicit HostService(HostServiceConfig config);
  ~HostService();  // stop()
  HostService(const HostService&) = delete;
  HostService& operator=(const HostService&) = delete;

  // Listens, then opens the router forward, the relay link and the STUN
  // round when asked. False (and `error` says why, as status().message) when
  // no port could be listened on or the config cannot work (no password).
  bool start(std::string& error);
  // One round: network events (waiting up to wait_ms), the forward's renewal,
  // the relay, the invite code. Call it in a loop until stop().
  void service(std::uint32_t wait_ms);
  // A friend's ANSWER CODE the host pasted (needs invite).
  nat::CodeError add_answer(const std::string& typed);
  // Tells the relay, removes the forward, says the totals. Safe twice.
  void stop();

  HostStatus status() const;
  bool running() const { return server_ != nullptr && !stopped_; }
  // start() failed because no port could be listened on (not the config).
  bool port_failed() const { return port_failed_; }
  std::uint16_t port() const;
  const Session* session() const;

 private:
  std::uint64_t now_ms() const;
  void say(const std::string& text) const;
  void set_message(const std::string& text, HostProblem problem);
  void finish_invite();
  void say_counts();

  HostServiceConfig config_;
  std::unique_ptr<EnetServer> server_;
  std::unique_ptr<upnp::Mapping> mapping_;
  std::unique_ptr<relay::RelayLink> link_;
  std::unique_ptr<nat::HostRendezvous> rendezvous_;
  bool stopped_ = false;
  bool port_failed_ = false;
  bool invite_shown_ = false;
  std::uint64_t shown_resets_ = 0;
  std::uint64_t shown_limited_ = 0;
  std::uint64_t shown_enemies_ = 0;
  std::uint64_t enemies_at_ = 0;
  std::uint64_t stream_bytes_at_ = 0;  // session stats at the last second
  std::uint64_t stream_second_ = 0;    // when that was, ms
  mutable std::mutex status_mutex_;
  HostStatus status_;
};

// "1.2.3.4" for an IPv4 address held first dotted part highest.
std::string show_ipv4(std::uint32_t ip);

}  // namespace coop::server
