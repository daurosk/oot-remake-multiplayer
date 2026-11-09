// The relay server's rules, driven without a network.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "session.hpp"

using namespace coop::protocol;
using namespace coop::server;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr const char* kGameBuild = "e0f3ccfaf50065ae";

Bytes bytes_of(const Message& m) {
  Bytes out;
  REQUIRE(encode(m, out) == Error::none);
  return out;
}

Hello hello_from(const std::string& nickname) {
  Hello h;
  h.build_id = "test 0.0";
  h.game_build_id = kGameBuild;
  h.game_version = "1.1.1";
  h.nickname = nickname;
  return h;
}

struct Sent {
  ConnectionId to;
  std::uint8_t channel;
  Message message;
};

// Runs the session and splits what it asked for into decoded sends and
// disconnects.
struct Harness {
  Session session;
  std::vector<Sent> sent;
  std::vector<ConnectionId> dropped;
  std::uint64_t now = 1'000'000;

  explicit Harness(SessionConfig config = {}) : session(std::move(config)) {}

  void collect() {
    for (Action& a : session.take_actions()) {
      if (auto* s = std::get_if<SendAction>(&a)) {
        Message m;
        REQUIRE(decode(s->bytes, m) == Error::none);
        // What the server sends must pass a client's own check.
        CHECK(check_received(m, Sender::server, s->channel) == Error::none);
        sent.push_back({s->to, s->channel, m});
      } else {
        dropped.push_back(std::get<DisconnectAction>(a).who);
      }
    }
  }

  void connect(ConnectionId id) {
    session.connected(id, now);
    collect();
  }

  void receive(ConnectionId id, const Message& m, std::uint8_t channel) {
    session.received(id, channel, bytes_of(m), now);
    collect();
  }

  void receive_raw(ConnectionId id, const Bytes& b, std::uint8_t channel) {
    session.received(id, channel, b, now);
    collect();
  }

  // Connects and joins a player; returns the WELCOME.
  Welcome join(ConnectionId id, const std::string& nickname) {
    connect(id);
    receive(id, hello_from(nickname), kChannelSession);
    auto msgs = to(id);
    REQUIRE(!msgs.empty());
    const auto* w = std::get_if<Welcome>(&msgs.back().message);
    REQUIRE(w != nullptr);
    return *w;
  }

  std::vector<Sent> to(ConnectionId id) const {
    std::vector<Sent> out;
    for (const Sent& s : sent) {
      if (s.to == id) out.push_back(s);
    }
    return out;
  }

  void clear() {
    sent.clear();
    dropped.clear();
  }
};

template <typename T>
std::optional<T> only_message_to(const Harness& h, ConnectionId id) {
  auto msgs = h.to(id);
  REQUIRE(msgs.size() == 1);
  if (const auto* m = std::get_if<T>(&msgs[0].message)) return *m;
  return std::nullopt;
}

}  // namespace

TEST_CASE("the first player is welcomed as player 1 with nobody else in") {
  Harness h;
  Welcome w = h.join(1, "Link");
  CHECK(w.player_number == 1);
  CHECK(w.players.empty());
  CHECK(w.tick_rate == 60);
  CHECK(w.server_time_us == h.now);
  CHECK(h.session.player_count() == 1);
  CHECK(h.dropped.empty());
}

TEST_CASE("a second player sees the first, and the first is told of the second") {
  Harness h;
  h.join(1, "Link");
  h.clear();
  Welcome w = h.join(2, "Zelda");
  CHECK(w.player_number == 2);
  REQUIRE(w.players.size() == 1);
  CHECK(w.players[0] == PlayerInfo{1, "Link"});

  const auto joined = only_message_to<PlayerJoined>(h, 1);
  REQUIRE(joined.has_value());
  CHECK(*joined == PlayerJoined{2, "Zelda"});
  CHECK(h.to(1)[0].channel == kChannelSession);
}

TEST_CASE("a fifth player is refused as server full and dropped") {
  Harness h;
  for (ConnectionId id = 1; id <= 4; ++id) h.join(id, "p" + std::to_string(id));
  h.clear();

  h.connect(5);
  h.receive(5, hello_from("p5"), kChannelSession);
  const auto refused = only_message_to<Refused>(h, 5);
  REQUIRE(refused.has_value());
  CHECK(refused->reason == RefuseReason::server_full);
  REQUIRE(h.dropped.size() == 1);
  CHECK(h.dropped[0] == 5);
  CHECK(h.session.player_count() == 4);
  // Nobody else heard of it.
  for (ConnectionId id = 1; id <= 4; ++id) CHECK(h.to(id).empty());

  // Anything more from it is ignored and counted.
  h.receive(5, Chat{0, "let me in"}, kChannelSession);
  CHECK(h.sent.size() == 1);
  CHECK(h.session.stats().after_refused == 1);
}

