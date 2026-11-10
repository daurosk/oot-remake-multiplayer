// The relay server's rules, driven without a network.

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
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

  void connect(ConnectionId id, std::optional<std::uint32_t> address = std::nullopt) {
    REQUIRE(session.connected(id, now, address));
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
  Welcome join(ConnectionId id, const std::string& nickname,
               std::optional<std::uint32_t> address = std::nullopt) {
    connect(id, address);
    receive(id, hello_from(nickname), kChannelSession);
    for (const Sent& s : to(id)) {
      if (const auto* w = std::get_if<Welcome>(&s.message)) return *w;
    }
    FAIL("no WELCOME");
    return {};
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

TEST_CASE("the server's log says which side of a version refusal is older") {
  std::vector<std::string> lines;
  SessionConfig config;
  config.log = [&lines](const std::string& line) { lines.push_back(line); };
  Harness h(config);
  h.connect(1);
  Bytes older = {1, 0, 2, 0, 1, 0};  // a HELLO of protocol 1
  h.receive_raw(1, older, kChannelSession);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].find("protocol 1, older than this server's") != std::string::npos);
  CHECK(lines[0].find("that player runs Update.bat") != std::string::npos);

  h.connect(2);
  const auto newer = static_cast<std::uint8_t>(kProtocolVersion + 1);
  Bytes b = {1, 0, 2, 0, newer, 0};
  h.receive_raw(2, b, kChannelSession);
  REQUIRE(lines.size() == 2);
  CHECK(lines[1].find("newer than this server's") != std::string::npos);
  CHECK(lines[1].find("run Update.bat in this kit") != std::string::npos);
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
  REQUIRE(msgs.size() == 3);  // WELCOME, then HOST and FLAG_SNAPSHOT
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

// ---------------------------------------------------------------- stage 1 steps 3 and 4

namespace {

FlagEntry flag(std::uint32_t hash, std::uint32_t value, std::uint16_t index = 0) {
  return FlagEntry{hash, index, FlagKind::uint32, value};
}

FlagEntry key64(std::uint64_t key, std::uint32_t value) {
  return FlagEntry{FlagKey::key64_flag(key), FlagKind::boolean, value};
}

std::vector<FlagEntry> many(std::uint32_t first, std::size_t n) {
  std::vector<FlagEntry> out;
  for (std::size_t i = 0; i < n; ++i) out.push_back(flag(first + static_cast<std::uint32_t>(i), 1));
  return out;
}

template <typename T>
std::vector<T> messages_of(const Harness& h, ConnectionId id) {
  std::vector<T> out;
  for (const Sent& s : h.to(id)) {
    if (const auto* m = std::get_if<T>(&s.message)) out.push_back(*m);
  }
  return out;
}

}  // namespace

TEST_CASE("the first player hosts; each joiner is told who hosts, then sent the table") {
  Harness h;
  CHECK(h.session.host() == std::nullopt);
  h.join(1, "Link");
  CHECK(h.session.host() == std::optional<std::uint8_t>{1});
  auto msgs = h.to(1);
  REQUIRE(msgs.size() == 3);
  CHECK(std::holds_alternative<Welcome>(msgs[0].message));
  CHECK(msgs[1].channel == kChannelProgress);
  CHECK(std::get<Host>(msgs[1].message) == Host{1});
  CHECK(msgs[2].channel == kChannelProgress);
  CHECK(std::get<FlagSnapshot>(msgs[2].message) == FlagSnapshot{0, 1, {}});

  h.clear();
  h.join(2, "Zelda");
  CHECK(h.session.host() == std::optional<std::uint8_t>{1});
  CHECK(messages_of<Host>(h, 2) == std::vector<Host>{Host{1}});
  CHECK(messages_of<FlagSnapshot>(h, 2).size() == 1);
  // The others are told of the joiner only.
  CHECK(only_message_to<PlayerJoined>(h, 1).has_value());
}

TEST_CASE("when the host leaves, the lowest remaining number hosts and everyone is told") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  h.session.disconnected(1, h.now);
  h.collect();
  CHECK(h.session.host() == std::optional<std::uint8_t>{2});
  for (ConnectionId id : {2u, 3u}) {
    auto msgs = h.to(id);
    REQUIRE(msgs.size() == 2);
    CHECK(std::holds_alternative<PlayerLeft>(msgs[0].message));
    CHECK(msgs[1].channel == kChannelProgress);
    CHECK(std::get<Host>(msgs[1].message) == Host{2});
  }

  // A newcomer takes the free number 1 but does not take over.
  h.clear();
  Welcome w = h.join(4, "Darunia");
  CHECK(w.player_number == 1);
  CHECK(h.session.host() == std::optional<std::uint8_t>{2});
  CHECK(messages_of<Host>(h, 4) == std::vector<Host>{Host{2}});

  // A player who is not the host leaving changes nothing.
  h.clear();
  h.session.disconnected(3, h.now);
  h.collect();
  CHECK(messages_of<Host>(h, 2).empty());
  CHECK(messages_of<Host>(h, 4).empty());

  // The host leaves again: now player 1 (connection 4) is the lowest.
  h.clear();
  h.session.disconnected(2, h.now);
  h.collect();
  CHECK(h.session.host() == std::optional<std::uint8_t>{1});
  CHECK(messages_of<Host>(h, 4) == std::vector<Host>{Host{1}});
}

