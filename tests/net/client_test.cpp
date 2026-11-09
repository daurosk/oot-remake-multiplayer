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
#include <thread>
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
    const auto bytes = proto::encode(message);
    REQUIRE(bytes.has_value());  // a test sends only messages that encode
    send_bytes(*bytes, channel);
  }

  void send_bytes(const std::vector<std::uint8_t>& bytes, std::uint8_t channel) {
    REQUIRE(last_peer != nullptr);
    ENetPacket* packet = enet_packet_create(
        bytes.data(), bytes.size(),
        channel < proto::kChannelPlayerSamples ? static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE) : 0u);
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
  // A sample is stamped with this (docs/protocol.md, "Encoding", times): the
  // client's time plus the offset, so the server's clock, not the client's.
  const std::int64_t offset = *client.clock_offset_us();
  CHECK(client.to_server_time(client_now) ==
        client_now + static_cast<std::uint64_t>(offset));
  const std::uint64_t before_stamp = client.now_us();
  const std::uint64_t stamp = client.server_now_us();
  const std::uint64_t after_stamp = client.now_us();
  CHECK(stamp >= before_stamp + static_cast<std::uint64_t>(offset));
  CHECK(stamp <= after_stamp + static_cast<std::uint64_t>(offset));

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
  server.send(config_for(server).hello, proto::kChannelSession);  // client-only type
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

TEST_CASE("client: a packet bigger than any message is never assembled") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  // 100 KB: more than the 4 + 65535 bytes any message can take. ENet must
  // refuse its fragments, so it never reaches decode (or the heap).
  const std::size_t rejected = client.rejected_count();
  std::vector<std::uint8_t> big(100'000, 0);
  big[0] = 0x09;
  server.send_bytes(big, proto::kChannelSession);
  pump(client, server, [] { return false; }, 300);
  CHECK(client.rejected_count() == rejected);
  CHECK(client.state() == State::connected);
}

TEST_CASE("client: one service() handles a bounded number of events") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.max_events_per_service = 8;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));
  events.all.clear();

  constexpr int kSent = 40;
  for (int i = 0; i < kSent; ++i) {
    server.send(proto::Chat{1, std::to_string(i)}, proto::kChannelSession);
  }
  // Let all of it land in the client's socket before one service().
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  client.service(0);
  events.drain(client);
  CHECK(events.count(EventKind::message) <= 8);
  CHECK(events.count(EventKind::message) >= 1);

  // The rest comes in later calls, in order.
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return events.count(EventKind::message) == kSent;
  }));
  int n = 0;
  for (const auto& e : events.all) {
    if (e.kind != EventKind::message) continue;
    CHECK(e.message == proto::Message{proto::Chat{1, std::to_string(n)}});
    ++n;
  }
}

TEST_CASE("client: a PING flood gets a bounded number of PONGs") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  const std::size_t before = server.received.size();
  for (std::uint64_t i = 1; i <= 50; ++i) {
    server.send(proto::Ping{i}, proto::kChannelSession);
  }
  pump(client, server, [] { return false; }, 200);
  int pongs = 0;
  for (std::size_t i = before; i < server.received.size(); ++i) {
    pongs += std::holds_alternative<proto::Pong>(server.received[i].message) ? 1 : 0;
  }
  CHECK(pongs >= 1);
  CHECK(pongs <= 6);  // a burst of 4, then one each 125 ms
  CHECK(client.state() == State::connected);
}

TEST_CASE("client: a server that welcomes and drops at once does not reset the backoff") {
  FakeServer server;
  Client client(config_for(server));  // retries 50 ms doubling to 400 ms
  Events events;
  REQUIRE(client.start());

  std::chrono::milliseconds last_gap{0};
  std::size_t hello_from = 0;
  for (int cycle = 0; cycle < 4; ++cycle) {
    welcome_client(client, server, 0, hello_from);
    REQUIRE(wait_event(client, server, events,
                       cycle == 0 ? EventKind::welcomed : EventKind::reconnected));
    server.drop();
    REQUIRE(wait_event(client, server, events, EventKind::disconnected));
    const auto dropped_at = std::chrono::steady_clock::now();
    const int connects = server.connects;
    hello_from = server.received.size();
    REQUIRE(pump(client, server, [&] { return server.connects > connects; }));
    last_gap = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dropped_at);
  }
  // 50, 100, 200, 400: the fourth wait is the longest, not 50 again.
  CHECK(last_gap.count() >= 300);
}

TEST_CASE("client: an injected clock that steps back does not end the attempt") {
  FakeServer server;
  ClientConfig config = config_for(server);
  std::uint64_t fake_now = 1'000'000'000'000;
  config.now_us = [&fake_now] { return fake_now; };
  config.handshake_timeout_ms = 1000;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));

  fake_now -= 1000;
  pump(client, server, [] { return false; }, 200);
  events.drain(client);
  CHECK(events.count(EventKind::connect_failed) == 0);
  CHECK(client.state() == State::handshaking);

  fake_now += 2'000'000;
  REQUIRE(wait_event(client, server, events, EventKind::connect_failed));
  CHECK(events.all[*events.find(EventKind::connect_failed)].drop_reason ==
        DropReason::handshake_timeout);
}

TEST_CASE("client: every refusal reason the protocol has is named") {
  CHECK(refuse_reason_text(proto::RefuseReason::wrong_protocol_version) ==
        "wrong protocol version");
  CHECK(refuse_reason_text(proto::RefuseReason::wrong_game_build) == "wrong game build");
  CHECK(refuse_reason_text(proto::RefuseReason::server_full) == "server full");
  CHECK(refuse_reason_text(proto::RefuseReason::name_taken) == "name taken");
  CHECK(refuse_reason_text(proto::RefuseReason::bad_name) == "bad name");
  CHECK(refuse_reason_text(static_cast<proto::RefuseReason>(6)) == "unknown reason");
}
