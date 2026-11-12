// The invite code's punch handshake over 127.0.0.1: a real game server with
// its HostRendezvous, Join.bat's helper, a fake STUN server and a raw ENet
// game. 127.0.0.1 has no routers, so this proves the messages and the
// forwarding, not a real NAT (that needs two networks).

#include <doctest/doctest.h>

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
#include "join_helper.hpp"
#include "nat_net.hpp"
#include "punch.hpp"
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

  World(const std::string& host_password, const std::string& guest_password, bool probe) {
    ServerOptions options;
    options.port = 0;
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
    jc.host = Addr{0x7F000001u, server->port()};
    jc.password = guest_password;
    jc.stun_servers = {stun.address()};
    jc.game_port = 0;
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
  CHECK(w.rendezvous->public_address().has_value());
  // Before the host pastes it, the game gets nowhere.
  w.game.connect(w.helper->game_port());
  w.pump_until([] { return false; }, 300);
  CHECK_FALSE(w.game.connected);
  CHECK(w.helper->state() == JoinState::kWaitingForHost);

  REQUIRE(w.rendezvous->add_answer(w.answer, ms_now()) == CodeError::kOk);
  REQUIRE(w.pump_until([&] { return w.helper->state() == JoinState::kOpen; }));
  CHECK_FALSE(w.helper->direct());
  CHECK(w.rendezvous->stats().guest_punches > 0);
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
