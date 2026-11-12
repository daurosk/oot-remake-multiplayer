// The real server on 127.0.0.1 with clients written in raw ENet.
// Single-threaded: the test pumps the server and every client in turn.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <ostream>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/join_password.hpp"
#include "coop/protocol/traffic_crypto.hpp"
#include "enet_server.hpp"
#include "host_service.hpp"

using namespace coop::protocol;
using namespace coop::server;

namespace {

struct Received {
  std::uint8_t channel;
  Message message;
};

// Every session has a join password and encrypted traffic (docs/protocol.md
// "Encrypted traffic"): each test server gets this one.
constexpr const char* kTestPassword = "test-pass";

ServerOptions server_options() {
  ServerOptions options;
  options.port = 0;  // any free port
  options.session.join_password = kTestPassword;
  return options;
}

// A game in raw ENet that does what the real one does: offers a key share in
// HELLO, answers the JOIN_CHALLENGE with the proof of `password`, and seals
// and opens everything after it.
struct Client {
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  bool connected = false;
  bool disconnected = false;
  std::vector<Received> inbox;
  std::string password = kTestPassword;
  std::optional<KeyPair> keys;
  std::vector<std::uint8_t> hello_bytes;
  std::unique_ptr<TrafficCipher> cipher;

  // The bytes for the wire: sealed once keyed.
  std::vector<std::uint8_t> wire(std::uint8_t channel, std::vector<std::uint8_t> plain) {
    if (cipher == nullptr) return plain;
    auto sealed = cipher->seal(channel, plain);
    REQUIRE(sealed.has_value());
    return *sealed;
  }

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
    std::optional<std::vector<std::uint8_t>> bytes;
    if (const auto* hello = std::get_if<Hello>(&m)) {
      Hello offered = *hello;
      keys.emplace(KeyPair::generate());
      offered.key_share = keys->share();
      bytes = encode(Message{offered});
      REQUIRE(bytes.has_value());
      hello_bytes = *bytes;
    } else {
      bytes = encode(m);
    }
    REQUIRE(bytes.has_value());
    bytes = wire(channel, std::move(*bytes));
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
        std::vector<std::uint8_t> plain;
        if (cipher != nullptr) {
          if (cipher->open(ev.channelID, b, plain) == OpenError::none) {
            b = plain;
          } else if (decode(b, m) != Error::none || !std::holds_alternative<Refused>(m)) {
            enet_packet_destroy(ev.packet);  // only a wrong password's REFUSED comes plain
            continue;
          }
        }
        if (decode(b, m) == Error::none && cipher == nullptr && keys) {
          if (const auto* challenge = std::get_if<JoinChallenge>(&m)) {
            REQUIRE(challenge->key_share.has_value());
            auto derived = derive_traffic_keys(*keys, *challenge->key_share, password, hello_bytes,
                                               b, /*is_server=*/false);
            REQUIRE(derived.has_value());
            const auto proof = encode(Message{JoinProof{
                join_proof(password, challenge->nonce, keys->share(), *challenge->key_share)}});
            REQUIRE(proof.has_value());
            ENetPacket* p = enet_packet_create(proof->data(), proof->size(), ENET_PACKET_FLAG_RELIABLE);
            REQUIRE(enet_peer_send(peer, kChannelSession, p) == 0);
            cipher = std::make_unique<TrafficCipher>(*derived);
            keys.reset();
            enet_packet_destroy(ev.packet);
            continue;
          }
        }
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
  ServerOptions options = server_options();
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
  ServerOptions options = server_options();
  EnetServer server(options);
  REQUIRE(server.start());

  Client c;
  c.connect(server.port());
  REQUIRE(pump_until(server, {&c}, [&] { return c.connected; }));
  const std::uint8_t raw[] = {1, 0, 2, 0, 99, 0};  // HELLO, version 99: never this build's
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
  ServerOptions options = server_options();
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
  ServerOptions options = server_options();
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
  ServerOptions options = server_options();
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
  const FlagEntry chest64{FlagKey::key64_flag(0x2fa4c1d09b7e6a35ull), FlagKind::boolean, 1};
  b.send(FlagSet{0, {chest, chest64}}, kChannelProgress);
  REQUIRE(pump_until(server, all, [&] { return a.find<FlagSet>() != nullptr; }));
  CHECK(*a.find<FlagSet>() == FlagSet{2, {chest, chest64}});
  // The host answers with the state, which reaches everyone else.
  a.send(FlagState{0, {chest, chest64}}, kChannelProgress);
  REQUIRE(pump_until(server, all, [&] {
    return b.find<FlagState>() != nullptr && c.find<FlagState>() != nullptr;
  }));
  CHECK(*b.find<FlagState>() == FlagState{1, {chest, chest64}});
  CHECK(*c.find<FlagState>() == FlagState{1, {chest, chest64}});
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
  CHECK(server.session().flag_count() == 2);

