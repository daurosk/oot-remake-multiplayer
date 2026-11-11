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

TEST_CASE("client: name taken is asked again, then welcomed (a crashed build's old self)") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.name_taken_retry_ms = 50;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));
  server.send(proto::Refused{proto::RefuseReason::name_taken}, proto::kChannelSession);

  // No refusal reported: the client waits and connects again.
  const std::size_t from = server.received.size();
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return server.find<proto::Hello>(from).has_value();
  }));
  CHECK(events.count(EventKind::refused) == 0);
  CHECK(server.connects == 2);
  CHECK(client.name_taken_retries() == 1);

  // This time the server lets it in.
  welcome_client(client, server, 0, from);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));
  CHECK(client.state() == State::connected);
  CHECK(events.count(EventKind::refused) == 0);
}

TEST_CASE("client: a name still taken after the retry time is reported and not asked again") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.name_taken_retry_ms = 40;  // raised to the 100 ms floor
  config.name_taken_retry_for_ms = 500;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  // The server refuses every HELLO: someone else really has the name.
  std::size_t answered = 0;
  REQUIRE(pump(client, server, [&] {
    while (auto at = server.find<proto::Hello>(answered)) {
      answered = *at + 1;
      server.send(proto::Refused{proto::RefuseReason::name_taken}, proto::kChannelSession);
    }
    events.drain(client);
    return events.find(EventKind::refused).has_value();
  }, 3000));
  const Event& refused = events.all[*events.find(EventKind::refused)];
  CHECK(refused.refuse_reason == proto::RefuseReason::name_taken);
  CHECK(client.state() == State::refused);
  CHECK(client.name_taken_retries() >= 3);
  const int connects = server.connects;
  pump(client, server, [] { return false; }, 200);
  CHECK(server.connects == connects);
}

TEST_CASE("client: with name_taken_retry_for_ms 0 a name taken is reported at once") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.name_taken_retry_for_ms = 0;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));
  server.send(proto::Refused{proto::RefuseReason::name_taken}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::refused));
  CHECK(client.state() == State::refused);
  CHECK(client.name_taken_retries() == 0);
}

TEST_CASE("client: with name_taken_retry_ms 0 a name taken is reported at once, not retried every 1 ms") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.name_taken_retry_ms = 0;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));
  server.send(proto::Refused{proto::RefuseReason::name_taken}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::refused));
  CHECK(client.state() == State::refused);
  CHECK(client.name_taken_retries() == 0);
  CHECK_FALSE(client.retrying_name_taken());
  pump(client, server, [] { return false; }, 200);
  CHECK(server.connects == 1);
}

TEST_CASE("client: a name taken retry interval below 100 ms is raised to 100 ms") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.name_taken_retry_ms = 1;
  config.name_taken_retry_for_ms = 2000;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  std::size_t answered = 0;
  // Refuse every HELLO for 450 ms: at 1 ms that would be hundreds of tries.
  pump(client, server, [&] {
    while (auto at = server.find<proto::Hello>(answered)) {
      answered = *at + 1;
      server.send(proto::Refused{proto::RefuseReason::name_taken}, proto::kChannelSession);
    }
    events.drain(client);
    return false;
  }, 450);
  CHECK(events.count(EventKind::refused) == 0);
  CHECK(client.retrying_name_taken());
  CHECK(client.name_taken_retries() >= 1);
  CHECK(client.name_taken_retries() <= 5);
}

TEST_CASE("client: an attempt that ends another way closes the name taken window") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.name_taken_retry_ms = 100;
  config.name_taken_retry_for_ms = 10000;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));
  server.send(proto::Refused{proto::RefuseReason::name_taken}, proto::kChannelSession);

  // The retry connects again; this time the server drops it before answering.
  std::size_t from = server.received.size();
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>(from).has_value(); }));
  CHECK(client.retrying_name_taken());
  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::connect_failed));
  CHECK_FALSE(client.retrying_name_taken());
  CHECK(client.state() == State::waiting_retry);
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
        "wrong protocol version: this game and the server are different releases; "
        "run Update.bat in every kit, the host's too");
  CHECK(refuse_reason_text(proto::RefuseReason::wrong_game_build) == "wrong game build");
  CHECK(refuse_reason_text(proto::RefuseReason::server_full) == "server full");
  CHECK(refuse_reason_text(proto::RefuseReason::name_taken) == "name taken");
  CHECK(refuse_reason_text(proto::RefuseReason::bad_name) == "bad name");
  CHECK(refuse_reason_text(static_cast<proto::RefuseReason>(6)) == "unknown reason");
}

TEST_CASE("client: send_chat sends CHAT and refuses empty, long or bad text") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  CHECK_FALSE(client.send_chat("before the welcome"));
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  const std::string longest(kMaxChatChars, 'a');
  std::string accented;  // 200 characters of 2 bytes each
  for (std::size_t i = 0; i < kMaxChatChars; ++i) accented += "\xc3\xa9";
  CHECK(client.send_chat("hi"));
  CHECK(client.send_chat(longest));
  CHECK(client.send_chat(accented));
  CHECK_FALSE(client.send_chat(""));
  CHECK_FALSE(client.send_chat(longest + "a"));
  CHECK_FALSE(client.send_chat(accented + "\xc3\xa9"));
  CHECK_FALSE(client.send_chat("line\nbreak"));  // a control character
  CHECK_FALSE(client.send_chat("\xc3"));         // cut-off UTF-8

  const std::size_t from = 0;
  REQUIRE(pump(client, server, [&] {
    std::size_t chats = 0;
    for (std::size_t i = from; i < server.received.size(); ++i) {
      chats += std::holds_alternative<proto::Chat>(server.received[i].message) ? 1 : 0;
    }
    return chats >= 3;
  }));
  pump(client, server, [] { return false; }, 100);
  std::vector<std::string> texts;
  for (const auto& r : server.received) {
    if (const auto* chat = std::get_if<proto::Chat>(&r.message)) {
      CHECK(r.channel == proto::kChannelSession);
      CHECK(chat->player_number == proto::kPlayerSelf);
      texts.push_back(chat->text);
    }
  }
  CHECK(texts == std::vector<std::string>{"hi", longest, accented});

  client.stop();
  CHECK_FALSE(client.send_chat("after stop"));
}

TEST_CASE("client: a relayed CHAT becomes a chat event with its sender") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);  // we are 2, Saria is 1
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  server.send(proto::Chat{3, "nobody"}, proto::kChannelSession);  // not in the room
  server.send(proto::Chat{2, "echo"}, proto::kChannelSession);    // our own number
  server.send(proto::Chat{1, "hey"}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::chat));
  pump(client, server, [] { return false; }, 100);
  events.drain(client);
  CHECK(events.count(EventKind::chat) == 1);
  const std::size_t chat_at = *events.find(EventKind::chat);
  const Event& chat = events.all[chat_at];
  CHECK(chat.player == 1);
  CHECK(chat.text == "hey");
  CHECK(chat.channel == proto::kChannelSession);
  // The raw message comes first, for hosts that read every message.
  const auto message_at = events.find(EventKind::message);
  REQUIRE(message_at.has_value());
  CHECK(*message_at + 1 == chat_at);
  CHECK(events.all[*message_at].message == proto::Message{proto::Chat{1, "hey"}});
  CHECK(events.count(EventKind::message) == 1);
  CHECK(client.rejected_count() == 2);
}

