// The real server on 127.0.0.1 with clients written in raw ENet.
// Single-threaded: the test pumps the server and every client in turn.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/codec.hpp"
#include "enet_server.hpp"

using namespace coop::protocol;
using namespace coop::server;

namespace {

struct Received {
  std::uint8_t channel;
  Message message;
};

struct Client {
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  bool connected = false;
  bool disconnected = false;
  std::vector<Received> inbox;

  Client() { host = enet_host_create(nullptr, 1, kChannelCount, 0, 0); }
  ~Client() {
    if (host != nullptr) enet_host_destroy(host);
  }

  void connect(std::uint16_t port) {
    ENetAddress address{};
    enet_address_set_host_ip(&address, "127.0.0.1");
    address.port = port;
    peer = enet_host_connect(host, &address, kChannelCount, 0);
  }

  void send(const Message& m, std::uint8_t channel) {
    auto bytes = encode(m);
    REQUIRE(bytes.has_value());
    enet_uint32 flags = channel <= kChannelOwnership ? static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE) : 0u;
    ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(), flags);
    REQUIRE(enet_peer_send(peer, channel, p) == 0);
  }

  void pump() {
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0) {
      if (ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
      if (ev.type == ENET_EVENT_TYPE_DISCONNECT) disconnected = true;
      if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        Message m;
        std::span<const std::uint8_t> b(ev.packet->data, ev.packet->dataLength);
        if (decode(b, m) == Error::none &&
            check_received(m, Sender::server, ev.channelID) == Error::none) {
          inbox.push_back({ev.channelID, m});
        }
        enet_packet_destroy(ev.packet);
      }
    }
  }

  template <typename T>
  const T* find() const {
    for (const Received& r : inbox) {
      if (const auto* m = std::get_if<T>(&r.message)) return m;
    }
    return nullptr;
  }
};

Hello hello_from(const std::string& nickname) {
  Hello h;
  h.build_id = "loopback test";
  h.game_build_id = "e0f3ccfaf50065ae";
  h.game_version = "1.1.1";
  h.nickname = nickname;
  return h;
}

// Pumps everything until `done` or two seconds pass.
bool pump_until(EnetServer& server, std::vector<Client*> clients,
                const std::function<bool()>& done) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    server.service(1);
    for (Client* c : clients) c->pump();
    if (done()) return true;
  }
  return false;
}

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

}  // namespace

TEST_CASE("loopback: two clients join, relay, ping and leave over real ENet") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;  // any free port
  EnetServer server(options);
  REQUIRE(server.start());
  std::uint16_t port = server.port();
  REQUIRE(port != 0);

  Client a;
  Client b;
  REQUIRE(a.host != nullptr);
  REQUIRE(b.host != nullptr);
  a.connect(port);
  REQUIRE(pump_until(server, {&a}, [&] { return a.connected; }));
  a.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, {&a}, [&] { return a.find<Welcome>() != nullptr; }));
  CHECK(a.find<Welcome>()->player_number == 1);

  b.connect(port);
  REQUIRE(pump_until(server, {&a, &b}, [&] { return b.connected; }));
  b.send(hello_from("Zelda"), kChannelSession);
  REQUIRE(pump_until(server, {&a, &b}, [&] {
    return b.find<Welcome>() != nullptr && a.find<PlayerJoined>() != nullptr;
  }));
  const Welcome* wb = b.find<Welcome>();
  CHECK(wb->player_number == 2);
  REQUIRE(wb->players.size() == 1);
  CHECK(wb->players[0] == PlayerInfo{1, "Link"});
  CHECK(*a.find<PlayerJoined>() == PlayerJoined{2, "Zelda"});

  // A sample on channel 3 and a chat line on channel 0 reach the other
  // player with the sender's number written in.
  PlayerSample sample;
  sample.time = {1000, 1};
  sample.position = Vec3{4.0f, 5.0f, 6.0f};
  a.send(sample, kChannelPlayerSamples);
  a.send(Chat{0, "hey"}, kChannelSession);
  a.send(Ping{31337}, kChannelSession);
  REQUIRE(pump_until(server, {&a, &b}, [&] {
    return b.find<PlayerSample>() != nullptr && b.find<Chat>() != nullptr;
  }));
  for (const Received& r : b.inbox) {
    if (const auto* s = std::get_if<PlayerSample>(&r.message)) {
      CHECK(r.channel == kChannelPlayerSamples);
      CHECK(s->player_number == 1);
      CHECK(s->position == sample.position);
    }
    if (const auto* c = std::get_if<Chat>(&r.message)) {
      CHECK(r.channel == kChannelSession);
      CHECK(*c == Chat{1, "hey"});
    }
  }
  CHECK(a.find<Chat>() == nullptr);  // not echoed back

  // Our PING is answered; the server's own PINGs arrive too.
  bool answered = pump_until(server, {&a, &b}, [&] {
    for (const Received& r : a.inbox) {
      if (const auto* p = std::get_if<Pong>(&r.message);
          p != nullptr && p->ping_sender_time_us == 31337) {
        return a.find<Ping>() != nullptr;
      }
    }
    return false;
  });
  CHECK(answered);

  // A spoofed chat line is dropped, never relayed.
  std::size_t b_before = b.inbox.size();
  a.send(Chat{2, "spoof"}, kChannelSession);
  REQUIRE(pump_until(server, {&a, &b}, [&] {
    return server.session().stats().errors(Error::wrong_sender) == 1;
  }));
  for (std::size_t i = b_before; i < b.inbox.size(); ++i) {
    CHECK(!std::holds_alternative<Chat>(b.inbox[i].message));
  }

  // Player 1 leaves cleanly; player 2 is told.
  enet_peer_disconnect(a.peer, 0);
  REQUIRE(pump_until(server, {&a, &b}, [&] { return b.find<PlayerLeft>() != nullptr; }));
  CHECK(*b.find<PlayerLeft>() == PlayerLeft{1, LeaveReason::left});
  CHECK(server.session().player_count() == 1);
}

