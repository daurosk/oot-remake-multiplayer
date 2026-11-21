// The relay with join codes (server/relay*.hpp): its datagrams, its rules
// without a network, and a whole game join through it on 127.0.0.1 (host's
// server + host's link + relay + a guest in raw ENet).

#include <doctest/doctest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/join_password.hpp"
#include "coop/protocol/traffic_crypto.hpp"
#include "enet_server.hpp"
#include "host_service.hpp"
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

Message register_room_message(std::uint64_t token, std::string room, std::string key = {}) {
  Message m;
  m.kind = Kind::kRegisterName;
  m.token = token;
  m.key = std::move(key);
  m.name = std::move(room);
  return m;
}

Message lookup_room_message(std::string room) {
  Message m;
  m.kind = Kind::kLookupName;
  m.name = std::move(room);
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
  // The relay's one answer to a host asking for a room.
  Message register_room(std::uint64_t token, const std::string& room, Endpoint from = kHost, std::uint64_t now = 0) {
    auto out = relay.received(from, encode(register_room_message(token, room)), now);
    REQUIRE(out.size() == 1);
    return decoded(out[0]);
  }
  LookupResult lookup_room(const std::string& room, Endpoint from = kGuest, std::uint64_t now = 0) {
    auto out = relay.received(from, encode(lookup_room_message(room)), now);
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

TEST_CASE("relay wire: room name messages, and what a room name is (issue 261)") {
  auto back = decode(encode(register_room_message(9, "Forest-Gang", "secret")));
  REQUIRE(back.has_value());
  CHECK(back->kind == Kind::kRegisterName);
  CHECK(back->token == 9);
  CHECK(back->key == "secret");
  CHECK(back->name == "Forest-Gang");

  back = decode(encode(lookup_room_message("forest-gang")));
  REQUIRE(back.has_value());
  CHECK(back->kind == Kind::kLookupName);
  CHECK(back->name == "forest-gang");

  Message held;
  held.kind = Kind::kRegisteredName;
  held.token = 3;
  held.code = "ABCDEFGH";
  held.name = "Forest-Gang";
  back = decode(encode(held));
  REQUIRE(back.has_value());
  CHECK(back->code == "ABCDEFGH");
  CHECK(back->name == "Forest-Gang");

  // A name longer than any room name is not a relay message.
  CHECK_FALSE(decode(encode(lookup_room_message(std::string(kMaxRoomLength + 1, 'a')))).has_value());
  auto cut = encode(lookup_room_message("forest-gang"));
  cut.pop_back();
  CHECK_FALSE(decode(cut).has_value());

  CHECK(normalize_room("forest-gang") == "forest-gang");
  CHECK(normalize_room("  Zelda_Fans_01\t") == "Zelda_Fans_01");
  CHECK(normalize_room("abc") == "abc");
  CHECK(normalize_room(std::string(kMaxRoomLength, 'z')).has_value());
  CHECK_FALSE(normalize_room("ab").has_value());
  CHECK_FALSE(normalize_room(std::string(kMaxRoomLength + 1, 'z')).has_value());
  CHECK_FALSE(normalize_room("forest gang").has_value());
  CHECK_FALSE(normalize_room("caf\xc3\xa9").has_value());
  CHECK_FALSE(normalize_room("").has_value());
  CHECK(room_key("Forest-Gang") == "forest-gang");
}

TEST_CASE("relay: a host holds a room name and a guest joins with it in any case (issue 261)") {
  Fixture f;
  Message held = f.register_room(21, "Forest-Gang");
  REQUIRE(held.kind == Kind::kRegisteredName);
  CHECK(held.name == "Forest-Gang");
  CHECK(held.code.size() == kCodeLength);
  CHECK(f.relay.rooms() == 1);
  // A retry with the same token keeps the code and the room.
  Message again = f.register_room(21, "Forest-Gang");
  CHECK(again.code == held.code);
  CHECK(f.relay.sessions() == 1);
  // The join code still works next to the name.
  CHECK(f.lookup(held.code, Endpoint{0x0500000Au, 1}) == LookupResult::kOk);

  CHECK(f.lookup_room("forest-gang") == LookupResult::kOk);
  const std::vector<std::uint8_t> enet_datagram = {0x80, 0x00, 0x12, 0x34};
  auto out = f.relay.received(kGuest, enet_datagram, 10);
  REQUIRE(out.size() == 1);
  CHECK(out[0].to == kHost);
  CHECK(decoded(out[0]).payload == enet_datagram);
  CHECK(f.lookup_room("  FOREST-GANG ", Endpoint{0x0600000Au, 1}) == LookupResult::kOk);

  // A keepalive is answered with the code; the room stays the host's.
  Message keep;
  keep.kind = Kind::kKeepalive;
  keep.token = 21;
  out = f.relay.received(kHost, encode(keep), 20);
  REQUIRE(out.size() == 1);
  CHECK(decoded(out[0]).kind == Kind::kRegistered);
  CHECK(f.relay.rooms() == 1);
}

TEST_CASE("relay: a room name is one host's until it stops, and bad names are refused (issue 261)") {
  Fixture f;
  REQUIRE(f.register_room(1, "forest-gang").kind == Kind::kRegisteredName);
  // Another host (another token) cannot take it, in any case; no session is made for it.
  Message taken = f.register_room(2, "Forest-GANG", Endpoint{0x0700000Au, 1});
  CHECK(taken.kind == Kind::kRefused);
  CHECK(static_cast<Refusal>(taken.status) == Refusal::kNameTaken);
  CHECK(f.relay.sessions() == 1);
  CHECK(f.relay.stats().rooms_taken == 1);

  Message bad = f.register_room(3, "no spaces here", Endpoint{0x0800000Au, 1});
  CHECK(static_cast<Refusal>(bad.status) == Refusal::kBadName);
  bad = f.register_room(3, "ab", Endpoint{0x0800000Au, 1});
  CHECK(static_cast<Refusal>(bad.status) == Refusal::kBadName);
  CHECK(f.relay.sessions() == 1);

  // The host moves to another name: the old one is free at once.
  REQUIRE(f.register_room(1, "lake-gang").name == "lake-gang");
  CHECK(f.relay.rooms() == 1);
  CHECK(f.register_room(2, "forest-gang", Endpoint{0x0700000Au, 1}).kind == Kind::kRegisteredName);
  CHECK(f.relay.rooms() == 2);

  // Bye frees it.
  Message bye;
  bye.kind = Kind::kBye;
  bye.token = 2;
  CHECK(f.relay.received(Endpoint{0x0700000Au, 1}, encode(bye), 10).empty());
  CHECK(f.relay.rooms() == 1);
  CHECK(f.lookup_room("forest-gang", Endpoint{0x0900000Au, 1}, 20) == LookupResult::kUnknown);

  // A host that goes quiet for 30 seconds loses its room too; then anyone may take it.
  f.relay.tick(30'000);
  CHECK(f.relay.sessions() == 0);
  CHECK(f.relay.rooms() == 0);
  CHECK(f.register_room(4, "lake-gang", Endpoint{0x0A00000Au, 1}, 31'000).kind == Kind::kRegisteredName);
}

TEST_CASE("relay: wrong room names count with wrong codes against one limit (issue 261)") {
  Fixture f;
  const std::string code = f.register_room(1, "forest-gang").code;
  std::string wrong = code;
  wrong[0] = wrong[0] == 'A' ? 'B' : 'A';
  for (int i = 0; i < 5; ++i) CHECK(f.lookup_room("lake-gang-" + std::to_string(i), kGuest, 100) == LookupResult::kUnknown);
  for (int i = 0; i < 4; ++i) CHECK(f.lookup(wrong, kGuest, 100) == LookupResult::kUnknown);
  // Not a room name at all counts too.
  CHECK(f.lookup_room("x", kGuest, 100) == LookupResult::kUnknown);
  CHECK(f.relay.stats().lookups_wrong == 10);
  // Ten wrong: even the right name or code waits.
  CHECK(f.lookup_room("forest-gang", kGuest, 200) == LookupResult::kSlowDown);
  CHECK(f.lookup(code, kGuest, 200) == LookupResult::kSlowDown);
  CHECK(f.lookup_room("forest-gang", Endpoint{0x0300000Au, 1}, 200) == LookupResult::kOk);
}

TEST_CASE("relay join text: a code or a room name with its relay, as Join.bat reads it (issue 261)") {
  auto j = parse_relay_join(" abcd-efgh@relay.example.net:27600 ");
  REQUIRE(j.has_value());
  CHECK(j->code == "ABCDEFGH");
  CHECK(j->room == "abcd-efgh");  // a room name too: the code is tried first
  CHECK(j->relay_host == "relay.example.net");
  CHECK(j->relay_port == 27600);

  j = parse_relay_join("Forest-Gang@1.2.3.4");
  REQUIRE(j.has_value());
  CHECK(j->code.empty());
  CHECK(j->room == "Forest-Gang");
  CHECK(j->relay_port == kDefaultRelayPort);

  j = parse_relay_join("zeldafan@1.2.3.4");  // shaped like a code: both, the code first
  REQUIRE(j.has_value());
  CHECK(j->code == "ZELDAFAN");
  CHECK(j->room == "zeldafan");

  j = parse_relay_join("ABCD-EFG@1.2.3.4");  // seven letters: only a room
  REQUIRE(j.has_value());
  CHECK(j->code.empty());
  CHECK(j->room == "ABCD-EFG");

  j = parse_relay_join("lake-gang@1.2.3.4");  // laid out as a code, and a room name too: both
  REQUIRE(j.has_value());
  CHECK(j->code == "LAKEGANG");
  CHECK(j->room == "lake-gang");

  j = parse_relay_join("bird-song@1.2.3.4");  // laid out as a code, but I and O are no code letters: a room
  REQUIRE(j.has_value());
  CHECK(j->code.empty());
  CHECK(j->room == "bird-song");

  for (const char* bad : {"forest-gang", "forest-gang@", "ab@1.2.3.4", "forest gang@1.2.3.4",
                          "forest-gang@1.2.3.4:0", "forest-gang@bad;host", "@1.2.3.4"}) {
    CHECK_MESSAGE(!parse_relay_join(bad).has_value(), bad);
  }

  RelayJoin room;
  room.room = "forest-gang";
  room.relay_host = "relay.example.net";
  CHECK(lookup_text(LookupResult::kUnknown, room).find("no room \"forest-gang\"") != std::string::npos);
  CHECK(lookup_text(std::nullopt, room).find("before room names") != std::string::npos);
  RelayJoin code;
  code.code = "ABCDEFGH";
  CHECK(lookup_text(LookupResult::kUnknown, code).find("join code") != std::string::npos);
  RelayJoin both = room;
  both.code = "LAKEGANG";
  CHECK(lookup_text(LookupResult::kUnknown, both).find("no join code or room \"forest-gang\"") != std::string::npos);
  CHECK(lookup_text(LookupResult::kOk, code).find("connecting") != std::string::npos);
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
  // After the window (10 minutes); the host kept its code with a keepalive.
  Message keep;
  keep.kind = Kind::kKeepalive;
  keep.token = 1;
  CHECK(f.relay.received(kHost, encode(keep), 100 + 10 * 60'000).size() == 1);
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
LookupResult socket_lookup(ENetSocket s, std::uint16_t relay_port, const relay::Message& lookup,
                           const std::function<void()>& pump) {
  ENetAddress to{};
  enet_address_set_host_ip(&to, "127.0.0.1");
  to.port = relay_port;
  const auto bytes = encode(lookup);
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

LookupResult socket_lookup(ENetSocket s, std::uint16_t relay_port, const std::string& code,
                           const std::function<void()>& pump) {
  return socket_lookup(s, relay_port, lookup_message(code), pump);
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
  options.bind = 0x7F000001u;  // (127.0.0.1 only)
  options.session.join_password = "relay-pass";  // every session has one
  EnetServer server(options);
  REQUIRE(server.start());

  RelayServer relay_server(0, 0x7F000001u, RelayConfig{});  // (127.0.0.1 only)
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
  const KeyPair keys = KeyPair::generate();
  hello.key_share = keys.share();
  auto bytes = coop::protocol::encode(coop::protocol::Message{hello});
  REQUIRE(bytes.has_value());
  ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(), ENET_PACKET_FLAG_RELIABLE);
  REQUIRE(enet_peer_send(guest.peer, kChannelSession, p) == 0);
  // The challenge comes back through the relay; the proof goes out, and from
  // then on the relay forwards sealed bytes it cannot read (WELCOME too).
  REQUIRE(pump_until([&] { return guest.find<JoinChallenge>() != nullptr; }));
  const JoinChallenge challenge = *guest.find<JoinChallenge>();
  REQUIRE(challenge.key_share.has_value());
  auto proof = coop::protocol::encode(coop::protocol::Message{
      JoinProof{join_proof("relay-pass", challenge.nonce, keys.share(), *challenge.key_share)}});
  REQUIRE(proof.has_value());
  p = enet_packet_create(proof->data(), proof->size(), ENET_PACKET_FLAG_RELIABLE);
  REQUIRE(enet_peer_send(guest.peer, kChannelSession, p) == 0);
  REQUIRE(pump_until([&] { return server.session().player_count() == 1; }));
  CHECK(guest.find<Welcome>() == nullptr);  // it came sealed: not readable plain
  CHECK(relay_server.relay().stats().to_host > 0);
  CHECK(relay_server.relay().stats().to_guest > 0);

  // The host stops: the relay drops the code at once.
  link.stop();
  REQUIRE(pump_until([&] { return relay_server.relay().sessions() == 0; }));
}

TEST_CASE("relay loopback: a host's link holds a room name and a second host cannot take it (issue 261)") {
  EnetLife enet;
  RelayServer relay_server(0, 0x7F000001u, RelayConfig{});  // (127.0.0.1 only)
  REQUIRE(relay_server.start());
  const std::uint16_t relay_port = relay_server.port();
  REQUIRE(relay_port != 0);

  auto link_config = [&](std::string room) {
    RelayLinkConfig c;
    c.relay_host = "127.0.0.1";
    c.relay_port = relay_port;
    c.server_port = 1;  // no game here: only the registration is tested
    c.room = std::move(room);
    return c;
  };
  std::string announced;
  RelayLinkConfig first_config = link_config(" Forest-Gang ");
  first_config.on_room = [&](const std::string& room) { announced = room; };
  RelayLink first(first_config);
  std::string why;
  REQUIRE(first.start(why));

  auto pump = [&] {
    first.service();
    relay_server.service(1);
    first.service();
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (first.room().empty() && std::chrono::steady_clock::now() < deadline) pump();
  REQUIRE(first.room() == "Forest-Gang");
  CHECK(announced == "Forest-Gang");
  CHECK(!first.code().empty());
  CHECK(relay_server.relay().rooms() == 1);

  // A second host asking for the same room (in another case) is refused and keeps no room.
  RelayLink second(link_config("forest-GANG"));
  REQUIRE(second.start(why));
  for (int i = 0; i < 50 && relay_server.relay().stats().rooms_taken == 0; ++i) {
    second.service();
    pump();
    second.service();
  }
  CHECK(relay_server.relay().stats().rooms_taken >= 1);
  CHECK(second.room().empty());
  CHECK(relay_server.relay().sessions() == 1);

  // A guest finds the room by name.
  ENetSocket lookup_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  REQUIRE(lookup_socket != ENET_SOCKET_NULL);
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  REQUIRE(enet_socket_bind(lookup_socket, &any) == 0);
  enet_socket_set_option(lookup_socket, ENET_SOCKOPT_NONBLOCK, 1);
  CHECK(socket_lookup(lookup_socket, relay_port, lookup_room_message("lake-gang"), pump) == LookupResult::kUnknown);
  CHECK(socket_lookup(lookup_socket, relay_port, lookup_room_message("forest-gang"), pump) == LookupResult::kOk);
  enet_socket_destroy(lookup_socket);

  // A name that is not one stops the link before it starts.
  RelayLink bad(link_config("no spaces"));
  CHECK_FALSE(bad.start(why));
  CHECK(why.find("not a room name") != std::string::npos);

  // lookup_on_relay, as the game's Join page uses it: the room, the code, and a room shaped like a code that
  // falls back from the code. The relay serves on this thread between the tries.
  ENetAddress relay_at{};
  enet_address_set_host_ip(&relay_at, "127.0.0.1");
  relay_at.port = relay_port;
  std::atomic<bool> done{false};
  std::thread serve([&] {
    while (!done) {
      relay_server.service(5);
      first.service();
    }
  });
  auto parsed = parse_relay_join("FOREST-GANG@127.0.0.1:" + std::to_string(relay_port));
  REQUIRE(parsed.has_value());
  CHECK(lookup_on_relay(relay_at, *parsed) == LookupResult::kOk);
  parsed = parse_relay_join(show_code(first.code()) + "@127.0.0.1:" + std::to_string(relay_port));
  REQUIRE(parsed.has_value());
  CHECK(lookup_on_relay(relay_at, *parsed) == LookupResult::kOk);
  parsed = parse_relay_join("lakegang@127.0.0.1:" + std::to_string(relay_port));
  REQUIRE(parsed.has_value());
  CHECK(lookup_on_relay(relay_at, *parsed) == LookupResult::kUnknown);
  done = true;
  serve.join();

  first.stop();
  second.stop();
}

TEST_CASE("relay loopback: a game without the code never reaches the host") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  options.bind = 0x7F000001u;  // (127.0.0.1 only)
  options.session.join_password = "relay-pass";
  EnetServer server(options);
  REQUIRE(server.start());
  RelayServer relay_server(0, 0x7F000001u, RelayConfig{});  // (127.0.0.1 only)
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

// --- The room list (kListRoom, kListRooms, kRoomList) ---
// (in a namespace of their own: coop::protocol's Message and encode are in sight here too)
namespace room_list_tests {

using coop::server::relay::decode;
using coop::server::relay::encode;
using coop::server::relay::Message;

namespace {

Message list_room_message(std::uint64_t token, bool listed, std::string host = "Saria", std::uint8_t players = 1,
                          std::uint8_t max = 4) {
  Message m;
  m.kind = Kind::kListRoom;
  m.token = token;
  m.listed = listed;
  m.entry.host = std::move(host);
  m.entry.players = players;
  m.entry.max_players = max;
  m.entry.version = 15;
  return m;
}

Message list_ask(std::uint8_t start) {
  Message m;
  m.kind = Kind::kListRooms;
  m.start = start;
  return m;
}

// A host on an address of its own (the relay takes 2 hosts an address) holding a room.
Endpoint host_at(int i) { return Endpoint{static_cast<std::uint32_t>(0x0A000000u + static_cast<std::uint32_t>(i)), 40000}; }

// The relay's page of the list from `start`, asked from `from`; nothing when it did not answer.
std::optional<Message> ask_list(Fixture& f, std::uint8_t start, Endpoint from = kGuest, std::uint64_t now = 0) {
  auto out = f.relay.received(from, encode(list_ask(start)), now);
  if (out.empty()) return std::nullopt;
  REQUIRE(out.size() == 1);
  CHECK(out[0].to == from);
  CHECK(out[0].bytes.size() <= kMaxPayloadBytes);
  return decoded(out[0]);
}

}  // namespace

TEST_CASE("relay wire: the room list's messages go through encode and decode, the ask padded") {
  Message listed = list_room_message(77, true, "Saria", 2, 4);
  auto back = decode(encode(listed));
  REQUIRE(back.has_value());
  CHECK(back->kind == Kind::kListRoom);
  CHECK(back->token == 77);
  CHECK(back->listed);
  CHECK(back->entry.host == "Saria");
  CHECK(back->entry.players == 2);
  CHECK(back->entry.max_players == 4);
  CHECK(back->entry.version == 15);
  back = decode(encode(list_room_message(77, false, "")));   // (off the list: no name needed)
  REQUIRE(back.has_value());
  CHECK_FALSE(back->listed);
  // A listed room with no name, or a control character in it, is not a message.
  CHECK_FALSE(decode(encode(list_room_message(77, true, ""))).has_value());
  CHECK_FALSE(decode(encode(list_room_message(77, true, std::string("Sa\nria")))).has_value());

  // The ask is padded with zeros to kListRequestBytes; shorter or with other bytes it is not one.
  std::vector<std::uint8_t> ask = encode(list_ask(22));
  CHECK(ask.size() == kListRequestBytes);
  back = decode(ask);
  REQUIRE(back.has_value());
  CHECK(back->kind == Kind::kListRooms);
  CHECK(back->start == 22);
  std::vector<std::uint8_t> shorter(ask.begin(), ask.end() - 1);
  CHECK_FALSE(decode(shorter).has_value());
  ask[300] = 1;
  CHECK_FALSE(decode(ask).has_value());

  Message page;
  page.kind = Kind::kRoomList;
  page.total = 3;
  page.start = 1;
  page.rooms = {RoomEntry{"Forest-Gang", "Saria", 2, 4, 15, true}, RoomEntry{"lake_gang", "Ruto", 1, 16, 15, true}};
  back = decode(encode(page));
  REQUIRE(back.has_value());
  CHECK(back->kind == Kind::kRoomList);
  CHECK(back->total == 3);
  CHECK(back->start == 1);
  CHECK(back->rooms == page.rooms);
  page.total = 2;   // (start 1 + 2 rooms is past a total of 2)
  CHECK_FALSE(decode(encode(page)).has_value());
  page.total = 3;
  page.rooms[0].room = "no spaces";
  CHECK_FALSE(decode(encode(page)).has_value());
}

TEST_CASE("relay wire: a host name as the list takes it") {
  CHECK(listable_host_name("Saria"));
  CHECK(listable_host_name("S\xc3\xa1ria"));   // (UTF-8 letters)
  CHECK_FALSE(listable_host_name(""));
  CHECK_FALSE(listable_host_name(std::string(kMaxHostNameBytes + 1, 'a')));
  CHECK_FALSE(listable_host_name("a\tb"));
  CHECK(listing_name("Sa\nria") == "Saria");
  CHECK(listing_name("") == "Host");
  CHECK(listing_name("\x01\x02") == "Host");
  // Cut to kMaxHostNameBytes before a UTF-8 letter's start, never in the middle of one.
  const std::string long_name = std::string(31, 'a') + "\xc3\xa1";
  CHECK(listing_name(long_name) == std::string(31, 'a'));
  CHECK(listable_host_name(listing_name(long_name)));
}

TEST_CASE("relay: a listed room is on the list newest first, never with a password, and leaves it with its host") {
  Fixture f;
  // Nothing listed yet: an empty list.
  auto page = ask_list(f, 0);
  REQUIRE(page.has_value());
  CHECK(page->total == 0);
  CHECK(page->rooms.empty());

  REQUIRE(f.register_room(1, "Forest-Gang", host_at(1)).kind == Kind::kRegisteredName);
  REQUIRE(f.register_room(2, "lake_gang", host_at(2)).kind == Kind::kRegisteredName);
  REQUIRE(f.register_room(3, "quiet-room", host_at(3)).kind == Kind::kRegisteredName);
  CHECK_FALSE(f.register_host(4, host_at(4)).empty());
  CHECK(f.relay.received(host_at(1), encode(list_room_message(1, true, "Saria", 2, 4)), 10).empty());
  CHECK(f.relay.received(host_at(2), encode(list_room_message(2, true, "Ruto", 1, 16)), 20).empty());
  // A host with a join code only is not listed: the list is for room names.
  CHECK(f.relay.received(host_at(4), encode(list_room_message(4, true, "Mido")), 30).empty());
  CHECK(f.relay.listed() == 2);

  page = ask_list(f, 0, kGuest, 40);
  REQUIRE(page.has_value());
  CHECK(page->total == 2);
  CHECK(page->start == 0);
  REQUIRE(page->rooms.size() == 2);
  CHECK(page->rooms[0] == RoomEntry{"lake_gang", "Ruto", 1, 16, 15, true});   // (the newest first)
  CHECK(page->rooms[1] == RoomEntry{"Forest-Gang", "Saria", 2, 4, 15, true});

  // A new listing with other numbers keeps the room's place; a host that unlists it takes it off.
  CHECK(f.relay.received(host_at(1), encode(list_room_message(1, true, "Saria", 3, 4)), 50).empty());
  page = ask_list(f, 0, kGuest, 60);
  REQUIRE(page.has_value());
  REQUIRE(page->rooms.size() == 2);
  CHECK(page->rooms[1].players == 3);
  CHECK(f.relay.received(host_at(1), encode(list_room_message(1, false, "")), 70).empty());
  page = ask_list(f, 0, kGuest, 80);
  REQUIRE(page.has_value());
  CHECK(page->total == 1);
  CHECK(page->rooms[0].room == "lake_gang");

  // Its host's BYE takes a room off the list, and so does a host that went quiet.
  Message bye;
  bye.kind = Kind::kBye;
  bye.token = 2;
  f.relay.received(host_at(2), encode(bye), 90);
  CHECK(f.relay.listed() == 0);
  CHECK(f.relay.received(host_at(3), encode(list_room_message(3, true, "Malon")), 100).empty());
  CHECK(f.relay.listed() == 1);
  f.relay.tick(100 + RelayConfig{}.session_timeout_ms + 1);
  CHECK(f.relay.listed() == 0);
  page = ask_list(f, 0, kGuest, 100 + RelayConfig{}.session_timeout_ms + 2);
  REQUIRE(page.has_value());
  CHECK(page->total == 0);

  // A listing for a host the relay does not know (it restarted) is refused, so the link registers again.
  auto out = f.relay.received(host_at(9), encode(list_room_message(99, true)), 200'000);
  REQUIRE(out.size() == 1);
  CHECK(decoded(out[0]).kind == Kind::kRefused);
  CHECK(decoded(out[0]).status == static_cast<std::uint8_t>(Refusal::kUnknownSession));
}

TEST_CASE("relay: the list comes in pages under a datagram, at most 50 rooms, and asks are limited per address") {
  Fixture f;
  for (int i = 0; i < 60; ++i) {
    const std::string room = "room-" + std::to_string(i) + std::string(24 - 5 - std::to_string(i).size(), 'x');
    REQUIRE(room.size() == kMaxRoomLength);
    REQUIRE(f.register_room(static_cast<std::uint64_t>(100 + i), room, host_at(i)).kind == Kind::kRegisteredName);
    const std::string host = std::string(kMaxHostNameBytes - 2, 'h') + (i < 10 ? "0" : "") + std::to_string(i);
    CHECK(f.relay.received(host_at(i), encode(list_room_message(static_cast<std::uint64_t>(100 + i), true, host)),
                           static_cast<std::uint64_t>(i))
              .empty());
  }
  CHECK(f.relay.listed() == 60);
  std::vector<RoomEntry> all;
  int pages = 0;
  while (true) {
    auto page = ask_list(f, static_cast<std::uint8_t>(all.size()), kGuest, 100);
    REQUIRE(page.has_value());
    CHECK(page->total == kMaxListedRooms);
    CHECK(page->start == all.size());
    if (page->rooms.empty()) break;
    all.insert(all.end(), page->rooms.begin(), page->rooms.end());
    ++pages;
    if (all.size() >= page->total) break;
  }
  CHECK(pages >= 3);   // (about 22 long entries a page)
  REQUIRE(all.size() == kMaxListedRooms);
  CHECK(all.front().room.rfind("room-59", 0) == 0);   // (the newest 50: 59 down to 10)
  CHECK(all.back().room.rfind("room-10", 0) == 0);
  // A start past the end: an empty page with the total.
  auto past = ask_list(f, 200, Endpoint{0x0300000Au, 1}, 100);
  REQUIRE(past.has_value());
  CHECK(past->rooms.empty());
  CHECK(past->total == kMaxListedRooms);

  // 30 asks a minute from one address; the next is not answered until the minute is over.
  const Endpoint asker{0x0400000Au, 2};
  for (std::uint32_t i = 0; i < RelayConfig{}.lists_per_window; ++i) CHECK(ask_list(f, 0, asker, 1000).has_value());
  CHECK_FALSE(ask_list(f, 0, asker, 1000).has_value());
  CHECK(f.relay.stats().lists_limited == 1);
  CHECK(ask_list(f, 0, Endpoint{0x0500000Au, 2}, 1000).has_value());   // (another address is not held)
  CHECK(ask_list(f, 0, asker, 1000 + RelayConfig{}.list_window_ms).has_value());

  // An ask without its padding gets nothing: an answer must never be much more than what was sent.
  std::vector<std::uint8_t> bare = encode(list_ask(0));
  bare.resize(kMagic.size() + 2);
  CHECK(f.relay.received(Endpoint{0x0600000Au, 3}, bare, 2000).empty());
}

TEST_CASE("relay: one address sends LIST_ROOM 30 times a minute and puts 6 rooms on the list in ten minutes") {
  Fixture f;
  REQUIRE(f.register_room(1, "Forest-Gang", host_at(1)).kind == Kind::kRegisteredName);
  const std::uint64_t minute = RelayConfig{}.list_window_ms;

  // 30 LIST_ROOM a minute from one address are taken; the next is not, until the minute is over.
  for (std::uint32_t i = 0; i < RelayConfig{}.listings_per_window; ++i)
    CHECK(f.relay.received(host_at(1), encode(list_room_message(1, true, "Saria", 1, 4)), 1000).empty());
  CHECK(f.relay.listed() == 1);
  CHECK(f.relay.received(host_at(1), encode(list_room_message(1, true, "Saria", 2, 4)), 1000).empty());
  CHECK(f.relay.stats().listings_limited == 1);
  auto page = ask_list(f, 0, kGuest, 1000);
  REQUIRE(page.has_value());
  REQUIRE(page->rooms.size() == 1);
  CHECK(page->rooms[0].players == 1);   // (the 31st not taken)
  CHECK(f.relay.received(host_at(1), encode(list_room_message(1, true, "Saria", 2, 4)), 1000 + minute).empty());
  page = ask_list(f, 0, kGuest, 1000 + minute);
  REQUIRE(page.has_value());
  REQUIRE(page->rooms.size() == 1);
  CHECK(page->rooms[0].players == 2);

  // Over the limit, a listing from a token the relay does not know gets no refusal either.
  const Endpoint forged{0x0700000Au, 5};
  for (std::uint32_t i = 0; i < RelayConfig{}.listings_per_window; ++i)
    CHECK(f.relay.received(forged, encode(list_room_message(99, true)), 2000).size() == 1);
  CHECK(f.relay.received(forged, encode(list_room_message(99, true)), 2000).empty());

  // Rooms put on the list: 6 from one address in ten minutes, each one taken off and listed again counted. The 7th
  // stays off the list until the window is over; a host on another address is not held.
  REQUIRE(f.register_room(2, "lake_gang", host_at(2)).kind == Kind::kRegisteredName);
  REQUIRE(f.register_room(3, "quiet-room", host_at(3)).kind == Kind::kRegisteredName);
  const std::uint64_t start = 10 * minute;
  for (std::uint32_t i = 0; i < RelayConfig{}.new_listings_per_window; ++i) {
    CHECK(f.relay.received(host_at(2), encode(list_room_message(2, true, "Ruto")), start).empty());
    CHECK(f.relay.listed() == 2);
    CHECK(f.relay.received(host_at(2), encode(list_room_message(2, false, "")), start).empty());
    CHECK(f.relay.listed() == 1);
  }
  const std::uint64_t limited = f.relay.stats().listings_limited;
  CHECK(f.relay.received(host_at(2), encode(list_room_message(2, true, "Ruto")), start).empty());
  CHECK(f.relay.listed() == 1);
  CHECK(f.relay.stats().listings_limited == limited + 1);
  CHECK(f.relay.received(host_at(3), encode(list_room_message(3, true, "Malon")), start).empty());
  CHECK(f.relay.listed() == 2);
  const std::uint64_t later = start + RelayConfig{}.window_ms;
  CHECK(f.relay.received(host_at(2), encode(list_room_message(2, true, "Ruto")), later).empty());
  CHECK(f.relay.listed() == 3);

  // The relay's tick drops the counts whose window is over: the same address starts again from none.
  f.relay.tick(later + RelayConfig{}.window_ms);
  CHECK(f.relay.received(forged, encode(list_room_message(99, true)), later + RelayConfig{}.window_ms).size() == 1);
}

TEST_CASE("relay loopback: a host's link lists its room, the game's list_rooms_on_relay reads it, a link without listing is not on it") {
  EnetLife enet;
  RelayServer relay_server(0, 0x7F000001u, RelayConfig{});  // (127.0.0.1 only)
  REQUIRE(relay_server.start());
  const std::uint16_t relay_port = relay_server.port();

  auto link_config = [&](std::string room) {
    RelayLinkConfig c;
    c.relay_host = "127.0.0.1";
    c.relay_port = relay_port;
    c.server_port = 1;  // no game here: only the room and its listing are tested
    c.room = std::move(room);
    return c;
  };
  std::atomic<int> players{1};
  RelayLinkConfig listed_config = link_config("Forest-Gang");
  listed_config.listing = [&]() -> std::optional<RoomEntry> {
    RoomEntry e;
    e.host = "Saria\x07";   // (a control character: dropped before it goes out)
    e.players = static_cast<std::uint8_t>(players.load());
    e.max_players = 4;
    e.version = 15;
    return e;
  };
  RelayLink listed(listed_config);
  RelayLink unlisted(link_config("lake_gang"));
  std::string why;
  REQUIRE(listed.start(why));
  REQUIRE(unlisted.start(why));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((listed.room().empty() || unlisted.room().empty() || relay_server.relay().listed() == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    listed.service();
    unlisted.service();
    relay_server.service(1);
  }
  REQUIRE(relay_server.relay().rooms() == 2);
  REQUIRE(relay_server.relay().listed() == 1);

  ENetAddress relay_at{};
  enet_address_set_host_ip(&relay_at, "127.0.0.1");
  relay_at.port = relay_port;
  std::atomic<bool> done{false};
  std::thread serve([&] {
    while (!done) {
      relay_server.service(5);
      listed.service();
      unlisted.service();
    }
  });
  const auto list = list_rooms_on_relay(relay_at);
  done = true;
  serve.join();
  REQUIRE(list.has_value());
  CHECK(list->total == 1);
  REQUIRE(list->rooms.size() == 1);
  CHECK(list->rooms[0] == RoomEntry{"Forest-Gang", "Saria", 1, 4, 15, true});

  // Its host stops: off the list at once.
  listed.stop();
  for (int i = 0; i < 50 && relay_server.relay().listed() != 0; ++i) relay_server.service(2);
  CHECK(relay_server.relay().listed() == 0);
  unlisted.stop();

  // No relay there: nothing.
  ENetAddress nobody{};
  enet_address_set_host_ip(&nobody, "127.0.0.1");
  nobody.port = 1;
  const auto started = std::chrono::steady_clock::now();
  std::atomic<int> asked{0};
  CHECK_FALSE(list_rooms_on_relay(nobody, [&] { return ++asked > 20; }).has_value());   // (stopped early)
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
}

}  // namespace room_list_tests

// Public games (HostServiceConfig::public_game, docs/play-online.md "Public games").
namespace {
bool join_with(Guest& guest, const std::string& password, const std::function<void()>& pump,
               const std::function<bool()>& joined) {
  Hello hello;
  hello.build_id = "relay test";
  hello.game_build_id = "e0f3ccfaf50065ae";
  hello.game_version = "1.1.1";
  hello.nickname = "Saria";
  const KeyPair keys = KeyPair::generate();
  hello.key_share = keys.share();
  auto bytes = coop::protocol::encode(coop::protocol::Message{hello});
  REQUIRE(bytes.has_value());
  ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(), ENET_PACKET_FLAG_RELIABLE);
  REQUIRE(enet_peer_send(guest.peer, kChannelSession, p) == 0);
  const auto until = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      pump();
      if (done()) return true;
    }
    return false;
  };
  REQUIRE(until([&] { return guest.find<JoinChallenge>() != nullptr; }));
  const JoinChallenge challenge = *guest.find<JoinChallenge>();
  REQUIRE(challenge.key_share.has_value());
  auto proof = coop::protocol::encode(coop::protocol::Message{
      JoinProof{join_proof(password, challenge.nonce, keys.share(), *challenge.key_share)}});
  REQUIRE(proof.has_value());
  p = enet_packet_create(proof->data(), proof->size(), ENET_PACKET_FLAG_RELIABLE);
  REQUIRE(enet_peer_send(guest.peer, kChannelSession, p) == 0);
  return until(joined);
}
}  // namespace

TEST_CASE("public game: the password comes from the room, a private game cannot use one of that shape") {
  using coop::protocol::is_public_join_password;
  using coop::protocol::public_join_password;
  CHECK(public_join_password("Forest-4821") == public_join_password("forest-4821"));
  CHECK(public_join_password("forest-4821") != public_join_password("forest-4822"));
  CHECK(is_public_join_password(public_join_password("lake")));
  CHECK(is_public_join_password("Squadala-Public-Game:x"));
  CHECK_FALSE(is_public_join_password("squadala-public-game"));
  CHECK_FALSE(is_public_join_password("my-friends-pass"));
  CHECK(coop::protocol::public_listing_version() == (0x8000 | coop::protocol::kProtocolVersion));

  EnetLife enet;
  {
    // A public game needs the relay and a room name.
    HostServiceConfig c;
    c.port = 0;
    c.public_game = true;
    c.relay_room = "forest-4821";
    HostService s(c);
    std::string error;
    CHECK_FALSE(s.start(error));
    CHECK(s.status().problem == HostProblem::kRelay);
  }
  {
    // A private game with a password of the public shape is refused (a public secret never opens a private room).
    HostServiceConfig c;
    c.port = 0;
    c.session.join_password = public_join_password("forest-4821");
    HostService s(c);
    std::string error;
    CHECK_FALSE(s.start(error));
    CHECK(s.status().problem == HostProblem::kNoPassword);
  }
  {
    // ... nor can a private host change to one while hosting.
    HostServiceConfig c;
    c.port = 0;
    c.bind = 0x7F000001u;
    c.session.join_password = "friends-only";
    HostService s(c);
    std::string error;
    REQUIRE(s.start(error));
    CHECK_FALSE(s.set_join_password(public_join_password("forest-4821"), error));
    CHECK(s.set_join_password("friends-only-2", error));
    s.stop();
  }
}

TEST_CASE("public game: listed with the public bit, relay only, a guest joins with the room's password, never another") {
  EnetLife enet;
  RelayServer relay_server(0, 0x7F000001u, RelayConfig{});  // (127.0.0.1 only)
  REQUIRE(relay_server.start());
  const std::uint16_t relay_port = relay_server.port();

  HostServiceConfig c;
  c.port = 0;
  c.public_game = true;
  c.relay_via = "127.0.0.1:" + std::to_string(relay_port);
  c.relay_room = "Forest-4821";
  c.session.join_password = "ignored";  // a public game's password is the room's
  c.upnp = true;                        // all forced off: relay only
  c.invite = true;
  c.session.same_network_open = true;
  HostService host(c);
  std::string error;
  REQUIRE(host.start(error));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((host.status().relay_room.empty() || relay_server.relay().listed() == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    host.service(0);
    relay_server.service(1);
  }
  REQUIRE(relay_server.relay().listed() == 1);
  CHECK_FALSE(host.status().upnp_open);
  CHECK(host.status().invite.empty());

  ENetAddress relay_at{};
  enet_address_set_host_ip(&relay_at, "127.0.0.1");
  relay_at.port = relay_port;
  std::atomic<bool> done{false};
  std::thread serve([&] {
    while (!done) {
      relay_server.service(5);
      host.service(0);
    }
  });
  const auto list = list_rooms_on_relay(relay_at);
  done = true;
  serve.join();
  REQUIRE(list.has_value());
  REQUIRE(list->rooms.size() == 1);
  CHECK(list->rooms[0].room == "Forest-4821");
  CHECK(list->rooms[0].host == "Forest-4821");  // the room's name: nothing of the host's PC
  CHECK(list->rooms[0].version == coop::protocol::public_listing_version());

  // The game port is on 127.0.0.1 only (the relay's link reaches it there); a guest on this PC stands in for the
  // relay's flow. The wrong password (a private one) is refused, the room's opens it.
  auto pump = [&] {
    host.service(0);
    relay_server.service(0);
  };
  Guest wrong;
  wrong.connect(host.port());
  for (int i = 0; i < 200 && !wrong.connected; ++i) {
    pump();
    wrong.pump();
  }
  REQUIRE(wrong.connected);
  CHECK_FALSE(join_with(wrong, "ignored", [&] { pump(); wrong.pump(); },
                        [&] { return host.session()->player_count() == 1; }));
  Guest right;
  right.connect(host.port());
  for (int i = 0; i < 200 && !right.connected; ++i) {
    pump();
    right.pump();
  }
  REQUIRE(right.connected);
  CHECK(join_with(right, coop::protocol::public_join_password("forest-4821"), [&] { pump(); right.pump(); },
                  [&] { return host.session()->player_count() == 1; }));
  host.stop();
}