TEST_CASE("client: the player list follows joins, statuses, leaves and reconnects") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  CHECK(client.players().empty());
  welcome_client(client, server);  // we are 2, Saria is 1
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  const Player saria{1, "Saria", std::nullopt, false, std::nullopt};
  const Player link{2, "Link", std::nullopt, true, std::nullopt};
  CHECK(client.players() == std::vector<Player>{saria, link});
  CHECK(client.find_player(4) == nullptr);

  // Join.
  server.send(proto::PlayerJoined{3, "Zelda"}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::player_joined));
  const Event joined = events.all[*events.find(EventKind::player_joined)];
  CHECK(joined.player == 3);
  CHECK(joined.nickname == "Zelda");
  REQUIRE(client.players().size() == 3);
  CHECK(client.players()[2] == Player{3, "Zelda", std::nullopt, false, std::nullopt});

  // Status, theirs received and ours sent; unknown bits are kept.
  const std::uint8_t flags = proto::status::paused | 0x80;
  server.send(proto::PlayerStatus{3, flags}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::player_status));
  const Event status = events.all[*events.find(EventKind::player_status)];
  CHECK(status.player == 3);
  CHECK(status.status_flags == flags);
  REQUIRE(client.find_player(3) != nullptr);
  CHECK(client.find_player(3)->status == flags);
  CHECK(client.send(proto::PlayerStatus{0, proto::status::loading}));
  CHECK(client.find_player(2)->status == proto::status::loading);

  // Refused: a leave or a status for nobody, a join, leave or status for us.
  const std::size_t rejected = client.rejected_count();
  server.send(proto::PlayerStatus{4, 0}, proto::kChannelSession);
  server.send(proto::PlayerLeft{4, proto::LeaveReason::left}, proto::kChannelSession);
  server.send(proto::PlayerJoined{2, "Impostor"}, proto::kChannelSession);
  server.send(proto::PlayerLeft{2, proto::LeaveReason::kicked}, proto::kChannelSession);
  server.send(proto::PlayerStatus{2, proto::status::dead}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == rejected + 5; }));
  CHECK(client.players().size() == 3);
  CHECK(client.find_player(2)->status == proto::status::loading);

  // Leave: the event keeps the nickname; an unknown reason is kept raw.
  server.send(proto::PlayerLeft{3, static_cast<proto::LeaveReason>(9)},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::player_left));
  const Event left = events.all[*events.find(EventKind::player_left)];
  CHECK(left.player == 3);
  CHECK(left.nickname == "Zelda");
  CHECK(static_cast<int>(left.leave_reason) == 9);
  CHECK(client.players() == std::vector<Player>{saria, Player{2, "Link", proto::status::loading, true, std::nullopt}});

  // A join for a number already in the room replaces that player.
  const std::size_t before = events.all.size();
  server.send(proto::PlayerJoined{1, "Mido"}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return events.find(EventKind::player_joined, before).has_value();
  }));
  CHECK(client.find_player(1)->nickname == "Mido");
  CHECK(client.players().size() == 2);

  // A drop empties the list; the next WELCOME rebuilds it with no statuses.
  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected));
  CHECK(client.players().empty());
  welcome_client(client, server);
  REQUIRE(wait_event(client, server, events, EventKind::reconnected));
  CHECK(client.players() == std::vector<Player>{saria, link});

  client.stop();
  CHECK(client.players().empty());
}

TEST_CASE("client: the new event kinds are named") {
  CHECK(to_string(EventKind::chat) == "chat");
  CHECK(to_string(EventKind::player_joined) == "player joined");
  CHECK(to_string(EventKind::player_left) == "player left");
  CHECK(to_string(EventKind::player_status) == "player status");
}

// ---- Shared flags and join partner ----

namespace {

// Answers the client's HELLO with WELCOME: we are `self`, the others given.
void welcome_as(Client& client, FakeServer& server, Events& events, std::uint8_t self,
                std::vector<proto::PlayerInfo> others,
                std::optional<std::uint8_t> host = std::nullopt) {
  // As the server does: WELCOME, then HOST (by default the lowest number
  // in, as if the players came in that order).
  std::uint8_t lowest = self;
  for (const auto& p : others) lowest = std::min(lowest, p.number);
  const std::uint8_t hosting = host.value_or(lowest);
  const std::size_t from = server.received.size();
  REQUIRE(pump(client, server, [&] {
    return server.find<proto::Hello>(from).has_value();
  }));
  proto::Welcome w;
  w.player_number = self;
  w.server_time_us = steady_us();
  w.tick_rate = 30;
  w.players = std::move(others);
  server.send(w, proto::kChannelSession);
  server.send(proto::Host{hosting}, proto::kChannelProgress);
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return client.state() == State::connected && client.host_number() == hosting;
  }));
}

// Waits for the server to get a message of type T from `from` on; its index.
template <typename T>
std::size_t wait_received(Client& client, FakeServer& server, std::size_t from) {
  std::optional<std::size_t> at;
  REQUIRE(pump(client, server, [&] {
    at = server.find<T>(from);
    return at.has_value();
  }));
  return *at;
}

// Messages of type T the server got from `from` on.
template <typename T>
std::size_t count_received(const FakeServer& server, std::size_t from) {
  std::size_t n = 0;
  for (std::size_t i = from; i < server.received.size(); ++i) {
    n += std::holds_alternative<T>(server.received[i].message) ? 1 : 0;
  }
  return n;
}

proto::FlagEntry bool_flag(std::uint32_t hash, bool value, std::uint16_t index = 0) {
  return {hash, index, proto::FlagKind::boolean, value ? 1u : 0u};
}

proto::FlagEntry key64_flag(std::uint64_t key, bool value) {
  return {proto::FlagKey::key64_flag(key), proto::FlagKind::boolean, value ? 1u : 0u};
}

proto::FlagEntry s32_flag(std::uint32_t hash, std::int32_t value) {
  return {hash, 0, proto::FlagKind::int32, static_cast<std::uint32_t>(value)};
}

// Services both ends for a while, for checks that something did not happen.
void settle(Client& client, FakeServer& server, Events& events, int ms = 100) {
  pump(client, server, [] { return false; }, ms);
  events.drain(client);
}

}  // namespace

