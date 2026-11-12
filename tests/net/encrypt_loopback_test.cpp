// Encrypted traffic end to end (docs/protocol.md "Encrypted traffic"): the
// real client and the real server on 127.0.0.1, one of the clients through a
// tap that keeps every datagram it passes, as someone on the path would see
// them.
#include <doctest/doctest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "enet_server.hpp"

namespace proto = coop::protocol;
using coop::net::Client;
using coop::net::ClientConfig;
using coop::net::EventKind;
using coop::net::State;

namespace {

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

// Client <-> tap <-> server; keeps a copy of every datagram, both ways.
class Tap {
 public:
  explicit Tap(std::uint16_t server_port) {
    outer_ = open();
    inner_ = open();
    ENetAddress got{};
    REQUIRE(enet_socket_get_address(outer_, &got) == 0);
    port_ = got.port;
    REQUIRE(enet_address_set_host_ip(&server_, "127.0.0.1") == 0);
    server_.port = server_port;
  }
  ~Tap() {
    enet_socket_destroy(outer_);
    enet_socket_destroy(inner_);
  }
  Tap(const Tap&) = delete;
  Tap& operator=(const Tap&) = delete;

  std::uint16_t port() const { return port_; }

  void service() {
    std::uint8_t data[65536];
    ENetBuffer buffer{};
    for (;;) {  // client to server
      ENetAddress from{};
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int n = enet_socket_receive(outer_, &from, &buffer, 1);
      if (n <= 0) break;
      client_ = from;
      have_client_ = true;
      pass(inner_, server_, data, static_cast<std::size_t>(n));
    }
    for (;;) {  // server to client
      ENetAddress from{};
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int n = enet_socket_receive(inner_, &from, &buffer, 1);
      if (n <= 0) break;
      if (have_client_) pass(outer_, client_, data, static_cast<std::size_t>(n));
    }
  }

  // Some datagram that went through holds these bytes.
  bool saw(const std::string& text) const {
    for (const auto& d : seen_) {
      if (std::search(d.begin(), d.end(), text.begin(), text.end()) != d.end()) return true;
    }
    return false;
  }

 private:
  static ENetSocket open() {
    const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    REQUIRE(s != ENET_SOCKET_NULL);
    ENetAddress any{};
    REQUIRE(enet_address_set_host_ip(&any, "127.0.0.1") == 0);
    any.port = 0;
    REQUIRE(enet_socket_bind(s, &any) == 0);
    REQUIRE(enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1) == 0);
    return s;
  }
  void pass(ENetSocket out, const ENetAddress& to, std::uint8_t* data, std::size_t n) {
    seen_.emplace_back(data, data + n);
    ENetBuffer b{};
    b.data = data;
    b.dataLength = n;
    enet_socket_send(out, &to, &b, 1);
  }

  ENetSocket outer_ = ENET_SOCKET_NULL;
  ENetSocket inner_ = ENET_SOCKET_NULL;
  ENetAddress server_{};
  ENetAddress client_{};
  bool have_client_ = false;
  std::uint16_t port_ = 0;
  std::vector<std::vector<std::uint8_t>> seen_;
};

ClientConfig config_to(std::uint16_t port, const std::string& name,
                       const std::string& password) {
  ClientConfig c;
  c.host = "127.0.0.1";
  c.port = port;
  c.hello.build_id = "encrypt test";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  c.hello.nickname = name;
  c.join_password = password;
  c.retry_first_ms = 50;
  c.retry_max_ms = 400;
  return c;
}

struct World {
  EnetLife enet;
  coop::server::EnetServer server;
  Tap tap;
  Client host;   // straight to the server
  Client guest;  // through the tap

  explicit World(const std::string& server_password, const std::string& guest_password)
      : server(options(server_password)),
        tap(start_server()),
        host(config_to(server.port(), "Link", server_password)),
        guest(config_to(tap.port(), "Saria", guest_password)) {}

  std::uint16_t start_server() {
    REQUIRE(server.start());
    return server.port();
  }

  static coop::server::ServerOptions options(const std::string& password) {
    coop::server::ServerOptions o;
    o.port = 0;
    o.session.join_password = password;
    return o;
  }