TEST_CASE("join partner goes to the named player only, and the answer back") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  h.receive(1, JoinPartner{0, 3, 7, JoinKind::request, std::nullopt}, kChannelSession);
  CHECK(h.to(2).empty());
  CHECK(h.to(1).empty());
  auto ask = only_message_to<JoinPartner>(h, 3);
  REQUIRE(ask.has_value());
  CHECK(ask->player_number == 1);
  CHECK(ask->other_player == 3);
  CHECK(ask->request_id == 7);
  CHECK(ask->kind == JoinKind::request);
  CHECK(h.sent[0].channel == kChannelSession);

  h.clear();
  JoinTarget where{"spot00", {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
  h.receive(3, JoinPartner{0, 1, 7, JoinKind::accept, where}, kChannelSession);
  CHECK(h.to(2).empty());
  CHECK(h.to(3).empty());
  auto yes = only_message_to<JoinPartner>(h, 1);
  REQUIRE(yes.has_value());
  CHECK(*yes == JoinPartner{3, 1, 7, JoinKind::accept, where});

  h.clear();
  h.receive(2, JoinPartner{0, 1, 8, JoinKind::refuse, std::nullopt, JoinRefuseReason::busy},
            kChannelSession);
  auto no = only_message_to<JoinPartner>(h, 1);
  REQUIRE(no.has_value());
  CHECK(no->player_number == 2);
  CHECK(no->kind == JoinKind::refuse);
  CHECK(no->reason == JoinRefuseReason::busy);
  CHECK(h.session.stats().join_relayed == 3);
}

TEST_CASE("a join request for a player not there is refused by the server, with the same id") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();

  h.receive(1, JoinPartner{0, 4, 9, JoinKind::request, std::nullopt}, kChannelSession);
  CHECK(h.to(2).empty());
  auto no = only_message_to<JoinPartner>(h, 1);
  REQUIRE(no.has_value());
  CHECK(no->player_number == 4);
  CHECK(no->other_player == 1);
  CHECK(no->request_id == 9);
  CHECK(no->kind == JoinKind::refuse);
  CHECK(no->reason == JoinRefuseReason::not_there);
  CHECK(h.session.stats().join_not_there == 1);
  CHECK(h.session.stats().join_relayed == 0);

  // An answer to a player who has gone is dropped, not answered.
  h.clear();
  h.receive(2, JoinPartner{0, 3, 9, JoinKind::refuse, std::nullopt, JoinRefuseReason::declined},
            kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().join_dropped == 1);
}

TEST_CASE("a join partner naming its own sender is dropped") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.receive(2, JoinPartner{0, 2, 1, JoinKind::request, std::nullopt}, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().join_dropped == 1);
  CHECK(h.session.stats().dropped() >= 1);
}

TEST_CASE("flag set goes to the host only, the host's own back to itself") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  h.receive(2, FlagSet{0, {flag(5, 1)}}, kChannelProgress);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 1);
  CHECK(h.sent[0].channel == kChannelProgress);
  CHECK(std::get<FlagSet>(h.sent[0].message) == FlagSet{2, {flag(5, 1)}});
  // A FLAG_SET changes nothing on the server: only the host's answer does.
  CHECK(h.session.flag_count() == 0);

  h.clear();
  h.receive(1, FlagSet{0, {flag(6, 2)}}, kChannelProgress);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 1);
  CHECK(std::get<FlagSet>(h.sent[0].message) == FlagSet{1, {flag(6, 2)}});
  CHECK(h.session.stats().flag_relayed == 2);
}

TEST_CASE("flag state is taken from the host only, kept, and relayed to everyone else") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  h.receive(2, FlagState{0, {flag(5, 1)}}, kChannelProgress);
  h.receive(3, FlagSnapshot{0, 1, {flag(5, 1)}}, kChannelProgress);
  CHECK(h.sent.empty());
  CHECK(h.session.flag_count() == 0);
  CHECK(h.session.stats().flag_refused == 2);

  h.receive(1, FlagState{0, {flag(5, 1), flag(5, 3, 2)}}, kChannelProgress);
  CHECK(h.to(1).empty());
  for (ConnectionId id : {2u, 3u}) {
    auto st = only_message_to<FlagState>(h, id);
    REQUIRE(st.has_value());
    CHECK(*st == FlagState{1, {flag(5, 1), flag(5, 3, 2)}});
  }
  CHECK(h.session.flag_count() == 2);  // (hash, index) keys: index 2 is its own

  // A later value replaces the kept one.
  h.receive(1, FlagState{0, {flag(5, 0)}}, kChannelProgress);
  CHECK(h.session.flag_count() == 2);
  h.clear();
  h.join(4, "Darunia");
  auto snap = messages_of<FlagSnapshot>(h, 4);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].flags == std::vector<FlagEntry>{flag(5, 0), flag(5, 3, 2)});
}