TEST_CASE("client flags: set_flags sends one FLAG_SET per call, checked") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  CHECK_FALSE(client.set_flag(bool_flag(1, true)));  // not connected
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK_FALSE(client.is_host());

  const std::size_t from = server.received.size();
  const std::vector<proto::FlagEntry> batch = {
      bool_flag(10, true), s32_flag(11, -5), bool_flag(10, false), bool_flag(10, true, 3)};
  CHECK(client.set_flags(batch));
  const std::size_t at = wait_received<proto::FlagSet>(client, server, from);
  CHECK(server.received[at].channel == proto::kChannelProgress);
  const auto& set = std::get<proto::FlagSet>(server.received[at].message);
  CHECK(set.player_number == proto::kPlayerSelf);
  // The same (hash, index) twice keeps the last value, in first-seen order.
  CHECK(set.flags == std::vector<proto::FlagEntry>{bool_flag(10, false), s32_flag(11, -5),
                                                    bool_flag(10, true, 3)});
  // A player's own change waits for the host's answer.
  CHECK(client.flags().empty());

  // Refused whole, nothing sent: none, a bool of 2, a NaN, a bad kind.
  const std::size_t sent = server.received.size();
  CHECK_FALSE(client.set_flags({}));
  CHECK_FALSE(client.set_flags(std::vector<proto::FlagEntry>{
      bool_flag(1, true), {2, 0, proto::FlagKind::boolean, 2}}));
  CHECK_FALSE(client.set_flag({3, 0, proto::FlagKind::float32, 0x7fc00000u}));
  CHECK_FALSE(client.set_flag({4, 0, static_cast<proto::FlagKind>(9), 0}));
  settle(client, server, events);
  CHECK(server.received.size() == sent);
  // More than one message's worth goes as several FLAG_SETs from one call.
  std::vector<proto::FlagEntry> many;
  for (std::uint32_t i = 0; i < proto::kMaxFlagEntries + 5; ++i) many.push_back(s32_flag(100 + i, 1));
  CHECK(client.set_flags(many));
  REQUIRE(pump(client, server, [&] { return count_received<proto::FlagSet>(server, sent) == 2; }));
  const auto first = server.find<proto::FlagSet>(sent);
  REQUIRE(first.has_value());
  CHECK(std::get<proto::FlagSet>(server.received[*first].message).flags.size() ==
        proto::kMaxFlagEntries);
}

TEST_CASE("client flags: every FLAG_STATE goes into the table with an event") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});

  const std::size_t rejected = client.rejected_count();
  server.send(proto::FlagState{3, {bool_flag(1, true)}}, proto::kChannelProgress);  // nobody
  server.send(proto::FlagState{2, {bool_flag(1, true)}}, proto::kChannelProgress);  // us
  server.send(proto::FlagState{1, {bool_flag(7, true), s32_flag(8, 42)}},
              proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_state));
  const Event& state = events.all[*events.find(EventKind::flag_state)];
  CHECK(state.player == 1);
  CHECK(state.channel == proto::kChannelProgress);
  CHECK(state.flags == std::vector<proto::FlagEntry>{bool_flag(7, true), s32_flag(8, 42)});
  CHECK(client.rejected_count() == rejected + 2);
  CHECK(client.flags().size() == 2);
  CHECK(client.find_flag(8) == s32_flag(8, 42));
  CHECK_FALSE(client.find_flag(8, 1).has_value());

  // A later state wins over the table, as over the game's own change.
  server.send(proto::FlagState{1, {s32_flag(8, 7)}}, proto::kChannelProgress);
  const std::size_t before = events.all.size();
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return events.find(EventKind::flag_state, before).has_value();
  }));
  CHECK(client.find_flag(8) == s32_flag(8, 7));
  CHECK(events.count(EventKind::flag_state) == 2);
}

TEST_CASE("client flags: a snapshot in parts gives one event and synced") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK_FALSE(client.synced());

  // A state from before goes; the snapshot is the whole table.
  server.send(proto::FlagState{1, {bool_flag(99999, true)}}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_state));

  // Three small parts, as split_snapshot would make of a bigger table.
  std::vector<proto::FlagEntry> table;
  std::vector<proto::FlagSnapshot> parts_list;
  for (std::uint16_t p = 0; p < 3; ++p) {
    proto::FlagSnapshot part{p, 3, {}};
    for (std::uint32_t i = 0; i < 4; ++i) {
      part.flags.push_back(s32_flag(p * 10u + i + 1, static_cast<std::int32_t>(i)));
    }
    table.insert(table.end(), part.flags.begin(), part.flags.end());
    parts_list.push_back(std::move(part));
  }
  const auto* parts = &parts_list;

  // Out of order: part 1 with no part 0 is refused, and so is part 2 after 0.
  const std::size_t rejected = client.rejected_count();
  server.send((*parts)[1], proto::kChannelProgress);
  server.send((*parts)[0], proto::kChannelProgress);
  server.send((*parts)[2], proto::kChannelProgress);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == rejected + 2; }));
  settle(client, server, events);
  CHECK_FALSE(client.synced());
  CHECK(events.count(EventKind::flag_snapshot) == 0);

  // In order: not synced until the last part.
  server.send((*parts)[0], proto::kChannelProgress);
  server.send((*parts)[1], proto::kChannelProgress);
  settle(client, server, events);
  CHECK_FALSE(client.synced());
  CHECK(events.count(EventKind::flag_snapshot) == 0);
  server.send((*parts)[2], proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_snapshot));
  CHECK(client.synced());
  CHECK(events.count(EventKind::flag_snapshot) == 1);
  CHECK(events.all[*events.find(EventKind::flag_snapshot)].flags == table);
  CHECK(client.flags().size() == table.size());
  CHECK_FALSE(client.find_flag(99999).has_value());
  CHECK(client.rejected_count() == rejected + 2);

  // An empty table is one empty part.
  server.send(proto::FlagSnapshot{0, 1, {}}, proto::kChannelProgress);
  const std::size_t before = events.all.size();
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return events.find(EventKind::flag_snapshot, before).has_value();
  }));
  CHECK(client.flags().empty());
  CHECK(client.synced());
}

