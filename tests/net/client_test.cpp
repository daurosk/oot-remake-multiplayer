// The client against a small fake server written with raw ENet on 127.0.0.1.
// Everything runs on this one thread: each loop step services the client and
// then the fake server.

#include <doctest/doctest.h>

// ENet pulls in windows.h on Windows; keep its min and max macros out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>  // doctest prints string_view through it
#include <span>
#include <string>
#include <vector>

#include "coop/net/client.hpp"
#include "coop/protocol/codec.hpp"

using namespace coop::net;
namespace proto = coop::protocol;

namespace {

std::uint64_t steady_us() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Received {
  ENetPeer* peer = nullptr;
  proto::Message message;
  std::uint8_t channel = 0;
};

class FakeServer {
 public:
  FakeServer() {
    REQUIRE(enet_initialize() == 0);
    ENetAddress address{};
    REQUIRE(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    address.port = 0;  // any free port; ENet reads back the one it got
    host_ = enet_host_create(&address, 4, proto::kChannelCount, 0, 0);
    REQUIRE(host_ != nullptr);
    port_ = host_->address.port;
    REQUIRE(port_ != 0);
  }
  ~FakeServer() {
    enet_host_destroy(host_);
    enet_deinitialize();
  }
  FakeServer(const FakeServer&) = delete;
  FakeServer& operator=(const FakeServer&) = delete;

  std::uint16_t port() const { return port_; }

  void service() {
    ENetEvent event;
    while (enet_host_service(host_, &event, 0) > 0) {
      switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
          ++connects;
          last_peer = event.peer;
          break;
        case ENET_EVENT_TYPE_RECEIVE: {
          Received r;
          r.peer = event.peer;
          r.channel = event.channelID;
          const auto decoded = proto::decode(
              std::span<const std::uint8_t>(event.packet->data,
                                            event.packet->dataLength),
              r.message);
          REQUIRE(decoded == proto::Error::none);
          // Everything the client sends must pass the server's own check.
          CHECK(proto::check_received(r.message, proto::Sender::client,
                                      r.channel) == proto::Error::none);
          received.push_back(std::move(r));
          enet_packet_destroy(event.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
          ++disconnects;
          if (event.peer == last_peer) last_peer = nullptr;
          break;
        case ENET_EVENT_TYPE_NONE:
          break;
      }
    }
    enet_host_flush(host_);
  }

  void send(const proto::Message& message, std::uint8_t channel) {
    REQUIRE(last_peer != nullptr);
    send_bytes(*proto::encode(message), channel);
  }

  void send_bytes(const std::vector<std::uint8_t>& bytes, std::uint8_t channel) {
    REQUIRE(last_peer != nullptr);
    ENetPacket* packet = enet_packet_create(
        bytes.data(), bytes.size(),
        channel < proto::kChannelPlayerSamples ? ENET_PACKET_FLAG_RELIABLE : 0u);
    REQUIRE(enet_peer_send(last_peer, channel, packet) == 0);
    enet_host_flush(host_);
  }

  // Disconnect the client with notice.
  void drop() {
    REQUIRE(last_peer != nullptr);
    enet_peer_disconnect_now(last_peer, 0);
    last_peer = nullptr;
  }

  // Forget the client without a word, as a crashed server would.
  void vanish() {
    REQUIRE(last_peer != nullptr);
    enet_peer_reset(last_peer);
    last_peer = nullptr;
  }

  // The first received message of type T from index `from` on, if any.
  template <typename T>
  std::optional<std::size_t> find(std::size_t from = 0) const {
    for (std::size_t i = from; i < received.size(); ++i) {
      if (std::holds_alternative<T>(received[i].message)) return i;
    }
    return std::nullopt;
  }

  int connects = 0;
  int disconnects = 0;
  ENetPeer* last_peer = nullptr;
  std::vector<Received> received;

 private:
  ENetHost* host_ = nullptr;
  std::uint16_t port_ = 0;
};

ClientConfig config_for(const FakeServer& server) {
  ClientConfig c;
  c.host = "127.0.0.1";
  c.port = server.port();
  c.hello.build_id = "coop-tests 0";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  c.hello.nickname = "Link";
  c.retry_first_ms = 50;
  c.retry_max_ms = 400;
  return c;
}

// Services both ends until `done` is true or the time runs out.
bool pump(Client& client, FakeServer& server, const std::function<bool()>& done,
          int timeout_ms = 3000) {
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    client.service(1);
    server.service();
    if (done()) return true;
  }
  return false;
}

// Collects events as they come, so a test can wait for a kind.
struct Events {
  std::vector<Event> all;
  void drain(Client& client) {
    while (auto e = client.next_event()) all.push_back(std::move(*e));
  }
  std::optional<std::size_t> find(EventKind kind, std::size_t from = 0) const {
    for (std::size_t i = from; i < all.size(); ++i) {
      if (all[i].kind == kind) return i;
    }
    return std::nullopt;
  }
  int count(EventKind kind) const {
    int n = 0;
    for (const auto& e : all) n += e.kind == kind ? 1 : 0;
    return n;
  }
};

bool wait_event(Client& client, FakeServer& server, Events& events,
                EventKind kind, int timeout_ms = 3000) {
  const std::size_t from = events.all.size();
  return pump(client, server, [&] {
    events.drain(client);
    return events.find(kind, from).has_value();
  }, timeout_ms);
}

// Waits for the client's HELLO and answers it with WELCOME.
// Waits from `from_index` on, or from now when it is not given.
proto::Welcome welcome_client(Client& client, FakeServer& server,
                              std::uint64_t server_offset_us = 0,
                              std::optional<std::size_t> from_index = std::nullopt) {
  const std::size_t from = from_index.value_or(server.received.size());
  REQUIRE(pump(client, server, [&] {
    return server.find<proto::Hello>(from).has_value();
  }));
  proto::Welcome w;
  w.player_number = 2;
  w.server_time_us = steady_us() + server_offset_us;
  w.tick_rate = 30;
  w.players = {{1, "Saria"}};
  server.send(w, proto::kChannelSession);
  return w;
}

}  // namespace

TEST_CASE("client: handshake, ping, clock offset and messages") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  CHECK(client.state() == State::idle);
  REQUIRE(client.start());