TEST_CASE("a wrong protocol version is refused, even when the rest cannot be read") {
  Harness h;
  h.connect(1);
  // A HELLO of version 7 whose body is just the version: unreadable as v0.
  Bytes b = {1, 0, 2, 0, 7, 0};
  h.receive_raw(1, b, kChannelSession);
  const auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused.has_value());
  CHECK(refused->reason == RefuseReason::wrong_protocol_version);
  CHECK(h.dropped == std::vector<ConnectionId>{1});
  CHECK(h.session.player_count() == 0);
}

TEST_CASE("a game build other than the session's is refused") {
  SUBCASE("set on the command line") {
    SessionConfig config;
    config.game_build_id = "something else";
    Harness h(config);
    h.connect(1);
    h.receive(1, hello_from("Link"), kChannelSession);
    const auto refused = only_message_to<Refused>(h, 1);
    REQUIRE(refused.has_value());
    CHECK(refused->reason == RefuseReason::wrong_game_build);
  }
  SUBCASE("decided by the first player") {
    Harness h;
    h.join(1, "Link");
    h.clear();
    h.connect(2);
    Hello other = hello_from("Zelda");
    other.game_build_id = "0000000000000000";
    h.receive(2, other, kChannelSession);
    const auto refused = only_message_to<Refused>(h, 2);
    REQUIRE(refused.has_value());
    CHECK(refused->reason == RefuseReason::wrong_game_build);
  }
}

TEST_CASE("a nickname in use is refused as name taken, a blank one as bad name") {
  Harness h;
  h.join(1, "Link");
  h.clear();
  const std::pair<const char*, RefuseReason> cases[] = {
      {"Link", RefuseReason::name_taken},
      {"LINK", RefuseReason::name_taken},
      {" ", RefuseReason::bad_name},
      {"   ", RefuseReason::bad_name}};
  ConnectionId id = 10;
  for (const auto& [name, reason] : cases) {
    CAPTURE(name);
    ++id;
    h.connect(id);
    h.receive(id, hello_from(name), kChannelSession);
    const auto refused = only_message_to<Refused>(h, id);
    REQUIRE(refused.has_value());
    CHECK(refused->reason == reason);
    h.clear();
  }
  CHECK(h.session.player_count() == 1);
}

TEST_CASE("a HELLO with a bad nickname is dropped and counted, never welcomed") {
  Harness h;
  h.connect(1);
  // Encode a valid HELLO, then turn its nickname's 'L' into a control
  // character the codec refuses.
  Bytes b = bytes_of(hello_from("Link"));
  b[b.size() - 4] = 0x01;
  h.receive_raw(1, b, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().errors(Error::bad_utf8) +
            h.session.stats().errors(Error::bad_value) ==
        1);
  CHECK(h.session.player_count() == 0);
}

TEST_CASE("a player who leaves is reported to the others and frees the number") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  h.session.disconnected(2, h.now);
  h.collect();
  for (ConnectionId id : {1u, 3u}) {
    const auto left = only_message_to<PlayerLeft>(h, id);
    REQUIRE(left.has_value());
    CHECK(*left == PlayerLeft{2, LeaveReason::left});
  }
  CHECK(h.session.player_count() == 2);

  h.clear();
  Welcome w = h.join(4, "Darunia");
  CHECK(w.player_number == 2);
  CHECK(w.players.size() == 2);
}