TEST_CASE("client flags: the host answers FLAG_SET through its callback") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 1, {{2, "Saria"}, {3, "Zelda"}});
  CHECK(client.is_host());

  // Default: accept every value; the answer is a FLAG_STATE, applied here too.
  std::size_t from = server.received.size();
  server.send(proto::FlagSet{2, {bool_flag(5, true), s32_flag(6, 3)}}, proto::kChannelProgress);
  std::size_t at = wait_received<proto::FlagState>(client, server, from);
  CHECK(server.received[at].channel == proto::kChannelProgress);
  CHECK(std::get<proto::FlagState>(server.received[at].message) ==
        proto::FlagState{0, {bool_flag(5, true), s32_flag(6, 3)}});
  REQUIRE(wait_event(client, server, events, EventKind::flag_state));
  const std::size_t state_at = *events.find(EventKind::flag_state);
  CHECK(events.all[state_at].player == 1);  // our own answer
  CHECK(events.all[state_at - 1].kind == EventKind::message);
  CHECK(client.find_flag(5) == bool_flag(5, true));

  // A callback that changes one, refuses one already in the table (sent back
  // with the table's value), refuses one not in it (left out), returns a bad
  // value (taken as refused), and gives another hash (kept as asked).
  std::vector<std::uint8_t> askers;
  client.set_flag_answer([&](std::uint8_t player, const proto::FlagEntry& asked)
                             -> std::optional<proto::FlagEntry> {
    askers.push_back(player);
    switch (asked.key.hash) {
      case 6: return s32_flag(6, 100);
      case 5: return std::nullopt;
      case 20: return std::nullopt;
      case 21: return proto::FlagEntry{21, 0, proto::FlagKind::boolean, 7};
      default: return s32_flag(999, 1);
    }
  });
  from = server.received.size();
  server.send(proto::FlagSet{3, {s32_flag(6, 4), bool_flag(5, false), bool_flag(20, true),
                                 bool_flag(21, true), s32_flag(22, 9)}},
              proto::kChannelProgress);
  at = wait_received<proto::FlagState>(client, server, from);
  CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
        std::vector<proto::FlagEntry>{s32_flag(6, 100), bool_flag(5, true), s32_flag(22, 1)});
  CHECK(askers == std::vector<std::uint8_t>{3, 3, 3, 3, 3});
  CHECK(client.find_flag(6) == s32_flag(6, 100));
  CHECK_FALSE(client.find_flag(20).has_value());

  // All refused and none known: no FLAG_STATE, no flag_state event.
  client.set_flag_answer([](std::uint8_t, const proto::FlagEntry&) {
    return std::optional<proto::FlagEntry>{};
  });
  settle(client, server, events);
  from = server.received.size();
  const int states = events.count(EventKind::flag_state);
  server.send(proto::FlagSet{2, {bool_flag(30, true)}}, proto::kChannelProgress);
  settle(client, server, events, 200);
  CHECK_FALSE(server.find<proto::FlagState>(from).has_value());
  CHECK(events.count(EventKind::flag_state) == states);

  // The host's own change goes out as FLAG_STATE and into the table, no event.
  client.set_flag_answer({});
  from = server.received.size();
  CHECK(client.set_flag(bool_flag(40, true)));
  at = wait_received<proto::FlagState>(client, server, from);
  CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
        std::vector<proto::FlagEntry>{bool_flag(40, true)});
  CHECK(client.find_flag(40) == bool_flag(40, true));
  CHECK_FALSE(server.find<proto::FlagSet>(from).has_value());

  // A FLAG_SET from a player whose PLAYER_LEFT came first (channel 0 is
  // not ordered against 1) is still answered: the answer is for everyone.
  int asked = 0;
  client.set_flag_answer([&](std::uint8_t, const proto::FlagEntry& f) {
    ++asked;
    return std::optional<proto::FlagEntry>{f};
  });
  const std::size_t rejected = client.rejected_count();
  from = server.received.size();
  server.send(proto::FlagSet{4, {bool_flag(41, true)}}, proto::kChannelProgress);
  at = wait_received<proto::FlagState>(client, server, from);
  CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
        std::vector<proto::FlagEntry>{bool_flag(41, true)});
  CHECK(asked == 1);

  // Our own FLAG_SET echoed (sent before we knew we host) is answered too,
  // without the callback: the table's value where it has one (40 was stated
  // since), else the asked one.
  from = server.received.size();
  server.send(proto::FlagSet{1, {bool_flag(40, false), bool_flag(42, true)}},
              proto::kChannelProgress);
  at = wait_received<proto::FlagState>(client, server, from);
  CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
        std::vector<proto::FlagEntry>{bool_flag(40, true), bool_flag(42, true)});
  CHECK(asked == 1);
  CHECK(client.find_flag(42) == bool_flag(42, true));
  CHECK(client.rejected_count() == rejected);
  client.set_flag_answer({});

  // The host's table goes to the server as FLAG_SNAPSHOT parts.
  from = server.received.size();
  std::vector<proto::FlagEntry> table;
  for (std::uint32_t i = 0; i < proto::kMaxFlagEntries + 1; ++i) table.push_back(s32_flag(i + 1, 1));
  CHECK(client.send_flag_snapshot(table));
  REQUIRE(pump(client, server, [&] { return count_received<proto::FlagSnapshot>(server, from) == 2; }));
  const auto part0 = server.find<proto::FlagSnapshot>(from);
  REQUIRE(part0.has_value());
  const auto& snap = std::get<proto::FlagSnapshot>(server.received[*part0].message);
  CHECK(snap.part == 0);
  CHECK(snap.part_count == 2);
  CHECK(server.received[*part0].channel == proto::kChannelProgress);
}

TEST_CASE("client flags: count deltas go to the host's game, never into the table") {
  auto delta = [](std::uint32_t hash, std::int32_t change) {
    return proto::FlagEntry{hash, 0, proto::FlagKind::count_delta,
                            static_cast<std::uint32_t>(change)};
  };
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());

  SUBCASE("as host") {
    welcome_as(client, server, events, 1, {{2, "Saria"}, {3, "Zelda"}});
    REQUIRE(client.is_host());
    // A delta with a value: the value is answered, the delta is handed to
    // our game in the event, whose watcher sends the total.
    std::size_t from = server.received.size();
    server.send(proto::FlagSet{2, {delta(7, 1), s32_flag(6, 3)}}, proto::kChannelProgress);
    const std::size_t at = wait_received<proto::FlagState>(client, server, from);
    CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
          std::vector<proto::FlagEntry>{s32_flag(6, 3)});
    const std::size_t seen_first = events.all.size();
    REQUIRE(wait_event(client, server, events, EventKind::flag_state));
    const auto& first = events.all[*events.find(EventKind::flag_state, seen_first)];
    CHECK(first.player == 1);
    CHECK(first.flags == std::vector<proto::FlagEntry>{s32_flag(6, 3), delta(7, 1)});
    CHECK_FALSE(client.find_flag(7).has_value());

    // Deltas only: no FLAG_STATE from the client, still an event.
    const int states = events.count(EventKind::flag_state);
    from = server.received.size();
    server.send(proto::FlagSet{3, {delta(7, -1)}}, proto::kChannelProgress);
    const std::size_t seen = events.all.size();
    REQUIRE(pump(client, server, [&] {
      events.drain(client);
      return events.count(EventKind::flag_state) > states;
    }));
    CHECK(events.all[*events.find(EventKind::flag_state, seen)].flags == std::vector<proto::FlagEntry>{delta(7, -1)});
    CHECK_FALSE(server.find<proto::FlagState>(from).has_value());

    // Our own delta echoed back: it is in our game already, not added again.
    server.send(proto::FlagSet{1, {delta(8, 1)}}, proto::kChannelProgress);
    settle(client, server, events, 200);
    CHECK(events.count(EventKind::flag_state) == states + 1);
    CHECK_FALSE(server.find<proto::FlagState>(from).has_value());

    // As host our own count goes out as its total; a delta is dropped.
    CHECK(client.set_flag(delta(9, 1)));
    settle(client, server, events, 200);
    CHECK_FALSE(server.find<proto::FlagState>(from).has_value());
    CHECK_FALSE(server.find<proto::FlagSet>(from).has_value());
  }

  SUBCASE("as player") {
    welcome_as(client, server, events, 2, {{1, "Saria"}, {3, "Zelda"}});
    std::size_t from = server.received.size();
    // Two keys in a call and one more later: each call one FLAG_SET; the
    // unanswered ones add up for a resend.
    CHECK(client.set_flags(std::vector<proto::FlagEntry>{delta(7, 1), delta(7, 1)}));
    CHECK(client.set_flag(delta(7, 1)));
    REQUIRE(pump(client, server, [&] { return count_received<proto::FlagSet>(server, from) == 2; }));
    CHECK(std::get<proto::FlagSet>(server.received[*server.find<proto::FlagSet>(from)].message)
              .flags == std::vector<proto::FlagEntry>{delta(7, 2)});
    REQUIRE(client.pending_flags().size() == 1);
    CHECK(client.pending_flags().begin()->second == delta(7, 3));

    // Host 1 leaves: the sum goes to the new host, once.
    from = server.received.size();
    server.send(proto::Host{3}, proto::kChannelProgress);
    REQUIRE(wait_event(client, server, events, EventKind::host_changed));
    const std::size_t at = wait_received<proto::FlagSet>(client, server, from);
    CHECK(std::get<proto::FlagSet>(server.received[at].message).flags ==
          std::vector<proto::FlagEntry>{delta(7, 3)});

    // We host next: our game holds the keys already; nothing is stated for
    // them and nothing waits.
    from = server.received.size();
    server.send(proto::Host{2}, proto::kChannelProgress);
    REQUIRE(wait_event(client, server, events, EventKind::host_changed));
    settle(client, server, events, 200);
    CHECK_FALSE(server.find<proto::FlagState>(from).has_value());
    CHECK(client.pending_flags().empty());
    CHECK_FALSE(client.find_flag(7).has_value());
  }
}

