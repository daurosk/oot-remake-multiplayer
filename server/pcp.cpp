#include "pcp.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include <enet/enet.h>

#include "local_net.hpp"
#include "nat_net.hpp"

namespace coop::server::pcp {

namespace {

constexpr std::uint8_t kPcpVersion = 2;
constexpr std::uint8_t kPcpMap = 1;
constexpr std::uint8_t kUdp = 17;
constexpr int kPcpUnsupportedVersion = 1;

void put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t get32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
// An IPv4 address as PCP writes it: ::ffff:a.b.c.d.
void put_mapped(std::uint8_t* p, std::uint32_t ip) {
  std::memset(p, 0, 10);
  p[10] = 0xFF;
  p[11] = 0xFF;
  put32(p + 12, ip);
}
std::optional<std::uint32_t> get_mapped(const std::uint8_t* p) {
  for (int i = 0; i < 10; ++i) {
    if (p[i]) return std::nullopt;
  }
  if (p[10] != 0xFF || p[11] != 0xFF) return std::nullopt;
  return get32(p + 12);
}

std::string dotted(std::uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255u) + "." + std::to_string((ip >> 8) & 255u) +
         "." + std::to_string(ip & 255u);
}

std::uint64_t steady_ms() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

std::array<std::uint8_t, kPcpMapBytes> pcp_map_request(std::uint32_t client_ip, const Nonce& nonce,
                                                       std::uint16_t internal_port, std::uint16_t suggested_port,
                                                       std::uint32_t lifetime) {
  std::array<std::uint8_t, kPcpMapBytes> out{};
  out[0] = kPcpVersion;
  out[1] = kPcpMap;  // R = 0: a request
  put32(&out[4], lifetime);
  put_mapped(&out[8], client_ip);
  std::memcpy(&out[24], nonce.data(), nonce.size());
  out[36] = kUdp;
  put16(&out[40], internal_port);
  put16(&out[42], suggested_port);
  put_mapped(&out[44], 0);  // no wish for the internet address
  return out;
}

MapReply read_pcp_map(std::span<const std::uint8_t> d, const Nonce& nonce, std::uint16_t internal_port) {
  MapReply r;
  // A NAT-PMP router answers a version it does not know with version 0 and
  // its "unsupported version" result (RFC 6887 section 9).
  if (d.size() >= 4 && d[0] == 0) {
    if (get16(&d[2]) == 1) r.reply = Reply::kOldVersion;
    return r;
  }
  if (d.size() < 4 || d[0] != kPcpVersion || d[1] != (0x80 | kPcpMap)) return r;
  r.result = d[3];
  if (r.result == kPcpUnsupportedVersion) {
    r.reply = Reply::kOldVersion;
    return r;
  }
  if (d.size() < kPcpMapBytes) return r;
  if (!std::equal(nonce.begin(), nonce.end(), &d[24]) || d[36] != kUdp || get16(&d[40]) != internal_port) return r;
  r.lifetime = get32(&d[4]);
  if (r.result != 0) {
    r.reply = Reply::kRefused;
    return r;
  }
  const auto ip = get_mapped(&d[44]);
  r.external = Addr{ip.value_or(0), get16(&d[42])};
  // A removal (lifetime 0) may answer with port 0.
  r.reply = ip && (r.external.port != 0 || r.lifetime == 0) ? Reply::kOk : Reply::kNotOurs;
  return r;
}

std::array<std::uint8_t, 2> natpmp_address_request() { return {0, 0}; }

std::array<std::uint8_t, 12> natpmp_map_request(std::uint16_t internal_port, std::uint16_t suggested_port,
                                                std::uint32_t lifetime) {
  std::array<std::uint8_t, 12> out{};
  out[1] = 1;  // map UDP
  put16(&out[4], internal_port);
  put16(&out[6], suggested_port);
  put32(&out[8], lifetime);
  return out;
}

MapReply read_natpmp_address(std::span<const std::uint8_t> d) {
  MapReply r;
  if (d.size() < 4 || d[0] != 0 || d[1] != 128) return r;
  r.result = get16(&d[2]);
  if (r.result != 0) {
    r.reply = Reply::kRefused;
    return r;
  }
  if (d.size() < 12) return r;
  r.external.ip = get32(&d[8]);
  r.reply = Reply::kOk;
  return r;
}

MapReply read_natpmp_map(std::span<const std::uint8_t> d, std::uint16_t internal_port) {
  MapReply r;
  if (d.size() < 4 || d[0] != 0 || d[1] != 129) return r;
  r.result = get16(&d[2]);
  if (r.result != 0) {
    r.reply = Reply::kRefused;
    return r;
  }
  if (d.size() < 16 || get16(&d[8]) != internal_port) return r;
  r.external.port = get16(&d[10]);
  r.lifetime = get32(&d[12]);
  r.reply = Reply::kOk;
  return r;
}

std::string explain_pcp(int result) {
  switch (result) {
    case 0: return "done";
    case 1: return "it does not know this PCP version";
    case 2: return "it does not allow forwards (PCP is off in its settings, or only for some PCs)";
    case 7: return "it is behind another router or the provider's (it cannot forward from the internet)";
    case 8: return "it has no room for another forward";
    case 11: return "another PC has the port";
    default: return "it said no (PCP result " + std::to_string(result) + ")";
  }
}

std::string explain_natpmp(int result) {
  switch (result) {
    case 0: return "done";
    case 2: return "it does not allow forwards (NAT-PMP is off in its settings)";
    case 3: return "it has no internet connection right now";
    case 4: return "it has no room for another forward";
    default: return "it said no (NAT-PMP result " + std::to_string(result) + ")";
  }
}

Mapping::Mapping(Config config) : config_(std::move(config)) {
  std::random_device device;
  for (auto& b : nonce_) b = static_cast<std::uint8_t>(device());
}

Mapping::~Mapping() { close(); }

void Mapping::say(const std::string& text) const {
  if (config_.log) config_.log(text);
}

std::optional<MapReply> Mapping::exchange(Addr to, std::span<const std::uint8_t> request,
                                          const std::function<MapReply(std::span<const std::uint8_t>)>& read,
                                          const std::vector<std::uint32_t>& waits) {
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return std::nullopt;
  struct Closer {
    ENetSocket s;
    ~Closer() { enet_socket_destroy(s); }
  } closer{s};
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  if (enet_socket_bind(s, &any) != 0) return std::nullopt;
  enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
  const ENetAddress where = nat::to_enet(to);
  for (std::uint32_t wait : waits) {
    ENetBuffer out;
    out.data = const_cast<std::uint8_t*>(request.data());
    out.dataLength = request.size();
    enet_socket_send(s, &where, &out, 1);
    const std::uint64_t until = steady_ms() + wait;
    for (;;) {
      const std::uint64_t now = steady_ms();
      if (now >= until) break;
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(s, &condition, static_cast<enet_uint32>(until - now)) != 0) break;
      if (!(condition & ENET_SOCKET_WAIT_RECEIVE)) continue;
      std::uint8_t data[1100];
      ENetAddress from{};
      ENetBuffer in;
      in.data = data;
      in.dataLength = sizeof data;
      const int got = enet_socket_receive(s, &from, &in, 1);
      if (got <= 0) continue;
      if (nat::from_enet(from) != to) continue;  // only the router's answers
      const MapReply r = read(std::span<const std::uint8_t>(data, static_cast<std::size_t>(got)));
      if (r.reply != Reply::kNotOurs) return r;
    }
  }
  return std::nullopt;
}