TEST_CASE("a joiner gets the table before any flag state sent after it joined") {
  Harness h;
  h.join(1, "Link");
  h.receive(1, FlagState{0, {flag(1, 1)}}, kChannelProgress);
  h.clear();

  h.join(2, "Zelda");
  h.receive(1, FlagState{0, {flag(2, 1)}}, kChannelProgress);
  auto msgs = h.to(2);
  REQUIRE(msgs.size() == 4);
  CHECK(std::holds_alternative<Welcome>(msgs[0].message));
  CHECK(std::get<Host>(msgs[1].message) == Host{1});
  CHECK(std::get<FlagSnapshot>(msgs[2].message) == FlagSnapshot{0, 1, {flag(1, 1)}});
  CHECK(std::get<FlagState>(msgs[3].message) == FlagState{1, {flag(2, 1)}});
  for (std::size_t i = 1; i < 4; ++i) CHECK(msgs[i].channel == kChannelProgress);
}

TEST_CASE("a large table goes to a joiner in parts") {
  Harness h;
  h.join(1, "Link");
  h.receive(1, FlagState{0, many(0, kMaxFlagEntries)}, kChannelProgress);
  h.receive(1, FlagState{0, many(100'000, kMaxFlagEntries)}, kChannelProgress);
  h.receive(1, FlagState{0, many(200'000, kMaxFlagEntries)}, kChannelProgress);
  h.receive(1, FlagState{0, {flag(999'999, 1)}}, kChannelProgress);
  CHECK(h.session.flag_count() == 3 * kMaxFlagEntries + 1);
  h.clear();

  h.join(2, "Zelda");
  auto parts = messages_of<FlagSnapshot>(h, 2);
  REQUIRE(parts.size() == 4);
  std::size_t total = 0;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    CHECK(parts[i].part == i);
    CHECK(parts[i].part_count == 4);
    total += parts[i].flags.size();
  }
  CHECK(total == 3 * kMaxFlagEntries + 1);
}

TEST_CASE("a new host's snapshot merges in; on a clash the server's value stays") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.receive(1, FlagState{0, {flag(1, 1), flag(2, 7)}}, kChannelProgress);
  h.session.disconnected(1, h.now);
  h.collect();
  REQUIRE(h.session.host() == std::optional<std::uint8_t>{2});
  h.clear();

  // Player 2's save has flag 1 the same, flag 2 different and flag 3 new.
  h.receive(2, FlagSnapshot{0, 1, {flag(1, 1), flag(2, 0), flag(3, 4)}}, kChannelProgress);
  auto to_host = messages_of<FlagState>(h, 2);
  REQUIRE(to_host.size() == 1);
  CHECK(to_host[0] == FlagState{2, {flag(2, 7)}});
  auto to_other = messages_of<FlagState>(h, 3);
  REQUIRE(to_other.size() == 1);
  CHECK(to_other[0] == FlagState{2, {flag(3, 4)}});
  CHECK(h.session.flag_count() == 3);
  CHECK(h.session.stats().flag_clashes == 1);
  CHECK(h.session.stats().snapshot_parts == 1);

  // The same table again: nothing new, nothing differs, nothing sent.
  h.clear();
  h.receive(2, FlagSnapshot{0, 1, {flag(1, 1), flag(2, 7), flag(3, 4)}}, kChannelProgress);
  CHECK(h.sent.empty());

  h.join(4, "Darunia");
  auto snap = messages_of<FlagSnapshot>(h, 4);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].flags == std::vector<FlagEntry>{flag(1, 1), flag(2, 7), flag(3, 4)});
}

TEST_CASE("64-bit keys are kept, merged and sent to joiners beside named flags") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  // Key 5 and named hash 5 are two flags.
  h.receive(1, FlagState{0, {flag(5, 1), key64(5, 1), key64(0x2fa4c1d09b7e6a35ull, 0)}},
            kChannelProgress);
  CHECK(h.session.flag_count() == 3);
  auto st = only_message_to<FlagState>(h, 2);
  REQUIRE(st.has_value());
  CHECK(st->flags.size() == 3);

  h.receive(1, FlagState{0, {key64(0x2fa4c1d09b7e6a35ull, 1)}}, kChannelProgress);
  CHECK(h.session.flag_count() == 3);

  // Player 1 leaves; player 2's save clashes on one key and adds another.
  h.session.disconnected(1, h.now);
  h.collect();
  REQUIRE(h.session.host() == std::optional<std::uint8_t>{2});
  h.clear();
  h.receive(2, FlagSnapshot{0, 1, {key64(5, 0), key64(6, 1)}}, kChannelProgress);
  auto to_host = messages_of<FlagState>(h, 2);
  REQUIRE(to_host.size() == 1);
  CHECK(to_host[0] == FlagState{2, {key64(5, 1)}});
  CHECK(h.session.flag_count() == 4);

  h.join(3, "Saria");
  auto snap = messages_of<FlagSnapshot>(h, 3);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].flags == std::vector<FlagEntry>{flag(5, 1), key64(5, 1), key64(6, 1),
                                                key64(0x2fa4c1d09b7e6a35ull, 1)});
}

TEST_CASE("the table is kept while anyone is in and forgotten when the session empties") {
  Harness h;
  h.join(1, "Link");
  h.receive(1, FlagState{0, {flag(1, 1)}}, kChannelProgress);
  h.session.disconnected(1, h.now);
  h.collect();
  CHECK(h.session.host() == std::nullopt);
  CHECK(h.session.flag_count() == 0);
  h.clear();
  h.join(2, "Zelda");
  CHECK(h.session.host() == std::optional<std::uint8_t>{1});
  CHECK(messages_of<FlagSnapshot>(h, 2) == std::vector<FlagSnapshot>{FlagSnapshot{0, 1, {}}});
}

TEST_CASE("the flag table has a size limit; values past it are relayed but not kept") {
  SessionConfig small;
  small.max_flags = 2;
  Harness h(small);
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.receive(1, FlagState{0, {flag(1, 1), flag(2, 1), flag(3, 1)}}, kChannelProgress);
  CHECK(h.session.flag_count() == 2);
  CHECK(h.session.stats().flag_table_full == 1);
  auto st = only_message_to<FlagState>(h, 2);
  REQUIRE(st.has_value());
  CHECK(st->flags.size() == 3);
  // An existing key still takes a new value.
  h.receive(1, FlagState{0, {flag(1, 5)}}, kChannelProgress);
  CHECK(h.session.stats().flag_table_full == 1);
}

TEST_CASE("a host snapshot past the size limit is relayed but not kept, like a flag state") {
  SessionConfig small;
  small.max_flags = 2;
  Harness h(small);
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.receive(1, FlagSnapshot{0, 1, {flag(1, 1), flag(2, 1), flag(3, 1)}}, kChannelProgress);
  CHECK(h.session.flag_count() == 2);
  CHECK(h.session.stats().flag_table_full == 1);
  auto st = only_message_to<FlagState>(h, 2);
  REQUIRE(st.has_value());
  CHECK(st->flags.size() == 3);
}

TEST_CASE("a host leaving halfway through its snapshot: parts merged so far stay") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();
  // Part 0 of 2 comes, then the host leaves before part 1.
  h.receive(1, FlagSnapshot{0, 2, {flag(1, 1)}}, kChannelProgress);
  // Player 2 is not the host yet: its snapshot is dropped.
  h.receive(2, FlagSnapshot{0, 1, {flag(2, 2)}}, kChannelProgress);
  CHECK(h.session.stats().flag_refused == 1);
  CHECK(h.session.flag_count() == 1);
  h.session.disconnected(1, h.now);
  h.collect();
  REQUIRE(h.session.host() == std::optional<std::uint8_t>{2});
  h.clear();
  // Now it hosts, and its table merges in over what part 0 left.
  h.receive(2, FlagSnapshot{0, 1, {flag(1, 9), flag(2, 2)}}, kChannelProgress);
  CHECK(messages_of<FlagState>(h, 2) == std::vector<FlagState>{FlagState{2, {flag(1, 1)}}});
  CHECK(messages_of<FlagState>(h, 3) == std::vector<FlagState>{FlagState{2, {flag(2, 2)}}});
  CHECK(h.session.flag_count() == 2);
  // A later joiner gets both.
  h.join(4, "Darunia");
  auto snap = messages_of<FlagSnapshot>(h, 4);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].flags == std::vector<FlagEntry>{flag(1, 1), flag(2, 2)});
}