TEST_CASE("client flags: 64-bit keys are set, stored and answered like named flags") {
  constexpr std::uint64_t kBox = 0x2fa4c1d09b7e6a35ull;
  SUBCASE("as a player") {
    FakeServer server;
    Client client(config_for(server));
    Events events;
    REQUIRE(client.start());
    welcome_as(client, server, events, 2, {{1, "Saria"}});

    std::size_t from = server.received.size();
    CHECK(client.set_flags(std::vector{key64_flag(kBox, true), bool_flag(1, true)}));
    const std::size_t at = wait_received<proto::FlagSet>(client, server, from);
    CHECK(std::get<proto::FlagSet>(server.received[at].message).flags ==
          std::vector{key64_flag(kBox, true), bool_flag(1, true)});
    CHECK(client.pending_flags().contains(proto::FlagKey::key64_flag(kBox)));
    // A 64-bit key is always a bool.
    CHECK_FALSE(client.set_flag({proto::FlagKey::key64_flag(kBox), proto::FlagKind::int32, 1}));

    server.send(proto::FlagState{1, {key64_flag(kBox, true), key64_flag(1, false)}},
                proto::kChannelProgress);
    REQUIRE(wait_event(client, server, events, EventKind::flag_state));
    CHECK(client.find_flag(proto::FlagKey::key64_flag(kBox)) == key64_flag(kBox, true));
    CHECK(client.find_flag(proto::FlagKey::key64_flag(1)) == key64_flag(1, false));
    CHECK_FALSE(client.find_flag(1).has_value());  // the named flag 1 is another
    CHECK_FALSE(client.pending_flags().contains(proto::FlagKey::key64_flag(kBox)));
    CHECK(client.pending_flags().contains(proto::FlagKey{1u, 0}));
  }
  SUBCASE("as host") {
    FakeServer server;
    Client client(config_for(server));
    Events events;
    REQUIRE(client.start());
    welcome_as(client, server, events, 1, {{2, "Saria"}});
    REQUIRE(client.is_host());
    std::size_t from = server.received.size();
    server.send(proto::FlagSet{2, {key64_flag(kBox, true)}}, proto::kChannelProgress);
    std::size_t at = wait_received<proto::FlagState>(client, server, from);
    CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
          std::vector{key64_flag(kBox, true)});

    // An answer for another key keeps the asked key; one that is not a bool
    // is refused and the table's value goes back.
    client.set_flag_answer([&](std::uint8_t, const proto::FlagEntry& asked)
                               -> std::optional<proto::FlagEntry> {
      if (asked.key.key == kBox) return proto::FlagEntry{asked.key, proto::FlagKind::int32, 0};
      return key64_flag(999, false);
    });
    from = server.received.size();
    server.send(proto::FlagSet{2, {key64_flag(kBox, false), key64_flag(7, true)}},
                proto::kChannelProgress);
    at = wait_received<proto::FlagState>(client, server, from);
    CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
          std::vector{key64_flag(kBox, true), key64_flag(7, false)});
    CHECK(client.find_flag(proto::FlagKey::key64_flag(7)) == key64_flag(7, false));
  }
}

TEST_CASE("client flags: who hosts, and a non-host refuses FLAG_SET") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  CHECK_FALSE(client.is_host());
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK_FALSE(client.is_host());
  CHECK_FALSE(client.send_flag_snapshot({}));

  const std::size_t from = server.received.size();
  const std::size_t rejected = client.rejected_count();
  server.send(proto::FlagSet{1, {bool_flag(1, true)}}, proto::kChannelProgress);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == rejected + 1; }));

  // The host leaves. Leaving alone does not make us host: HOST does. The
  // old host's last FLAG_STATE may come after its PLAYER_LEFT (another
  // channel) and is still applied.
  server.send(proto::PlayerLeft{1, proto::LeaveReason::left}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::player_left));
  CHECK_FALSE(client.is_host());
  CHECK(client.host_number() == 1);
  server.send(proto::FlagState{1, {bool_flag(9, true)}}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_state));
  CHECK(client.find_flag(9) == bool_flag(9, true));

  const std::size_t before = events.all.size();
  server.send(proto::Host{2}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::host_changed));
  const std::size_t changed = *events.find(EventKind::host_changed, before);
  CHECK(events.all[changed].player == 2);
  CHECK(events.all[changed - 1].kind == EventKind::message);
  CHECK(client.is_host());
  CHECK(client.host_number() == 2);
  // A FLAG_STATE now carries the new host's number; the old one's is refused.
  const std::size_t rejected2 = client.rejected_count();
  server.send(proto::FlagState{1, {bool_flag(9, false)}}, proto::kChannelProgress);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == rejected2 + 1; }));
  CHECK(client.find_flag(9) == bool_flag(9, true));

  // The same host again gives no event.
  const int changes = events.count(EventKind::host_changed);
  server.send(proto::Host{2}, proto::kChannelProgress);
  settle(client, server, events);
  CHECK(events.count(EventKind::host_changed) == changes);

  CHECK(client.send_flag_snapshot({}));
  const std::size_t at = wait_received<proto::FlagSnapshot>(client, server, from);
  CHECK(std::get<proto::FlagSnapshot>(server.received[at].message) ==
        proto::FlagSnapshot{0, 1, {}});
  CHECK_FALSE(server.find<proto::FlagState>(from).has_value());
}

