// The invite code's punch handshake over 127.0.0.1: a real game server with
// its HostRendezvous, Join.bat's helper, a fake STUN server and a raw ENet
// game. 127.0.0.1 has no routers, so this proves the messages and the
// forwarding, not a real NAT (that needs two networks).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/codec.hpp"
#include "enet_server.hpp"
#include "host_service.hpp"
#include "join_helper.hpp"
#include "nat_net.hpp"
#include "punch.hpp"
#include "relay.hpp"
#include "relay_link.hpp"
#include "stun.hpp"

using namespace coop::server;
using namespace coop::server::nat;
using namespace coop::protocol;

namespace {

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

std::uint64_t ms_now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

// Answers every Binding request with the sender's own address.
struct FakeStun {
  ENetSocket socket = ENET_SOCKET_NULL;
  int answered = 0;
  FakeStun() {
    socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress local = to_enet(Addr{0x7F000001u, 0});
    REQUIRE(enet_socket_bind(socket, &local) == 0);
    enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1);
  }
  ~FakeStun() { enet_socket_destroy(socket); }
  Addr address() const {
    ENetAddress a{};
    enet_socket_get_address(socket, &a);
    return Addr{0x7F000001u, a.port};
  }
  void pump() {
    std::uint8_t data[512];
    for (;;) {
      ENetAddress from{};
      ENetBuffer in;
      in.data = data;
      in.dataLength = sizeof data;
      const int n = enet_socket_receive(socket, &from, &in, 1);
      if (n < 20) return;
      const Addr who = from_enet(from);
      const std::uint16_t port = static_cast<std::uint16_t>(who.port ^ 0x2112);
      const std::uint32_t ip = who.ip ^ kStunCookie;
      std::vector<std::uint8_t> out = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
      out.insert(out.end(), data + 8, data + 20);
      const std::vector<std::uint8_t> attr = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01,
                                              static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port),
                                              static_cast<std::uint8_t>(ip >> 24), static_cast<std::uint8_t>(ip >> 16),
                                              static_cast<std::uint8_t>(ip >> 8), static_cast<std::uint8_t>(ip)};
      out.insert(out.end(), attr.begin(), attr.end());
      ENetBuffer reply;
      reply.data = out.data();
      reply.dataLength = out.size();
      enet_socket_send(socket, &from, &reply, 1);
      ++answered;
    }
  }
};