TEST_CASE("loopback: a wrong protocol version gets REFUSED and is disconnected") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  EnetServer server(options);
  REQUIRE(server.start());

  Client c;
  c.connect(server.port());
  REQUIRE(pump_until(server, {&c}, [&] { return c.connected; }));
  const std::uint8_t raw[] = {1, 0, 2, 0, 9, 0};  // HELLO, version 9
  ENetPacket* p = enet_packet_create(raw, sizeof raw, ENET_PACKET_FLAG_RELIABLE);
  REQUIRE(enet_peer_send(c.peer, kChannelSession, p) == 0);
  REQUIRE(pump_until(server, {&c}, [&] { return c.disconnected; }));
  const Refused* refused = c.find<Refused>();
  REQUIRE(refused != nullptr);
  CHECK(refused->reason == RefuseReason::wrong_protocol_version);
  CHECK(server.session().player_count() == 0);
}

TEST_CASE("loopback: a packet longer than any message is thrown away by ENet unread") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  EnetServer server(options);
  REQUIRE(server.start());

  Client c;
  c.connect(server.port());
  REQUIRE(pump_until(server, {&c}, [&] { return c.connected; }));
  c.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, {&c}, [&] { return c.find<Welcome>() != nullptr; }));

  // 100 kB in fragments, more than a u16 body length can say. Unreliable
  // fragments, so ENet does not send them again and again, and their loss
  // does not hold up the PING behind them.
  std::vector<std::uint8_t> big(100'000, 0);
  big[0] = static_cast<std::uint8_t>(MessageType::test_payload);
  ENetPacket* p =
      enet_packet_create(big.data(), big.size(), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
  REQUIRE(enet_peer_send(c.peer, kChannelPlayerSamples, p) == 0);
  // Out on its own: ENet throws away the rest of a datagram after a fragment
  // it refuses, and the PING must not share one.
  enet_host_flush(c.host);
  c.send(Ping{5}, kChannelSession);
  REQUIRE(pump_until(server, {&c}, [&] { return c.find<Pong>() != nullptr; }));
  auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (std::chrono::steady_clock::now() < settle) {
    server.service(1);
    c.pump();
  }
  // Had ENet put it together, the session would have counted it refused.
  CHECK(server.session().stats().dropped() == 0);
  CHECK(server.session().player_count() == 1);
  CHECK_FALSE(c.disconnected);
}

TEST_CASE("loopback: a fifth half-open connection from one address is cut at once") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  EnetServer server(options);
  REQUIRE(server.start());

  // All at the same moment, as the one-PC 4-client test does.
  Client clients[5];
  for (Client& c : clients) c.connect(server.port());
  std::vector<Client*> all;
  for (Client& c : clients) all.push_back(&c);
  auto cut = [&] {
    int n = 0;
    for (Client& c : clients) n += c.disconnected ? 1 : 0;
    return n;
  };
  REQUIRE(pump_until(server, all, [&] {
    int connected = 0;
    for (Client& c : clients) connected += c.connected ? 1 : 0;
    return connected == 5 && cut() == 1;
  }));
  CHECK(server.session().stats().half_open_limited == 1);

  // The 4 left send HELLO and are all let in.
  const char* names[] = {"Link", "Zelda", "Saria", "Darunia"};
  std::vector<Client*> kept;
  for (Client& c : clients) {
    if (!c.disconnected) kept.push_back(&c);
  }
  REQUIRE(kept.size() == 4);
  for (std::size_t i = 0; i < kept.size(); ++i) {
    kept[i]->send(hello_from(names[i]), kChannelSession);
  }
  REQUIRE(pump_until(server, kept, [&] { return server.session().player_count() == 4; }));
  for (Client* c : kept) CHECK(c->find<Welcome>() != nullptr);
  CHECK(cut() == 1);
  CHECK(server.session().stats().half_open_limited == 1);
}