TEST_CASE("client flags: a joiner who is the lowest number does not host by that") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 1, {{2, "Saria"}}, 2);
  CHECK_FALSE(client.is_host());
  CHECK(events.count(EventKind::host_changed) == 1);
  const std::size_t from = server.received.size();
  CHECK(client.set_flag(bool_flag(1, true)));
  wait_received<proto::FlagSet>(client, server, from);
}

TEST_CASE("client flags: unanswered FLAG_SETs go again when the host changes") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}, {3, "Zelda"}});
  CHECK(client.pending_flags().empty());

  CHECK(client.set_flags(std::vector<proto::FlagEntry>{bool_flag(1, true), bool_flag(2, true)}));
  CHECK(client.set_flag(bool_flag(3, true)));
  CHECK(client.set_flag(bool_flag(1, false)));  // a newer value for 1
  CHECK(client.pending_flags().size() == 3);
  // The host answers 2 (with another value): it is no longer pending.
  server.send(proto::FlagState{1, {bool_flag(2, false)}}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_state));
  CHECK(client.pending_flags().size() == 2);

  // Host 1 leaves with our sets in flight: they go to the new host.
  std::size_t from = server.received.size();
  server.send(proto::PlayerLeft{1, proto::LeaveReason::timed_out}, proto::kChannelSession);
  server.send(proto::Host{3}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::host_changed));
  std::size_t at = wait_received<proto::FlagSet>(client, server, from);
  CHECK(std::get<proto::FlagSet>(server.received[at].message).flags ==
        std::vector<proto::FlagEntry>{bool_flag(1, false), bool_flag(3, true)});
  CHECK(count_received<proto::FlagSet>(server, from) == 1);
  CHECK(client.pending_flags().size() == 2);  // still waiting for host 3

  // A snapshot also covers them.
  server.send(proto::FlagSnapshot{0, 1, {bool_flag(3, true)}}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_snapshot));
  CHECK(client.pending_flags().size() == 1);

  // Host 3 leaves and we host: what is still pending is ours to state.
  from = server.received.size();
  server.send(proto::Host{2}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::host_changed));
  at = wait_received<proto::FlagState>(client, server, from);
  CHECK(std::get<proto::FlagState>(server.received[at].message).flags ==
        std::vector<proto::FlagEntry>{bool_flag(1, false)});
  CHECK_FALSE(server.find<proto::FlagSet>(from).has_value());
  CHECK(client.pending_flags().empty());
  CHECK(client.find_flag(1) == bool_flag(1, false));

  // A drop forgets them, and who hosts.
  CHECK(client.set_flag(bool_flag(5, true)));  // as host: not pending
  CHECK(client.pending_flags().empty());
  server.send(proto::Host{3}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::host_changed));
  CHECK(client.set_flag(bool_flag(6, true)));
  CHECK(client.pending_flags().size() == 1);
  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected));
  CHECK(client.pending_flags().empty());
  CHECK(client.host_number() == 0);
}

TEST_CASE("client flags: as host, the server's value on a clash is applied") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 1, {{2, "Saria"}});
  REQUIRE(client.is_host());
  CHECK(client.set_flag(bool_flag(7, false)));
  CHECK(client.send_flag_snapshot(std::vector<proto::FlagEntry>{bool_flag(7, false)}));
  // The server keeps its value and sends it back with our own number.
  const std::size_t rejected = client.rejected_count();
  server.send(proto::FlagState{1, {bool_flag(7, true)}}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_state));
  const Event& e = events.all[*events.find(EventKind::flag_state)];
  CHECK(e.player == 1);
  CHECK(e.flags == std::vector<proto::FlagEntry>{bool_flag(7, true)});
  CHECK(client.find_flag(7) == bool_flag(7, true));
  CHECK(client.rejected_count() == rejected);
}

TEST_CASE("client flags: HOST and the snapshot may come before WELCOME") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.max_early_messages = 3;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>().has_value(); }));

  // Channel 1 overtakes channel 0: HOST and a two-part snapshot first.
  server.send(proto::Host{1}, proto::kChannelProgress);
  server.send(proto::FlagSnapshot{0, 2, {bool_flag(1, true)}}, proto::kChannelProgress);
  server.send(proto::FlagSnapshot{1, 2, {bool_flag(2, true)}}, proto::kChannelProgress);
  // Past max_early_messages: dropped and counted.
  server.send(proto::FlagState{1, {bool_flag(3, true)}}, proto::kChannelProgress);
  // Not the progress channel: refused as before.
  server.send(proto::Chat{1, "hi"}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == 2; }));
  settle(client, server, events);
  CHECK(client.state() == State::handshaking);
  CHECK(events.count(EventKind::host_changed) == 0);

  proto::Welcome w;
  w.player_number = 2;
  w.server_time_us = steady_us();
  w.tick_rate = 30;
  w.players = {{1, "Saria"}};
  server.send(w, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::flag_snapshot));
  CHECK(client.rejected_count() == 2);
  CHECK(client.host_number() == 1);
  CHECK(client.synced());
  CHECK(client.flags().size() == 2);
  // In order: welcomed, then HOST, then the snapshot.
  const auto welcomed = events.find(EventKind::welcomed);
  const auto host = events.find(EventKind::host_changed);
  const auto snapshot = events.find(EventKind::flag_snapshot);
  REQUIRE(welcomed.has_value());
  REQUIRE(host.has_value());
  CHECK(*welcomed < *host);
  CHECK(*host < *snapshot);

  // Kept messages do not outlive the attempt they came on.
  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected));
  const std::size_t from = server.received.size();
  REQUIRE(pump(client, server, [&] { return server.find<proto::Hello>(from).has_value(); }));
  server.send(proto::Host{1}, proto::kChannelProgress);
  settle(client, server, events);
  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::connect_failed));
  const int changes = events.count(EventKind::host_changed);
  welcome_as(client, server, events, 2, {{1, "Saria"}, {3, "Zelda"}}, 3);
  settle(client, server, events);
  CHECK(client.host_number() == 3);
  CHECK(events.count(EventKind::host_changed) == changes + 1);  // not 1, then 3
  CHECK_FALSE(client.synced());
}