  Client d;
  all.push_back(&d);
  d.connect(server.port());
  REQUIRE(pump_until(server, all, [&] { return d.connected; }));
  d.send(hello_from("Darunia"), kChannelSession);
  REQUIRE(pump_until(server, all, [&] { return d.find<FlagSnapshot>() != nullptr; }));
  CHECK(*d.find<Host>() == Host{2});
  CHECK(*d.find<FlagSnapshot>() == FlagSnapshot{0, 1, {chest, chest64}});
  CHECK(server.session().stats().flag_relayed == 2);
  CHECK(server.session().stats().join_relayed == 2);
  CHECK(server.session().stats().join_not_there == 1);
  CHECK(server.session().stats().dropped() == 0);
}

// Full-size messages of 64-bit keys (3600 entries, 65 KB, about 48 ENet
// fragments) both ways. ENet 1.3
// shifts `1 << 31` on an int for the 32nd fragment; the sanitizer build
// passes this only because cmake/enet.cmake turns off that one check.
TEST_CASE("loopback: a host's full-size snapshot reaches a joiner in full-size parts") {
  EnetLife enet;
  ServerOptions options = server_options();
  EnetServer server(options);
  REQUIRE(server.start());

  Client host;
  std::vector<Client*> all{&host};
  host.connect(server.port());
  REQUIRE(pump_until(server, all, [&] { return host.connected; }));
  host.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, all, [&] { return host.find<FlagSnapshot>() != nullptr; }));
  REQUIRE(host.find<Host>() != nullptr);
  CHECK(*host.find<Host>() == Host{1});

  // Two full parts from the host, then one full FLAG_STATE.
  std::vector<FlagEntry> table;
  for (std::uint32_t i = 0; i < 2 * kMaxFlagEntries; ++i) {
    table.push_back(FlagEntry{FlagKey::key64_flag(0x8000'0000'0000'0000ull + i),
                              FlagKind::boolean, i & 1u});
  }
  const auto parts = split_snapshot(table);
  REQUIRE(parts.has_value());
  REQUIRE(parts->size() == 2);
  for (const FlagSnapshot& part : *parts) {
    CHECK(part.flags.size() == kMaxFlagEntries);
    host.send(part, kChannelProgress);
  }
  REQUIRE(pump_until(server, all, [&] {
    return server.session().flag_count() == table.size();
  }));

  Client joiner;
  all.push_back(&joiner);
  joiner.connect(server.port());
  REQUIRE(pump_until(server, all, [&] { return joiner.connected; }));
  joiner.send(hello_from("Zelda"), kChannelSession);
  std::vector<FlagEntry> got;
  REQUIRE(pump_until(server, all, [&] {
    got.clear();
    bool last = false;
    for (const Received& r : joiner.inbox) {
      if (const auto* p = std::get_if<FlagSnapshot>(&r.message)) {
        got.insert(got.end(), p->flags.begin(), p->flags.end());
        last = last || p->part + 1u == p->part_count;
      }
    }
    return last;
  }));
  CHECK(got == table);

  std::vector<FlagEntry> changed(table.begin(), table.begin() + kMaxFlagEntries);
  for (FlagEntry& e : changed) e.value ^= 1u;
  host.send(FlagState{0, changed}, kChannelProgress);
  REQUIRE(pump_until(server, all, [&] { return joiner.find<FlagState>() != nullptr; }));
  CHECK(*joiner.find<FlagState>() == FlagState{1, changed});
  CHECK(server.session().stats().dropped() == 0);
}

