// The router forward for routers without UPnP (server/pcp.hpp): the PCP and
// NAT-PMP datagrams as RFC 6887 / 6886 lay them out, and a fake router on
// 127.0.0.1 that answers one way or the other.

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "local_net.hpp"
#include "nat_net.hpp"
#include "pcp.hpp"

using namespace coop::server::pcp;
using coop::server::nat::Addr;

namespace {

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t get32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
void put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

constexpr std::uint32_t kRouterInternet = 0xCB007107u;  // 203.0.113.7

// A PCP MAP answer to `request` giving `port`, or the error `result`.
std::vector<std::uint8_t> pcp_answer(std::span<const std::uint8_t> request, std::uint16_t port, int result = 0) {
  std::vector<std::uint8_t> a(60, 0);
  a[0] = 2;
  a[1] = 0x81;
  a[3] = static_cast<std::uint8_t>(result);
  std::memcpy(&a[4], &request[4], 4);  // the lifetime asked for
  put32(&a[8], 1234);                  // epoch
  std::memcpy(&a[24], &request[24], 12 + 1 + 3 + 2);
  put16(&a[42], port);
  a[54] = 0xFF;
  a[55] = 0xFF;
  put32(&a[56], kRouterInternet);
  return a;
}

// A router on 127.0.0.1 that answers on its own thread.
struct FakeRouter {
  enum class Mode { kPcp, kNatPmp, kRefuse, kSilent, kPcpPortZero, kNatPmpNoLease };
  explicit FakeRouter(Mode m) : mode(m) {
    socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress local{};
    enet_address_set_host_ip(&local, "127.0.0.1");
    local.port = 0;
    REQUIRE(enet_socket_bind(socket, &local) == 0);
    ENetAddress bound{};
    REQUIRE(enet_socket_get_address(socket, &bound) == 0);
    port = bound.port;
    enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1);
    thread = std::thread([this] { run(); });
  }
  ~FakeRouter() {
    stop = true;
    thread.join();
    enet_socket_destroy(socket);
  }
  Addr address() const { return Addr{0x7F000001u, port}; }
  void run() {
    while (!stop) {
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(socket, &condition, 20) != 0 || !(condition & ENET_SOCKET_WAIT_RECEIVE)) continue;
      std::uint8_t data[1100];
      ENetAddress from{};
      ENetBuffer in{};
      in.data = data;
      in.dataLength = sizeof data;
      const int got = enet_socket_receive(socket, &from, &in, 1);
      if (got <= 0) continue;
      const std::vector<std::uint8_t> request(data, data + got);
      {
        std::lock_guard lock(mutex);
        requests.push_back(request);
      }
      std::vector<std::uint8_t> answer;
      if (mode == Mode::kSilent) continue;
      if (request[0] == 2 && request.size() == 60) {
        if (mode == Mode::kPcp) answer = pcp_answer(request, get32(&request[4]) ? 40001 : 0);
        if (mode == Mode::kRefuse) answer = pcp_answer(request, 0, 2);
        if (mode == Mode::kPcpPortZero) answer = pcp_answer(request, 0);  // "done", but no port
        if (mode == Mode::kNatPmp || mode == Mode::kNatPmpNoLease) {
          answer = {0, 0x80, 0, 1, 0, 0, 0, 0};  // version 0: unsupported version
        }
      } else if (request[0] == 0 && request[1] == 0 && (mode == Mode::kNatPmp || mode == Mode::kNatPmpNoLease)) {
        answer.assign(12, 0);
        answer[1] = 128;
        put32(&answer[8], kRouterInternet);
      } else if (request[0] == 0 && request[1] == 1 && request.size() == 12 &&
                 (mode == Mode::kNatPmp || mode == Mode::kNatPmpNoLease)) {
        answer.assign(16, 0);
        answer[1] = 129;
        std::memcpy(&answer[8], &request[4], 2);   // internal port
        put16(&answer[10], get32(&request[8]) ? 40002 : 0);
        if (mode == Mode::kNatPmp) std::memcpy(&answer[12], &request[8], 4);  // lifetime (NoLease: 0)
      }
      if (answer.empty()) continue;
      ENetBuffer out{};
      out.data = answer.data();
      out.dataLength = answer.size();
      enet_socket_send(socket, &from, &out, 1);
    }
  }
  std::vector<std::vector<std::uint8_t>> seen() {
    std::lock_guard lock(mutex);
    return requests;
  }

  Mode mode;
  ENetSocket socket = ENET_SOCKET_NULL;
  std::uint16_t port = 0;
  std::atomic<bool> stop{false};
  std::thread thread;
  std::mutex mutex;
  std::vector<std::vector<std::uint8_t>> requests;
};