TEST_CASE("client join: request, accept, refuse, not there and stale answers") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  CHECK_FALSE(client.request_join(1).has_value());  // not connected
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}, {3, "Zelda"}});

  CHECK_FALSE(client.request_join(2).has_value());  // us
  CHECK_FALSE(client.request_join(4).has_value());  // nobody
  CHECK_FALSE(client.join_request_id().has_value());

  // Request and accept.
  std::size_t from = server.received.size();
  const auto id = client.request_join(1);
  REQUIRE(id.has_value());
  CHECK(client.join_request_id() == id);
  const std::size_t at = wait_received<proto::JoinPartner>(client, server, from);
  CHECK(server.received[at].channel == proto::kChannelSession);
  const auto& request = std::get<proto::JoinPartner>(server.received[at].message);
  CHECK(request.player_number == proto::kPlayerSelf);
  CHECK(request.other_player == 1);
  CHECK(request.request_id == *id);
  CHECK(request.kind == proto::JoinKind::request);

  const proto::JoinTarget where{"spot04", {1, 2, 3}, {0, 0, 0, 1}};
  const proto::JoinPartner accept{1, 2, *id, proto::JoinKind::accept, where, {}};
  server.send(accept, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_accepted));
  const Event accepted = events.all[*events.find(EventKind::join_accepted)];
  CHECK(accepted.player == 1);
  CHECK(accepted.request_id == *id);
  CHECK(accepted.join_target == where);
  CHECK_FALSE(client.join_request_id().has_value());

  // The same answer again is stale now; so is one from another player than
  // the one asked.
  const auto id2 = client.request_join(3);
  REQUIRE(id2.has_value());
  CHECK(*id2 != *id);
  server.send(accept, proto::kChannelSession);
  server.send(proto::JoinPartner{1, 2, *id2, proto::JoinKind::accept, where, {}},
              proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.stale_join_answers() == 2; }));

  // A refusal keeps its reason, an unknown one raw.
  server.send(proto::JoinPartner{3, 2, *id2, proto::JoinKind::refuse, std::nullopt,
                                 static_cast<proto::JoinRefuseReason>(9)},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_refused));
  const Event refused = events.all[*events.find(EventKind::join_refused)];
  CHECK(refused.player == 3);
  CHECK(refused.request_id == *id2);
  CHECK(static_cast<int>(refused.join_reason) == 9);
  CHECK(events.count(EventKind::join_accepted) == 1);

  // A newer request makes the older one stale.
  const auto old_id = client.request_join(1);
  const auto id3 = client.request_join(3);
  REQUIRE(old_id.has_value());
  REQUIRE(id3.has_value());
  server.send(proto::JoinPartner{1, 2, *old_id, proto::JoinKind::accept, where, {}},
              proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.stale_join_answers() == 3; }));

  // The server's own refusal for a player who has just left.
  server.send(proto::PlayerLeft{3, proto::LeaveReason::left}, proto::kChannelSession);
  server.send(proto::JoinPartner{3, 2, *id3, proto::JoinKind::refuse, std::nullopt,
                                 proto::JoinRefuseReason::not_there},
              proto::kChannelSession);
  const std::size_t before = events.all.size();
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return events.find(EventKind::join_refused, before).has_value();
  }));
  const Event gone = events.all[*events.find(EventKind::join_refused, before)];
  CHECK(gone.player == 3);
  CHECK(gone.join_reason == proto::JoinRefuseReason::not_there);
  CHECK(client.stale_join_answers() == 3);
}

TEST_CASE("client join: a request times out on its own") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.join_timeout_ms = 200;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK(ClientConfig{}.join_timeout_ms == 5000);

  const auto started = std::chrono::steady_clock::now();
  const auto id = client.request_join(1);
  REQUIRE(id.has_value());
  REQUIRE(wait_event(client, server, events, EventKind::join_timed_out));
  CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(200));
  const Event timed_out = events.all[*events.find(EventKind::join_timed_out)];
  CHECK(timed_out.player == 1);
  CHECK(timed_out.request_id == *id);
  CHECK_FALSE(client.join_request_id().has_value());

  // A late answer is stale.
  server.send(proto::JoinPartner{1, 2, *id, proto::JoinKind::refuse, std::nullopt,
                                 proto::JoinRefuseReason::declined},
              proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.stale_join_answers() == 1; }));
  CHECK(events.count(EventKind::join_refused) == 0);
}

TEST_CASE("client join: a request from another player and its answer") {
  FakeServer server;
  ClientConfig config = config_for(server);
  config.join_timeout_ms = 300;
  Client client(config);
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}, {3, "Zelda"}});

  // Refused: from nobody, or for someone else.
  const std::size_t rejected = client.rejected_count();
  server.send(proto::JoinPartner{4, 2, 1, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  server.send(proto::JoinPartner{3, 1, 1, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == rejected + 2; }));

  server.send(proto::JoinPartner{1, 2, 77, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_request));
  const Event asked = events.all[*events.find(EventKind::join_request)];
  CHECK(asked.player == 1);
  CHECK(asked.request_id == 77);

  const proto::JoinTarget where{"spot00", {4, 5, 6}, {0, 0, 0, 1}};
  CHECK_FALSE(client.accept_join(1, 78, where));  // not that id
  CHECK_FALSE(client.accept_join(3, 77, where));  // not that player
  CHECK_FALSE(client.accept_join(1, 77, {"", {}, {0, 0, 0, 1}}));  // no scene
  std::size_t from = server.received.size();
  CHECK(client.accept_join(1, 77, where));
  const std::size_t at = wait_received<proto::JoinPartner>(client, server, from);
  CHECK(std::get<proto::JoinPartner>(server.received[at].message) ==
        proto::JoinPartner{0, 1, 77, proto::JoinKind::accept, where,
                           proto::JoinPartner{}.reason});  // not on the wire
  CHECK_FALSE(client.accept_join(1, 77, where));  // answered already

  // A refusal.
  server.send(proto::JoinPartner{3, 2, 5, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_request));
  from = server.received.size();
  CHECK(client.refuse_join(3, 5, proto::JoinRefuseReason::busy));
  const std::size_t refused_at = wait_received<proto::JoinPartner>(client, server, from);
  CHECK(std::get<proto::JoinPartner>(server.received[refused_at].message) ==
        proto::JoinPartner{0, 3, 5, proto::JoinKind::refuse, std::nullopt,
                           proto::JoinRefuseReason::busy});

  // A request left past the timeout can no longer be answered.
  server.send(proto::JoinPartner{3, 2, 6, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_request));
  settle(client, server, events, 400);
  CHECK_FALSE(client.refuse_join(3, 6, proto::JoinRefuseReason::declined));

  // Nor one from a player who then leaves.
  server.send(proto::JoinPartner{3, 2, 7, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_request));
  server.send(proto::PlayerLeft{3, proto::LeaveReason::left}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::player_left));
  CHECK_FALSE(client.refuse_join(3, 7, proto::JoinRefuseReason::declined));
}