TEST_CASE("a size limit set past what a snapshot can carry still keeps and sends the table") {
  SessionConfig huge;
  huge.max_flags = static_cast<std::size_t>(-1);
  Harness h(huge);
  h.join(1, "Link");
  h.receive(1, FlagState{0, many(1, 10)}, kChannelProgress);
  CHECK(h.session.flag_count() == 10);
  h.clear();
  h.join(2, "Zelda");
  CHECK(messages_of<FlagSnapshot>(h, 2).size() == 1);
}

namespace {

constexpr std::uint32_t kHomePc = 0x0100007F;
constexpr std::uint32_t kOtherPc = 0x0200000A;

// Sends a HELLO on a new connection; returns what that connection was sent.
std::vector<Sent> hello_on(Harness& h, ConnectionId id, const std::string& nickname,
                           std::optional<std::uint32_t> address) {
  h.clear();
  h.connect(id, address);
  h.receive(id, hello_from(nickname), kChannelSession);
  return h.to(id);
}

RefuseReason refused_reason(const std::vector<Sent>& msgs) {
  REQUIRE(msgs.size() == 1);
  return std::get<Refused>(msgs[0].message).reason;
}

}  // namespace

TEST_CASE("a killed build restarted on the same PC takes its old place after 1 s quiet") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  h.join(3, "Saria", kOtherPc);
  // Link's build is killed: nothing more from connection 1. The others talk.
  h.now += 1'000'000;
  h.receive(2, Ping{h.now}, kChannelSession);

  auto msgs = hello_on(h, 9, "link", kHomePc);
  REQUIRE(!msgs.empty());
  const auto* w = std::get_if<Welcome>(&msgs[0].message);
  REQUIRE(w != nullptr);
  CHECK(w->player_number == 1);  // its old number, not just the lowest free
  REQUIRE(w->players.size() == 2);
  CHECK(w->players[0].nickname == "Zelda");
  CHECK(h.dropped == std::vector<ConnectionId>{1});
  for (ConnectionId other : {2u, 3u}) {
    auto seen = h.to(other);
    // Left, the host handed on (Link hosted), then joined again.
    REQUIRE(seen.size() >= 3);
    CHECK(std::get<PlayerLeft>(seen[0].message) == PlayerLeft{1, LeaveReason::timed_out});
    CHECK(std::get<Host>(seen[1].message) == Host{2});
    CHECK(std::get<PlayerJoined>(seen[2].message) == PlayerJoined{1, "link"});
  }
  CHECK(h.session.player_count() == 3);
  CHECK(h.session.player_number(9) == std::optional<std::uint8_t>{1});
  CHECK(h.session.stats().rejoined == 1);
  CHECK(h.session.stats().dropped() == 0);

  // ENet reporting the old peer gone later changes nothing.
  h.clear();
  h.session.disconnected(1, h.now);
  h.collect();
  CHECK(h.sent.empty());
  CHECK(h.session.player_count() == 3);
}