struct Game {
  ENetHost* host = nullptr;
  ENetPeer* peer = nullptr;
  bool connected = false;
  std::vector<Message> inbox;
  Game() { host = enet_host_create(nullptr, 1, kChannelCount, 0, 0); }
  ~Game() {
    if (host != nullptr) enet_host_destroy(host);
  }
  void connect(std::uint16_t port) {
    ENetAddress address = to_enet(Addr{0x7F000001u, port});
    host->mtu = kDatagramBytes;
    peer = enet_host_connect(host, &address, kChannelCount, 0);
  }
  void pump() {
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0) {
      if (ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
      if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        Message m;
        std::span<const std::uint8_t> b(ev.packet->data, ev.packet->dataLength);
        if (decode(b, m) == Error::none) inbox.push_back(m);
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

struct World {
  EnetLife enet;
  FakeStun stun;
  std::unique_ptr<EnetServer> server;
  std::unique_ptr<HostRendezvous> rendezvous;
  std::unique_ptr<JoinHelper> helper;
  Game game;
  std::string answer;
  std::vector<std::string> host_log;

  // invite_host: the address the invite code names (nullopt: the server's
  // own 127.0.0.1 one); nearby: JoinConfig::nearby (nullopt: only the host).
  World(const std::string& host_password, const std::string& guest_password, bool probe,
        std::optional<Addr> invite_host = std::nullopt, std::optional<std::vector<Addr>> nearby = std::vector<Addr>{}) {
    ServerOptions options;
    options.port = 0;
    options.bind = 0x7F000001u;  // 127.0.0.1 only: the one way in these tests reach (the others are meant not to)
    options.session.join_password = host_password;
    server = std::make_unique<EnetServer>(options);
    REQUIRE(server->start());

    HostConfig hc;
    hc.password = host_password;
    hc.stun_servers = {stun.address()};
    hc.allow_private = true;  // 127.0.0.1 is not an internet address
    hc.send = [this](Addr to, std::span<const std::uint8_t> d) { server->send_raw(to_enet(to), d); };
    hc.log = [this](const std::string& s) { host_log.push_back(s); };
    rendezvous = std::make_unique<HostRendezvous>(hc);
    HostRendezvous* r = rendezvous.get();
    server->set_raw_handler([r](const ENetAddress& from, std::span<const std::uint8_t> d) {
      return r->on_datagram(from_enet(from), d, ms_now());
    });
    rendezvous->start(ms_now());

    JoinConfig jc;
    jc.host = invite_host.value_or(Addr{0x7F000001u, server->port()});
    if (nearby && nearby->size() == 1 && nearby->front().port == 0) nearby->front().port = server->port();
    jc.nearby = nearby;
    jc.password = guest_password;
    jc.stun_servers = {stun.address()};
    jc.game_port = 0;
    jc.bind = 0x7F000001u;  // (127.0.0.1 only)
    jc.direct_ms = probe ? 3000 : 0;
    jc.probe = probe;
    jc.on_answer = [this](const std::string& code) { answer = code; };
    helper = std::make_unique<JoinHelper>(jc);
    std::string why;
    REQUIRE(helper->start(why));
  }
  void pump() {
    server->service(0);
    rendezvous->service(ms_now());
    stun.pump();
    helper->service(1);
    game.pump();
  }
  bool pump_until(const std::function<bool()>& done, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
      pump();
      if (done()) return true;
    }
    return false;
  }
  void say_hello() {
    Hello hello;
    hello.build_id = "nat test";
    hello.game_build_id = "e0f3ccfaf50065ae";
    hello.game_version = "1.1.1";
    hello.nickname = "Saria";
    hello.key_share = KeyPair::generate().share();  // as every game sends
    auto bytes = encode(Message{hello});
    REQUIRE(bytes.has_value());
    ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(), ENET_PACKET_FLAG_RELIABLE);
    REQUIRE(enet_peer_send(game.peer, kChannelSession, p) == 0);
  }
};

}  // namespace

TEST_CASE("nat loopback: answer code, punches both ways, then the game reaches the server") {
  World w("pw-loop", "pw-loop", false);
  REQUIRE(w.pump_until([&] { return !w.answer.empty(); }));
  REQUIRE(w.helper->public_address().has_value());
  CHECK(w.helper->public_address()->port == w.helper->public_socket_port());
  // The helper may make its answer code before the server has serviced its own STUN answer.
  CHECK(w.pump_until([&] { return w.rendezvous->public_address().has_value(); }));
  // Before the host pastes it, the game gets nowhere.
  w.game.connect(w.helper->game_port());
  w.pump_until([] { return false; }, 300);
  CHECK_FALSE(w.game.connected);
  CHECK(w.helper->state() == JoinState::kWaitingForHost);

  REQUIRE(w.rendezvous->add_answer(w.answer, ms_now()) == CodeError::kOk);
  REQUIRE(w.pump_until([&] { return w.helper->state() == JoinState::kOpen; }));
  CHECK_FALSE(w.helper->direct());
  CHECK(w.rendezvous->stats().punches_sent > 0);
  // The helper answers the host's punch once; the server sees it on a later service.
  CHECK(w.pump_until([&] { return w.rendezvous->stats().guest_punches > 0; }));
  // ENet's connect is sent again by itself; the server asks for the password.
  REQUIRE(w.pump_until([&] { return w.game.connected; }, 8000));
  w.say_hello();
  REQUIRE(w.pump_until([&] { return w.game.find<JoinChallenge>() != nullptr; }));
  CHECK(w.helper->stats().to_host > 0);
  CHECK(w.helper->stats().to_game > 0);
}