TEST_CASE("client flags and join: a reconnect clears the shared state") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});

  server.send(proto::FlagSnapshot{0, 1, {bool_flag(1, true)}}, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_snapshot));
  server.send(proto::JoinPartner{1, 2, 9, proto::JoinKind::request, std::nullopt, {}},
              proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::join_request));
  // Half a snapshot, and our own open request.
  server.send(proto::FlagSnapshot{0, 2, {bool_flag(2, true)}}, proto::kChannelProgress);
  const auto id = client.request_join(1);
  REQUIRE(id.has_value());
  settle(client, server, events);
  CHECK(client.synced());
  CHECK(client.flags().size() == 1);

  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected));
  CHECK_FALSE(client.synced());
  CHECK(client.flags().empty());
  CHECK_FALSE(client.join_request_id().has_value());

  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK_FALSE(client.synced());
  CHECK_FALSE(client.refuse_join(1, 9, proto::JoinRefuseReason::declined));
  // The old answer is stale; the old half snapshot is not finished by part 1.
  const std::size_t rejected = client.rejected_count();
  server.send(proto::JoinPartner{1, 2, *id, proto::JoinKind::refuse, std::nullopt,
                                 proto::JoinRefuseReason::busy},
              proto::kChannelSession);
  server.send(proto::FlagSnapshot{1, 2, {bool_flag(3, true)}}, proto::kChannelProgress);
  REQUIRE(pump(client, server, [&] {
    return client.stale_join_answers() == 1 && client.rejected_count() == rejected + 1;
  }));
  CHECK_FALSE(client.synced());
  CHECK(client.flags().empty());

  client.stop();
  CHECK_FALSE(client.set_flag(bool_flag(1, true)));
  CHECK_FALSE(client.request_join(1).has_value());
}

TEST_CASE("client: the flag and join event kinds are named") {
  CHECK(to_string(EventKind::flag_state) == "flag state");
  CHECK(to_string(EventKind::flag_snapshot) == "flag snapshot");
  CHECK(to_string(EventKind::join_request) == "join request");
  CHECK(to_string(EventKind::join_accepted) == "join accepted");
  CHECK(to_string(EventKind::join_refused) == "join refused");
  CHECK(to_string(EventKind::join_timed_out) == "join timed out");
  CHECK(to_string(EventKind::host_changed) == "host changed");
}

TEST_CASE("client: equipment is kept per player and ours goes to each newcomer") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_client(client, server);  // we are 2, Saria is 1
  REQUIRE(wait_event(client, server, events, EventKind::welcomed));

  // Theirs: a later message keeps what it leaves out.
  proto::Equipment saria;
  saria.weapon = "Weapon_Sword_Kokiri";
  saria.drawn = 1;
  server.send(proto::PlayerEquip{1, saria}, proto::kChannelSession);
  REQUIRE(wait_event(client, server, events, EventKind::player_equip));
  const Event first = events.all[*events.find(EventKind::player_equip)];
  CHECK(first.player == 1);
  CHECK(first.equipment == saria);
  const std::size_t before = events.all.size();
  proto::Equipment sheathed;
  sheathed.drawn = 0;
  server.send(proto::PlayerEquip{1, sheathed}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] {
    events.drain(client);
    return events.find(EventKind::player_equip, before).has_value();
  }));
  REQUIRE(client.find_player(1)->equipment);
  CHECK(client.find_player(1)->equipment->weapon == "Weapon_Sword_Kokiri");
  CHECK(client.find_player(1)->equipment->drawn == 0);

  // Refused: equipment for nobody, or for us.
  const std::size_t rejected = client.rejected_count();
  server.send(proto::PlayerEquip{4, saria}, proto::kChannelSession);
  server.send(proto::PlayerEquip{2, saria}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.rejected_count() == rejected + 2; }));

  // Ours: kept when sent, and sent again when someone joins.
  proto::Equipment link;
  link.armor = "Armor_Adult_Zora";
  const std::size_t from = server.received.size();
  REQUIRE(client.send(proto::PlayerEquip{0, link}));
  CHECK(client.find_player(2)->equipment == link);
  auto count_equips = [&] {
    std::size_t n = 0;
    for (std::size_t i = from; i < server.received.size(); ++i) {
      if (const auto* e = std::get_if<proto::PlayerEquip>(&server.received[i].message)) {
        CHECK(e->equipment == link);
        CHECK(server.received[i].channel == proto::kChannelSession);
        ++n;
      }
    }
    return n;
  };
  REQUIRE(pump(client, server, [&] { return count_equips() == 1; }));
  server.send(proto::PlayerJoined{3, "Zelda"}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return count_equips() == 2; }));
  CHECK(to_string(EventKind::player_equip) == "player equip");
  client.stop();
}

TEST_CASE("client: datagrams are VPN-safe and a large snapshot still arrives whole") {
  // Issue 143: over Hamachi (about 1404 bytes a packet) ENet's default 1392-byte
  // datagrams make 1420-byte packets; the game was cut every 15-18 s.
  FakeServer server;  // ENet's default size: the smaller side wins
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK(client.link().mtu == proto::kDatagramBytes);
  REQUIRE(server.last_peer != nullptr);
  CHECK(server.last_peer->mtu == proto::kDatagramBytes);
  CHECK(proto::kDatagramBytes + 28 <= 1280);  // IP and UDP headers: fits Tailscale and Hamachi

  // A snapshot many datagrams long comes through in full.
  proto::FlagSnapshot big{0, 1, {}};
  for (std::uint32_t i = 0; i < 600; ++i) {
    big.flags.push_back(s32_flag(i + 1, static_cast<std::int32_t>(i)));
  }
  const auto bytes = proto::encode(big);
  REQUIRE(bytes.has_value());
  CHECK(bytes->size() > 4 * proto::kDatagramBytes);
  server.send(big, proto::kChannelProgress);
  REQUIRE(wait_event(client, server, events, EventKind::flag_snapshot));
  CHECK(events.all[*events.find(EventKind::flag_snapshot)].flags == big.flags);
  CHECK(client.link().largest_sent > 0);  // HELLO at least
}

TEST_CASE("client: link() counts the time since the server's last packet") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  CHECK(client.link().quiet_ms < 200);

  // The server says nothing (its PINGs are lost): the quiet grows.
  pump(client, server, [] { return false; }, 600);
  CHECK(client.link().quiet_ms >= 500);

  // A packet comes: back near 0.
  server.send(proto::Ping{1}, proto::kChannelSession);
  REQUIRE(pump(client, server, [&] { return client.link().quiet_ms < 100; }));
}

TEST_CASE("client: a lost connection reports how the link looked") {
  FakeServer server;
  Client client(config_for(server));
  Events events;
  REQUIRE(client.start());
  welcome_as(client, server, events, 2, {{1, "Saria"}});
  pump(client, server, [] { return false; }, 300);
  server.drop();
  REQUIRE(wait_event(client, server, events, EventKind::disconnected));
  const Event& lost = events.all[*events.find(EventKind::disconnected)];
  CHECK(lost.link.mtu == proto::kDatagramBytes);
  CHECK(lost.link.largest_sent > 0);
  CHECK(lost.link.quiet_ms < 2000);
}