TEST_CASE("a rejoining host hands over as on any leave; alone it hosts again with the table") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  h.receive(1, FlagState{0, {FlagEntry{0x1234, 0, FlagKind::boolean, 1}}}, kChannelProgress);
  h.now += 1'500'000;
  auto msgs = hello_on(h, 9, "Link", kHomePc);
  CHECK(h.session.host() == std::optional<std::uint8_t>{2});
  CHECK(messages_of<Host>(h, 2) == std::vector<Host>{Host{2}});
  CHECK(messages_of<Host>(h, 9) == std::vector<Host>{Host{2}});
  // The table the old host built is kept and sent to the rejoiner.
  CHECK(h.session.flag_count() == 1);
  CHECK(messages_of<FlagSnapshot>(h, 9).size() == 1);

  Harness solo;
  solo.join(1, "Link", kHomePc);
  solo.now += 1'000'000;
  auto again = hello_on(solo, 9, "Link", kHomePc);
  REQUIRE(std::holds_alternative<Welcome>(again[0].message));
  CHECK(std::get<Welcome>(again[0].message).player_number == 1);
  CHECK(solo.session.host() == std::optional<std::uint8_t>{1});
}

TEST_CASE("a rejoin into a full session takes the old place instead of being refused as full") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  h.join(3, "Saria", kOtherPc);
  h.join(4, "Darunia", kOtherPc);
  h.now += 1'000'000;
  auto msgs = hello_on(h, 9, "Link", kHomePc);
  REQUIRE(std::holds_alternative<Welcome>(msgs[0].message));
  CHECK(h.session.player_count() == 4);
  // A fifth name is still refused as full.
  CHECK(refused_reason(hello_on(h, 10, "Ruto", kHomePc)) == RefuseReason::server_full);
}

TEST_CASE("a live player's name stays refused, from any PC; another PC never takes over") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);

  // Same PC, but Link was heard 999 ms ago: still a live player.
  h.now += 999'000;
  CHECK(refused_reason(hello_on(h, 10, "Link", kHomePc)) == RefuseReason::name_taken);
  // Another PC, however long Link has been quiet.
  h.now += 5'000'000;
  CHECK(refused_reason(hello_on(h, 11, "LINK", kOtherPc)) == RefuseReason::name_taken);
  // A connection with no address never takes over.
  CHECK(refused_reason(hello_on(h, 12, "Link", std::nullopt)) == RefuseReason::name_taken);
  CHECK(h.dropped == std::vector<ConnectionId>{12});  // only the refused one
  CHECK(h.session.player_number(1) == std::optional<std::uint8_t>{1});
  CHECK(h.session.stats().rejoined == 0);

  // Link answers again: a later same-PC HELLO is refused too.
  h.receive(1, Ping{h.now}, kChannelSession);
  h.now += 100'000;
  CHECK(refused_reason(hello_on(h, 13, "Link", kHomePc)) == RefuseReason::name_taken);
}

TEST_CASE("rejoin_stale_us 0 turns the takeover off") {
  SessionConfig config;
  config.rejoin_stale_us = 0;
  Harness h(config);
  h.join(1, "Link", kHomePc);
  h.now += 9'000'000;
  CHECK(refused_reason(hello_on(h, 9, "Link", kHomePc)) == RefuseReason::name_taken);
  CHECK(h.session.player_number(1) == std::optional<std::uint8_t>{1});
}

// ---------------------------------------------------------------- objects