TEST_CASE("nat loopback: a helper with another password never gets a punch back") {
  World w("pw-host", "pw-other", false);
  REQUIRE(w.pump_until([&] { return !w.answer.empty(); }));
  CHECK(w.rendezvous->add_answer(w.answer, ms_now()) == CodeError::kBadSignature);
  w.game.connect(w.helper->game_port());
  w.pump_until([] { return false; }, 1500);
  CHECK(w.helper->state() == JoinState::kWaitingForHost);
  CHECK_FALSE(w.game.connected);
  CHECK(w.rendezvous->stats().rejected > 0);
  CHECK(w.rendezvous->stats().punches_sent == 0);
  CHECK(w.server->session().player_count() == 0);
}

TEST_CASE("nat loopback: a server that hears the helper directly answers its probe") {
  World w("pw-direct", "pw-direct", true);
  REQUIRE(w.pump_until([&] { return w.helper->state() == JoinState::kOpen; }));
  CHECK(w.helper->direct());
  CHECK(w.answer.empty());
  CHECK(w.rendezvous->stats().probes_answered > 0);
  w.game.connect(w.helper->game_port());
  REQUIRE(w.pump_until([&] { return w.game.connected; }, 8000));
}

TEST_CASE("nat loopback: a host on this PC is reached on 127.0.0.1 when its internet address does not answer") {
  // The invite names an internet address that never answers (a router without
  // hairpin NAT); the server listens on this PC.
  const Addr unreachable{0xC0000201u, 9};  // 192.0.2.1:9 (TEST-NET-1)
  World w("pw-same-pc", "pw-same-pc", true, unreachable, std::vector<Addr>{Addr{0x7F000001u, 0}});
  REQUIRE(w.pump_until([&] { return w.helper->state() == JoinState::kOpen; }));
  CHECK(w.helper->direct());
  CHECK(w.helper->nearby_peer());
  CHECK(w.helper->peer() == Addr{0x7F000001u, w.server->port()});
  CHECK(w.answer.empty());
  w.game.connect(w.helper->game_port());
  REQUIRE(w.pump_until([&] { return w.game.connected; }, 8000));
  w.say_hello();
  REQUIRE(w.pump_until([&] { return w.game.find<JoinChallenge>() != nullptr; }));
}

TEST_CASE("nat loopback: a nearby server with another password does not open the way") {
  const Addr unreachable{0xC0000201u, 9};
  World w("pw-host", "pw-other", true, unreachable, std::vector<Addr>{Addr{0x7F000001u, 0}});
  w.pump_until([] { return false; }, 1200);
  CHECK(w.helper->state() != JoinState::kOpen);
  CHECK_FALSE(w.helper->nearby_peer());
  CHECK(w.rendezvous->stats().probes_answered == 0);
  CHECK(w.rendezvous->stats().rejected > 0);
}

