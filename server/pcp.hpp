#pragma once
// The router forward for routers without UPnP: PCP (RFC 6887, Port Control
// Protocol) and its older form NAT-PMP (RFC 6886, Apple routers and many
// others). One UDP request to the router on port 5351 asks it to forward the
// server's UDP port to this PC; the answer says the router's internet address
// and the port it gave. Used when the UPnP forward (upnp.hpp) does not work,
// under the same switch (upnp = no turns both off).
//
// The mapping has a lease (10 minutes) renewed while the server runs, so a
// crash leaves it open for at most that long; a normal stop removes it.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nat_codes.hpp"

namespace coop::server::pcp {

using nat::Addr;

inline constexpr std::uint16_t kRouterPort = 5351;

// --- Pure parts (tests feed them a router's answers) ---
using Nonce = std::array<std::uint8_t, 12>;
inline constexpr std::size_t kPcpMapBytes = 60;
// A PCP MAP request for UDP from `client_ip` (this PC's address toward the
// router): forward `suggested_port` (0: any) to `internal_port`.
std::array<std::uint8_t, kPcpMapBytes> pcp_map_request(std::uint32_t client_ip, const Nonce& nonce,
                                                       std::uint16_t internal_port, std::uint16_t suggested_port,
                                                       std::uint32_t lifetime);
enum class Reply {
  kOk,
  kRefused,      // the router answered with an error (`result`)
  kOldVersion,   // a NAT-PMP router answering a PCP request: ask it with NAT-PMP
  kNotOurs,      // not an answer to our request (another nonce, port, size)
};
struct MapReply {
  Reply reply = Reply::kNotOurs;
  int result = 0;              // the router's result code
  Addr external;               // the router's internet address (0 for NAT-PMP) and port
  std::uint32_t lifetime = 0;  // seconds
};
MapReply read_pcp_map(std::span<const std::uint8_t> data, const Nonce& nonce, std::uint16_t internal_port);

// NAT-PMP: the internet address, and a UDP mapping.
std::array<std::uint8_t, 2> natpmp_address_request();
std::array<std::uint8_t, 12> natpmp_map_request(std::uint16_t internal_port, std::uint16_t suggested_port,
                                                std::uint32_t lifetime);
// kOk with external.ip set, or kRefused / kNotOurs.
MapReply read_natpmp_address(std::span<const std::uint8_t> data);
// kOk with external.port and lifetime, or kRefused / kNotOurs.
MapReply read_natpmp_map(std::span<const std::uint8_t> data, std::uint16_t internal_port);

// One line for a person for a router's result code.
std::string explain_pcp(int result);
std::string explain_natpmp(int result);

// --- The mapping (network) ---
struct Config {
  std::uint16_t internal_port = 0;
  std::uint16_t external_port = 0;  // the one asked for; the router may give another
  std::uint32_t lease_seconds = 600;
  // The routers to ask, with this PC's address toward each (tests: a fake
  // router on 127.0.0.1). Empty: each network card's router (local_net.hpp).
  struct Router {
    Addr at;
    std::uint32_t client_ip = 0;
  };
  std::vector<Router> routers;
  // Waits for an answer, doubling each time (RFC 6886: 250 ms first).
  std::vector<std::uint32_t> waits_ms{250, 500, 1000};
  // A renewal runs inside the server's loop, so it asks once and waits
  // little (a router on the home network answers in milliseconds); a missed
  // one is asked again a quarter of the lease later.
  std::vector<std::uint32_t> renew_waits_ms{100};
  std::function<void(const std::string&)> log;
};

class Mapping {
 public:
  explicit Mapping(Config config);
  ~Mapping();  // removes the mapping
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  // Asks each router (PCP, then NAT-PMP); says what happened in the log.
  bool open();
  // Renews the lease when half of it has gone. Call it now and then.
  void renew_if_due();
  // Removes the mapping (also done by the destructor).
  void close();
  bool is_open() const { return open_; }
  // The router's internet address and the port it forwards.
  Addr external() const { return external_; }
  bool natpmp() const { return natpmp_; }

 private:
  enum class Try { kOk, kRefused, kOldVersion, kNoAnswer };
  Try ask_pcp(const Config::Router& router, std::uint32_t lifetime, const std::vector<std::uint32_t>& waits);
  Try ask_natpmp(const Config::Router& router, std::uint32_t lifetime, const std::vector<std::uint32_t>& waits);
  // Sends `request` and waits for an answer from `to` that `read` takes.
  std::optional<MapReply> exchange(Addr to, std::span<const std::uint8_t> request,
                                   const std::function<MapReply(std::span<const std::uint8_t>)>& read,
                                   const std::vector<std::uint32_t>& waits);
  void say(const std::string& text) const;

  Config config_;
  Nonce nonce_{};
  std::optional<Config::Router> router_;
  bool natpmp_ = false;
  bool open_ = false;
  Addr external_;
  std::uint32_t lease_ = 0;
  std::uint64_t opened_ms_ = 0;
};

}  // namespace coop::server::pcp