namespace {

ObjectId placed(std::uint64_t hash) { return ObjectId{ObjectKind::placed, 0, hash}; }

using OwnerEpoch = std::optional<std::pair<std::uint8_t, std::uint16_t>>;

OwnerEpoch owned(std::uint8_t owner, std::uint16_t epoch) {
  return std::make_pair(owner, epoch);
}

ObjectPresent present_in(const std::string& scene, std::vector<ObjectId> ids) {
  ObjectPresent p;
  p.scene = scene;
  for (const ObjectId& id : ids) p.objects.push_back({id, 0});
  return p;
}

ObjectState state_at(float x, std::int32_t life) {
  ObjectState s;
  s.time.server_time_us = 5;
  s.position = Vec3{x, 0, 0};
  s.life = ObjectLife{life, 12};
  return s;
}

template <typename T>
std::vector<T> messages_to(const Harness& h, ConnectionId id) {
  std::vector<T> out;
  for (const Sent& s : h.to(id)) {
    if (const auto* m = std::get_if<T>(&s.message)) out.push_back(*m);
  }
  return out;
}

// Three players in, nothing sent yet about objects.
struct ObjectHarness : Harness {
  ObjectHarness() {
    join(1, "Link");
    join(2, "Zelda");
    join(3, "Saria");
    clear();
  }
};

}  // namespace

TEST_CASE("the first game to report a placed object owns it, epoch 0") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  // Only player 1 is known to be in the Graveyard: only it is told.
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 1);
  CHECK(h.sent[0].channel == kChannelOwnership);
  CHECK(std::get<ObjectOwner>(h.sent[0].message) == ObjectOwner{placed(7), 1, 0, 0});

  // A second game reporting it is told the owner; the owner stays.
  h.clear();
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 2);
  CHECK(std::get<ObjectOwner>(h.sent[0].message) == ObjectOwner{placed(7), 1, 0, 0});
  // OBJECT_PRESENT itself is not relayed.
  CHECK(h.session.stats().relayed == 0);
}

TEST_CASE("object samples go only from the owner of the current epoch, to its scene") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(3, present_in("Market", {}), kChannelOwnership);
  h.clear();

  h.receive(1, ObjectSample{0, placed(7), 0, state_at(1, 12)}, kChannelObjectSamples);
  REQUIRE(h.sent.size() == 1);  // not to 3, in the Market
  CHECK(h.sent[0].to == 2);
  CHECK(h.sent[0].channel == kChannelObjectSamples);
  CHECK(std::get<ObjectSample>(h.sent[0].message) ==
        ObjectSample{1, placed(7), 0, state_at(1, 12)});

  h.clear();
  h.receive(2, ObjectSample{0, placed(7), 0, state_at(2, 12)}, kChannelObjectSamples);  // not owner
  h.receive(1, ObjectSample{0, placed(7), 3, state_at(2, 12)}, kChannelObjectSamples);  // old epoch
  h.receive(1, ObjectSample{0, placed(8), 0, state_at(2, 12)}, kChannelObjectSamples);  // unknown
  CHECK(h.sent.empty());
  CHECK(h.session.stats().object_refused == 3);
  CHECK(h.session.stats().object_relayed == 1);
}

TEST_CASE("damage goes to the owner alone, whatever epoch the hitter knew") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.clear();

  const Damage hit{0, placed(7), 9, 2, static_cast<std::uint8_t>(DamageCause::sword), {1, 2, 3}};
  h.receive(2, hit, kChannelOwnership);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 1);
  CHECK(h.sent[0].channel == kChannelOwnership);
  Damage expected = hit;
  expected.player_number = 2;
  CHECK(std::get<Damage>(h.sent[0].message) == expected);

  h.clear();
  h.receive(1, hit, kChannelOwnership);                                // from the owner itself
  h.receive(2, Damage{0, placed(8), 0, 2, 0, {}}, kChannelOwnership);  // unknown object
  CHECK(h.sent.empty());
  CHECK(h.session.stats().damage_relayed == 1);
  CHECK(h.session.stats().damage_dropped == 2);
}

TEST_CASE("when the owner leaves, the next game takes it with epoch + 1 and the last sample") {
  ObjectHarness h;
  h.receive(3, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(3, ObjectSample{0, placed(7), 0, state_at(4, 6)}, kChannelObjectSamples);
  h.clear();

  h.session.disconnected(3, h.now);
  h.collect();
  CHECK(h.session.object_owner(placed(7)) == owned(1, 1));
  // Player 1 (the lowest) gets the left news, the last state, then the owner.
  const auto to1 = h.to(1);
  REQUIRE(to1.size() == 3);
  CHECK(std::holds_alternative<PlayerLeft>(to1[0].message));
  CHECK(std::get<HandoverState>(to1[1].message) ==
        HandoverState{3, placed(7), 1, state_at(4, 6)});
  CHECK(std::get<ObjectOwner>(to1[2].message) == ObjectOwner{placed(7), 1, 1, 0});
  CHECK(messages_to<ObjectOwner>(h, 2) == std::vector<ObjectOwner>{{placed(7), 1, 1, 0}});
  CHECK(messages_to<HandoverState>(h, 2).empty());

  // A sample of the old epoch is refused, even from the new owner.
  h.clear();
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(5, 6)}, kChannelObjectSamples);
  CHECK(h.sent.empty());
  h.receive(1, ObjectSample{0, placed(7), 1, state_at(5, 6)}, kChannelObjectSamples);
  CHECK(h.sent.size() == 1);
  CHECK(h.session.stats().owner_changes == 1);
}