// HostService (host_service.hpp) with invite: the same STUN round and code as
// coop_server --invite, read from its status (what the game's menu shows).
TEST_CASE("host service: the invite code holds this server's address and the password") {
  EnetLife enet;
  FakeStun stun;
  std::vector<std::string> log;
  HostServiceConfig config;
  config.port = 0;
  config.bind = 0x7F000001u;  // (127.0.0.1 only)
  config.session.join_password = make_password();
  config.invite = true;
  config.stun_servers = {stun.address()};
  config.stun_allow_private = true;  // 127.0.0.1 is not an internet address
  config.log = [&log](const std::string& line) { log.push_back(line); };
  HostService service(config);
  std::string why;
  REQUIRE(service.start(why));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!service.status().invite_done && std::chrono::steady_clock::now() < deadline) {
    service.service(1);
    stun.pump();
  }
  const HostStatus status = service.status();
  REQUIRE(status.invite_done);
  REQUIRE_FALSE(status.invite.empty());
  const InviteRead read = read_invite(status.invite);
  // 127.0.0.1 is not an internet address: read, but refused for a real join.
  REQUIRE(read.error == CodeError::kNotPublic);
  CHECK(read.invite.host.ip == 0x7F000001u);
  CHECK(read.invite.host.port == service.port());
  CHECK(read.invite.password == config.session.join_password);
  bool shown = false;
  for (const auto& line : log) shown = shown || line == "invite code: " + status.invite;
  CHECK(shown);

  // The game's log (log_invite false) never has the code or the password.
  {
    FakeStun stun2;
    std::vector<std::string> quiet;
    HostServiceConfig c2 = config;
    c2.stun_servers = {stun2.address()};
    c2.log_invite = false;
    c2.log = [&quiet](const std::string& line) { quiet.push_back(line); };
    HostService s2(c2);
    REQUIRE(s2.start(why));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!s2.status().invite_done && std::chrono::steady_clock::now() < until) {
      s2.service(1);
      stun2.pump();
    }
    REQUIRE_FALSE(s2.status().invite.empty());
    for (const auto& line : quiet) {
      CHECK(line.find(s2.status().invite) == std::string::npos);
      CHECK(line.find(c2.session.join_password) == std::string::npos);
    }
  }

  // A pasted answer that is not one is explained, not used.
  CHECK(service.add_answer("hello") != CodeError::kOk);
  bool explained = false;
  for (const auto& line : log) explained = explained || line.rfind("answer code not used: ", 0) == 0;
  CHECK(explained);
}

TEST_CASE("host service: no STUN answer, no invite code, and the status says why") {
  EnetLife enet;
  HostServiceConfig config;
  config.port = 0;
  config.bind = 0x7F000001u;  // (127.0.0.1 only)
  config.session.join_password = make_password();
  config.invite = true;  // and no STUN server at all
  std::uint64_t now = 0;
  config.now_ms = [&now] { return now; };
  HostService service(config);
  std::string why;
  REQUIRE(service.start(why));
  for (int i = 0; i < 100 && !service.status().invite_done; ++i) {
    now += 500;
    service.service(0);
  }
  const HostStatus status = service.status();
  REQUIRE(status.invite_done);
  CHECK(status.invite.empty());
  CHECK(status.message.find("no invite code") != std::string::npos);
  CHECK(status.problem == HostProblem::kNoInvite);
  CHECK(status.state == HostState::kHosting);  // LAN and VPN players still get in
}

TEST_CASE("join helper: the probe also goes to this PC and the local network, at the invite's port and 27510") {
  const Addr host{0xCB007105u, 40000};  // 203.0.113.5:40000 (a router that maps another port)
  const std::vector<Addr> want{{0x7F000001u, 40000}, {0x7F000001u, 27510}, {0xFFFFFFFFu, 40000}, {0xFFFFFFFFu, 27510}};
  CHECK(nearby_targets(host) == want);
  // The usual port once each; a host already on 127.0.0.1 is not in it twice.
  const std::vector<Addr> usual{{0x7F000001u, 27510}, {0xFFFFFFFFu, 27510}};
  CHECK(nearby_targets(Addr{0xCB007105u, 27510}) == usual);
  CHECK(nearby_targets(Addr{0x7F000001u, 27510}) == std::vector<Addr>{{0xFFFFFFFFu, 27510}});
}

TEST_CASE("join helper: the local-network probe only for a long or Host.bat-made password, to each card's broadcast") {
  CHECK(lan_probe_ok(make_password()));
  CHECK(lan_probe_ok(std::string(20, 'x')));
  CHECK_FALSE(lan_probe_ok("hunter2"));
  CHECK_FALSE(lan_probe_ok(std::string(19, 'x')));
  const Addr host{0xCB007105u, 27510};
  // A short password: only this PC.
  CHECK(nearby_targets(host, {}, false) == std::vector<Addr>{{0x7F000001u, 27510}});
  // Two cards (the home network and a VPN): each one's broadcast address too; a /32 has none.
  const std::vector<LocalNet> cards{{0xC0A80105u, 0xFFFFFF00u, 0xC0A80101u},
                                    {0x1A000002u, 0xFF000000u, 0},
                                    {0x0A000009u, 0xFFFFFFFFu, 0}};
  const std::vector<Addr> want{{0x7F000001u, 27510}, {0xFFFFFFFFu, 27510}, {0xC0A801FFu, 27510}, {0x1AFFFFFFu, 27510}};
  CHECK(nearby_targets(host, cards, true) == want);
}