TEST_CASE("relayed messages carry the sender's real number, on the channel they came in on") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  PlayerSample sample;
  sample.time = {123, 7};
  sample.position = Vec3{1.0f, 2.0f, 3.0f};
  h.receive(2, sample, kChannelPlayerSamples);
  h.receive(2, Chat{0, "hello"}, kChannelSession);
  h.receive(2, PlayerStatus{0, status::paused}, kChannelSession);

  CHECK(h.to(2).empty());  // never echoed to the sender
  for (ConnectionId id : {1u, 3u}) {
    auto msgs = h.to(id);
    REQUIRE(msgs.size() == 3);
    CHECK(msgs[0].channel == kChannelPlayerSamples);
    const auto* s = std::get_if<PlayerSample>(&msgs[0].message);
    REQUIRE(s != nullptr);
    CHECK(s->player_number == 2);
    CHECK(s->time == sample.time);
    CHECK(s->position == sample.position);
    CHECK(msgs[1].channel == kChannelSession);
    CHECK(std::get<Chat>(msgs[1].message) == Chat{2, "hello"});
    CHECK(std::get<PlayerStatus>(msgs[2].message) == PlayerStatus{2, status::paused});
  }
  CHECK(h.session.stats().relayed == 3);
}

TEST_CASE("TEST_PAYLOAD is relayed unchanged on channel 0 or 3") {
  Harness h;
  h.join(1, "a");
  h.join(2, "b");
  h.clear();
  TestPayload p{42, {9, 9, 9}};
  h.receive(1, p, kChannelSession);
  h.receive(1, p, kChannelPlayerSamples);
  auto msgs = h.to(2);
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0].channel == kChannelSession);
  CHECK(msgs[1].channel == kChannelPlayerSamples);
  CHECK(std::get<TestPayload>(msgs[1].message) == p);
}

TEST_CASE("a spoofed sender, a wrong channel or a server-only message is refused") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();

  h.receive(1, Chat{2, "I am Zelda"}, kChannelSession);  // claims a number
  h.receive(1, PlayerStatus{0, 0}, kChannelPlayerSamples);  // wrong channel
  h.receive(1, PlayerJoined{3, "ghost"}, kChannelSession);  // server only
  h.receive(1, PlayerLeft{2, LeaveReason::kicked}, kChannelSession);

  CHECK(h.sent.empty());
  const Stats& st = h.session.stats();
  CHECK(st.errors(Error::wrong_sender) == 3);
  CHECK(st.errors(Error::wrong_channel) == 1);
  CHECK(st.relayed == 0);
  CHECK(h.session.player_count() == 2);
}

TEST_CASE("garbage, unknown types and a second HELLO are dropped and counted") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();

  h.receive_raw(1, {}, kChannelSession);
  h.receive_raw(1, {0xff}, kChannelSession);
  h.receive_raw(1, {200, 0, 0, 0}, kChannelSession);  // unknown type 200
  h.receive(1, hello_from("Link again"), kChannelSession);

  CHECK(h.sent.empty());
  const Stats& st = h.session.stats();
  CHECK(st.errors(Error::truncated) == 2);
  CHECK(st.errors(Error::unknown_type) == 1);
  CHECK(st.repeated_hello == 1);
  CHECK(st.dropped() == 4);
}

TEST_CASE("nothing but HELLO is accepted before WELCOME") {
  Harness h;
  h.join(1, "Link");
  h.clear();
  h.connect(2);
  h.receive(2, Chat{0, "hi"}, kChannelSession);
  h.receive(2, Ping{5}, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().before_welcome == 2);
}

TEST_CASE("PING is answered with PONG echoing its time") {
  Harness h;
  h.join(1, "Link");
  h.clear();
  h.now += 500;
  h.receive(1, Ping{777}, kChannelSession);
  const auto pong = only_message_to<Pong>(h, 1);
  REQUIRE(pong.has_value());
  CHECK(pong->ping_sender_time_us == 777);
  CHECK(pong->receiver_time_us == h.now);
}

TEST_CASE("the server pings each player every 250 ms") {
  Harness h;
  h.join(1, "Link");
  h.clear();
  h.now += 200'000;
  h.session.tick(h.now);
  h.collect();
  CHECK(h.sent.empty());
  h.now += 50'000;
  h.session.tick(h.now);
  h.collect();
  const auto ping = only_message_to<Ping>(h, 1);
  REQUIRE(ping.has_value());
  CHECK(ping->sender_time_us == h.now);
}

TEST_CASE("no HELLO within 10 seconds drops the connection") {
  Harness h;
  h.connect(1);
  h.now += 9'999'999;
  h.session.tick(h.now);
  h.collect();
  CHECK(h.dropped.empty());
  h.now += 1;
  h.session.tick(h.now);
  h.collect();
  CHECK(h.dropped == std::vector<ConnectionId>{1});
  CHECK(h.sent.empty());
}

