// The relay with join codes (server/relay*.hpp): its datagrams, its rules
// without a network, and a whole game join through it on 127.0.0.1 (host's
// server + host's link + relay + a guest in raw ENet).

#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/codec.hpp"
#include "enet_server.hpp"
#include "relay.hpp"
#include "relay_link.hpp"
#include "relay_wire.hpp"

using namespace coop::server;
using namespace coop::server::relay;

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

Message decoded(const Outgoing& out) {
  auto m = decode(out.bytes);
  REQUIRE(m.has_value());
  return *m;
}

Message register_message(std::uint64_t token, std::string key = {}) {
  Message m;
  m.kind = Kind::kRegister;
  m.token = token;
  m.key = std::move(key);
  return m;
}

Message lookup_message(const std::string& code) {
  Message m;
  m.kind = Kind::kLookup;
  m.code = code;
  return m;
}

const Endpoint kHost{0x0100007Fu, 40000};
const Endpoint kGuest{0x0200000Au, 50000};

struct Fixture {
  std::uint64_t next_random = 12345;
  Relay relay{[this] {
    RelayConfig c;
    c.random = [this] { return next_random++ * 0x9E3779B97F4A7C15ull; };
    return c;
  }()};

  std::string register_host(std::uint64_t token, Endpoint from = kHost, std::uint64_t now = 0) {
    auto out = relay.received(from, encode(register_message(token)), now);
    REQUIRE(out.size() == 1);
    Message m = decoded(out[0]);
    REQUIRE(m.kind == Kind::kRegistered);
    return m.code;
  }
  LookupResult lookup(const std::string& code, Endpoint from = kGuest, std::uint64_t now = 0) {
    auto out = relay.received(from, encode(lookup_message(code)), now);
    REQUIRE(out.size() == 1);
    Message m = decoded(out[0]);
    REQUIRE(m.kind == Kind::kFound);
    return static_cast<LookupResult>(m.status);
  }
};

}  // namespace

TEST_CASE("relay wire: every message goes through encode and decode") {
  Message m;
  m.kind = Kind::kLinkData;
  m.token = 0x0102030405060708ull;
  m.flow = 7;
  m.payload = {1, 2, 3};
  auto back = decode(encode(m));
  REQUIRE(back.has_value());
  CHECK(back->token == m.token);
  CHECK(back->flow == 7);
  CHECK(back->payload == m.payload);

  Message r = register_message(9, "secret");
  back = decode(encode(r));
  REQUIRE(back.has_value());
  CHECK(back->key == "secret");

  // Not ours: ENet's datagrams, truncated or unknown ones.
  CHECK_FALSE(decode(bytes_of("\x00\x01\x02\x03\x04")).has_value());
  auto cut = encode(r);
  cut.pop_back();
  CHECK_FALSE(decode(cut).has_value());
  auto unknown = encode(r);
  unknown[4] = 0x77;
  CHECK_FALSE(decode(unknown).has_value());
  CHECK_FALSE(decode(encode(lookup_message("ABCD0FGH"))).has_value());  // 0 is no code letter
  // ENet's first byte never has the "compressed" bit we use as the magic.
  CHECK((kMagic[0] & 0x40) != 0);
}

TEST_CASE("relay wire: join codes are typed loosely and shown in two halves") {
  const std::string code = make_code(0x123456789Aull);
  CHECK(code.size() == kCodeLength);
  CHECK(normalize_code(code) == code);
  CHECK(normalize_code(" " + show_code(code) + " ") == code);
  std::string lower = code;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  CHECK(normalize_code(lower) == code);
  CHECK_FALSE(normalize_code("ABC").has_value());
  CHECK_FALSE(normalize_code("ABCDEFGI").has_value());
  CHECK(show_code("ABCDEFGH") == "ABCD-EFGH");
  CHECK(make_code(1) != make_code(2));
}