Mapping::Try Mapping::ask_pcp(const Config::Router& router, std::uint32_t lifetime,
                              const std::vector<std::uint32_t>& waits) {
  const std::uint16_t internal = config_.internal_port;
  const auto request =
      pcp_map_request(router.client_ip, nonce_, internal, open_ ? external_.port : config_.external_port, lifetime);
  const Nonce nonce = nonce_;
  const auto reply = exchange(
      router.at, request, [&](std::span<const std::uint8_t> d) { return read_pcp_map(d, nonce, internal); }, waits);
  if (!reply) return Try::kNoAnswer;
  if (reply->reply == Reply::kOldVersion) return Try::kOldVersion;
  if (reply->reply != Reply::kOk) {
    if (lifetime) {
      say("the router " + nat::show(router.at) + " did not forward the port with PCP: " + explain_pcp(reply->result));
    }
    return Try::kRefused;
  }
  if (lifetime) {
    // A forward of port 0, or one that ends at once, is no forward.
    if (reply->external.port == 0 || reply->lifetime == 0) {
      say("the router " + nat::show(router.at) + " answered PCP without a port or a lease; not used");
      return Try::kRefused;
    }
    external_ = reply->external;
    lease_ = reply->lifetime;
  }
  return Try::kOk;
}

Mapping::Try Mapping::ask_natpmp(const Config::Router& router, std::uint32_t lifetime,
                                 const std::vector<std::uint32_t>& waits) {
  const std::uint16_t internal = config_.internal_port;
  // A removal asks for port 0 (RFC 6886 section 3.4).
  const std::uint16_t suggested = lifetime ? (open_ ? external_.port : config_.external_port) : 0;
  const auto request = natpmp_map_request(internal, suggested, lifetime);
  const auto reply = exchange(
      router.at, request, [&](std::span<const std::uint8_t> d) { return read_natpmp_map(d, internal); }, waits);
  if (!reply) return Try::kNoAnswer;
  if (reply->reply != Reply::kOk) {
    if (lifetime) {
      say("the router " + nat::show(router.at) + " did not forward the port with NAT-PMP: " +
          explain_natpmp(reply->result));
    }
    return Try::kRefused;
  }
  if (!lifetime) return Try::kOk;
  if (reply->external.port == 0 || reply->lifetime == 0) {
    say("the router " + nat::show(router.at) + " answered NAT-PMP without a port or a lease; not used");
    return Try::kRefused;
  }
  const std::uint16_t port = reply->external.port;
  lease_ = reply->lifetime;
  if (!open_) {
    const auto address = exchange(router.at, natpmp_address_request(), read_natpmp_address, waits);
    external_.ip = address && address->reply == Reply::kOk ? address->external.ip : 0;
  }
  external_.port = port;
  return Try::kOk;
}