  bool pump(const std::function<bool()>& done, int ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
      server.service(1);
      host.service(0);
      tap.service();
      guest.service(0);
      tap.service();
      if (done()) return true;
    }
    return false;
  }
};

// The text of the first chat event `c` got, if any.
std::optional<std::string> chat_seen(Client& c) {
  while (auto e = c.next_event()) {
    if (e->kind == EventKind::chat) return e->text;
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("encrypted traffic: with a join password nothing readable crosses the path") {
  World w("deku-sprout-31", "deku-sprout-31");
  REQUIRE(w.host.start());
  REQUIRE(w.pump([&] { return w.host.state() == State::connected; }));
  REQUIRE(w.guest.start());
  REQUIRE(w.pump([&] { return w.guest.state() == State::connected; }));
  CHECK(w.host.encrypted());
  CHECK(w.guest.encrypted());
  CHECK(w.server.encrypted_connections() == 2);

  const std::string secret = "the chest is behind the waterfall";
  REQUIRE(w.guest.send_chat(secret));
  std::optional<std::string> got;
  REQUIRE(w.pump([&] { return (got = chat_seen(w.host)).has_value(); }));
  CHECK(*got == secret);  // it does get there, opened
  const std::string answer = "on my way to Zora's river";
  REQUIRE(w.host.send_chat(answer));
  REQUIRE(w.pump([&] { return (got = chat_seen(w.guest)).has_value(); }));
  CHECK(*got == answer);

  // Neither chat line, nor the host's name in the guest's WELCOME, nor the
  // password was in any datagram.
  CHECK_FALSE(w.tap.saw(secret));
  CHECK_FALSE(w.tap.saw(answer));
  CHECK_FALSE(w.tap.saw("deku-sprout-31"));
  CHECK_FALSE(w.tap.saw("Link"));
  CHECK(w.server.unsealed_drops() == 0);
}

TEST_CASE("encrypted traffic: without a password the traffic stays plain (the tap reads it)") {
  World w("", "");
  REQUIRE(w.host.start());
  REQUIRE(w.pump([&] { return w.host.state() == State::connected; }));
  REQUIRE(w.guest.start());
  REQUIRE(w.pump([&] { return w.guest.state() == State::connected; }));
  CHECK_FALSE(w.host.encrypted());
  CHECK_FALSE(w.guest.encrypted());
  CHECK(w.server.encrypted_connections() == 0);
  REQUIRE(w.guest.send_chat("plain as day"));
  std::optional<std::string> got;
  REQUIRE(w.pump([&] { return (got = chat_seen(w.host)).has_value(); }));
  CHECK(w.tap.saw("plain as day"));  // so the check above can see a chat line
}

TEST_CASE("encrypted traffic: a wrong password is refused as before and gets no keys") {
  World w("deku-sprout-31", "deku-sprout-32");
  REQUIRE(w.host.start());
  REQUIRE(w.pump([&] { return w.host.state() == State::connected; }));
  REQUIRE(w.guest.start());
  REQUIRE(w.pump([&] { return w.guest.state() == State::refused; }));
  std::optional<proto::RefuseReason> reason;
  while (auto e = w.guest.next_event()) {
    if (e->kind == EventKind::refused) reason = e->refuse_reason;
  }
  REQUIRE(reason.has_value());
  CHECK(*reason == proto::RefuseReason::bad_password);
  w.pump([] { return false; }, 200);
  CHECK(w.server.encrypted_connections() == 1);  // only the host's
}

TEST_CASE("encrypted traffic: a game with a password does not join a server without one") {
  World w("", "leftover-password");
  REQUIRE(w.guest.start());
  REQUIRE(w.pump([&] { return w.guest.state() == State::refused; }));
  std::optional<proto::RefuseReason> reason;
  while (auto e = w.guest.next_event()) {
    if (e->kind == EventKind::refused) reason = e->refuse_reason;
  }
  REQUIRE(reason.has_value());
  CHECK(*reason == proto::RefuseReason::not_encrypted);
}
