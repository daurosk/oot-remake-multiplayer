#pragma once

// coop_server --upnp: asks the home router (UPnP Internet Gateway Device) to
// forward the server's UDP port to this PC while the server runs, and to stop
// when it stops. docs/play-online.md, "Let the router forward the port
// (UPnP)". A minimal client of our own: SSDP to find the router, its
// description to find the WAN connection service, and three SOAP actions
// (GetExternalIPAddress, AddPortMapping, DeletePortMapping).
//
// The mapping has a lease (10 minutes) renewed while the server runs, so a
// crash leaves it open for at most that long. Routers that only take
// permanent mappings get one, which is removed on a normal stop.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::server::upnp {

// --- Pure parts (tests feed them a router's answers) ---

struct Url {
  std::string host;  // as written (routers use IPv4 addresses)
  std::uint16_t port = 80;
  std::string path = "/";
};
std::optional<Url> parse_url(std::string_view text);
// `relative` resolved against `base` (an absolute URL, or a path).
std::optional<Url> resolve_url(const Url& base, std::string_view relative);

// The M-SEARCH datagram for `search_target`.
std::string search_request(std::string_view search_target);
// The LOCATION of an SSDP answer that is a 200 OK; nothing otherwise.
std::optional<std::string> search_location(std::string_view answer);

struct Service {
  std::string type;  // urn:schemas-upnp-org:service:WANIPConnection:1, ...
  Url control;
};
// The WAN connection service of a device description (WANIPConnection 2 or
// 1, else WANPPPConnection 1) and its control URL; nothing when there is none.
std::optional<Service> find_wan_service(std::string_view description_xml, const Url& description_url);

// An HTTP/1.1 request: GET for a path, or a SOAP POST.
std::string get_request(const Url& url);
std::string soap_request(const Service& service, std::string_view action,
                         const std::vector<std::pair<std::string, std::string>>& arguments);

struct HttpReply {
  int status = 0;
  std::string body;  // chunked bodies put back together
};
std::optional<HttpReply> parse_http_reply(std::string_view raw);

// The text inside the first <Tag> or <prefix:Tag> element; nothing if absent.
std::optional<std::string> xml_value(std::string_view xml, std::string_view tag);
// A SOAP fault's UPnP errorCode (718 conflict, 725 only permanent leases);
// 0 when there is none.
int upnp_error(std::string_view body);

// A dotted IPv4 address as a number (first part highest).
std::optional<std::uint32_t> parse_ipv4(std::string_view text);
// Private, shared (CGNAT 100.64/10), loopback or unset: a router whose
// "internet address" is one of these is itself behind another router, and a
// mapping on it cannot let anyone in from the internet.
bool is_not_public(std::uint32_t address);

// --- The mapping (network) ---

struct Config {
  std::uint16_t internal_port = 0;
  std::uint16_t external_port = 0;
  std::string description = "OoT co-op server";
  std::uint32_t lease_seconds = 600;
  std::string discovery_host = "239.255.255.250";  // tests: a fake router
  std::uint16_t discovery_port = 1900;
  std::uint32_t discovery_ms = 2500;
  std::uint32_t http_timeout_ms = 3000;
  // A renewal runs inside the server's loop and blocks it while it waits, so
  // it waits less (a router on the home network answers in milliseconds).
  std::uint32_t renew_timeout_ms = 1000;
  std::function<void(const std::string&)> log;
};

class Mapping {
 public:
  explicit Mapping(Config config);
  ~Mapping();  // removes the mapping
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  // Finds the router and adds the mapping; says what happened in the log.
  bool open();
  // Renews the lease when half of it has gone. Call it now and then.
  void renew_if_due();
  // Removes the mapping (also done by the destructor).
  void close();

  bool is_open() const { return open_; }
  const std::string& external_address() const { return external_; }
  const std::string& internal_address() const { return internal_; }

 private:
  bool add(std::uint32_t lease, std::uint32_t timeout_ms);
  void say(const std::string& text) const;

  Config config_;
  std::optional<Service> service_;
  std::string external_;
  std::string internal_;
  std::uint32_t lease_ = 0;
  bool open_ = false;
  std::uint64_t opened_ms_ = 0;
};

}  // namespace coop::server::upnp