Config config_for(const FakeRouter& router) {
  Config c;
  c.internal_port = 27510;
  c.external_port = 27510;
  c.routers = {{router.address(), 0x7F000001u}};
  c.waits_ms = {150, 150};
  c.renew_waits_ms = {150};
  return c;
}

}  // namespace

TEST_CASE("pcp: a MAP request as RFC 6887 lays it out") {
  Nonce nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<std::uint8_t>(i + 1);
  const auto r = pcp_map_request(0xC0A80102u, nonce, 27510, 27511, 600);
  CHECK(r[0] == 2);                       // version
  CHECK(r[1] == 1);                       // a request, MAP
  CHECK(get32(&r[4]) == 600);             // lifetime
  CHECK(r[18] == 0xFF);                   // ::ffff:192.168.1.2
  CHECK(r[19] == 0xFF);
  CHECK(get32(&r[20]) == 0xC0A80102u);
  CHECK(std::memcmp(&r[24], nonce.data(), 12) == 0);
  CHECK(r[36] == 17);                     // UDP
  CHECK(get16(&r[40]) == 27510);
  CHECK(get16(&r[42]) == 27511);
  CHECK(r[54] == 0xFF);                   // no wish for the address: ::ffff:0.0.0.0
  CHECK(get32(&r[56]) == 0);

  const auto ok = read_pcp_map(pcp_answer(r, 40001), nonce, 27510);
  CHECK(ok.reply == Reply::kOk);
  CHECK(ok.external == Addr{kRouterInternet, 40001});
  CHECK(ok.lifetime == 600);
  // Another nonce or port is not ours; an error is said.
  Nonce other = nonce;
  other[0] ^= 1;
  CHECK(read_pcp_map(pcp_answer(r, 40001), other, 27510).reply == Reply::kNotOurs);
  CHECK(read_pcp_map(pcp_answer(r, 40001), nonce, 27512).reply == Reply::kNotOurs);
  const auto refused = read_pcp_map(pcp_answer(r, 0, 2), nonce, 27510);
  CHECK(refused.reply == Reply::kRefused);
  CHECK(refused.result == 2);
  // A NAT-PMP router's "unsupported version", and nonsense.
  const std::uint8_t old[] = {0, 0x80, 0, 1, 0, 0, 0, 0};
  CHECK(read_pcp_map(old, nonce, 27510).reply == Reply::kOldVersion);
  const std::uint8_t short_one[] = {2, 0x81, 0, 0};
  CHECK(read_pcp_map(short_one, nonce, 27510).reply == Reply::kNotOurs);
  CHECK(read_pcp_map({}, nonce, 27510).reply == Reply::kNotOurs);
}

TEST_CASE("nat-pmp: requests and answers as RFC 6886 lays them out") {
  CHECK(natpmp_address_request() == std::array<std::uint8_t, 2>{0, 0});
  const auto m = natpmp_map_request(27510, 27510, 600);
  const std::array<std::uint8_t, 12> want{0, 1, 0, 0, 0x6B, 0x76, 0x6B, 0x76, 0, 0, 0x02, 0x58};
  CHECK(m == want);
  const std::uint8_t address[] = {0, 128, 0, 0, 0, 0, 0, 9, 203, 0, 113, 7};
  const auto a = read_natpmp_address(address);
  CHECK(a.reply == Reply::kOk);
  CHECK(a.external.ip == kRouterInternet);
  const std::uint8_t map[] = {0, 129, 0, 0, 0, 0, 0, 9, 0x6B, 0x76, 0x9C, 0x42, 0, 0, 0x02, 0x58};
  const auto r = read_natpmp_map(map, 27510);
  CHECK(r.reply == Reply::kOk);
  CHECK(r.external.port == 40002);
  CHECK(r.lifetime == 600);
  CHECK(read_natpmp_map(map, 27511).reply == Reply::kNotOurs);
  const std::uint8_t refused[] = {0, 129, 0, 2, 0, 0, 0, 9, 0x6B, 0x76, 0, 0, 0, 0, 0, 0};
  CHECK(read_natpmp_map(refused, 27510).reply == Reply::kRefused);
  CHECK(read_natpmp_map(refused, 27510).result == 2);
  const std::uint8_t truncated[] = {0, 129, 0, 0, 0, 0};
  CHECK(read_natpmp_map(truncated, 27510).reply == Reply::kNotOurs);
}

