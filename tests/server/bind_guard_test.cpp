#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <optional>

#include <enet/enet.h>

#include "bind_host.hpp"
#include "coop/net/bind_guard.hpp"
#include "enet_server.hpp"
#include "host_service.hpp"
#include "join_helper.hpp"
#include "options.hpp"
#include "pcp.hpp"
#include "relay_link.hpp"
#include "upnp.hpp"

// Windows Defender Firewall asks about every program path that listens on all addresses, so the test programs
// (tests/main.cpp) turn the guard on and fail when anything but 127.x.x.x is bound. Players' programs never do: with
// no address given, a listener still binds all addresses. These tests keep both halves.

using namespace coop;

namespace {

// Takes the refusals a test makes on purpose out of the count tests/main.cpp checks at exit.
struct KeepGuardCount {
  int before = net::refused_binds();
  ~KeepGuardCount() {
    net::reset_refused_binds();
    for (int i = 0; i < before; ++i) (void)net::bind_allowed(std::nullopt);
  }
};

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

}  // namespace

TEST_CASE("bind guard: the test programs run with it on") {
  CHECK(net::loopback_binds_required());
}

TEST_CASE("bind guard: only 127.x.x.x passes while it is on, and a refusal is counted") {
  KeepGuardCount keep;
  net::reset_refused_binds();
  CHECK(net::bind_allowed(0x7F000001u));
  CHECK(net::bind_allowed(0x7F010203u));
  CHECK(net::refused_binds() == 0);
  CHECK_FALSE(net::bind_allowed(std::nullopt));  // (all addresses)
  CHECK_FALSE(net::bind_allowed(0x0A000001u));   // 10.0.0.1
  CHECK_FALSE(net::bind_allowed(0x00000000u));   // 0.0.0.0
  CHECK_FALSE(net::bind_allowed(0xC0A80101u));   // 192.168.1.1
  CHECK(net::refused_binds() == 4);
}

TEST_CASE("bind guard: bind_socket refuses all addresses and a LAN address before it binds, and binds loopback") {
  KeepGuardCount keep;
  EnetLife enet;
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  REQUIRE(s != ENET_SOCKET_NULL);
  CHECK(server::bind_socket(s, std::nullopt, 0) == -1);
  CHECK(server::bind_socket(s, 0xC0A80101u, 0) == -1);
  CHECK(server::bind_socket(s, 0x7F000001u, 0) == 0);
  ENetAddress got{};
  REQUIRE(enet_socket_get_address(s, &got) == 0);
  CHECK(got.port != 0);
  enet_socket_destroy(s);
}

TEST_CASE("bind guard: with it off (a player's program) every address is allowed") {
  KeepGuardCount keep;
  net::require_loopback_binds(false);
  struct Restore {
    ~Restore() { net::require_loopback_binds(true); }
  } restore;
  CHECK(net::bind_allowed(std::nullopt));
  CHECK(net::bind_allowed(0xC0A80101u));
  CHECK(net::refused_binds() == 0);
}

TEST_CASE("bind default: no address means all addresses, for a player's server, relay, helper and mapping") {
  // The address the sockets would bind (nothing is bound here).
  const ENetAddress all = server::local_address(std::nullopt, 27510);
  CHECK(all.host == ENET_HOST_ANY);
  CHECK(all.port == 27510);
  const ENetAddress one = server::local_address(0x7F000001u, 7);
  unsigned char b[4];
  std::memcpy(b, &one.host, 4);
  CHECK(b[0] == 127);
  CHECK(b[1] == 0);
  CHECK(b[2] == 0);
  CHECK(b[3] == 1);
  CHECK(one.port == 7);

  // Every config a player's program fills leaves the bind address unset.
  CHECK_FALSE(server::ServerOptions{}.bind.has_value());
  CHECK_FALSE(server::CommandLine{}.bind.has_value());
  CHECK_FALSE(server::HostServiceConfig{}.bind.has_value());
  CHECK_FALSE(server::HostServiceConfig{}.helper_bind.has_value());
  CHECK_FALSE(server::nat::JoinConfig{}.bind.has_value());
  CHECK_FALSE(server::pcp::Config{}.bind.has_value());
  CHECK_FALSE(server::upnp::Config{}.bind.has_value());
  CHECK_FALSE(server::relay::RelayLinkConfig{}.bind.has_value());
}

TEST_CASE("bind default: the test bind of the Hookshot menu's helper sockets is loopback in tests and none for a player") {
  REQUIRE(net::test_bind().has_value());  // (tests/main.cpp)
  CHECK(*net::test_bind() == net::kLoopbackIp);
  net::set_test_bind(std::nullopt);       // (a player's program never sets it)
  CHECK_FALSE(net::test_bind().has_value());
  net::set_test_bind(net::kLoopbackIp);
}