  const std::size_t from = server.received.size();
  const proto::Welcome sent = welcome_client(client, server, 1'000'000'000);

  // The HELLO is the configured one, on channel 0.
  const auto hello_at = server.find<proto::Hello>(from);
  REQUIRE(hello_at.has_value());
  const auto& hello = std::get<proto::Hello>(server.received[*hello_at].message);
  CHECK(server.received[*hello_at].channel == proto::kChannelSession);
  CHECK(hello.nickname == "Link");
  CHECK(hello.protocol_version == proto::kProtocolVersion);

  REQUIRE(wait_event(client, server, events, EventKind::welcomed));
  const Event& welcomed = events.all[*events.find(EventKind::welcomed)];
  CHECK(welcomed.welcome == sent);
  CHECK(client.state() == State::connected);
  CHECK(client.player_number() == 2);
  // The WELCOME alone already gives a rough offset.
  REQUIRE(client.clock_offset_us().has_value());

  // The client pings on channel 0; answer the PING as a server 1000 s ahead.
  const std::size_t before_ping = server.received.size();
  REQUIRE(pump(client, server, [&] {
    return server.find<proto::Ping>(before_ping).has_value();
  }));
  const auto ping_at = *server.find<proto::Ping>(before_ping);
  CHECK(server.received[ping_at].channel == proto::kChannelSession);
  const auto ping = std::get<proto::Ping>(server.received[ping_at].message);
  server.send(proto::Pong{ping.sender_time_us, steady_us() + 1'000'000'000},
              proto::kChannelSession);
  pump(client, server, [] { return false; }, 100);
  REQUIRE(client.round_trip_us().has_value());
  CHECK(*client.round_trip_us() < 200'000u);
  REQUIRE(client.clock_offset_us().has_value());
  const std::int64_t error = *client.clock_offset_us() - 1'000'000'000;
  CHECK(error > -50'000);
  CHECK(error < 50'000);
  const std::uint64_t client_now = client.now_us();
  CHECK(client.to_client_time(client.to_server_time(client_now)) == client_now);

  // A second ping comes about 250 ms later.
  const std::size_t after_first = ping_at + 1;
  REQUIRE(pump(client, server, [&] {
    return server.find<proto::Ping>(after_first).has_value();
  }, 1000));

  // The server's own PING is answered with its time echoed.
  const std::size_t before_pong = server.received.size();
  server.send(proto::Ping{424242}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] {
    return server.find<proto::Pong>(before_pong).has_value();
  }));
  const auto pong =
      std::get<proto::Pong>(server.received[*server.find<proto::Pong>(before_pong)].message);
  CHECK(pong.ping_sender_time_us == 424242u);

  // A relayed CHAT reaches the caller; a bad one is dropped and counted.
  server.send(proto::Chat{1, "hey"}, proto::kChannelSession);
  server.send(proto::Chat{0, "no player"}, proto::kChannelSession);  // wrong sender
  server.send(proto::Chat{1, "wrong channel"}, proto::kChannelPlayerSamples);
  server.send(proto::Hello{}, proto::kChannelSession);  // client-only type
  server.send_bytes({0xff, 0xff, 0x00, 0x00}, proto::kChannelSession);  // unknown type
  server.send_bytes({0x09, 0x00}, proto::kChannelSession);  // truncated
  REQUIRE(wait_event(client, server, events, EventKind::message));
  pump(client, server, [] { return false; }, 100);
  events.drain(client);
  CHECK(events.count(EventKind::message) == 1);
  const Event& chat = events.all[*events.find(EventKind::message)];
  CHECK(chat.channel == proto::kChannelSession);
  CHECK(chat.message == proto::Message{proto::Chat{1, "hey"}});
  CHECK(client.rejected_count() == 5);

  // Sending: each message on its own channel; wrong ones refused locally.
  CHECK(client.send(proto::Chat{0, "hello"}));
  proto::PlayerSample sample;
  sample.time = {client.now_us(), 1};
  sample.position = proto::Vec3{1.0f, 2.0f, 3.0f};
  CHECK(client.send(sample));
  CHECK(client.send_on(proto::TestPayload{7, {1, 2, 3}}, proto::kChannelSession));
  CHECK(client.send_on(proto::TestPayload{8, {}}, proto::kChannelPlayerSamples));
  CHECK_FALSE(client.send(proto::Chat{3, "pretending"}));
  CHECK_FALSE(client.send(proto::Welcome{}));
  CHECK_FALSE(client.send_on(proto::Chat{0, "x"}, proto::kChannelPlayerSamples));
  const std::size_t before_send = server.received.size();
  REQUIRE(pump(client, server, [&] {
    return server.received.size() >= before_send + 4 &&
           server.find<proto::PlayerSample>(before_send).has_value();
  }));
  const auto sample_at = *server.find<proto::PlayerSample>(before_send);
  CHECK(server.received[sample_at].channel == proto::kChannelPlayerSamples);
  const auto chat_at = *server.find<proto::Chat>(before_send);
  CHECK(server.received[chat_at].channel == proto::kChannelSession);

  client.stop();
  CHECK(client.state() == State::stopped);
  CHECK_FALSE(client.send(proto::Chat{0, "after stop"}));
  REQUIRE(pump(client, server, [&] { return server.disconnects == 1; }));
}

TEST_CASE("client: refusal is reported and not retried") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));
  server.send(proto::Refused{proto::RefuseReason::server_full}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::refused));
  const Event& refused = events.all[*events.find(EventKind::refused)];
  CHECK(refused.refuse_reason == proto::RefuseReason::server_full);
  CHECK(refuse_reason_text(refused.refuse_reason) == "server full");
  CHECK(client.state() == State::refused);

  // Well past the first retry delay: no second attempt.
  pump(client, server, [] { return false; }, 400);
  CHECK(server.connects == 1);
  CHECK(events.count(EventKind::connect_failed) == 0);
}