TEST_CASE("loopback: a dropped client showing its rejoin token is back in within 2 s") {
  EnetLife enet;
  ServerOptions options = server_options();
  EnetServer server(options);
  REQUIRE(server.start());

  auto killed = std::make_unique<Client>();
  Client zelda;
  killed->connect(server.port());
  zelda.connect(server.port());
  REQUIRE(pump_until(server, {killed.get(), &zelda},
                     [&] { return killed->connected && zelda.connected; }));
  killed->send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, {killed.get()}, [&] { return killed->find<RejoinToken>() != nullptr; }));
  const RejoinTokenBytes token = killed->find<RejoinToken>()->token;
  zelda.send(hello_from("Zelda"), kChannelSession);
  REQUIRE(pump_until(server, {&zelda}, [&] { return zelda.find<Welcome>() != nullptr; }));

  // Killed: the ENet host goes away without a word to the server.
  killed.reset();
  const auto kill_time = std::chrono::steady_clock::now();

  // Straight away the name still belongs to a player heard just now.
  Client early;
  early.connect(server.port());
  REQUIRE(pump_until(server, {&early, &zelda}, [&] { return early.connected; }));
  early.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, {&early, &zelda}, [&] { return early.disconnected; }));
  const Refused* refused = early.find<Refused>();
  REQUIRE(refused != nullptr);
  CHECK(refused->reason == RefuseReason::name_taken);

  while (std::chrono::steady_clock::now() - kill_time < std::chrono::milliseconds(1050)) {
    server.service(1);
    zelda.pump();
  }
  // Issue 163: a second later, a connection without the token (a restarted
  // build, or another player behind the same address) is still refused.
  Client no_token;
  no_token.connect(server.port());
  REQUIRE(pump_until(server, {&no_token, &zelda}, [&] { return no_token.connected; }));
  no_token.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, {&no_token, &zelda}, [&] { return no_token.disconnected; }));
  REQUIRE(no_token.find<Refused>() != nullptr);
  CHECK(no_token.find<Refused>()->reason == RefuseReason::name_taken);
  CHECK(server.session().stats().rejoin_no_token == 1);

  // The same game, reconnecting with its token, takes the old place.
  Client restarted;
  restarted.connect(server.port());
  REQUIRE(pump_until(server, {&restarted, &zelda}, [&] { return restarted.connected; }));
  restarted.send(RejoinClaim{token}, kChannelSession);
  restarted.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_until(server, {&restarted, &zelda}, [&] {
    return restarted.find<Welcome>() != nullptr && zelda.find<PlayerJoined>() != nullptr;
  }));
  const auto back_after = std::chrono::steady_clock::now() - kill_time;
  CHECK(back_after < std::chrono::seconds(2));
  CHECK(restarted.find<Welcome>()->player_number == 1);
  REQUIRE(zelda.find<PlayerLeft>() != nullptr);
  CHECK(*zelda.find<PlayerLeft>() == PlayerLeft{1, LeaveReason::timed_out});
  CHECK(*zelda.find<PlayerJoined>() == PlayerJoined{1, "Link"});
  CHECK(server.session().player_count() == 2);
  CHECK(server.session().stats().rejoined == 1);
}

TEST_CASE("loopback: an older game with ENet's default datagrams gets VPN-safe ones from the server") {
  // Issue 143: the server alone sets the datagram size, so a game built before
  // the fix still gets packets that fit a VPN (Hamachi about 1404 bytes).
  EnetLife enet;
  ServerOptions options = server_options();
  EnetServer server(options);
  REQUIRE(server.start());

  Client c;  // ENet's default 1392
  c.connect(server.port());
  REQUIRE(pump_until(server, {&c}, [&] { return c.connected; }));
  CHECK(c.peer->mtu == kDatagramBytes);
}