bool Mapping::open() {
  if (open_) return true;
  std::vector<Config::Router> routers = config_.routers;
  if (routers.empty()) {
    // Only a router on the card's own network, private, the default route's
    // card first (the others, a VPN's or a virtual switch's, only if it fails).
    std::vector<nat::LocalNet> nets = nat::local_networks();
    std::stable_partition(nets.begin(), nets.end(), [](const nat::LocalNet& n) { return n.default_route; });
    for (const nat::LocalNet& net : nets) {
      if (!nat::gateway_on_link(net) || nat::is_public(net.gateway)) continue;
      const Config::Router r{Addr{net.gateway, kRouterPort}, net.ip};
      if (std::none_of(routers.begin(), routers.end(), [&](const Config::Router& o) { return o.at == r.at; })) {
        routers.push_back(r);
      }
    }
  }
  if (routers.empty()) {
    say("no router found to ask with PCP / NAT-PMP");
    return false;
  }
  for (const Config::Router& router : routers) {
    natpmp_ = false;
    Try t = ask_pcp(router, config_.lease_seconds, config_.waits_ms);
    if (t == Try::kOldVersion || t == Try::kNoAnswer) {
      natpmp_ = true;
      t = ask_natpmp(router, config_.lease_seconds, config_.waits_ms);
    }
    if (t != Try::kOk) continue;
    router_ = router;
    open_ = true;
    opened_ms_ = steady_ms();
    say("the router now forwards UDP port " + std::to_string(external_.port) + " to this PC's port " +
        std::to_string(config_.internal_port) + " (" + (natpmp_ ? "NAT-PMP" : "PCP") + ", internet address " +
        (external_.ip ? dotted(external_.ip) : std::string("not said")) + "); it stops when the server stops");
    return true;
  }
  natpmp_ = false;
  say("no router answered PCP / NAT-PMP (many routers have only UPnP, or it is off)");
  return false;
}

void Mapping::renew_if_due() {
  if (!open_ || !router_) return;
  // Our lease, or the router's when it gives less (one of hours is renewed on our timing).
  const std::uint32_t lease = (std::min)(lease_, config_.lease_seconds);
  const std::uint64_t lease_ms = static_cast<std::uint64_t>((std::max<std::uint32_t>)(lease, 60)) * 1000;
  if (steady_ms() - opened_ms_ < lease_ms / 2) return;
  const std::uint16_t before = external_.port;
  const Try t = natpmp_ ? ask_natpmp(*router_, config_.lease_seconds, config_.renew_waits_ms)
                        : ask_pcp(*router_, config_.lease_seconds, config_.renew_waits_ms);
  opened_ms_ = steady_ms();
  if (t != Try::kOk) {
    say("the router did not renew the port forward this time; trying again later");
    opened_ms_ -= lease_ms / 4;  // again in a quarter of the lease
  } else if (external_.port != before) {
    say("the router now forwards another port (" + std::to_string(external_.port) +
        "); invite codes made before this may not work");
  }
}

void Mapping::close() {
  if (!open_ || !router_) return;
  if (natpmp_) {
    ask_natpmp(*router_, 0, config_.renew_waits_ms);
  } else {
    ask_pcp(*router_, 0, config_.renew_waits_ms);
  }
  open_ = false;
  say("the router's port forward is removed");
}

}  // namespace coop::server::pcp