// The direct way through a relay (issue 261): a room name finds the host, the relay passes the guest's answer code
// to the host's server, and only when the host took it does its address come back; the game then reaches the
// host's server through the join helper, not through the relay.
namespace {

struct DirectWorld {
  EnetLife enet;
  FakeStun stun;
  coop::server::relay::RelayServer relay{0, 0x7F000001u, coop::server::relay::RelayConfig{}};
  std::unique_ptr<HostService> host;
  ENetAddress relay_at{};
  std::string pw = make_password();

  explicit DirectWorld(bool invite) {
    REQUIRE(relay.start());
    HostServiceConfig c;
    c.port = 0;
    c.bind = 0x7F000001u;
    c.helper_bind = 0x7F000001u;
    c.session.join_password = pw;
    c.invite = invite;
    c.stun_servers = {stun.address()};
    c.stun_allow_private = true;  // 127.0.0.1 is not an internet address
    c.relay_via = "127.0.0.1:" + std::to_string(relay.port());
    c.relay_room = "Forest-Gang";
    host = std::make_unique<HostService>(c);
    std::string why;
    REQUIRE(host->start(why));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((host->status().relay_room.empty() || (invite && !host->status().invite_done)) &&
           std::chrono::steady_clock::now() < deadline) {
      host->service(0);
      relay.service(1);
      stun.pump();
    }
    REQUIRE_FALSE(host->status().relay_room.empty());
    relay_at = to_enet(Addr{0x7F000001u, relay.port()});
  }
  const std::string& password() const { return pw; }
};

// The host's side (server, relay, STUN) on a thread of its own while `body` runs, as two PCs would.
void with_host(DirectWorld& w, const std::function<void()>& body) {
  std::atomic<bool> done{false};
  std::thread serve([&] {
    while (!done) {
      w.relay.service(1);
      w.host->service(0);
      w.stun.pump();
    }
  });
  try {
    body();
  } catch (...) {
    done = true;  // (a failed REQUIRE: stop the host's thread before the test ends)
    serve.join();
    throw;
  }
  done = true;
  serve.join();
}

std::unique_ptr<JoinHelper> unknown_host_helper(DirectWorld& w, const std::string& password, std::string& answer) {
  JoinConfig jc;
  jc.host = Addr{};  // (the relay's reply gives it)
  jc.password = password;
  jc.stun_servers = {w.stun.address()};
  jc.game_port = 0;
  jc.bind = 0x7F000001u;
  jc.direct_ms = 0;
  jc.on_answer = [&answer](const std::string& code) { answer = code; };
  auto helper = std::make_unique<JoinHelper>(jc);
  std::string why;
  REQUIRE(helper->start(why));
  return helper;
}

}  // namespace