TEST_CASE("a refused connection that keeps sending is still dropped after the grace time") {
  Harness h;
  h.connect(1);
  Hello wrong = hello_from("Link");
  wrong.protocol_version = kProtocolVersion + 1;
  h.receive(1, wrong, kChannelSession);
  REQUIRE(h.dropped == std::vector<ConnectionId>{1});
  h.clear();
  // The peer never lets the transport close it and keeps talking.
  for (int i = 0; i < 100; ++i) {
    h.now += 100'000;
    h.receive(1, Ping{h.now}, kChannelSession);
    h.session.tick(h.now);
    h.collect();
  }
  CHECK(h.dropped == std::vector<ConnectionId>{1});
  CHECK(h.sent.empty());
  CHECK(h.session.stats().after_refused == 100);
  // Gone from the session: what it sends now is not even counted.
  h.receive(1, Ping{h.now}, kChannelSession);
  CHECK(h.session.stats().after_refused == 100);
}

TEST_CASE("a player flooding reliable messages is held to the rate, after a burst") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  // 60 a second with bursts of 120: 200 at once lets 120 through.
  for (int i = 0; i < 200; ++i) h.receive(1, Chat{0, "spam"}, kChannelSession);
  CHECK(h.to(2).size() == 120);
  CHECK(h.session.stats().rate_limited == 80);
  // Half a second later, 30 more.
  h.clear();
  h.now += 500'000;
  for (int i = 0; i < 100; ++i) h.receive(1, Chat{0, "spam"}, kChannelSession);
  CHECK(h.to(2).size() == 30);
  // PINGs share the budget, so a flood of them gets no flood of PONGs.
  h.clear();
  h.receive(1, Ping{1}, kChannelSession);
  CHECK(h.to(1).empty());
  // The other player and the unreliable channel are not affected.
  h.receive(2, Chat{0, "hi"}, kChannelSession);
  h.receive(1, TestPayload{1, {}}, kChannelPlayerSamples);
  CHECK(h.to(1).size() == 1);
  CHECK(h.to(2).size() == 1);
}

TEST_CASE("a player flooding samples is held to the unreliable rate") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  PlayerSample sample;
  sample.time = {1, 1};
  for (int i = 0; i < 1000; ++i) h.receive(1, sample, kChannelPlayerSamples);
  CHECK(h.to(2).size() == 480);
  // A player at the protocol's own rate, 60 samples a second, is never cut.
  h.clear();
  h.now += 10'000'000;
  for (int i = 0; i < 600; ++i) {
    h.now += 16'667;
    h.receive(1, sample, kChannelPlayerSamples);
  }
  CHECK(h.to(2).size() == 600);
}

TEST_CASE("a player silent for 10 seconds is dropped as timed out") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  // Player 1 keeps talking; player 2 goes quiet.
  for (int i = 0; i < 40; ++i) {
    h.now += 250'000;
    h.receive(1, Ping{h.now}, kChannelSession);
    h.session.tick(h.now);
    h.collect();
  }
  CHECK(h.dropped == std::vector<ConnectionId>{2});
  CHECK(h.session.player_count() == 1);
  bool told = false;
  for (const Sent& s : h.to(1)) {
    if (const auto* left = std::get_if<PlayerLeft>(&s.message)) {
      CHECK(*left == PlayerLeft{2, LeaveReason::timed_out});
      told = true;
    }
  }
  CHECK(told);
  // The transport reporting the same drop later changes nothing.
  h.clear();
  h.session.disconnected(2, h.now);
  h.collect();
  CHECK(h.sent.empty());
}

TEST_CASE("a transport drop after long silence is reported as timed out") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.now += 6'000'000;
  h.session.disconnected(2, h.now);
  h.collect();
  const auto left = only_message_to<PlayerLeft>(h, 1);
  REQUIRE(left.has_value());
  CHECK(left->reason == LeaveReason::timed_out);
}

TEST_CASE("an empty session forgets the game build the first player set") {
  Harness h;
  h.join(1, "Link");
  h.session.disconnected(1, h.now);
  h.collect();
  h.clear();
  h.connect(2);
  Hello other = hello_from("Zelda");
  other.game_build_id = "0000000000000000";
  h.receive(2, other, kChannelSession);
  auto msgs = h.to(2);
  REQUIRE(msgs.size() == 1);
  CHECK(std::holds_alternative<Welcome>(msgs[0].message));
}