TEST_CASE("unloading or leaving the scene hands it over; nobody left forgets it") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7), placed(8)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.clear();

  // Player 1 unloads 7: player 2 takes it, with no state to pass (no sample yet).
  h.receive(1, present_in("Graveyard", {placed(8)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  CHECK(messages_to<HandoverState>(h, 2).empty());
  CHECK(messages_to<ObjectOwner>(h, 2) == std::vector<ObjectOwner>{{placed(7), 2, 1, 0}});

  // Player 1 walks into the Market (the scene of its player sample): 8 had
  // nobody else, so it is forgotten.
  h.clear();
  PlayerSample walk;
  walk.time.server_time_us = 1;
  walk.scene = "Market";
  h.receive(1, walk, kChannelPlayerSamples);
  CHECK_FALSE(h.session.object_owner(placed(8)).has_value());
  CHECK(h.session.object_count() == 1);
  CHECK(messages_to<ObjectOwner>(h, 1).empty());
}

TEST_CASE("a removed enemy stays owned by nobody until no game has it, then is forgotten") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.clear();

  h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::dead, {1, 1, 1}}, kChannelOwnership);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 2);
  CHECK(std::get<ObjectRemove>(h.sent[0].message) ==
        ObjectRemove{1, placed(7), 0, RemoveReason::dead, {1, 1, 1}});
  CHECK(h.session.object_owner(placed(7)) == owned(0, 0));

  // Player 2's copy, still listed, does not take it; damage to it goes nowhere.
  h.clear();
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(2, Damage{0, placed(7), 0, 2, 0, {}}, kChannelOwnership);
  CHECK(h.sent.empty());
  // Player 1 leaves the scene: a removed object is not handed over.
  h.receive(1, present_in("Market", {}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(0, 0));
  h.receive(2, present_in("Graveyard", {}), kChannelOwnership);
  CHECK(h.session.object_count() == 0);
}

TEST_CASE("the owner of a placed object may name its actor to the scene; nobody else may") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(9)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {}), kChannelOwnership);
  h.clear();
  ObjectSpawn spawn{0, placed(9), "Enemy_Skulltula_Gold", 0, state_at(3, 1)};
  h.receive(1, spawn, kChannelOwnership);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 2);
  spawn.player_number = 1;
  CHECK(std::get<ObjectSpawn>(h.sent[0].message) == spawn);
  CHECK(h.session.object_owner(placed(9)) == owned(1, 0));  // unchanged
  // Not the owner, or an object nobody listed: refused.
  h.clear();
  h.receive(2, ObjectSpawn{0, placed(9), "Enemy_Skulltula_Gold", 0, state_at(3, 1)},
            kChannelOwnership);
  h.receive(1, ObjectSpawn{0, placed(8), "Enemy_Skulltula_Gold", 0, state_at(3, 1)},
            kChannelOwnership);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().object_refused == 2);
}

TEST_CASE("a made object belongs to its maker; it goes on every screen when the maker leaves") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {}), kChannelOwnership);
  h.clear();

  const ObjectId bomb{ObjectKind::made, 1, 1};
  ObjectSpawn spawn{0, bomb, "Bomb", object_spawn::projectile, state_at(3, 1)};
  h.receive(1, spawn, kChannelOwnership);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 2);
  spawn.player_number = 1;
  CHECK(std::get<ObjectSpawn>(h.sent[0].message) == spawn);
  CHECK(h.session.object_owner(bomb) == owned(1, 0));

  // Not twice, and not with another maker's number.
  h.clear();
  h.receive(1, ObjectSpawn{0, bomb, "Bomb", 0, state_at(3, 1)}, kChannelOwnership);
  h.receive(2, ObjectSpawn{0, ObjectId{ObjectKind::made, 1, 2}, "Bomb", 0, state_at(3, 1)},
            kChannelOwnership);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().object_refused == 2);
  // Player 2 listing it does not make it an owner when the maker goes.
  h.receive(2, present_in("Graveyard", {bomb}), kChannelOwnership);
  CHECK(h.sent.empty());

  h.session.disconnected(1, h.now);
  h.collect();
  const auto removes = messages_to<ObjectRemove>(h, 2);
  REQUIRE(removes.size() == 1);
  CHECK(removes[0] == ObjectRemove{1, bomb, 0, RemoveReason::gone, {3, 0, 0}});
  CHECK(h.session.object_count() == 0);
}

TEST_CASE("the maker's remove of a made object is relayed and forgets it") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {}), kChannelOwnership);
  const ObjectId arrow{ObjectKind::made, 1, 5};
  h.receive(1, ObjectSpawn{0, arrow, "Arrow", object_spawn::projectile, state_at(0, 1)},
            kChannelOwnership);
  h.clear();
  h.receive(1, ObjectRemove{0, arrow, 0, RemoveReason::hit, {}}, kChannelOwnership);
  REQUIRE(h.sent.size() == 1);
  CHECK(h.sent[0].to == 2);
  CHECK(h.session.object_count() == 0);
}