TEST_CASE("client: an unknown refusal reason is kept") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));
  server.send_bytes({0x03, 0x00, 0x01, 0x00, 0x09}, proto::kChannelSession);  // REFUSED 9
  REQUIRE(wait_event(client, server, events, EventKind::refused));
  const Event& refused = events.all[*events.find(EventKind::refused)];
  CHECK(static_cast<int>(refused.refuse_reason) == 9);
  CHECK(refuse_reason_text(refused.refuse_reason) == "unknown reason");
}

TEST_CASE("client: comes back after the server drops it") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected));
  CHECK(events.all[*events.find(EventKind::disconnected)].drop_reason ==
        DropReason::connection_lost);
  CHECK(client.state() != State::connected);
  CHECK_FALSE(client.send(proto::Chat{0, "while down"}));

  // It connects again and sends HELLO again; WELCOME makes it "reconnected".
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::reconnected));
  CHECK(server.connects == 2);
  CHECK(client.state() == State::connected);
  CHECK(events.count(EventKind::welcomed) == 1);
  CHECK(client.send(proto::Chat{0, "back"}));
}

TEST_CASE("client: a server that vanishes is noticed by the timeout") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.timeout_ms = 1000;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  server.vanish();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected, 5000));
  // The server is still there, so the client gets back in.
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::reconnected));
}

TEST_CASE("client: no answer to HELLO counts as a failed attempt and retries") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.handshake_timeout_ms = 300;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(wait_event(client, server, events, EventKind::connect_failed));
  CHECK(events.all[*events.find(EventKind::connect_failed)].drop_reason ==
        DropReason::handshake_timeout);
  CHECK(client.failed_attempts() >= 1);
  const auto first_hello = server.find<proto::Hello>();
  REQUIRE(first_hello.has_value());

  // The retry is answered this time; the first WELCOME is "welcomed".
  welcome_client(client, server, 0, *first_hello + 1);
  CHECK(server.connects == 2);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));
  CHECK(client.failed_attempts() == 0);
}