TEST_CASE("relay: a host gets a code, and a guest with it reaches the host and back") {
  Fixture f;
  const std::string code = f.register_host(77);
  CHECK(f.relay.sessions() == 1);
  // The same token again (a retry) keeps the code.
  CHECK(f.register_host(77) == code);
  CHECK(f.relay.sessions() == 1);

  // Before the code: turned away.
  const std::vector<std::uint8_t> enet_datagram = {0x80, 0x00, 0x12, 0x34, 0x56};
  CHECK(f.relay.received(kGuest, enet_datagram, 10).empty());
  CHECK(f.relay.stats().not_let_in == 1);

  CHECK(f.lookup(code) == LookupResult::kOk);
  auto out = f.relay.received(kGuest, enet_datagram, 20);
  REQUIRE(out.size() == 1);
  CHECK(out[0].to == kHost);
  Message to_host = decoded(out[0]);
  CHECK(to_host.kind == Kind::kRelayData);
  CHECK(to_host.payload == enet_datagram);

  // The host's answer goes to that guest port, unwrapped.
  Message answer;
  answer.kind = Kind::kLinkData;
  answer.token = 77;
  answer.flow = to_host.flow;
  answer.payload = {9, 8, 7};
  out = f.relay.received(kHost, encode(answer), 30);
  REQUIRE(out.size() == 1);
  CHECK(out[0].to == kGuest);
  CHECK(out[0].bytes == answer.payload);

  // A second game from the same address (another port) is another flow.
  const Endpoint second{kGuest.host, 50001};
  out = f.relay.received(second, enet_datagram, 40);
  REQUIRE(out.size() == 1);
  CHECK(decoded(out[0]).flow != to_host.flow);
  CHECK(f.relay.flows() == 2);

  // A wrong token cannot send to the guests.
  answer.token = 78;
  out = f.relay.received(kHost, encode(answer), 50);
  REQUIRE(out.size() == 1);
  CHECK(decoded(out[0]).kind == Kind::kRefused);
}