TEST_CASE("loopback: the server sends long samples as unreliable fragments") {
  // Issue 143 review: without ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, ENet sends
  // a packet on channel 3 or 4 longer than one datagram reliably.
  CHECK(packet_flags(kChannelSession) == ENET_PACKET_FLAG_RELIABLE);
  CHECK(packet_flags(kChannelProgress) == ENET_PACKET_FLAG_RELIABLE);
  CHECK(packet_flags(kChannelOwnership) == ENET_PACKET_FLAG_RELIABLE);
  CHECK(packet_flags(kChannelPlayerSamples) == ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
  CHECK(packet_flags(kChannelObjectSamples) == ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

namespace {

// UDP between a client and the server that loses some of the client's
// datagrams on the way, as a line over the internet does (issue 175).
class LossyPath {
 public:
  explicit LossyPath(std::uint16_t server_port) {
    outer_ = open();
    inner_ = open();
    ENetAddress got{};
    REQUIRE(enet_socket_get_address(outer_, &got) == 0);
    port_ = got.port;
    REQUIRE(enet_address_set_host_ip(&server_, "127.0.0.1") == 0);
    server_.port = server_port;
  }
  ~LossyPath() {
    enet_socket_destroy(outer_);
    enet_socket_destroy(inner_);
  }
  LossyPath(const LossyPath&) = delete;
  LossyPath& operator=(const LossyPath&) = delete;

  std::uint16_t port() const { return port_; }
  void service() {
    std::uint8_t data[65536];
    ENetBuffer buffer{};
    for (;;) {  // client to server, some lost
      ENetAddress from{};
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int n = enet_socket_receive(outer_, &from, &buffer, 1);
      if (n <= 0) break;
      client_ = from;
      have_client_ = true;
      if (lose_to_server && lose_to_server(static_cast<std::size_t>(n))) {
        ++lost;
        continue;
      }
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

  std::function<bool(std::size_t)> lose_to_server;
  int lost = 0;

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
  static void pass(ENetSocket out, const ENetAddress& to, std::uint8_t* data, std::size_t n) {
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
};

// Link's pose: about 3.4 KB, three full datagrams and a short one.
PlayerPose link_pose(std::uint32_t sequence) {
  PlayerPose pose;
  pose.skeleton = 7;
  pose.time.sequence = sequence;
  for (std::uint16_t b = 0; b < 200; ++b) {
    PoseBone bone;
    bone.bone = b;
    bone.rotation = std::array<std::int16_t, 4>{1, 2, 3, static_cast<std::int16_t>(b)};
    bone.translation = std::array<std::int16_t, 3>{4, 5, 6};
    pose.bones.push_back(bone);
  }
  return pose;
}

void send_fast(Client& c, const Message& m) {
  auto bytes = encode(m);
  REQUIRE(bytes.has_value());
  bytes = c.wire(channel_of(m), std::move(*bytes));
  ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(),
                                     static_cast<enet_uint32>(ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
  REQUIRE(enet_peer_send(c.peer, channel_of(m), p) == 0);
}

}  // namespace

TEST_CASE("loopback: a guest's lost pose fragments do not fill the server's 1 MB waiting count (issue 175)") {
  EnetLife enet;
  std::vector<std::string> lines;
  ServerOptions options = server_options();
  options.session.log = [&](const std::string& line) { lines.push_back(line); };
  EnetServer server(options);
  REQUIRE(server.start());
  LossyPath path(server.port());

  Client c;
  c.connect(path.port());
  auto step = [&] {
    path.service();
    server.service(0);
    path.service();
    c.pump();
  };
  auto run_until = [&](const std::function<bool()>& done, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      step();
      if (done()) return true;
    }
    return false;
  };
  REQUIRE(run_until([&] { return c.connected; }, 2000));
  c.send(hello_from("Link"), kChannelSession);
  REQUIRE(run_until([&] { return c.find<Welcome>() != nullptr; }, 2000));

  // Every ninth full datagram lost: every third pose misses a fragment and is
  // thrown away by ENet. 3000 poses: about 3 MB of them, far past the 1 MB cap
  // (some fewer if ENet holds back fast packets on a busy line).
  int big = 0;
  path.lose_to_server = [&big](std::size_t n) { return n > 1000 && ++big % 9 == 0; };
  for (std::uint32_t i = 0; i < 3000; ++i) {
    send_fast(c, link_pose(i));
    step();
  }
  run_until([] { return false; }, 100);
  CHECK(path.lost > 0);
  CHECK(server.mended_bytes() > (std::uint64_t{1} << 20));

  // A reliable PING after it is still taken and answered.
  path.lose_to_server = nullptr;
  c.send(Ping{4242}, kChannelSession);
  CHECK(run_until([&] {
    for (const Received& r : c.inbox) {
      const auto* pong = std::get_if<Pong>(&r.message);
      if (pong != nullptr && pong->ping_sender_time_us == 4242) return true;
    }
    return false;
  }, 2000));

  // One more pose that loses a fragment and stays waiting; then the guest
  // leaves. The drop line has the link from before ENet reset the peer.
  path.lose_to_server = [](std::size_t n) { return n > 1000; };
  send_fast(c, link_pose(5000));
  run_until([] { return false; }, 100);
  path.lose_to_server = nullptr;
  enet_peer_disconnect(c.peer, 0);
  REQUIRE(run_until([&] { return c.disconnected && server.session().player_count() == 0; }, 2000));
  std::string left;
  for (const std::string& line : lines) {
    if (line.find("left: Link") != std::string::npos) left = line;
  }
  CAPTURE(left);
  REQUIRE_FALSE(left.empty());
  CHECK(left.find("round trip 500 ms") == std::string::npos);  // ENet's reset value
  CHECK(left.find("round trip ") != std::string::npos);
  CHECK(left.find("waiting 0 bytes") == std::string::npos);    // the stuck pose counts
}

// HostService (host_service.hpp): coop_server's hosting loop as an object,
// the one the Hookshot module runs on its own thread.
namespace {

HostServiceConfig service_config(std::vector<std::string>* log) {
  HostServiceConfig config;
  config.port = 0;  // any free port
  config.session.join_password = kTestPassword;
  if (log != nullptr) config.log = [log](const std::string& line) { log->push_back(line); };
  return config;
}

bool pump_service(HostService& service, std::vector<Client*> clients, const std::function<bool()>& done) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    service.service(1);
    for (Client* c : clients) c->pump();
    if (done()) return true;
  }
  return false;
}

bool said(const std::vector<std::string>& log, const std::string& part) {
  for (const auto& line : log) {
    if (line.find(part) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("host service: a player joins with the password, the status counts them, stop says the totals") {
  EnetLife enet;
  std::vector<std::string> log;
  HostService service(service_config(&log));
  CHECK(service.status().state == HostState::kOff);
  std::string why;
  REQUIRE(service.start(why));
  CHECK(why.empty());
  const std::uint16_t port = service.port();
  REQUIRE(port != 0);
  CHECK(service.status().state == HostState::kHosting);
  CHECK(service.status().port == port);
  CHECK(said(log, "listening on port " + std::to_string(port) + " (all network adapters"));
  CHECK(said(log, "players must give the join password to join; the traffic is encrypted"));

  Client a;
  a.connect(port);
  REQUIRE(pump_service(service, {&a}, [&] { return a.connected; }));
  a.send(hello_from("Link"), kChannelSession);
  REQUIRE(pump_service(service, {&a}, [&] { return a.find<Welcome>() != nullptr; }));
  CHECK(a.cipher != nullptr);  // the traffic is encrypted
  CHECK(service.status().players == 1);
  // No password, port or invite text in the log beyond what coop_server says.
  for (const auto& line : log) CHECK(line.find(kTestPassword) == std::string::npos);

  service.stop();
  CHECK(service.status().state == HostState::kStopped);
  CHECK(service.status().players == 0);
  CHECK(said(log, "stopping; relayed "));
  const std::size_t lines = log.size();
  service.stop();  // a second stop does nothing
  service.service(1);
  CHECK(log.size() == lines);
  CHECK_FALSE(service.running());
}

TEST_CASE("host service: a taken port tries the next ones, then says which program to close") {
  EnetLife enet;
  HostService first(service_config(nullptr));
  std::string why;
  REQUIRE(first.start(why));
  const std::uint16_t taken = first.port();

  HostServiceConfig config = service_config(nullptr);
  config.port = taken;
  config.port_tries = 1;
  HostService second(config);
  CHECK_FALSE(second.start(why));
  CHECK(why.find("cannot listen on port " + std::to_string(taken)) != std::string::npos);
  CHECK(why.find("is a co-op server window open already?") != std::string::npos);
  CHECK(second.status().state == HostState::kFailed);
  CHECK(second.status().message == why);
  CHECK(second.status().problem == HostProblem::kPortTaken);
  CHECK(second.port_failed());

  config.port_tries = 3;
  HostService third(config);
  if (third.start(why)) {  // unless the next ports are taken too on this PC
    CHECK(third.port() != taken);
    CHECK(third.port() > taken);
    CHECK(third.port() <= taken + 2);
  } else {
    CHECK(why.find("or the 2 after it") != std::string::npos);
  }
}

TEST_CASE("host service: no join password, no session") {
  EnetLife enet;
  HostServiceConfig config = service_config(nullptr);
  config.session.join_password.clear();
  HostService service(config);
  std::string why;
  CHECK_FALSE(service.start(why));
  CHECK(why.find("a join password is needed") != std::string::npos);
  CHECK(service.port() == 0);
  CHECK(service.add_answer("ABCDE") == coop::server::nat::CodeError::kNotACode);
}

TEST_CASE("host service: a wrong password is refused") {
  EnetLife enet;
  HostService service(service_config(nullptr));
  std::string why;
  REQUIRE(service.start(why));
  Client a;
  a.password = "not-the-pass";
  a.connect(service.port());
  REQUIRE(pump_service(service, {&a}, [&] { return a.connected; }));
  a.send(hello_from("Link"), kChannelSession);
  pump_service(service, {&a}, [&] { return a.find<Refused>() != nullptr || a.disconnected; });
  CHECK(a.find<Welcome>() == nullptr);
  CHECK(service.status().players == 0);
}