TEST_CASE("one address may hold only 4 connections without HELLO; one more is refused") {
  Harness h;
  constexpr std::uint32_t kHome = 0x0100007F;  // 127.0.0.1 as ENet keeps it
  constexpr std::uint32_t kAway = 0x0200000A;
  for (ConnectionId id = 1; id <= 4; ++id) CHECK(h.session.connected(id, h.now, kHome));
  CHECK(h.session.half_open_count(kHome) == 4);

  CHECK_FALSE(h.session.connected(5, h.now, kHome));
  CHECK_FALSE(h.session.connected(6, h.now, kHome));
  CHECK(h.session.stats().half_open_limited == 2);
  CHECK(h.session.half_open_count(kHome) == 4);
  // The refused ones get nothing and are not in the session: their packets
  // are the transport's mistake, a HELLO from them never welcomed.
  h.collect();
  CHECK(h.sent.empty());
  CHECK(h.dropped.empty());
  h.receive(5, hello_from("Ghost"), kChannelSession);
  CHECK(h.sent.empty());
  CHECK(!h.session.player_number(5).has_value());

  // Another address is not affected, nor a connection with no address.
  for (ConnectionId id = 10; id <= 13; ++id) CHECK(h.session.connected(id, h.now, kAway));
  CHECK(h.session.connected(20, h.now));
  CHECK(h.session.stats().half_open_limited == 2);
  CHECK(h.session.stats().dropped() == 0);  // connections, not packets
}

TEST_CASE("a HELLO, a refusal or a drop frees a half-open place") {
  Harness h;
  constexpr std::uint32_t kHome = 0x0100007F;
  for (ConnectionId id = 1; id <= 4; ++id) REQUIRE(h.session.connected(id, h.now, kHome));
  REQUIRE_FALSE(h.session.connected(5, h.now, kHome));

  h.receive(1, hello_from("Link"), kChannelSession);
  REQUIRE(h.session.player_number(1) == 1);
  CHECK(h.session.half_open_count(kHome) == 3);
  CHECK(h.session.connected(6, h.now, kHome));

  h.receive(2, hello_from("link"), kChannelSession);  // name taken: refused
  CHECK(h.session.half_open_count(kHome) == 3);
  CHECK(h.session.connected(7, h.now, kHome));

  h.session.disconnected(3, h.now);
  CHECK(h.session.connected(8, h.now, kHome));
  CHECK_FALSE(h.session.connected(9, h.now, kHome));
  CHECK(h.session.stats().half_open_limited == 2);
}

TEST_CASE("4 players from one address joining at the same moment are all let in") {
  Harness h;
  constexpr std::uint32_t kHome = 0x0100007F;
  for (ConnectionId id = 1; id <= 4; ++id) REQUIRE(h.session.connected(id, h.now, kHome));
  const char* names[] = {"Link", "Zelda", "Saria", "Darunia"};
  for (ConnectionId id = 1; id <= 4; ++id) {
    h.receive(id, hello_from(names[id - 1]), kChannelSession);
  }
  CHECK(h.session.player_count() == 4);
  // With all of them in, the address may again open 4 more (a fifth player
  // is then told the server is full, not cut silently).
  for (ConnectionId id = 5; id <= 8; ++id) CHECK(h.session.connected(id, h.now, kHome));
  CHECK(h.session.stats().half_open_limited == 0);
}

TEST_CASE("the half-open limit is a setting, and 0 turns it off") {
  SessionConfig two;
  two.max_half_open_per_address = 2;
  Harness h(two);
  CHECK(h.session.connected(1, h.now, 7u));
  CHECK(h.session.connected(2, h.now, 7u));
  CHECK_FALSE(h.session.connected(3, h.now, 7u));

  SessionConfig off;
  off.max_half_open_per_address = 0;
  Harness unlimited(off);
  for (ConnectionId id = 1; id <= 64; ++id) CHECK(unlimited.session.connected(id, unlimited.now, 7u));
  CHECK(unlimited.session.stats().half_open_limited == 0);
}

TEST_CASE("the HELLO timeout still drops half-open connections, freeing their places") {
  Harness h;
  constexpr std::uint32_t kHome = 0x0100007F;
  for (ConnectionId id = 1; id <= 4; ++id) REQUIRE(h.session.connected(id, h.now, kHome));
  h.now += 10'000'000;
  h.session.tick(h.now);
  h.collect();
  CHECK(h.dropped.size() == 4);
  CHECK(h.session.half_open_count(kHome) == 0);
  CHECK(h.session.connected(5, h.now, kHome));
}