TEST_CASE("relay: wrong codes are counted and then refused for a while") {
  Fixture f;
  const std::string code = f.register_host(1);
  std::string wrong = code;
  wrong[0] = wrong[0] == 'A' ? 'B' : 'A';
  for (int i = 0; i < 10; ++i) CHECK(f.lookup(wrong, kGuest, 100) == LookupResult::kUnknown);
  // Over the limit: even the right code waits.
  CHECK(f.lookup(code, kGuest, 200) == LookupResult::kSlowDown);
  CHECK(f.relay.stats().lookups_limited == 1);
  // Another address is not held up.
  CHECK(f.lookup(code, Endpoint{0x0300000Au, 1}, 200) == LookupResult::kOk);
  // After the window (10 minutes).
  f.relay.tick(100 + 10 * 60'000);
  CHECK(f.lookup(code, kGuest, 100 + 10 * 60'000) == LookupResult::kOk);
}

TEST_CASE("relay: a quiet host's code ends, and keepalives keep it and follow a new address") {
  Fixture f;
  const std::string code = f.register_host(5);
  CHECK(f.lookup(code) == LookupResult::kOk);
  CHECK(f.relay.received(kGuest, std::vector<std::uint8_t>{0x80, 1}, 0).size() == 1);

  // The host's router gave it another port: the token says it is the same.
  const Endpoint moved{kHost.host, 40001};
  Message keep;
  keep.kind = Kind::kKeepalive;
  keep.token = 5;
  auto out = f.relay.received(moved, encode(keep), 20'000);
  REQUIRE(out.size() == 1);
  CHECK(decoded(out[0]).code == code);
  out = f.relay.received(kGuest, std::vector<std::uint8_t>{0x80, 2}, 20'000);
  REQUIRE(out.size() == 1);
  CHECK(out[0].to == moved);

  // The guest went quiet for a minute: its flow closes and the host hears it.
  CHECK(f.relay.received(moved, encode(keep), 70'000).size() == 1);
  out = f.relay.tick(81'000);
  REQUIRE(out.size() == 1);
  CHECK(decoded(out[0]).kind == Kind::kRelayClose);
  CHECK(f.relay.flows() == 0);

  // 30 seconds without the host: gone, and its code with it.
  f.relay.tick(70'000 + 30'000);
  CHECK(f.relay.sessions() == 0);
  CHECK(f.lookup(code, Endpoint{0x0400000Au, 1}, 101'000) == LookupResult::kUnknown);
  CHECK(f.relay.received(kGuest, std::vector<std::uint8_t>{0x80, 3}, 101'000).empty());
  // Its keepalive is now refused, so the link asks for a new code.
  out = f.relay.received(moved, encode(keep), 102'000);
  REQUIRE(out.size() == 1);
  CHECK(static_cast<Refusal>(decoded(out[0]).status) == Refusal::kUnknownSession);
}

TEST_CASE("relay: host key, per-address limit and bye") {
  RelayConfig config;
  config.host_key = "letmein";
  Relay relay(config);
  auto out = relay.received(kHost, encode(register_message(1, "nope")), 0);
  REQUIRE(out.size() == 1);
  CHECK(static_cast<Refusal>(decoded(out[0]).status) == Refusal::kWrongKey);
  CHECK(relay.sessions() == 0);

  out = relay.received(kHost, encode(register_message(1, "letmein")), 0);
  REQUIRE(decoded(out[0]).kind == Kind::kRegistered);
  out = relay.received(Endpoint{kHost.host, 2}, encode(register_message(2, "letmein")), 0);
  REQUIRE(decoded(out[0]).kind == Kind::kRegistered);
  out = relay.received(Endpoint{kHost.host, 3}, encode(register_message(3, "letmein")), 0);
  CHECK(static_cast<Refusal>(decoded(out[0]).status) == Refusal::kTooMany);
  CHECK(relay.sessions() == 2);

  Message bye;
  bye.kind = Kind::kBye;
  bye.token = 1;
  CHECK(relay.received(kHost, encode(bye), 1).empty());
  CHECK(relay.sessions() == 1);
}

TEST_CASE("relay: link addresses") {
  std::string host;
  std::uint16_t port = 0;
  CHECK(parse_relay_address("relay.example.net", host, port));
  CHECK(host == "relay.example.net");
  CHECK(port == kDefaultRelayPort);
  CHECK(parse_relay_address("1.2.3.4:4000", host, port));
  CHECK(host == "1.2.3.4");
  CHECK(port == 4000);
  CHECK_FALSE(parse_relay_address("1.2.3.4:0", host, port));
  CHECK_FALSE(parse_relay_address("", host, port));
  CHECK_FALSE(parse_relay_address("bad host", host, port));
}

namespace {

using namespace coop::protocol;

struct Guest {
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  bool connected = false;
  std::vector<coop::protocol::Message> inbox;

  Guest() { host = enet_host_create(nullptr, 1, kChannelCount, 0, 0); }
  ~Guest() {
    if (host != nullptr) enet_host_destroy(host);
  }
  void connect(std::uint16_t port) {
    ENetAddress address{};
    enet_address_set_host_ip(&address, "127.0.0.1");
    address.port = port;
    host->mtu = kDatagramBytes;
    peer = enet_host_connect(host, &address, kChannelCount, 0);
  }
  void pump() {
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0) {
      if (ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
      if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        coop::protocol::Message m;
        std::span<const std::uint8_t> b(ev.packet->data, ev.packet->dataLength);
        if (coop::protocol::decode(b, m) == Error::none) inbox.push_back(m);
        enet_packet_destroy(ev.packet);
      }
    }
  }
  template <typename T>
  const T* find() const {
    for (const auto& m : inbox) {
      if (const auto* t = std::get_if<T>(&m)) return t;
    }
    return nullptr;
  }
};

// A UDP socket like Join.bat's lookup.
LookupResult socket_lookup(ENetSocket s, std::uint16_t relay_port, const std::string& code,
                           const std::function<void()>& pump) {
  ENetAddress to{};
  enet_address_set_host_ip(&to, "127.0.0.1");
  to.port = relay_port;
  const auto bytes = encode(lookup_message(code));
  ENetBuffer b;
  b.data = const_cast<std::uint8_t*>(bytes.data());
  b.dataLength = bytes.size();
  REQUIRE(enet_socket_send(s, &to, &b, 1) > 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::uint8_t data[64];
  while (std::chrono::steady_clock::now() < deadline) {
    pump();
    ENetAddress from{};
    ENetBuffer in;
    in.data = data;
    in.dataLength = sizeof data;
    const int n = enet_socket_receive(s, &from, &in, 1);
    if (n > 0) {
      auto m = decode(std::span<const std::uint8_t>(data, static_cast<std::size_t>(n)));
      REQUIRE(m.has_value());
      REQUIRE(m->kind == Kind::kFound);
      return static_cast<LookupResult>(m->status);
    }
  }
  FAIL("the relay did not answer the lookup");
  return LookupResult::kUnknown;
}

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

}  // namespace

TEST_CASE("relay loopback: a guest joins the host's server through the relay with the code") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  options.session.join_password = "";
  EnetServer server(options);
  REQUIRE(server.start());

  RelayServer relay_server(0, std::nullopt, RelayConfig{});
  REQUIRE(relay_server.start());
  const std::uint16_t relay_port = relay_server.port();
  REQUIRE(relay_port != 0);

  RelayLinkConfig link_config;
  link_config.relay_host = "127.0.0.1";
  link_config.relay_port = relay_port;
  link_config.server_port = server.port();
  std::string announced;
  link_config.on_code = [&](const std::string& code) { announced = code; };
  RelayLink link(link_config);
  std::string why;
  REQUIRE(link.start(why));

  Guest guest;
  auto pump = [&] {
    server.service(0);
    link.service();
    relay_server.service(1);
    link.service();
    guest.pump();
  };
  auto pump_until = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      pump();
      if (done()) return true;
    }
    return false;
  };

  REQUIRE(pump_until([&] { return !link.code().empty(); }));
  CHECK(announced == link.code());

  ENetSocket lookup_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  REQUIRE(lookup_socket != ENET_SOCKET_NULL);
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  REQUIRE(enet_socket_bind(lookup_socket, &any) == 0);
  enet_socket_set_option(lookup_socket, ENET_SOCKOPT_NONBLOCK, 1);

  // A wrong code first: no luck.
  std::string wrong = link.code();
  wrong[7] = wrong[7] == 'A' ? 'B' : 'A';
  CHECK(socket_lookup(lookup_socket, relay_port, wrong, pump) == LookupResult::kUnknown);
  CHECK(socket_lookup(lookup_socket, relay_port, link.code(), pump) == LookupResult::kOk);
  enet_socket_destroy(lookup_socket);

  // The game talks plain ENet to the relay's port.
  guest.connect(relay_port);
  REQUIRE(pump_until([&] { return guest.connected; }));
  CHECK(link.flows() == 1);
  Hello hello;
  hello.build_id = "relay test";
  hello.game_build_id = "e0f3ccfaf50065ae";
  hello.game_version = "1.1.1";
  hello.nickname = "Saria";
  auto bytes = coop::protocol::encode(coop::protocol::Message{hello});
  REQUIRE(bytes.has_value());
  ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(), ENET_PACKET_FLAG_RELIABLE);
  REQUIRE(enet_peer_send(guest.peer, kChannelSession, p) == 0);
  REQUIRE(pump_until([&] { return guest.find<Welcome>() != nullptr; }));
  CHECK(guest.find<Welcome>()->player_number == 1);
  CHECK(server.session().player_count() == 1);
  CHECK(relay_server.relay().stats().to_host > 0);
  CHECK(relay_server.relay().stats().to_guest > 0);

  // The host stops: the relay drops the code at once.
  link.stop();
  REQUIRE(pump_until([&] { return relay_server.relay().sessions() == 0; }));
}

TEST_CASE("relay loopback: a game without the code never reaches the host") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  EnetServer server(options);
  REQUIRE(server.start());
  RelayServer relay_server(0, std::nullopt, RelayConfig{});
  REQUIRE(relay_server.start());
  RelayLinkConfig link_config;
  link_config.relay_host = "127.0.0.1";
  link_config.relay_port = relay_server.port();
  link_config.server_port = server.port();
  RelayLink link(link_config);
  std::string why;
  REQUIRE(link.start(why));

  Guest guest;
  guest.connect(relay_server.port());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < deadline) {
    server.service(0);
    link.service();
    relay_server.service(1);
    guest.pump();
  }
  CHECK_FALSE(link.code().empty());
  CHECK_FALSE(guest.connected);
  CHECK(link.flows() == 0);
  CHECK(relay_server.relay().stats().not_let_in > 0);
}