TEST_CASE("handover state from a client is refused: the server does not ask for one yet") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.clear();
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 1)}, kChannelOwnership);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().object_refused == 1);
}

TEST_CASE("the object table has a size limit") {
  SessionConfig config;
  config.max_objects = 2;
  Harness h(config);
  h.join(1, "Link");
  h.clear();
  h.receive(1, present_in("Graveyard", {placed(1), placed(2), placed(3)}), kChannelOwnership);
  CHECK(h.session.object_count() == 2);
  CHECK(h.session.stats().object_table_full == 1);
}

TEST_CASE("objects are forgotten when the session empties") {
  ObjectHarness h;
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.session.disconnected(1, h.now);
  h.session.disconnected(2, h.now);
  h.session.disconnected(3, h.now);
  CHECK(h.session.object_count() == 0);
}

// ---------------------------------------------------------------- field grass

namespace {

GrassCut cut_at(const std::string& scene, float x) {
  GrassCut c;
  c.scene = scene;
  c.position = Vec3{x, 20.0f, 3.0f};
  c.radius = 1.5f;
  c.flags = 1;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  c.sweep = {Vec3{nan, nan, nan}, Vec3{nan, nan, nan}, Vec3{nan, nan, nan}};
  return c;
}

PlayerSample sample_in(const std::string& scene) {
  PlayerSample s;
  s.time.server_time_us = 1;
  s.scene = scene;
  return s;
}

GrassCut from(std::uint8_t player, GrassCut c) {
  c.player_number = player;
  return c;
}

}  // namespace

TEST_CASE("a grass cut goes to the players in its scene, once") {
  ObjectHarness h;
  h.receive(2, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);
  h.clear();

  // Player 1's own sample has not said where it is: the cut does.
  h.receive(1, cut_at("HyruleField_Child", 1), kChannelSession);
  CHECK(messages_to<GrassCut>(h, 1).empty());
  CHECK(messages_to<GrassCut>(h, 2) ==
        std::vector<GrassCut>{from(1, cut_at("HyruleField_Child", 1))});
  CHECK(messages_to<GrassCut>(h, 3).empty());
  CHECK(h.session.stats().grass_relayed == 1);

  // The same cut again, from the game that made it for player 1: not passed on.
  h.clear();
  h.receive(2, cut_at("HyruleField_Child", 1), kChannelSession);
  CHECK(messages_to<GrassCut>(h, 1).empty());
  CHECK(h.session.stats().grass_repeated == 1);
}

TEST_CASE("a player who enters a scene gets the grass cut there before") {
  ObjectHarness h;
  h.receive(1, cut_at("HyruleField_Child", 1), kChannelSession);
  h.receive(1, cut_at("HyruleField_Child", 2), kChannelSession);
  h.receive(1, cut_at("Market", 3), kChannelSession);  // player 1 is in the Market now
  h.clear();

  // Nobody was left in the field when player 1 went: its grass has grown back.
  h.receive(2, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 2).empty());

  // The Market's cut is kept while player 1 is there.
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 3) == std::vector<GrassCut>{from(1, cut_at("Market", 3))});
  CHECK(h.session.stats().grass_replayed == 1);

  // Player 2 cuts in the field; player 1 walks in and gets it, in order.
  h.receive(2, cut_at("HyruleField_Child", 4), kChannelSession);
  h.receive(2, cut_at("HyruleField_Child", 5), kChannelSession);
  h.clear();
  h.receive(1, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 1) ==
        std::vector<GrassCut>{from(2, cut_at("HyruleField_Child", 4)),
                              from(2, cut_at("HyruleField_Child", 5))});

  // A player who joins late gets them with its first sample there.
  h.join(4, "Ruto");
  h.clear();
  h.receive(4, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 4).size() == 2);
}

TEST_CASE("the cuts of a scene end when its last player leaves or the list is full") {
  ObjectHarness h;
  h.receive(1, cut_at("HyruleField_Child", 1), kChannelSession);
  h.receive(2, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  h.session.disconnected(1, h.now);  // player 2 still shows the cut
  h.receive(3, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 3).size() == 1);
  h.session.disconnected(2, h.now);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);  // the field is empty
  h.clear();
  h.receive(3, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 3).empty());

  // Only the newest kMaxGrassCuts are kept.
  for (std::size_t i = 0; i < kMaxGrassCuts + 3; ++i) {
    h.now += 100'000;  // under the rate limit
    h.receive(3, cut_at("HyruleField_Child", 100.0f + static_cast<float>(i)), kChannelSession);
  }
  h.join(1, "Link");
  h.clear();
  h.receive(1, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  const auto got = messages_to<GrassCut>(h, 1);
  REQUIRE(got.size() == kMaxGrassCuts);
  CHECK(got.front() == from(3, cut_at("HyruleField_Child", 103.0f)));
}