TEST_CASE("direct way: the relay passes the answer code, the host's address comes back, the game reaches the host directly") {
  DirectWorld w(true);
  std::string answer;
  auto helper = unknown_host_helper(w, w.password(), answer);
  CHECK_FALSE(helper->host_known());
  with_host(w, [&] {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (answer.empty() && std::chrono::steady_clock::now() < until) helper->service(1);
    REQUIRE_FALSE(answer.empty());
    // Before the host's reply nothing goes toward the host: the helper has no address for it.
    CHECK(helper->stats().to_host == 0);

    coop::server::relay::RelayJoin join;
    join.room = "forest-gang";
    join.relay_host = "127.0.0.1";
    join.relay_port = w.relay.port();
    const auto r = coop::server::relay::direct_on_relay(w.relay_at, join, answer, {}, 4000, 0x7F000001u);
    REQUIRE(r.found == coop::server::relay::LookupResult::kOk);
    REQUIRE(r.host.has_value());
    CHECK(r.host->ip == 0x7F000001u);
    CHECK(r.host->port == w.host->port());   // (the invite's address: the game port, not the relay link's)
    helper->set_host(Addr{r.host->ip, r.host->port});
    CHECK(helper->host_known());

    const auto open_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (helper->state() != JoinState::kOpen && std::chrono::steady_clock::now() < open_until) helper->service(1);
    REQUIRE(helper->state() == JoinState::kOpen);
    CHECK(helper->peer() == Addr{r.host->ip, r.host->port});

    // The game's own traffic now goes through the helper to the host's server: it asks for the password.
    Game game;
    game.connect(helper->game_port());
    const auto game_until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool hello_sent = false;
    while (std::chrono::steady_clock::now() < game_until && game.find<JoinChallenge>() == nullptr) {
      helper->service(1);
      game.pump();
      if (game.connected && !hello_sent) {
        hello_sent = true;
        Hello hello;
        hello.build_id = "nat test";
        hello.game_build_id = "e0f3ccfaf50065ae";
        hello.game_version = "1.1.1";
        hello.nickname = "Saria";
        hello.key_share = KeyPair::generate().share();
        auto bytes = encode(Message{hello});
        REQUIRE(bytes.has_value());
        ENetPacket* p = enet_packet_create(bytes->data(), bytes->size(), ENET_PACKET_FLAG_RELIABLE);
        REQUIRE(enet_peer_send(game.peer, kChannelSession, p) == 0);
      }
    }
    CHECK(game.find<JoinChallenge>() != nullptr);
    CHECK(helper->stats().to_host > 0);
  });
  // Not one game datagram went through the relay.
  CHECK(w.relay.relay().stats().to_host == 0);
  CHECK(w.relay.relay().stats().directs_passed >= 1);
  CHECK(w.relay.relay().stats().directs_answered >= 1);
  w.host->stop();
}

TEST_CASE("direct way: another join password, or a host without an invite code, gives no address back") {
  coop::server::relay::RelayJoin join;
  join.room = "Forest-Gang";
  join.relay_host = "127.0.0.1";
  SUBCASE("another join password: the host's server does not take the answer code") {
    DirectWorld w(true);
    join.relay_port = w.relay.port();
    std::string answer;
    auto helper = unknown_host_helper(w, "another-password-of-ours", answer);
    with_host(w, [&] {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (answer.empty() && std::chrono::steady_clock::now() < until) helper->service(1);
      REQUIRE_FALSE(answer.empty());
      const auto r = coop::server::relay::direct_on_relay(w.relay_at, join, answer, {}, 4000, 0x7F000001u);
      CHECK(r.found == coop::server::relay::LookupResult::kOk);
      CHECK_FALSE(r.host.has_value());
    });
    w.host->stop();
  }
  SUBCASE("no invite code on the host (as a public game): no direct way") {
    DirectWorld w(false);
    join.relay_port = w.relay.port();
    with_host(w, [&] {
      const auto r = coop::server::relay::direct_on_relay(w.relay_at, join, "ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567",
                                                          {}, 4000, 0x7F000001u);
      CHECK(r.found == coop::server::relay::LookupResult::kOk);
      CHECK_FALSE(r.host.has_value());
    });
    w.host->stop();
  }
  SUBCASE("a room nobody holds: unknown, and nothing passed") {
    DirectWorld w(true);
    join.relay_port = w.relay.port();
    join.room = "nobody-here";
    with_host(w, [&] {
      const auto r = coop::server::relay::direct_on_relay(w.relay_at, join, "ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567",
                                                          {}, 4000, 0x7F000001u);
      CHECK(r.found == coop::server::relay::LookupResult::kUnknown);
      CHECK_FALSE(r.host.has_value());
    });
    CHECK(w.relay.relay().stats().directs_passed == 0);
    w.host->stop();
  }
}