TEST_CASE("pcp router: forwards, renews nothing early, and removes the forward on close") {
  EnetLife enet;
  FakeRouter router(FakeRouter::Mode::kPcp);
  {
    Mapping m(config_for(router));
    REQUIRE(m.open());
    CHECK_FALSE(m.natpmp());
    CHECK(m.external() == Addr{kRouterInternet, 40001});
    m.renew_if_due();  // not half the lease yet: nothing sent
    CHECK(router.seen().size() == 1);
  }  // the destructor removes it
  const auto seen = router.seen();
  REQUIRE(seen.size() == 2);
  CHECK(get32(&seen[1][4]) == 0);  // lifetime 0: removal
  CHECK(std::memcmp(&seen[0][24], &seen[1][24], 12) == 0);  // the same nonce
}

TEST_CASE("pcp router: a NAT-PMP router gets NAT-PMP, with its internet address") {
  EnetLife enet;
  FakeRouter router(FakeRouter::Mode::kNatPmp);
  Mapping m(config_for(router));
  REQUIRE(m.open());
  CHECK(m.natpmp());
  CHECK(m.external() == Addr{kRouterInternet, 40002});
  m.close();
  const auto seen = router.seen();
  REQUIRE(seen.size() == 4);  // PCP, NAT-PMP map, address, removal
  CHECK(seen[3][1] == 1);
  CHECK(get16(&seen[3][6]) == 0);  // removal: port 0, lifetime 0
  CHECK(get32(&seen[3][8]) == 0);
  CHECK_FALSE(m.is_open());
}

TEST_CASE("pcp router: a router that says no, and one that does not answer") {
  EnetLife enet;
  {
    FakeRouter router(FakeRouter::Mode::kRefuse);
    std::vector<std::string> log;
    Config c = config_for(router);
    c.log = [&](const std::string& s) { log.push_back(s); };
    Mapping m(c);
    CHECK_FALSE(m.open());
    CHECK(router.seen().size() == 1);  // a PCP "no" is not asked again with NAT-PMP
    bool said = false;
    for (const auto& s : log) said = said || s.find("does not allow forwards") != std::string::npos;
    CHECK(said);
  }
  {
    FakeRouter router(FakeRouter::Mode::kSilent);
    Mapping m(config_for(router));
    CHECK_FALSE(m.open());
    CHECK(router.seen().size() == 4);  // PCP twice, then NAT-PMP twice
  }
}

TEST_CASE("local networks: broadcast addresses and masks") {
  using coop::server::nat::broadcast_of;
  using coop::server::nat::LocalNet;
  using coop::server::nat::mask_of;
  CHECK(mask_of(24) == 0xFFFFFF00u);
  CHECK(mask_of(0) == 0);
  CHECK(mask_of(32) == 0xFFFFFFFFu);
  CHECK(broadcast_of(LocalNet{0xC0A80105u, 0xFFFFFF00u, 0}) == 0xC0A801FFu);
  CHECK(broadcast_of(LocalNet{0x0A000005u, 0xFF000000u, 0}) == 0x0AFFFFFFu);
  CHECK(broadcast_of(LocalNet{0x0A000005u, 0xFFFFFFFEu, 0}) == 0);  // /31
  CHECK(broadcast_of(LocalNet{0x0A000005u, 0xFFFFFFFFu, 0}) == 0);  // /32
  // The system's list never has loopback, and never crashes.
  for (const LocalNet& net : coop::server::nat::local_networks()) CHECK((net.ip >> 24) != 127u);
}

TEST_CASE("pcp router: a 'done' without a port or a lease opens nothing") {
  EnetLife enet;
  {
    FakeRouter router(FakeRouter::Mode::kPcpPortZero);
    Mapping m(config_for(router));
    CHECK_FALSE(m.open());
    CHECK_FALSE(m.is_open());
  }
  {
    FakeRouter router(FakeRouter::Mode::kNatPmpNoLease);
    Mapping m(config_for(router));
    CHECK_FALSE(m.open());
    CHECK_FALSE(m.is_open());
  }
}

TEST_CASE("local networks: only a router on the card's own network is asked") {
  using coop::server::nat::gateway_on_link;
  using coop::server::nat::LocalNet;
  CHECK(gateway_on_link(LocalNet{0xC0A80105u, 0xFFFFFF00u, 0xC0A80101u}));
  CHECK_FALSE(gateway_on_link(LocalNet{0xC0A80105u, 0xFFFFFF00u, 0xC0A80201u}));  // another network
  CHECK_FALSE(gateway_on_link(LocalNet{0xC0A80105u, 0xFFFFFF00u, 0}));
  CHECK_FALSE(gateway_on_link(LocalNet{0xC0A80105u, 0xFFFFFF00u, 0xC0A80105u}));  // itself
}