TEST_CASE("loopback: three clients share a flag through the host and join a partner") {
  EnetLife enet;
  ServerOptions options;
  options.port = 0;
  EnetServer server(options);
  REQUIRE(server.start());

  Client a;
  Client b;
  Client c;
  std::vector<Client*> all{&a, &b, &c};
  const char* names[] = {"Link", "Zelda", "Saria"};
  // One at a time, so the numbers are 1, 2 and 3 in order.
  for (std::size_t i = 0; i < all.size(); ++i) {
    Client* cl = all[i];
    cl->connect(server.port());
    REQUIRE(pump_until(server, all, [&] { return cl->connected; }));
    cl->send(hello_from(names[i]), kChannelSession);
    REQUIRE(pump_until(server, all, [&] {
      return cl->find<Welcome>() != nullptr && cl->find<FlagSnapshot>() != nullptr;
    }));
    CHECK(cl->find<Welcome>()->player_number == i + 1);
    REQUIRE(cl->find<Host>() != nullptr);
    CHECK(*cl->find<Host>() == Host{1});
    CHECK(*cl->find<FlagSnapshot>() == FlagSnapshot{0, 1, {}});
  }

  // Player 2 sets a flag: it goes to the host (player 1) only.
  const FlagEntry chest{flag_hash("chest"), 3, FlagKind::boolean, 1};
  b.send(FlagSet{0, {chest}}, kChannelProgress);
  REQUIRE(pump_until(server, all, [&] { return a.find<FlagSet>() != nullptr; }));
  CHECK(*a.find<FlagSet>() == FlagSet{2, {chest}});
  // The host answers with the state, which reaches everyone else.
  a.send(FlagState{0, {chest}}, kChannelProgress);
  REQUIRE(pump_until(server, all, [&] {
    return b.find<FlagState>() != nullptr && c.find<FlagState>() != nullptr;
  }));
  CHECK(*b.find<FlagState>() == FlagState{1, {chest}});
  CHECK(*c.find<FlagState>() == FlagState{1, {chest}});
  CHECK(b.find<FlagSet>() == nullptr);
  CHECK(c.find<FlagSet>() == nullptr);
  CHECK(a.find<FlagState>() == nullptr);

  // Player 3 asks to join player 2, who accepts.
  c.send(JoinPartner{0, 2, 41, JoinKind::request, std::nullopt}, kChannelSession);
  REQUIRE(pump_until(server, all, [&] { return b.find<JoinPartner>() != nullptr; }));
  const JoinPartner ask = *b.find<JoinPartner>();
  CHECK(ask.player_number == 3);
  CHECK(ask.request_id == 41);
  CHECK(ask.kind == JoinKind::request);
  JoinTarget where{"spot04", {10.0f, 0.0f, -2.5f}, {0.0f, 0.0f, 0.0f, 1.0f}};
  b.send(JoinPartner{0, 3, 41, JoinKind::accept, where}, kChannelSession);
  REQUIRE(pump_until(server, all, [&] { return c.find<JoinPartner>() != nullptr; }));
  CHECK(*c.find<JoinPartner>() == JoinPartner{2, 3, 41, JoinKind::accept, where});
  CHECK(a.find<JoinPartner>() == nullptr);

  // Asking for a player who is not there is refused by the server.
  c.inbox.clear();
  c.send(JoinPartner{0, 4, 42, JoinKind::request, std::nullopt}, kChannelSession);
  REQUIRE(pump_until(server, all, [&] { return c.find<JoinPartner>() != nullptr; }));
  const JoinPartner no = *c.find<JoinPartner>();
  CHECK(no.player_number == 4);
  CHECK(no.request_id == 42);
  CHECK(no.kind == JoinKind::refuse);
  CHECK(no.reason == JoinRefuseReason::not_there);

  // The host leaves: player 2 hosts, and the flag stays on the server.
  b.inbox.clear();
  c.inbox.clear();
  enet_peer_disconnect(a.peer, 0);
  REQUIRE(pump_until(server, all, [&] {
    return b.find<Host>() != nullptr && c.find<Host>() != nullptr;
  }));
  CHECK(*b.find<Host>() == Host{2});
  CHECK(*c.find<Host>() == Host{2});
  CHECK(server.session().flag_count() == 1);

  Client d;
  all.push_back(&d);
  d.connect(server.port());
  REQUIRE(pump_until(server, all, [&] { return d.connected; }));
  d.send(hello_from("Darunia"), kChannelSession);
  REQUIRE(pump_until(server, all, [&] { return d.find<FlagSnapshot>() != nullptr; }));
  CHECK(*d.find<Host>() == Host{2});
  CHECK(*d.find<FlagSnapshot>() == FlagSnapshot{0, 1, {chest}});
  CHECK(server.session().stats().flag_relayed == 2);
  CHECK(server.session().stats().join_relayed == 2);
  CHECK(server.session().stats().join_not_there == 1);
  CHECK(server.session().stats().dropped() == 0);
}
