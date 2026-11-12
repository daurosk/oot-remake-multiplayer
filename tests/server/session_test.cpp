// The relay server's rules, driven without a network.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "session.hpp"
#include "world.hpp"

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
  // Every game since protocol 10 offers a key share (traffic_crypto.hpp).
  static const KeyPair game_keys = KeyPair::from_secret({7, 7, 7});
  h.key_share = game_keys.share();
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
  static constexpr const char* kTestPassword = "test-pass";
  bool auto_answer;      // declared before session: the constructor sets these first
  std::string password;
  Session session;
  std::vector<Sent> sent;
  std::vector<ConnectionId> dropped;
  // Connections the session switched to encrypted traffic (EncryptAction).
  std::vector<ConnectionId> encrypted;
  // The server's key share sent with each JOIN_CHALLENGE nonce.
  std::map<JoinNonce, KeyShare> server_shares;

  // The game's JOIN_PROOF for `nonce` (bound to both key shares, as
  // hello_from's game sends it). A nonce never sent gets a zero share.
  JoinProofBytes proof(const std::string& password, const JoinNonce& nonce) const {
    auto it = server_shares.find(nonce);
    const KeyShare server = it != server_shares.end() ? it->second : KeyShare{};
    return join_proof(password, nonce, *hello_from("x").key_share, server);
  }
  // The REJOIN_TOKEN each connection was sent last (right after WELCOME).
  std::map<ConnectionId, RejoinTokenBytes> tokens;
  std::uint64_t now = 1'000'000;

  // Every session has a join password (there is no plain way in). A config
  // without one gets kTestPassword, and the harness answers each challenge
  // with its right proof at once, so the tests that are not about the
  // password see HELLO -> WELCOME as before (the challenge is kept out of
  // `sent`). A config with its own password is driven by hand.
  // The owner hold after a start (SessionConfig::owner_hold_us) is off unless
  // a test asks for it: the object tests see the first lister own at once.
  explicit Harness(SessionConfig config = {}, bool owner_hold = false)
      : auto_answer(config.join_password.empty()),
        password(auto_answer ? std::string(kTestPassword) : config.join_password),
        session(with_join_password(owner_hold ? std::move(config) : hold_off(std::move(config)), password)) {}

  static SessionConfig hold_off(SessionConfig config) {
    config.owner_hold_us = 0;
    return config;
  }

  static SessionConfig with_join_password(SessionConfig config, const std::string& password) {
    config.join_password = password;
    return config;
  }

  void collect() {
    for (Action& a : session.take_actions()) {
      if (auto* s = std::get_if<SendAction>(&a)) {
        Message m;
        REQUIRE(decode(s->bytes, m) == Error::none);
        // What the server sends must pass a client's own check.
        CHECK(check_received(m, Sender::server, s->channel) == Error::none);
        // Kept apart, so the tests that count what a joiner is sent need not
        // count it (see tokens).
        if (const auto* token = std::get_if<RejoinToken>(&m)) {
          CHECK(s->channel == kChannelSession);
          tokens[s->to] = token->token;
          continue;
        }
        if (const auto* challenge = std::get_if<JoinChallenge>(&m)) {
          if (challenge->key_share) server_shares[challenge->nonce] = *challenge->key_share;
          if (auto_answer) {
            to_answer.emplace_back(s->to, challenge->nonce);
            continue;
          }
        }
        sent.push_back({s->to, s->channel, m});
      } else if (auto* d = std::get_if<DisconnectAction>(&a)) {
        dropped.push_back(d->who);
      } else {
        encrypted.push_back(std::get<EncryptAction>(a).who);
      }
    }
    // The game's answer, as a real one sends it right after the challenge.
    std::vector<std::pair<ConnectionId, JoinNonce>> answer = std::move(to_answer);
    to_answer.clear();
    for (const auto& [id, nonce] : answer) {
      session.received(id, kChannelSession, bytes_of(JoinProof{proof(password, nonce)}), now);
      collect();
    }
  }
  std::vector<std::pair<ConnectionId, JoinNonce>> to_answer;

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
  SessionConfig four;
  four.max_players = 4;  // the old cap; the default is 16 (below)
  Harness h(four);
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
  // A HELLO of version 99 (not ours) whose body is just the version: unreadable as v0.
  Bytes b = {1, 0, 2, 0, 99, 0};
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
  Hello hello = hello_from("Link");
  hello.key_share.reset();  // so the nickname ends the packet
  Bytes b = bytes_of(hello);
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

// Stray (2026-11-12): after a server restart the games came back in the order
// they noticed it, so A (was 1) became 2 and B (was 2) became 1. With their
// previous numbers in HELLO (protocol 11) they keep them.
TEST_CASE("a restarted server gives each game back its previous number when free") {
  Harness h;
  Hello b = hello_from("B");
  b.previous_number = 2;
  h.connect(1);
  h.receive(1, b, kChannelSession);
  Welcome wb;
  for (const auto& s : h.to(1)) {
    if (const auto* w = std::get_if<Welcome>(&s.message)) wb = *w;
  }
  CHECK(wb.player_number == 2);  // B came back first, still 2
  Hello a = hello_from("A");
  a.previous_number = 1;
  h.connect(2);
  h.receive(2, a, kChannelSession);
  Welcome wa;
  for (const auto& s : h.to(2)) {
    if (const auto* w = std::get_if<Welcome>(&s.message)) wa = *w;
  }
  CHECK(wa.player_number == 1);
  // A number already taken is not given twice: the lowest free one instead.
  Hello c = hello_from("C");
  c.previous_number = 1;
  h.connect(3);
  h.receive(3, c, kChannelSession);
  Welcome wc;
  for (const auto& s : h.to(3)) {
    if (const auto* w = std::get_if<Welcome>(&s.message)) wc = *w;
  }
  CHECK(wc.player_number == 3);
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

TEST_CASE("one address may hold only 4 connections without HELLO, one more is refused") {
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

TEST_CASE("the first player hosts, each joiner is told who hosts, then sent the table") {
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
  // The set to the host; who asked (FLAG_ORIGIN) to the others but the host.
  REQUIRE(h.sent.size() == 2);
  CHECK(h.sent[0].to == 1);
  CHECK(h.sent[0].channel == kChannelProgress);
  CHECK(std::get<FlagSet>(h.sent[0].message) == FlagSet{2, {flag(5, 1)}});
  CHECK(h.sent[1].to == 3);
  CHECK(h.sent[1].channel == kChannelProgress);
  CHECK(std::get<FlagOrigin>(h.sent[1].message) == FlagOrigin{2, {flag(5, 1)}});
  // A FLAG_SET changes nothing on the server: only the host's answer does.
  CHECK(h.session.flag_count() == 0);

  h.clear();
  h.receive(1, FlagSet{0, {flag(6, 2)}}, kChannelProgress);
  REQUIRE(h.sent.size() == 3);
  CHECK(h.sent[0].to == 1);
  CHECK(std::get<FlagSet>(h.sent[0].message) == FlagSet{1, {flag(6, 2)}});
  CHECK(std::get<FlagOrigin>(h.sent[1].message) == FlagOrigin{1, {flag(6, 2)}});
  CHECK(std::get<FlagOrigin>(h.sent[2].message) == FlagOrigin{1, {flag(6, 2)}});
  CHECK(h.session.stats().flag_relayed == 2);
  CHECK(h.session.stats().origins_sent == 3);
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

TEST_CASE("a host's FLAG_RESET replaces the kept values, 0s included, and goes to everyone else") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.receive(1, FlagState{0, {flag(5, 1), flag(6, 1)}}, kChannelProgress);
  h.clear();

  h.receive(2, FlagReset{0, {flag(5, 0)}}, kChannelProgress);  // not the host
  CHECK(h.sent.empty());

  h.receive(1, FlagReset{0, {flag(5, 0)}}, kChannelProgress);  // the host loaded an older save
  CHECK(h.to(1).empty());
  auto r = only_message_to<FlagReset>(h, 2);
  REQUIRE(r.has_value());
  CHECK(*r == FlagReset{1, {flag(5, 0)}});
  CHECK(h.session.stats().host_resets == 1);  // on_host_reset ran once
  h.clear();
  h.join(3, "Saria");
  auto snap = messages_of<FlagSnapshot>(h, 3);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].flags == std::vector<FlagEntry>{flag(5, 0), flag(6, 1)});
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

TEST_CASE("a new host's snapshot merges in, on a clash the server's value stays") {
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

TEST_CASE("the flag table has a size limit, values past it are relayed but not kept") {
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

// The same, but the connection first shows a REJOIN_CLAIM with that token.
std::vector<Sent> rejoin_on(Harness& h, ConnectionId id, const std::string& nickname,
                            std::optional<std::uint32_t> address, const RejoinTokenBytes& token) {
  h.clear();
  h.connect(id, address);
  h.receive(id, RejoinClaim{token}, kChannelSession);
  h.receive(id, hello_from(nickname), kChannelSession);
  return h.to(id);
}

// The token connection `id` holds (REQUIREs one).
RejoinTokenBytes token_of(const Harness& h, ConnectionId id) {
  auto it = h.tokens.find(id);
  REQUIRE(it != h.tokens.end());
  return it->second;
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

  auto msgs = rejoin_on(h, 9, "link", kHomePc, token_of(h, 1));
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

TEST_CASE("a rejoining host hands over as on any leave, alone it hosts again with the table") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  h.receive(1, FlagState{0, {FlagEntry{0x1234, 0, FlagKind::boolean, 1}}}, kChannelProgress);
  h.now += 1'500'000;
  auto msgs = rejoin_on(h, 9, "Link", kHomePc, token_of(h, 1));
  CHECK(h.session.host() == std::optional<std::uint8_t>{2});
  CHECK(messages_of<Host>(h, 2) == std::vector<Host>{Host{2}});
  CHECK(messages_of<Host>(h, 9) == std::vector<Host>{Host{2}});
  // The table the old host built is kept and sent to the rejoiner.
  CHECK(h.session.flag_count() == 1);
  CHECK(messages_of<FlagSnapshot>(h, 9).size() == 1);

  Harness solo;
  solo.join(1, "Link", kHomePc);
  solo.now += 1'000'000;
  auto again = rejoin_on(solo, 9, "Link", kHomePc, token_of(solo, 1));
  REQUIRE(std::holds_alternative<Welcome>(again[0].message));
  CHECK(std::get<Welcome>(again[0].message).player_number == 1);
  CHECK(solo.session.host() == std::optional<std::uint8_t>{1});
}

TEST_CASE("a rejoin into a full session takes the old place instead of being refused as full") {
  SessionConfig four;
  four.max_players = 4;
  Harness h(four);
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  h.join(3, "Saria", kOtherPc);
  h.join(4, "Darunia", kOtherPc);
  h.now += 1'000'000;
  auto msgs = rejoin_on(h, 9, "Link", kHomePc, token_of(h, 1));
  REQUIRE(std::holds_alternative<Welcome>(msgs[0].message));
  CHECK(h.session.player_count() == 4);
  // A fifth name is still refused as full.
  CHECK(refused_reason(hello_on(h, 10, "Ruto", kHomePc)) == RefuseReason::server_full);
}

TEST_CASE("a live player's name stays refused, from any PC, another PC never takes over") {
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
  CHECK(refused_reason(rejoin_on(h, 9, "Link", kHomePc, token_of(h, 1))) ==
        RefuseReason::name_taken);
  CHECK(h.session.player_number(1) == std::optional<std::uint8_t>{1});
}

TEST_CASE("issue 163: without the rejoin token a quiet player's place is never taken") {
  Harness h;
  // Two players behind one router: the same public address.
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kHomePc);
  h.now += 5'000'000;  // Link stalls for 5 s; Zelda talks
  h.receive(2, Ping{h.now}, kChannelSession);

  // Zelda's PC (or anyone there) asks for Link's name: no token, refused.
  CHECK(refused_reason(hello_on(h, 9, "Link", kHomePc)) == RefuseReason::name_taken);
  // With Zelda's own token, not Link's: refused too.
  CHECK(refused_reason(rejoin_on(h, 10, "Link", kHomePc, token_of(h, 2))) ==
        RefuseReason::name_taken);
  // A made-up token: refused.
  RejoinTokenBytes guess{};
  guess.fill(0x5A);
  CHECK(refused_reason(rejoin_on(h, 11, "Link", kHomePc, guess)) == RefuseReason::name_taken);
  CHECK(h.session.player_number(1) == std::optional<std::uint8_t>{1});
  CHECK(h.session.stats().rejoined == 0);
  CHECK(h.session.stats().rejoin_no_token == 3);

  // Link's own token from another address: still refused (the address rule stays).
  CHECK(refused_reason(rejoin_on(h, 12, "Link", kOtherPc, token_of(h, 1))) ==
        RefuseReason::name_taken);
  // Link's own token while Link is live: refused.
  h.receive(1, Ping{h.now}, kChannelSession);
  CHECK(refused_reason(rejoin_on(h, 13, "Link", kHomePc, token_of(h, 1))) ==
        RefuseReason::name_taken);
  CHECK(h.session.player_number(1) == std::optional<std::uint8_t>{1});

  // Link quiet again and its own build comes back with its token: it rejoins.
  h.now += 1'000'000;
  h.receive(2, Ping{h.now}, kChannelSession);
  auto msgs = rejoin_on(h, 14, "Link", kHomePc, token_of(h, 1));
  REQUIRE(!msgs.empty());
  REQUIRE(std::holds_alternative<Welcome>(msgs[0].message));
  CHECK(std::get<Welcome>(msgs[0].message).player_number == 1);
  CHECK(h.session.stats().rejoined == 1);
  CHECK(h.session.player_number(2) == std::optional<std::uint8_t>{2});
}

TEST_CASE("issue 163: every WELCOME comes with a fresh token, an old one is spent") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  const RejoinTokenBytes first = token_of(h, 1);
  CHECK(first != token_of(h, 2));
  h.now += 1'000'000;
  auto back = rejoin_on(h, 9, "Link", kHomePc, first);
  REQUIRE(!back.empty());
  REQUIRE(std::holds_alternative<Welcome>(back[0].message));
  const RejoinTokenBytes second = token_of(h, 9);
  CHECK(second != first);
  // The first token no longer takes the place over.
  h.now += 1'000'000;
  CHECK(refused_reason(rejoin_on(h, 10, "Link", kHomePc, first)) == RefuseReason::name_taken);
  CHECK(h.session.player_number(9) == std::optional<std::uint8_t>{1});
}

TEST_CASE("issue 163: a REJOIN_CLAIM after HELLO is dropped and never relayed") {
  Harness h;
  h.join(1, "Link", kHomePc);
  h.join(2, "Zelda", kOtherPc);
  h.clear();
  h.receive(2, RejoinClaim{token_of(h, 1)}, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().stray_rejoin_claim == 1);
  // Who may send which, and on which channel.
  CHECK_FALSE(may_send(Sender::client, MessageType::rejoin_token));
  CHECK_FALSE(may_send(Sender::server, MessageType::rejoin_claim));
  CHECK(allowed_on_channel(MessageType::rejoin_claim, kChannelSession));
  CHECK_FALSE(allowed_on_channel(MessageType::rejoin_claim, kChannelProgress));
}

TEST_CASE("issue 163: a rejoin with a join password shows the token, then the proof") {
  SessionConfig config;
  config.join_password = "kokiri";
  Harness h(config);
  auto join_with_proof = [&](ConnectionId id, const std::string& name,
                             std::optional<RejoinTokenBytes> token) {
    h.clear();
    h.connect(id, kHomePc);
    if (token) h.receive(id, RejoinClaim{*token}, kChannelSession);
    h.receive(id, hello_from(name), kChannelSession);
    const auto challenge = only_message_to<JoinChallenge>(h, id);
    REQUIRE(challenge.has_value());
    h.clear();
    h.receive(id, JoinProof{h.proof("kokiri", challenge->nonce)}, kChannelSession);
    return h.to(id);
  };
  auto first = join_with_proof(1, "Link", std::nullopt);
  REQUIRE(!first.empty());
  REQUIRE(std::holds_alternative<Welcome>(first[0].message));
  h.now += 1'000'000;
  // The password alone does not take the place over (a housemate knows it).
  CHECK(refused_reason(join_with_proof(9, "Link", std::nullopt)) == RefuseReason::name_taken);
  auto with_token = join_with_proof(10, "Link", token_of(h, 1));
  REQUIRE(!with_token.empty());
  REQUIRE(std::holds_alternative<Welcome>(with_token[0].message));
  CHECK(h.session.stats().rejoined == 1);
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

// The owner hold after a (re)start (SessionConfig::owner_hold_us, stray
// 2026-11-12): B came back first and listed the Graveyard first, so it got
// every object A owned before; A's game aborted when its live actors all
// became copies at once.
struct HoldHarness : Harness {
  HoldHarness() : Harness({}, true) {
    join(1, "A");  // player 1 (the old owner, keep-number)
    join(2, "B");  // player 2
    clear();
  }
  void tick(std::uint64_t by) {
    now += by;
    session.tick(now);
    collect();
  }
};

TEST_CASE("after a start, objects get no owner until the hold ends, then the lowest player number") {
  HoldHarness h;
  h.receive(2, present_in("Graveyard", {placed(7), placed(8)}), kChannelOwnership);   // B lists first
  h.now += 20'000;
  h.receive(1, present_in("Graveyard", {placed(7), placed(8)}), kChannelOwnership);   // A, 20 ms later
  CHECK(messages_to<ObjectOwner>(h, 1).empty());   // nobody is told an owner yet
  CHECK(messages_to<ObjectOwner>(h, 2).empty());
  CHECK(h.session.object_owner(placed(7)) == owned(0, 0));
  h.tick(1'000'000);
  CHECK(h.session.object_owner(placed(7)) == owned(0, 0));   // still in the hold
  h.tick(1'000'000);                                          // 2 s after B's listing
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  CHECK(h.session.object_owner(placed(8)) == owned(1, 0));
  // Both games in the Graveyard are told, once, epoch 0: no owner change for anyone.
  for (ConnectionId id : {ConnectionId{1}, ConnectionId{2}}) {
    const auto told = messages_to<ObjectOwner>(h, id);
    REQUIRE(told.size() == 2);
    CHECK(told[0].owner == 1);
    CHECK(told[0].epoch == 0);
  }
  CHECK(h.session.stats().owners_held == 2);
  CHECK(h.session.stats().owner_changes == 0);
}

TEST_CASE("after the hold the old rule: the first game to list a new object owns it at once") {
  HoldHarness h;
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.tick(2'000'000);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 0));   // B alone listed it: B's
  h.clear();
  h.receive(2, present_in("Graveyard", {placed(7), placed(9)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(9)) == owned(2, 0));   // listed after the hold: B's at once
  CHECK(messages_to<ObjectOwner>(h, 2) == std::vector<ObjectOwner>{{placed(9), 2, 0, 0}});
  h.clear();
  h.receive(1, present_in("Graveyard", {placed(7), placed(9)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(9)) == owned(2, 0));   // A only learns it
  CHECK(messages_to<ObjectOwner>(h, 1).size() == 2);
}

TEST_CASE("a held object nobody lists any more at the end of the hold is forgotten, then owned anew") {
  HoldHarness h;
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("Graveyard", {}), kChannelOwnership);   // B's game unloads it
  h.tick(2'000'000);
  CHECK_FALSE(h.session.object_owner(placed(7)).has_value());
  h.receive(1, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
}

TEST_CASE("owner_hold_us 0: the first game to list owns at once, as before the hold") {
  SessionConfig config;
  config.owner_hold_us = 0;
  Harness h(config, true);
  h.join(1, "A");
  h.join(2, "B");
  h.clear();
  h.receive(2, present_in("Graveyard", {placed(7)}), kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 0));
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

TEST_CASE("unloading or leaving the scene hands it over, nobody left forgets it") {
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

TEST_CASE("the owner of a placed object may name its actor to the scene, nobody else may") {
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

TEST_CASE("a made object belongs to its maker, it goes on every screen when the maker leaves") {
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

TEST_CASE("handover state from a client is refused when the server did not ask for it") {
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
  h.receive(1, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  h.clear();

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
  h.receive(1, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  h.receive(1, cut_at("HyruleField_Child", 1), kChannelSession);
  h.receive(1, cut_at("HyruleField_Child", 2), kChannelSession);
  h.receive(1, sample_in("Market"), kChannelPlayerSamples);
  h.receive(1, cut_at("Market", 3), kChannelSession);
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
  h.now += 4'000'000;  // past the server's hold on a scene just left
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
  h.receive(1, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  h.receive(1, cut_at("HyruleField_Child", 1), kChannelSession);
  h.receive(2, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  h.session.disconnected(1, h.now);  // player 2 still shows the cut
  h.receive(3, sample_in("HyruleField_Child"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 3).size() == 1);
  h.session.disconnected(2, h.now);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);  // the field is empty
  h.clear();
  h.now += 4'000'000;  // past the server's hold on a scene just left
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

// ------------------------------------------------- shared world objects

namespace {

WorldBreak broke(const std::string& scene, std::uint64_t hash, std::uint8_t flags = 0) {
  WorldBreak b;
  b.scene = scene;
  b.hash = hash;
  b.position = Vec3{1.0f, 2.0f, 3.0f};
  b.flags = flags;
  return b;
}

WorldPlace rested(const std::string& scene, std::uint64_t hash, float x) {
  WorldPlace p;
  p.scene = scene;
  p.hash = hash;
  p.position = Vec3{x, 0.0f, 0.0f};
  return p;
}

WorldMove moved(const std::string& scene, std::uint64_t hash, float x) {
  WorldMove v;
  v.scene = scene;
  v.hash = hash;
  v.time_us = 5;
  v.position = Vec3{x, 0.0f, 0.0f};
  return v;
}

template <typename T>
T by(std::uint8_t player, T m) {
  m.player_number = player;
  return m;
}

}  // namespace

TEST_CASE("a break goes to the players in its scene once and to those who come later") {
  ObjectHarness h;
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);
  h.clear();

  h.receive(1, broke("Kakariko", 0x11), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 1).empty());
  CHECK(messages_to<WorldBreak>(h, 2) == std::vector<WorldBreak>{by(1, broke("Kakariko", 0x11))});
  CHECK(messages_to<WorldBreak>(h, 3).empty());
  CHECK(h.session.stats().world_relayed == 1);

  // Player 2's game saw its own copy break: the scene has it already.
  h.clear();
  h.receive(2, broke("Kakariko", 0x11), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 1).empty());
  CHECK(h.session.stats().world_repeated == 1);

  // Player 3 walks in and is told; so is a player who joins late.
  h.receive(1, broke("Kakariko", 0x22), kChannelSession);
  h.clear();
  h.receive(3, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 3) == std::vector<WorldBreak>{by(1, broke("Kakariko", 0x11)),
                                                                by(1, broke("Kakariko", 0x22))});
  CHECK(h.session.stats().world_replayed == 2);
  h.join(4, "Ruto");
  h.clear();
  h.receive(4, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 4).size() == 2);
}

TEST_CASE("a late break from the scene just left is passed on there but does not take the player back") {
  ObjectHarness h;
  h.receive(1, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);
  h.receive(1, broke("Kakariko", 0x11), kChannelSession);
  // Player 1 walks into the Market; a break its game made in Kakariko just
  // before arrives after the new scene's sample.
  h.receive(1, sample_in("Market"), kChannelPlayerSamples);
  h.clear();
  h.receive(1, broke("Kakariko", 0x22), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 2) == std::vector<WorldBreak>{by(1, broke("Kakariko", 0x22))});
  CHECK(messages_to<WorldBreak>(h, 3).empty());
  // Still in the Market: a break there reaches player 3.
  h.clear();
  h.receive(1, broke("Market", 0x33), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 3) == std::vector<WorldBreak>{by(1, broke("Market", 0x33))});
  CHECK(messages_to<WorldBreak>(h, 2).empty());
  // And a real walk back to Kakariko is taken at once, with what is kept there.
  h.clear();
  h.receive(1, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 1).size() == 2);
}

TEST_CASE("a late cut or break never takes the player back to the scene it left") {
  // No break or cut from player 1 before: nothing primes the scene it is in.
  ObjectHarness h;
  h.receive(1, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);
  h.receive(1, sample_in("Market"), kChannelPlayerSamples);
  h.clear();
  // Made in Kakariko just before the walk, arriving after the Market sample.
  h.receive(1, cut_at("Kakariko", 1), kChannelSession);
  h.receive(1, broke("Kakariko", 0x22), kChannelSession);
  CHECK(messages_to<GrassCut>(h, 2) == std::vector<GrassCut>{from(1, cut_at("Kakariko", 1))});
  CHECK(messages_to<WorldBreak>(h, 2) == std::vector<WorldBreak>{by(1, broke("Kakariko", 0x22))});
  h.receive(1, sample_in("Market"), kChannelPlayerSamples);
  CHECK(h.session.stats().stale_scenes == 0);
  // Player 1 is in the Market: a break there by player 3 reaches it.
  h.clear();
  h.receive(3, broke("Market", 0x33), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 1) == std::vector<WorldBreak>{by(3, broke("Market", 0x33))});
  CHECK(messages_to<WorldBreak>(h, 2).empty());
}

TEST_CASE("a late cut or break for a scene nobody is in is not kept") {
  ObjectHarness h;
  h.receive(1, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(1, sample_in("Market"), kChannelPlayerSamples);  // Kakariko is empty
  h.receive(1, cut_at("Kakariko", 1), kChannelSession);
  h.receive(1, broke("Kakariko", 0x22), kChannelSession);
  CHECK(h.session.stats().world_unkept == 2);
  h.clear();
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<GrassCut>(h, 2).empty());
  CHECK(messages_to<WorldBreak>(h, 2).empty());
}

TEST_CASE("a break that the game put back is forgotten") {
  ObjectHarness h;
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(1, broke("Kakariko", 0x11), kChannelSession);
  h.clear();
  // Nothing to forget for this one: not passed on.
  h.receive(1, broke("Kakariko", 0x99, world_break::restored), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 2).empty());
  h.receive(1, broke("Kakariko", 0x11, world_break::restored), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 2) ==
        std::vector<WorldBreak>{by(1, broke("Kakariko", 0x11, world_break::restored))});
  h.clear();
  h.receive(3, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 3).empty());
}

TEST_CASE("a lock's break is kept when the scene empties, and a second player's is passed on") {
  ObjectHarness h;
  h.receive(1, sample_in("WaterTemple"), kChannelPlayerSamples);
  h.receive(2, sample_in("WaterTemple"), kChannelPlayerSamples);
  h.clear();
  h.receive(1, broke("WaterTemple", 0x11, world_break::kept), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 2) ==
        std::vector<WorldBreak>{by(1, broke("WaterTemple", 0x11, world_break::kept))});
  // Player 2 opened the same lock at once: the others hear it (one key goes back).
  h.clear();
  h.receive(2, broke("WaterTemple", 0x11, world_break::kept), kChannelSession);
  CHECK(messages_to<WorldBreak>(h, 1) ==
        std::vector<WorldBreak>{by(2, broke("WaterTemple", 0x11, world_break::kept))});
  CHECK(h.session.stats().world_repeated == 1);
  // A pot broken there too. Both leave: the pot is forgotten, the lock is not.
  h.receive(1, broke("WaterTemple", 0x22), kChannelSession);
  h.receive(1, sample_in("LakeHylia"), kChannelPlayerSamples);
  h.receive(2, sample_in("LakeHylia"), kChannelPlayerSamples);
  h.clear();
  h.receive(3, sample_in("WaterTemple"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 3) ==
        std::vector<WorldBreak>{by(1, broke("WaterTemple", 0x11, world_break::kept))});
  // A late joiner who comes in gets it too.
  h.join(4, "Ruto");
  h.clear();
  h.receive(4, sample_in("WaterTemple"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 4).size() == 1);
}

TEST_CASE("kept breaks are forgotten on a host save load and are capped in all") {
  coop::server::WorldStore store;
  CHECK(store.on_break(broke("WaterTemple", 0x11, world_break::kept)));
  CHECK(store.on_break(broke("WaterTemple", 0x22)));
  store.forget("WaterTemple");
  CHECK(store.breaks("WaterTemple") == 1);  // the lock stays when the scene empties
  store.forget_kept();                      // host-save's FLAG_RESET: as the save has it
  CHECK(store.breaks("WaterTemple") == 0);
  CHECK(store.replay("WaterTemple").empty());
  // At most kMaxWorldKept a scene and kMaxWorldKeptTotal in all, whatever the scene names.
  std::size_t taken = 0;
  for (std::uint64_t s = 0; s < 20; ++s) {
    for (std::uint64_t h = 1; h <= 300; ++h) {
      taken += store.on_break(broke("Scene" + std::to_string(s), h, world_break::kept)) ? 1 : 0;
    }
  }
  CHECK(taken == kMaxWorldKeptTotal);
  CHECK(store.breaks("Scene0") == kMaxWorldKept);
  store.forget_kept();
  CHECK(store.on_break(broke("Scene19", 1, world_break::kept)));
}

TEST_CASE("moves are passed on, the last rest place is kept for those who come later") {
  ObjectHarness h;
  h.receive(1, sample_in("ForestTemple"), kChannelPlayerSamples);
  h.receive(2, sample_in("ForestTemple"), kChannelPlayerSamples);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);
  h.clear();

  h.receive(1, moved("ForestTemple", 0x50, 1.0f), kChannelObjectSamples);
  CHECK(messages_to<WorldMove>(h, 2) ==
        std::vector<WorldMove>{by(1, moved("ForestTemple", 0x50, 1.0f))});
  CHECK(messages_to<WorldMove>(h, 3).empty());
  // A move for a scene the sender is not in is dropped.
  h.clear();
  h.receive(1, moved("Market", 0x50, 1.0f), kChannelObjectSamples);
  CHECK(messages_to<WorldMove>(h, 3).empty());

  h.receive(1, rested("ForestTemple", 0x50, 2.0f), kChannelSession);
  h.receive(1, rested("ForestTemple", 0x50, 4.0f), kChannelSession);
  CHECK(messages_to<WorldPlace>(h, 2).size() == 2);
  h.clear();
  h.receive(3, sample_in("ForestTemple"), kChannelPlayerSamples);
  CHECK(messages_to<WorldPlace>(h, 3) ==
        std::vector<WorldPlace>{by(1, rested("ForestTemple", 0x50, 4.0f))});
  CHECK(messages_to<WorldMove>(h, 3).empty());

  // A broken object rests nowhere.
  h.receive(2, broke("ForestTemple", 0x50), kChannelSession);
  h.join(4, "Ruto");
  h.clear();
  h.receive(4, sample_in("ForestTemple"), kChannelPlayerSamples);
  CHECK(messages_to<WorldPlace>(h, 4).empty());
  CHECK(messages_to<WorldBreak>(h, 4).size() == 1);
}

TEST_CASE("the breaks and places of a scene end when its last player leaves") {
  ObjectHarness h;
  h.receive(1, sample_in("Kakariko"), kChannelPlayerSamples);
  h.receive(1, broke("Kakariko", 0x11), kChannelSession);
  h.receive(1, rested("Kakariko", 0x12, 1.0f), kChannelSession);
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 2).size() == 1);
  CHECK(messages_to<WorldPlace>(h, 2).size() == 1);
  h.receive(1, sample_in("Market"), kChannelPlayerSamples);  // player 2 still has them
  h.receive(3, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 3).size() == 1);
  h.session.disconnected(2, h.now);
  h.receive(3, sample_in("Market"), kChannelPlayerSamples);  // Kakariko is empty
  h.clear();
  h.receive(3, sample_in("Kakariko"), kChannelPlayerSamples);
  CHECK(messages_to<WorldBreak>(h, 3).empty());
  CHECK(messages_to<WorldPlace>(h, 3).empty());
}

TEST_CASE("sword hits on a sign are passed on and the last four kept for those who come later") {
  ObjectHarness h;
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  h.clear();
  WorldHit hit;
  hit.scene = "Kakariko";
  hit.hash = 0x70;
  for (int i = 0; i < 6; ++i) {
    hit.at = Vec3{static_cast<float>(i), 1.0f, 2.0f};
    h.receive(1, hit, kChannelSession);
  }
  CHECK(messages_to<WorldHit>(h, 2).size() == 6);
  CHECK(messages_to<WorldHit>(h, 1).empty());
  h.clear();
  h.receive(3, sample_in("Kakariko"), kChannelPlayerSamples);
  const auto got = messages_to<WorldHit>(h, 3);
  REQUIRE(got.size() == kMaxWorldHitsPerObject);
  CHECK(got.front().at[0] == 2.0f);
  CHECK(got.front().player_number == 1);
}

TEST_CASE("a sign's cut words are passed on and the latest kept for those who come later") {
  ObjectHarness h;
  h.receive(2, sample_in("Kakariko"), kChannelPlayerSamples);
  h.clear();
  SignCut cut;
  cut.scene = "Kakariko";
  cut.hash = 0x70;
  cut.words = {{(4ull << 32) | 0x18, 1}};
  h.receive(1, cut, kChannelSession);
  cut.words = {{(4ull << 32) | 0x18, 2}, {(4ull << 32) | 0x20, 3}};
  h.receive(1, cut, kChannelSession);
  CHECK(messages_to<SignCut>(h, 2).size() == 2);
  CHECK(messages_to<SignCut>(h, 1).empty());
  h.clear();
  h.receive(3, sample_in("Kakariko"), kChannelPlayerSamples);
  const auto got = messages_to<SignCut>(h, 3);
  REQUIRE(got.size() == 1);
  CHECK(got.front().words.size() == 2);
  CHECK(got.front().player_number == 1);
}
// ------------------------------------------------------ ownership rules
// docs/objects.md "Enemies": the distance rule, the fight lock, groups, the
// hand-over that carries the state, and what a player entering a scene gets.

namespace {

constexpr std::uint64_t kSecond = 1'000'000;

PlayerSample standing(const std::string& scene, float x) {
  PlayerSample s;
  s.time.server_time_us = 1;
  s.scene = scene;
  s.position = Vec3{x, 0, 0};
  return s;
}

SessionConfig quiet_config() {
  SessionConfig config;
  // The clock runs for a minute in these tests with nobody answering PINGs.
  config.silence_timeout_us = 1'000'000 * kSecond;
  config.ping_interval_us = 1'000'000 * kSecond;
  config.position_stale_us = 1'000'000 * kSecond;
  return config;
}

// Players 1 and 2 in the Field, both with enemy 7 loaded. Player 1 owns it
// and has said it stands at x = 0 with life 12.
struct OwnerHarness : Harness {
  explicit OwnerHarness(SessionConfig config = quiet_config()) : Harness(std::move(config)) {
    join(1, "Link");
    join(2, "Zelda");
    receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
    receive(2, present_in("Field", {placed(7)}), kChannelOwnership);
    receive(1, ObjectSample{0, placed(7), 0, state_at(0, 12)}, kChannelObjectSamples);
    clear();
  }

  void stand(ConnectionId id, float x) {
    receive(id, standing("Field", x), kChannelPlayerSamples);
  }

  // Lets the clock run, ticking every 250 ms as the rules do.
  void run(std::uint64_t us) {
    for (std::uint64_t t = 0; t < us; t += 250'000) {
      now += 250'000;
      session.tick(now);
      collect();
    }
  }

  std::size_t asks() const { return messages_to<HandoverAsk>(*this, 1).size(); }

  // The owner at `owner_x` and the other player at `other_x`, 4 s long:
  // whether the server asked the owner to let go.
  static bool moves(float owner_x, float other_x) {
    OwnerHarness h;
    h.stand(1, owner_x);
    h.stand(2, other_x);
    h.run(4 * kSecond);
    return h.asks() == 1;
  }
};

}  // namespace

TEST_CASE("out of a fight an enemy goes to the player near it after 3 s, with its state") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  h.clear();
  h.run(2'750'000);
  CHECK(h.asks() == 0);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  h.run(500'000);
  REQUIRE(h.asks() == 1);
  CHECK(messages_to<HandoverAsk>(h, 1)[0] == HandoverAsk{placed(7), 0, 2});
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));  // not before the answer

  // The owner's samples still go out while it is asked.
  h.clear();
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(1, 12)}, kChannelObjectSamples);
  CHECK(messages_to<ObjectSample>(h, 2).size() == 1);

  h.clear();
  h.receive(1, HandoverState{0, placed(7), 0, state_at(2, 9)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  const auto to2 = h.to(2);
  REQUIRE(to2.size() == 2);  // the state first, then the news
  CHECK(std::get<HandoverState>(to2[0].message) == HandoverState{1, placed(7), 1, state_at(2, 9)});
  CHECK(std::get<ObjectOwner>(to2[1].message) == ObjectOwner{placed(7), 2, 1, 0});
  CHECK(messages_to<ObjectOwner>(h, 1) == std::vector<ObjectOwner>{{placed(7), 2, 1, 0}});
  CHECK(messages_to<HandoverState>(h, 1).empty());

  // A late sample from the old owner, or one with the old epoch, is dropped.
  h.clear();
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(3, 9)}, kChannelObjectSamples);
  h.receive(1, ObjectSample{0, placed(7), 1, state_at(3, 9)}, kChannelObjectSamples);
  h.receive(2, ObjectSample{0, placed(7), 0, state_at(3, 9)}, kChannelObjectSamples);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().object_refused == 3);
  h.receive(2, ObjectSample{0, placed(7), 1, state_at(3, 9)}, kChannelObjectSamples);
  CHECK(messages_to<ObjectSample>(h, 1).size() == 1);

  // It does not go straight back: 5 s must pass after a change, then 3 s.
  h.stand(1, 5);
  h.stand(2, 50);
  h.clear();
  h.run(4 * kSecond);
  CHECK(messages_to<HandoverAsk>(h, 2).empty());
  h.run(5 * kSecond);
  CHECK(messages_to<HandoverAsk>(h, 2) == std::vector<HandoverAsk>{{placed(7), 1, 1}});
}

TEST_CASE("the distance rule: within 30 m, 10 m nearer, the owner far or the other close") {
  CHECK(OwnerHarness::moves(50, 5));       // the plain case
  CHECK(OwnerHarness::moves(45, 28));      // owner over 40 m: 30 m is near enough
  CHECK(OwnerHarness::moves(34, 5));       // issue 101: owner 34 m off, the other chased
  CHECK_FALSE(OwnerHarness::moves(12, 5));   // only 7 m nearer
  CHECK_FALSE(OwnerHarness::moves(35, 24));  // owner within 40 m, the other not within 20 m
  CHECK_FALSE(OwnerHarness::moves(100, 31)); // nobody within 30 m
  CHECK_FALSE(OwnerHarness::moves(5, 50));   // the owner is the near one
}

TEST_CASE("a player who walks off again before the 3 s are over does not get it") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  h.run(2 * kSecond);
  h.stand(2, 45);
  h.run(kSecond);
  h.stand(2, 5);
  h.run(2 * kSecond);  // near again, but the 3 s start over
  CHECK(h.asks() == 0);
  h.run(1'500'000);
  CHECK(h.asks() == 1);
}

TEST_CASE("an owner that does not answer: it moves after 500 ms with the last relayed sample") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  h.run(3'250'000);
  REQUIRE(h.asks() == 1);
  h.clear();
  h.run(250'000);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  h.run(250'000);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  CHECK(h.session.stats().handover_unanswered == 1);
  CHECK(messages_to<HandoverState>(h, 2) ==
        std::vector<HandoverState>{{1, placed(7), 1, state_at(0, 12)}});
  // The answer, when it comes after all, is refused.
  h.receive(1, HandoverState{0, placed(7), 0, state_at(2, 9)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
}

TEST_CASE("a hand-over is called off when the player it was for leaves the scene") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  h.run(3'250'000);
  REQUIRE(h.asks() == 1);
  h.receive(2, standing("Kakariko", 0), kChannelPlayerSamples);
  h.receive(1, HandoverState{0, placed(7), 0, state_at(2, 9)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
}

TEST_CASE("a fight keeps an enemy with its owner for 12 s after the last sign of it") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  SUBCASE("its life changed") {
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);
  }
  SUBCASE("its owner says it is fighting") {
    ObjectPresent p = present_in("Field", {placed(7)});
    p.objects[0].flags = object_presence::in_fight;
    h.receive(1, p, kChannelOwnership);
  }
  SUBCASE("its owner's sample says so") {
    ObjectState s = state_at(0, 12);
    s.state = object_state::in_fight;
    h.receive(1, ObjectSample{0, placed(7), 0, s}, kChannelObjectSamples);
  }
  CHECK(h.session.object_in_fight(placed(7), h.now));
  h.run(11'500'000);
  CHECK(h.asks() == 0);
  CHECK(h.session.object_in_fight(placed(7), h.now));
  h.run(kSecond);
  CHECK_FALSE(h.session.object_in_fight(placed(7), h.now));
  h.run(3 * kSecond);  // the fight is over: the 3 s of the distance rule
  CHECK(h.asks() == 1);
}

TEST_CASE("another player's fight flags do not start a fight") {
  OwnerHarness h;
  ObjectPresent p = present_in("Field", {placed(7)});
  p.objects[0].flags = object_presence::in_fight | object_presence::targeting;
  h.receive(2, p, kChannelOwnership);
  CHECK_FALSE(h.session.object_in_fight(placed(7), h.now));
}

SessionConfig hit_handover_config() {  // the boss rule before gohma-sync
  SessionConfig config = quiet_config();
  config.boss_hit_handover = true;
  config.boss_near_hold_us = 4'000'000;
  return config;
}

SessionConfig near_config() {  // the near-player rule, off by default since gohma-sync
  SessionConfig config = quiet_config();
  config.boss_near_hold_us = 4'000'000;
  return config;
}

TEST_CASE("a boss stays in its fight for 60 s") {
  SessionConfig config = quiet_config();
  config.boss_near_hold_us = 0;  // the near-player rule below is its own case
  OwnerHarness h(config);
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  CHECK(h.session.object_group(placed(7)) == 0u);  // alone: no group
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);
  h.stand(1, 50);
  h.stand(2, 5);
  h.run(59 * kSecond);
  CHECK(h.session.object_in_fight(placed(7), h.now));
  CHECK(h.asks() == 0);
  h.run(2 * kSecond);
  CHECK_FALSE(h.session.object_in_fight(placed(7), h.now));
}

TEST_CASE("issue 150: a boss goes to the player who hurt it, at most every 8 s") {
  OwnerHarness h(hit_handover_config());
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  h.stand(1, 4);  // both in the boss room, a few metres apart
  h.stand(2, 6);
  h.clear();
  h.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
  const auto to1 = h.to(1);
  REQUIRE(to1.size() == 2);  // the hit first, then the ask
  CHECK(std::get<Damage>(to1[0].message).amount == 1);
  CHECK(std::get<HandoverAsk>(to1[1].message) == HandoverAsk{placed(7), 0, 2});
  CHECK(h.session.stats().boss_to_hitter == 1);
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  // Player 1 hits back at once: the hit goes to player 2, the boss stays there.
  h.clear();
  h.receive(1, Damage{0, placed(7), 1, 1, 1, {}}, kChannelOwnership);
  CHECK(messages_to<Damage>(h, 2).size() == 1);
  CHECK(messages_to<HandoverAsk>(h, 2).empty());
  // 8 s after the change, player 1's hit takes it back.
  h.run(8 * kSecond);
  h.clear();
  h.receive(1, Damage{0, placed(7), 1, 1, 1, {}}, kChannelOwnership);
  CHECK(messages_to<HandoverAsk>(h, 2) == std::vector<HandoverAsk>{{placed(7), 1, 1}});
  CHECK(h.session.stats().boss_to_hitter == 2);
}

TEST_CASE("issue 150: two players hitting in turn do not bounce the boss") {
  OwnerHarness h(hit_handover_config());
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  h.stand(1, 4);
  h.stand(2, 6);
  // 45 s of turns: the owner hits (its life falls in its samples), 1 s later
  // player 2 hits, 2 s after that the owner again; the boss stays.
  std::int32_t life = 100;  // enough for every round's two hits
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, life)}, kChannelObjectSamples);
  for (int round = 0; round < 15; ++round) {
    --life;
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, life)}, kChannelObjectSamples);
    h.run(kSecond);
    h.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
    h.run(250'000);
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, --life)}, kChannelObjectSamples);  // 2's hit taken
    h.run(1'750'000);  // the owner's next own hit is well past the hit window
  }
  CHECK(h.session.stats().boss_to_hitter == 0);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  // The owner stops hitting: 6 s later player 2's hit takes it.
  h.run(6 * kSecond);
  h.clear();
  h.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});
  CHECK(h.session.stats().boss_to_hitter == 1);
}

TEST_CASE("issue 150: a hit on a boss's part asks for the boss, plain enemies keep the distance rule") {
  OwnerHarness h(hit_handover_config());
  h.receive(1, present_in("Field", {placed(7), placed(8)}), kChannelOwnership);
  h.receive(2, present_in("Field", {placed(7), placed(8)}), kChannelOwnership);
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}, {placed(8), placed(7), 0}}},
            kChannelOwnership);
  h.stand(1, 4);
  h.stand(2, 6);
  h.clear();
  h.receive(2, Damage{0, placed(8), 0, 1, 1, {}}, kChannelOwnership);  // the part (an egg)
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});

  OwnerHarness plain;  // no boss: two players near it, no move on a hit
  plain.stand(1, 4);
  plain.stand(2, 6);
  plain.clear();
  plain.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
  CHECK(plain.asks() == 0);
  CHECK(plain.session.stats().boss_to_hitter == 0);

  SessionConfig off = hit_handover_config();  // the switch
  off.boss_hitter_cooldown_us = 0;
  OwnerHarness h0(off);
  h0.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  h0.stand(1, 4);
  h0.stand(2, 6);
  h0.clear();
  h0.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
  CHECK(h0.asks() == 0);
}

TEST_CASE("issue 150: a boss in its fight goes to the player clearly nearer it, held 4 s") {
  OwnerHarness h(near_config());
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);  // a fight
  REQUIRE(h.session.object_in_fight(placed(7), h.now));
  h.stand(1, 12);  // the owner by the wall
  h.stand(2, 3);   // player 2 at the boss
  h.clear();
  // That life drop was the owner's own hit: 6 s of hold first.
  h.run(6 * kSecond);
  CHECK(h.asks() == 0);
  h.run(3 * kSecond + 500'000);
  CHECK(h.asks() == 0);
  h.run(kSecond);
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});
  CHECK(h.session.stats().boss_to_near == 1);
}

TEST_CASE("issue 150: after a near-player move the boss stays 8 s, then needs the 4 m margin again") {
  // Handed to player 2 by the near rule.
  auto handed = [](OwnerHarness& h) {
    h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);
    h.stand(1, 12);
    h.stand(2, 3);
    h.run(10 * kSecond + 500'000);
    REQUIRE(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});
    h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 11)}, kChannelOwnership);
    REQUIRE(h.session.object_owner(placed(7)) == owned(2, 1));
    h.clear();
  };
  {  // the players swap places: not back before 8 s + 4 s held
    OwnerHarness h(near_config());
    handed(h);
    h.stand(1, 1);
    h.stand(2, 12);
    h.run(7 * kSecond + 500'000);
    CHECK(messages_to<HandoverAsk>(h, 2).empty());
    h.run(4 * kSecond);
    CHECK(messages_to<HandoverAsk>(h, 2).empty());
    h.run(kSecond);
    CHECK(messages_to<HandoverAsk>(h, 2) == std::vector<HandoverAsk>{{placed(7), 1, 1}});
  }
  {  // player 1 comes back, but only 3 m nearer than the new owner: it stays
    OwnerHarness h(near_config());
    handed(h);
    h.stand(1, 1);
    h.stand(2, 4);
    h.run(20 * kSecond);
    CHECK(messages_to<HandoverAsk>(h, 2).empty());
    CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  }
}

TEST_CASE("issue 150: no near-player move when both stand close, one is too far, or the owner hits it") {
  {  // 2 is nearer, but not by 4 m
    OwnerHarness h(near_config());
    h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);
    h.stand(1, 6);
    h.stand(2, 3);
    h.run(10 * kSecond);
    CHECK(h.asks() == 0);
  }
  {  // 2 is over 15 m from it
    OwnerHarness h(near_config());
    h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);
    h.stand(1, 40);
    h.stand(2, 20);
    h.run(10 * kSecond);
    CHECK(h.asks() == 0);
  }
  {  // the owner keeps hitting it (its life falls in its samples)
    OwnerHarness h(near_config());
    h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
    h.stand(1, 12);
    h.stand(2, 3);
    std::int32_t life = 40;
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, life)}, kChannelObjectSamples);
    for (int i = 0; i < 5; ++i) {
      h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, --life)}, kChannelObjectSamples);
      h.run(2 * kSecond);
    }
    CHECK(h.asks() == 0);
  }
  {  // the switch
    SessionConfig off = near_config();
    off.boss_near_hold_us = 0;
    OwnerHarness h(off);
    h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
    h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);
    h.stand(1, 12);
    h.stand(2, 3);
    h.run(10 * kSecond);
    CHECK(h.asks() == 0);
  }
}

TEST_CASE("gohma-sync: by default a boss hit that takes life stays with the owner, a stun hands it over") {
  OwnerHarness h;  // the defaults
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);  // a fight
  h.stand(1, 12);  // the owner by the wall, player 2 at the boss: no near-player move
  h.stand(2, 3);
  h.clear();
  h.receive(2, Damage{0, placed(7), 0, 2, 1, {}}, kChannelOwnership);
  // The hit goes to the owner, who takes it off its boss; no ask.
  REQUIRE(messages_to<Damage>(h, 1).size() == 1);
  CHECK(messages_to<Damage>(h, 1)[0].amount == 2);
  CHECK(h.asks() == 0);
  h.run(30 * kSecond);  // hits every few seconds, player 2 standing nearer all along
  for (int i = 0; i < 5; ++i) {
    h.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
    h.run(3 * kSecond);
  }
  CHECK(h.asks() == 0);
  CHECK(h.session.stats().boss_to_hitter == 0);
  CHECK(h.session.stats().boss_to_near == 0);
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  // Player 2's seed stuns its copy (DAMAGE 0): that one hands the boss over.
  h.clear();
  h.receive(2, Damage{0, placed(7), 0, 0, 0, {}}, kChannelOwnership);
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});
  CHECK(h.session.stats().boss_to_hitter == 1);
}

TEST_CASE("issue 150: a DAMAGE of 0 (a stun in the hitter's game) asks for the boss, harmless to the owner") {
  OwnerHarness h;
  h.receive(1, ObjectGroup{0, {{placed(7), placed(7), object_group::boss}}}, kChannelOwnership);
  h.stand(1, 4);
  h.stand(2, 6);
  h.clear();
  h.receive(2, Damage{0, placed(7), 0, 0, 0, {}}, kChannelOwnership);
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  CHECK(messages_to<HandoverState>(h, 2) ==
        std::vector<HandoverState>{{1, placed(7), 1, state_at(0, 12)}});
}

TEST_CASE("a hit by the player near it, its owner well away, takes it at once, the hit is kept") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 2);
  h.clear();
  h.receive(2, Damage{0, placed(7), 0, 3, 1, {}}, kChannelOwnership);
  const auto to1 = h.to(1);
  REQUIRE(to1.size() == 2);  // the hit first, then the ask
  CHECK(std::get<Damage>(to1[0].message).amount == 3);
  CHECK(std::get<HandoverAsk>(to1[1].message) == HandoverAsk{placed(7), 0, 2});
  CHECK(h.session.object_in_fight(placed(7), h.now));
  // The owner answers before its game took the life off: the new owner
  // still gets 12 - 3.
  h.clear();
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  CHECK(messages_to<HandoverState>(h, 2) ==
        std::vector<HandoverState>{{1, placed(7), 1, state_at(0, 9)}});
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
}

TEST_CASE("an answer that already has the hit in it is not lowered twice") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 2);
  h.receive(2, Damage{0, placed(7), 0, 3, 1, {}}, kChannelOwnership);
  h.clear();
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 8)}, kChannelOwnership);
  CHECK(messages_to<HandoverState>(h, 2) ==
        std::vector<HandoverState>{{1, placed(7), 1, state_at(0, 8)}});
}

TEST_CASE("hits by a player in the same fight as the owner do not move the enemy") {
  OwnerHarness h;
  h.stand(1, 3);
  h.stand(2, 2);
  h.clear();
  for (int i = 0; i < 5; ++i) h.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
  CHECK(messages_to<Damage>(h, 1).size() == 5);
  CHECK(h.asks() == 0);
  h.run(11 * kSecond);  // and the fight they started keeps it there
  CHECK(h.asks() == 0);
}

// Issue 101: an enemy chased player 2 from afar in 2's game; its owner's
// enemy (at x = 0) stayed home, so no distance said 2 was near it.
namespace {
ObjectPresent chasing_in(const std::string& scene, std::vector<ObjectId> ids) {
  ObjectPresent p = present_in(scene, std::move(ids));
  for (ObjectPresence& e : p.objects) e.flags = object_presence::awake | object_presence::targeting;
  return p;
}
}  // namespace

TEST_CASE("issue 101: a copy that chases its player gives that player the enemy, wherever it is") {
  OwnerHarness h;
  h.stand(1, 34);
  h.stand(2, 60);  // led its copy far off
  h.run(4 * kSecond);
  CHECK(h.asks() == 0);  // no distance rule can see it
  h.receive(2, chasing_in("Field", {placed(7)}), kChannelOwnership);
  h.run(250'000);
  CHECK(h.asks() == 0);
  h.run(500'000);
  REQUIRE(h.asks() == 1);
  CHECK(messages_to<HandoverAsk>(h, 1)[0] == HandoverAsk{placed(7), 0, 2});
  CHECK(h.session.stats().handover_chased == 1);
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));

  // Now the owner, 2 says the same: a fight, it stays. 1's copy chasing 1
  // later does not pull it back while 2's own enemy chases 2.
  h.receive(2, chasing_in("Field", {placed(7)}), kChannelOwnership);
  h.receive(1, chasing_in("Field", {placed(7)}), kChannelOwnership);
  h.clear();
  h.run(10 * kSecond);
  CHECK(messages_to<HandoverAsk>(h, 2).empty());
}

TEST_CASE("issue 101: a chase shorter than 500 ms, or one the owner's enemy shares, moves nothing") {
  {
    OwnerHarness h;
    h.stand(1, 34);
    h.stand(2, 60);
    h.receive(2, chasing_in("Field", {placed(7)}), kChannelOwnership);
    h.run(250'000);
    h.receive(2, present_in("Field", {placed(7)}), kChannelOwnership);
    h.run(4 * kSecond);
    CHECK(h.asks() == 0);
  }
  {
    // Both games' enemies chase their own Link: the owner's report is a fight.
    OwnerHarness h;
    h.stand(1, 3);
    h.stand(2, 4);
    h.receive(1, chasing_in("Field", {placed(7)}), kChannelOwnership);
    h.receive(2, chasing_in("Field", {placed(7)}), kChannelOwnership);
    h.run(4 * kSecond);
    CHECK(h.asks() == 0);
    CHECK(h.session.object_in_fight(placed(7), h.now));
  }
  {
    // A chaser that unloaded it is not given it.
    OwnerHarness h;
    h.stand(1, 34);
    h.stand(2, 60);
    h.receive(2, chasing_in("Field", {placed(7)}), kChannelOwnership);
    h.receive(2, present_in("Field", {}), kChannelOwnership);
    h.run(4 * kSecond);
    CHECK(h.asks() == 0);
  }
}

TEST_CASE("issue 101: a chase of a group's part moves the root, and the part with it") {
  OwnerHarness h;
  h.receive(1, present_in("Field", {placed(7), placed(8)}), kChannelOwnership);
  h.receive(2, present_in("Field", {placed(7), placed(8)}), kChannelOwnership);
  h.receive(1, ObjectGroup{0, {{placed(8), placed(7), 0}}}, kChannelOwnership);
  h.stand(1, 34);
  h.stand(2, 60);
  h.clear();
  ObjectPresent p = present_in("Field", {placed(7), placed(8)});
  p.objects[1].flags = object_presence::targeting;
  h.receive(2, p, kChannelOwnership);
  h.run(750'000);
  auto asks = messages_to<HandoverAsk>(h, 1);
  REQUIRE(asks.size() == 1);
  CHECK(asks[0] == HandoverAsk{placed(7), 0, 2});
  // The owner does not answer: the root moves, then the part follows it.
  h.run(kSecond);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  asks = messages_to<HandoverAsk>(h, 1);
  REQUIRE(asks.size() == 2);
  CHECK(asks[1] == HandoverAsk{placed(8), 0, 2});
}

TEST_CASE("issue 101: a hit where the hitter's copy stands, far from the owner's enemy, takes it") {
  {
    OwnerHarness h;
    h.stand(1, 34);
    h.stand(2, 60);
    h.clear();
    h.receive(2, Damage{0, placed(7), 0, 1, 1, Vec3{59, 0, 1}}, kChannelOwnership);
    CHECK(h.asks() == 1);
  }
  {
    // Without where it hit, 2 is 60 m from the owner's enemy: nothing moves.
    OwnerHarness h;
    h.stand(1, 34);
    h.stand(2, 60);
    h.clear();
    h.receive(2, Damage{0, placed(7), 0, 1, 1, {}}, kChannelOwnership);
    CHECK(h.asks() == 0);
  }
  {
    // The owner fighting its own enemy near it keeps it.
    OwnerHarness h;
    h.stand(1, 3);
    h.stand(2, 30);
    h.clear();
    h.receive(2, Damage{0, placed(7), 0, 1, 1, Vec3{29, 0, 0}}, kChannelOwnership);
    CHECK(h.asks() == 0);
  }
}

TEST_CASE("a kill by the old owner that crossed the hand-over still counts, for 2 s") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  h.run(3'250'000);
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 1)}, kChannelOwnership);
  REQUIRE(h.session.object_owner(placed(7)) == owned(2, 1));
  h.clear();
  SUBCASE("in time") {
    h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::dead, {1, 2, 3}}, kChannelOwnership);
    CHECK(messages_to<ObjectRemove>(h, 2) ==
          std::vector<ObjectRemove>{{1, placed(7), 1, RemoveReason::dead, {1, 2, 3}}});
    CHECK(h.session.object_owner(placed(7)) == owned(0, 1));
    CHECK(h.session.stats().late_removes == 1);
  }
  SUBCASE("too late, or not a kill") {
    h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::gone, {}}, kChannelOwnership);
    h.run(2'250'000);
    h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::dead, {}}, kChannelOwnership);
    CHECK(messages_to<ObjectRemove>(h, 2).empty());
    CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
    CHECK(h.session.stats().object_refused == 2);
  }
}

TEST_CASE("an owner that is loading, paused 6 s or dead 10 s gives it up, even in a fight") {
  OwnerHarness h;
  h.receive(1, ObjectSample{0, placed(7), 0, state_at(0, 11)}, kChannelObjectSamples);  // a fight
  SUBCASE("loading") {
    h.receive(1, PlayerStatus{0, status::loading}, kChannelSession);
    h.run(250'000);
    CHECK(h.asks() == 1);
  }
  SUBCASE("paused") {
    h.receive(1, PlayerStatus{0, status::paused}, kChannelSession);
    h.run(5'750'000);
    CHECK(h.asks() == 0);
    h.run(500'000);
    CHECK(h.asks() == 1);
  }
  SUBCASE("a pause that ends in time") {
    h.receive(1, PlayerStatus{0, status::paused}, kChannelSession);
    h.run(5 * kSecond);
    h.receive(1, PlayerStatus{0, 0}, kChannelSession);
    h.run(5 * kSecond);
    CHECK(h.asks() == 0);
  }
  SUBCASE("dead") {
    h.receive(1, PlayerStatus{0, status::dead}, kChannelSession);
    h.run(9'750'000);
    CHECK(h.asks() == 0);
    h.run(500'000);
    CHECK(h.asks() == 1);
  }
  SUBCASE("nobody else can play: it stays") {
    h.receive(2, PlayerStatus{0, status::paused}, kChannelSession);
    h.receive(1, PlayerStatus{0, status::loading}, kChannelSession);
    h.run(kSecond);
    CHECK(h.asks() == 0);
  }
}

TEST_CASE("a paused player is not given an enemy by the distance rule") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);
  h.receive(2, PlayerStatus{0, status::paused}, kChannelSession);
  h.run(5 * kSecond);
  CHECK(h.asks() == 0);
}

TEST_CASE("when the owner leaves, the nearest player who has it takes it") {
  OwnerHarness h;
  h.join(3, "Saria");
  h.receive(3, present_in("Field", {placed(7)}), kChannelOwnership);
  h.stand(2, 40);
  h.stand(3, 4);
  h.session.disconnected(1, h.now);
  CHECK(h.session.object_owner(placed(7)) == owned(3, 1));
}

TEST_CASE("a group: the parts follow the root's owner and share its fight") {
  OwnerHarness h;
  h.receive(1, present_in("Field", {placed(7), placed(8)}), kChannelOwnership);
  h.receive(2, present_in("Field", {placed(7), placed(8)}), kChannelOwnership);
  h.clear();
  h.receive(1, ObjectGroup{0, {{placed(8), placed(7), 0}}}, kChannelOwnership);
  const std::uint32_t group = h.session.object_group(placed(7)).value_or(0);
  CHECK(group != 0);
  CHECK(h.session.object_group(placed(8)) == group);
  CHECK(h.session.stats().group_links == 1);
  // Everyone in the scene learns the group of both; the message is not relayed.
  CHECK(messages_to<ObjectOwner>(h, 2) ==
        std::vector<ObjectOwner>{{placed(7), 1, 0, group}, {placed(8), 1, 0, group}});
  CHECK(messages_to<ObjectGroup>(h, 2).empty());

  // A hit on the part is the root's fight; it does not move the part alone.
  h.stand(1, 50);
  h.stand(2, 2);
  h.receive(1, ObjectSample{0, placed(8), 0, state_at(0, 5)}, kChannelObjectSamples);
  h.clear();
  h.receive(2, Damage{0, placed(8), 0, 1, 1, {}}, kChannelOwnership);
  CHECK(h.asks() == 0);
  CHECK(h.session.object_in_fight(placed(7), h.now));

  // The root moves (its owner is loading): the part is asked for next.
  h.receive(1, PlayerStatus{0, status::loading}, kChannelSession);
  h.clear();
  h.run(250'000);
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(7), 0, 2}});
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  h.clear();
  h.run(250'000);
  CHECK(messages_to<HandoverAsk>(h, 1) == std::vector<HandoverAsk>{{placed(8), 0, 2}});
  h.receive(1, HandoverState{0, placed(8), 0, state_at(0, 5)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  CHECK(h.session.object_owner(placed(8)) == owned(2, 1));
  CHECK(messages_to<ObjectOwner>(h, 1).back() == ObjectOwner{placed(8), 2, 1, group});
}

TEST_CASE("group entries that make no sense are turned away, a root's root is the group's") {
  OwnerHarness h;
  h.receive(1, present_in("Field", {placed(7), placed(8), placed(9)}), kChannelOwnership);
  h.receive(1, ObjectGroup{0, {{placed(8), placed(7), 0}}}, kChannelOwnership);
  h.receive(1, ObjectGroup{0, {{placed(7), placed(8), 0}}}, kChannelOwnership);    // a loop
  h.receive(1, ObjectGroup{0, {{placed(9), placed(55), 0}}}, kChannelOwnership);   // no such root
  h.receive(2, ObjectGroup{0, {{placed(9), placed(7), 0}}}, kChannelOwnership);    // not loaded by 2
  CHECK(h.session.stats().group_refused == 3);
  CHECK(h.session.stats().dropped() == 3);
  h.receive(1, ObjectGroup{0, {{placed(9), placed(8), 0}}}, kChannelOwnership);    // 8 follows 7
  CHECK(h.session.object_group(placed(9)) == h.session.object_group(placed(7)));
  // An entry naming itself leaves the group; a forgotten root frees its parts.
  h.receive(1, ObjectGroup{0, {{placed(9), placed(9), 0}}}, kChannelOwnership);
  CHECK(h.session.object_group(placed(9)) == 0u);
  h.receive(1, present_in("Field", {placed(8), placed(9)}), kChannelOwnership);
  h.receive(2, present_in("Field", {}), kChannelOwnership);
  CHECK_FALSE(h.session.object_owner(placed(7)).has_value());
  CHECK(h.session.object_group(placed(8)) == 0u);
}

TEST_CASE("a player entering a scene later gets the spawns still alive in it") {
  OwnerHarness h;
  const ObjectId stalchild{ObjectKind::made, 1, 1};
  const ObjectId dead{ObjectKind::made, 1, 2};
  const ObjectId arrow{ObjectKind::made, 1, 3};
  h.receive(1, ObjectSpawn{0, stalchild, "Enemy_Stalchild", 0, state_at(3, 2)}, kChannelOwnership);
  h.receive(1, ObjectSpawn{0, dead, "Enemy_Stalchild", 0, state_at(3, 2)}, kChannelOwnership);
  h.receive(1, ObjectSpawn{0, arrow, "NormalArrow", object_spawn::projectile, state_at(3, 1)},
            kChannelOwnership);
  h.receive(1, ObjectSpawn{0, placed(7), "Enemy_Skulltula_Gold", 0, state_at(0, 12)},
            kChannelOwnership);
  h.receive(1, ObjectSample{0, stalchild, 0, state_at(4, 2)}, kChannelObjectSamples);
  h.receive(1, ObjectRemove{0, dead, 0, RemoveReason::dead, {}}, kChannelOwnership);

  h.join(3, "Saria");
  h.clear();
  h.receive(3, standing("Field", 0), kChannelPlayerSamples);
  CHECK(messages_to<ObjectSpawn>(h, 3).empty());  // not before its game lists the scene
  h.receive(3, present_in("Field", {}), kChannelOwnership);
  CHECK(messages_to<ObjectSpawn>(h, 3) ==
        std::vector<ObjectSpawn>{
            {1, placed(7), "Enemy_Skulltula_Gold", 0, state_at(0, 12)},
            {1, stalchild, "Enemy_Stalchild", 0, state_at(4, 2)},  // where it is now
            {1, arrow, "NormalArrow", object_spawn::projectile, state_at(3, 1)}});
  CHECK(h.session.stats().spawns_replayed == 3);
  // Once: the next list brings nothing, and its maker never gets its own.
  h.clear();
  h.receive(3, present_in("Field", {}), kChannelOwnership);
  h.receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectSpawn>(h, 3).empty());
  CHECK(messages_to<ObjectSpawn>(h, 1).empty());
}

TEST_CASE("a made enemy goes on with a player who has its copy when its owner leaves") {
  OwnerHarness h;
  const ObjectId stalchild{ObjectKind::made, 1, 1};
  h.receive(1, ObjectSpawn{0, stalchild, "Enemy_Stalchild", 0, state_at(3, 2)}, kChannelOwnership);
  h.receive(2, present_in("Field", {placed(7), stalchild}), kChannelOwnership);
  h.clear();
  SUBCASE("the owner changes scene") {
    h.receive(1, standing("Kakariko", 0), kChannelPlayerSamples);
  }
  SUBCASE("the owner stops listing it and sends no remove: after 1 s") {
    h.receive(1, present_in("Field", {placed(7), stalchild}), kChannelOwnership);
    h.receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
    h.run(750'000);
    CHECK(h.session.object_owner(stalchild) == owned(1, 0));
    h.run(500'000);
  }
  CHECK(h.session.object_owner(stalchild) == owned(2, 1));
  CHECK(messages_to<ObjectRemove>(h, 2).empty());
  CHECK(messages_to<HandoverState>(h, 2).back() == HandoverState{1, stalchild, 1, state_at(3, 2)});
  CHECK(messages_to<ObjectOwner>(h, 2).back() == ObjectOwner{stalchild, 2, 1, 0});

  // A player entering now learns that its maker no longer owns it.
  h.join(3, "Saria");
  h.clear();
  h.receive(3, present_in("Field", {}), kChannelOwnership);
  CHECK(messages_to<ObjectSpawn>(h, 3) ==
        std::vector<ObjectSpawn>{{2, stalchild, "Enemy_Stalchild", 0, state_at(3, 2)}});
  CHECK(messages_to<ObjectOwner>(h, 3) == std::vector<ObjectOwner>{{stalchild, 2, 1, 0}});
}

TEST_CASE("the owner's remove right after it stops listing its made enemy removes it") {
  OwnerHarness h;
  const ObjectId stalchild{ObjectKind::made, 1, 1};
  h.receive(1, ObjectSpawn{0, stalchild, "Enemy_Stalchild", 0, state_at(3, 2)}, kChannelOwnership);
  h.receive(1, present_in("Field", {placed(7), stalchild}), kChannelOwnership);
  h.receive(2, present_in("Field", {placed(7), stalchild}), kChannelOwnership);
  h.clear();
  h.receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
  h.receive(1, ObjectRemove{0, stalchild, 0, RemoveReason::dead, {}}, kChannelOwnership);
  h.run(2 * kSecond);
  CHECK_FALSE(h.session.object_owner(stalchild).has_value());
  CHECK(messages_to<ObjectRemove>(h, 2).size() == 1);
  CHECK(messages_to<ObjectOwner>(h, 2).empty());
}

TEST_CASE("a late message does not take back a scene the player left, a real return does") {
  OwnerHarness h;
  h.receive(1, standing("Kakariko", 0), kChannelPlayerSamples);
  REQUIRE(h.session.object_owner(placed(7)) == owned(2, 1));
  h.clear();
  // Its object thread still lists the Field; its samples already say Kakariko.
  h.receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
  h.receive(1, standing("Kakariko", 0), kChannelPlayerSamples);
  h.receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
  CHECK(h.session.stats().stale_scenes == 2);
  CHECK(messages_to<ObjectOwner>(h, 1).empty());
  CHECK(messages_to<ObjectOwner>(h, 2).empty());
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
  // A real walk back is taken at once, however soon: its samples say so.
  h.receive(1, standing("Field", 3), kChannelPlayerSamples);
  h.receive(1, present_in("Field", {placed(7)}), kChannelOwnership);
  CHECK(h.session.stats().stale_scenes == 2);
  CHECK(messages_to<ObjectOwner>(h, 1) == std::vector<ObjectOwner>{{placed(7), 2, 1, 0}});
  // And the other way round: the list moves on first, a late sample repeats.
  h.clear();
  h.receive(1, present_in("Kakariko", {}), kChannelOwnership);
  h.receive(1, standing("Field", 3), kChannelPlayerSamples);
  CHECK(h.session.stats().stale_scenes == 3);
  h.receive(2, ObjectSample{0, placed(7), 1, state_at(1, 12)}, kChannelObjectSamples);
  CHECK(messages_to<ObjectSample>(h, 1).empty());  // player 1 is in Kakariko
}

TEST_CASE("owners are said again every 10 s, for a game that forgot them") {
  OwnerHarness h;
  h.run(9 * kSecond);
  h.receive(2, present_in("Field", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectOwner>(h, 2).empty());
  h.run(kSecond);
  h.receive(2, present_in("Field", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectOwner>(h, 2) == std::vector<ObjectOwner>{{placed(7), 1, 0, 0}});
  CHECK(h.session.stats().owners_refreshed == 1);
  h.clear();
  h.receive(2, present_in("Field", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectOwner>(h, 2).empty());
}

// Issue 133 (Discord testers): A killed a Deku Tree room, B came in
// later and found the enemies still standing. The server had forgotten them
// once no game had them loaded.
TEST_CASE("a kill is told to a game that loads the enemy later, until a witness respawns it") {
  ObjectHarness h;
  h.receive(1, present_in("DekuTree", {placed(7), placed(8)}), kChannelOwnership);
  REQUIRE(h.session.object_owner(placed(7)) == owned(1, 0));
  h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::dead, {1, 2, 3}}, kChannelOwnership);
  h.receive(1, present_in("DekuTree", {placed(8)}), kChannelOwnership);
  CHECK_FALSE(h.session.object_owner(placed(7)).has_value());  // nobody has it: forgotten
  h.clear();

  // B comes in later: told it is dead, no owner, once.
  h.receive(2, present_in("DekuTree", {placed(7), placed(8)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 2) ==
        std::vector<ObjectRemove>{{1, placed(7), 0, RemoveReason::dead, {1, 2, 3}}});
  for (const ObjectOwner& o : messages_to<ObjectOwner>(h, 2)) CHECK(o.id != placed(7));
  CHECK_FALSE(h.session.object_owner(placed(7)).has_value());
  CHECK(h.session.stats().dead_told == 1);
  h.clear();
  h.receive(2, present_in("DekuTree", {placed(7), placed(8)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 2).empty());
  // Its copy deleted, B lists it no more; a third player coming in is told too.
  h.receive(2, present_in("DekuTree", {placed(8)}), kChannelOwnership);
  h.clear();
  h.receive(3, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 3).size() == 1);
  h.receive(3, present_in("DekuTree", {}), kChannelOwnership);

  // A's game loads the room again and lists it: respawned, alive for all.
  h.clear();
  h.receive(1, present_in("DekuTree", {placed(7), placed(8)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 1).empty());
  CHECK(h.session.object_owner(placed(7)) == owned(1, 0));
  CHECK(h.session.stats().dead_respawned == 1);
  h.receive(2, present_in("DekuTree", {placed(7), placed(8)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 2).empty());
}

TEST_CASE("a copy still dying in a witness's game is no respawn, and an empty scene forgets kills") {
  ObjectHarness h;
  h.receive(1, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::dead, {}}, kChannelOwnership);
  h.receive(1, present_in("DekuTree", {}), kChannelOwnership);
  h.clear();
  // B's copy is still on its list for a moment: nothing told, nothing respawned.
  h.receive(2, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 2).empty());
  CHECK(h.session.stats().dead_respawned == 0);
  h.receive(2, present_in("DekuTree", {}), kChannelOwnership);
  // A newcomer while A and B are still in the scene is told.
  h.receive(3, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 3).size() == 1);
  h.receive(3, present_in("Field", {}), kChannelOwnership);

  // Everyone leaves the Deku Tree: its games load the room anew next time.
  h.receive(1, present_in("Field", {}), kChannelOwnership);
  h.receive(2, present_in("Field", {}), kChannelOwnership);
  h.clear();
  h.receive(3, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 3).empty());
  CHECK(h.session.object_owner(placed(7)) == owned(3, 0));
}

TEST_CASE("a witness that leaves the scene and comes back with the enemy listed has respawned it") {
  ObjectHarness h;
  h.receive(1, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  h.receive(2, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  h.receive(1, ObjectRemove{0, placed(7), 0, RemoveReason::dead, {}}, kChannelOwnership);
  h.receive(1, present_in("DekuTree", {}), kChannelOwnership);
  // B leaves while its dying copy was still listed, and comes back: its game
  // loaded the room anew, so the enemy is alive and shared again (owned by
  // B), not left with no owner. A stays in the scene.
  h.receive(2, present_in("Field", {}), kChannelOwnership);
  h.clear();
  h.receive(2, present_in("DekuTree", {placed(7)}), kChannelOwnership);
  CHECK(messages_to<ObjectRemove>(h, 2).empty());
  CHECK(h.session.object_owner(placed(7)) == owned(2, 0));
  CHECK(h.session.stats().dead_respawned == 1);
}

// ---- Security (net-security) ----

namespace {

// ENet's form of an IPv4 address: the four bytes in memory order.
std::uint32_t enet_address(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
  const unsigned char bytes[4] = {a, b, c, d};
  std::uint32_t out = 0;
  std::memcpy(&out, bytes, 4);
  return out;
}

}  // namespace

TEST_CASE("security: a player relaying big messages is held to the byte rate") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  // TEST_PAYLOAD is relayed as it came; 60 KB each, 100 at once, used to
  // reach every other player whole (6 MB in one go, 15 MB/s sustained).
  TestPayload big{1, std::vector<std::uint8_t>(60'000, 0)};
  const std::size_t size = bytes_of(big).size();
  for (int i = 0; i < 100; ++i) h.receive(1, big, kChannelPlayerSamples);
  // Unreliable: down to the quarter second kept for the reliable channels.
  const std::size_t reserve = (1u << 20) / 4;
  const std::size_t burst = (2 * (1u << 20) - reserve) / size;  // 30
  CHECK(h.to(2).size() == burst);
  CHECK(h.session.stats().rate_limited == 100 - burst);
  // A second later one second's worth more goes through.
  h.clear();
  h.now += 1'000'000;
  for (int i = 0; i < 100; ++i) h.receive(1, big, kChannelPlayerSamples);
  const std::size_t left = 2 * (1u << 20) - burst * size;  // what the burst did not use
  CHECK(h.to(2).size() == (left + (1u << 20) - reserve) / size);
  // The other player is not affected, and small messages flow again once
  // the budget has refilled.
  h.clear();
  h.receive(2, Chat{0, "hi"}, kChannelSession);
  CHECK(h.to(1).size() == 1);
  h.now += 100'000;
  h.clear();
  h.receive(1, Chat{0, "hi"}, kChannelSession);
  CHECK(h.to(2).size() == 1);
}

TEST_CASE("security: the protocol's own traffic stays under the byte rate") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  PlayerSample sample;
  sample.time = {1, 1};
  sample.position = Vec3{1, 2, 3};
  sample.scene = std::string(kMaxSceneChars, 'x');
  sample.animation = Animation{std::string(kMaxAnimationChars, 'a'), 1.0f, 0.0f};
  // 60 full samples and 10 chats of 200 characters a second for 10 seconds.
  int sent = 0;
  for (int i = 0; i < 600; ++i) {
    h.now += 16'667;
    h.receive(1, sample, kChannelPlayerSamples);
    ++sent;
    if (i % 6 == 0) {
      h.receive(1, Chat{0, std::string(kMaxChatChars, 'c')}, kChannelSession);
      ++sent;
    }
  }
  CHECK(h.to(2).size() == static_cast<std::size_t>(sent));
  CHECK(h.session.stats().rate_limited == 0);
}

TEST_CASE("security: full poses with held pieces stay under the byte rate") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  // The heaviest real traffic (docs/puppet.md, "Pose"): Link's 300 bones with
  // rotation, translation and scale, 30 a second, plus two held pieces of 64
  // bones (sling-fire), and 60 samples a second, for 10 seconds.
  auto pose = [](std::uint32_t skeleton, std::uint16_t bones) {
    PlayerPose p;
    p.time = {1, 1};
    p.skeleton = skeleton;
    for (std::uint16_t b = 0; b < bones; ++b) {
      PoseBone bone;
      bone.bone = b;
      bone.rotation = std::array<std::int16_t, 4>{1, 2, 3, 4};
      bone.translation = std::array<std::int16_t, 3>{1, 2, 3};
      bone.scale = std::array<std::int16_t, 3>{1, 2, 3};
      p.bones.push_back(bone);
    }
    return p;
  };
  const PlayerPose link = pose(1, 300);
  const PlayerPose sling = pose(2, 64);
  const PlayerPose shield = pose(3, 64);
  PlayerSample sample;
  sample.time = {1, 1};
  sample.position = Vec3{1, 2, 3};
  int sent = 0;
  for (int i = 0; i < 600; ++i) {
    h.now += 16'667;
    h.receive(1, sample, kChannelPlayerSamples);
    ++sent;
    if (i % 2 == 0) {
      h.receive(1, link, kChannelPlayerSamples);
      h.receive(1, sling, kChannelPlayerSamples);
      h.receive(1, shield, kChannelPlayerSamples);
      sent += 3;
    }
  }
  CHECK(h.to(2).size() == static_cast<std::size_t>(sent));
  CHECK(h.session.stats().rate_limited == 0);
}

TEST_CASE("security: an unreliable flood leaves bytes for the reliable channels") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  TestPayload big{1, std::vector<std::uint8_t>(60'000, 0)};
  for (int i = 0; i < 100; ++i) h.receive(1, big, kChannelPlayerSamples);
  CHECK(h.session.stats().rate_limited > 0);
  h.clear();
  // Same moment, no refill: the player's chat still goes.
  h.receive(1, Chat{0, std::string(kMaxChatChars, 'c')}, kChannelSession);
  CHECK(h.to(2).size() == 1);
}

TEST_CASE("security: --allow lets only the listed addresses connect") {
  SessionConfig config;
  config.allowed = {*parse_address_range("25.0.0.0/8"), *parse_address_range("192.168.1.20")};
  Session s(config);
  CHECK(s.connected(1, 0, enet_address(25, 1, 2, 3)));
  CHECK(s.connected(2, 0, enet_address(192, 168, 1, 20)));
  CHECK(!s.connected(3, 0, enet_address(192, 168, 1, 21)));
  CHECK(!s.connected(4, 0, enet_address(26, 1, 2, 3)));
  CHECK(!s.connected(5, 0, std::nullopt));  // no address: not allowed either
  CHECK(s.stats().not_allowed == 3);
  CHECK(s.stats().dropped() == 0);  // connections, not packets
  // Without --allow, anyone.
  Session open{SessionConfig{}};
  CHECK(open.connected(1, 0, enet_address(8, 8, 8, 8)));
  CHECK(open.stats().not_allowed == 0);
}

TEST_CASE("security: address ranges parse strictly") {
  const auto one = parse_address_range("25.1.2.3");
  REQUIRE(one.has_value());
  CHECK(one->first == 0x19010203u);
  CHECK(one->prefix == 32);
  CHECK(one->contains(0x19010203u));
  CHECK(!one->contains(0x19010204u));
  const auto net = parse_address_range("100.64.0.0/10");
  REQUIRE(net.has_value());
  CHECK(net->contains(ipv4_of(enet_address(100, 100, 1, 1))));
  CHECK(!net->contains(ipv4_of(enet_address(100, 128, 0, 1))));
  CHECK(parse_address_range("0.0.0.0/0")->contains(0xFFFFFFFFu));
  for (const char* bad : {"", "25", "25.1.2", "25.1.2.3.4", "256.1.1.1", "25.1.2.3/33", "25.1.2.3/",
                          "25.1.2.3/8x", "a.b.c.d", "25.1.2.3 ", " 25.1.2.3", "::1", "localhost",
                          "1234.1.1.1", "25..2.3"}) {
    CAPTURE(bad);
    CHECK(!parse_address_range(bad).has_value());
  }
  CHECK(ipv4_of(enet_address(1, 2, 3, 4)) == 0x01020304u);
}

TEST_CASE("security: names with invisible or bidi characters are refused") {
  Harness h;
  h.join(1, "Link");
  h.clear();
  const char* cases[] = {
      "Li" "\xE2\x80\x8B" "nk",       // zero-width space: looks like Link
      "" "\xE2\x80\xAE" "kniL",       // right-to-left override
      "Link" "\xE2\x81\xA0" "",       // word joiner
      "" "\xEF\xBB\xBF" "Link",       // byte order mark
      "Li" "\xC2\xAD" "nk",       // soft hyphen
      "Link\U000E0041",   // tag character
  };
  ConnectionId id = 10;
  for (const char* name : cases) {
    CAPTURE(name);
    ++id;
    h.connect(id);
    h.receive(id, hello_from(name), kChannelSession);
    const auto refused = only_message_to<Refused>(h, id);
    REQUIRE(refused.has_value());
    CHECK(refused->reason == RefuseReason::bad_name);
    h.clear();
  }
  // Ordinary letters from other scripts are fine.
  h.join(30, "Zelda" "\xC3\xA9" "");
  h.join(31, "" "\xE3\x83\xAA" "" "\xE3\x83\xB3" "" "\xE3\x82\xAF" "");
  // An emoji with its presentation selector (U+2764 U+FE0F, a red heart).
  h.join(32, "Link " "\xE2\x9D\xA4" "\xEF\xB8\x8F");
  CHECK(h.session.player_count() == 4);
}

// --- The join password (protocol 9, docs/protocol.md "Join password") ---

namespace {

SessionConfig with_password(const std::string& password) {
  SessionConfig config;
  config.join_password = password;
  return config;
}

// Sends HELLO and returns the JOIN_CHALLENGE's nonce.
JoinNonce challenge_for(Harness& h, ConnectionId id, const std::string& nickname) {
  h.connect(id);
  h.clear();
  h.receive(id, hello_from(nickname), kChannelSession);
  auto challenge = only_message_to<JoinChallenge>(h, id);
  REQUIRE(challenge);
  CHECK(h.dropped.empty());
  return challenge->nonce;
}

}  // namespace

TEST_CASE("join password: the right proof is welcomed") {
  Harness h(with_password("hunter2"));
  const JoinNonce nonce = challenge_for(h, 1, "Link");
  CHECK(h.session.player_count() == 0);  // not in before the proof
  h.clear();
  h.receive(1, JoinProof{h.proof("hunter2", nonce)}, kChannelSession);
  bool welcomed = false;
  for (const Sent& s : h.to(1)) welcomed = welcomed || std::holds_alternative<Welcome>(s.message);
  CHECK(welcomed);
  CHECK(h.dropped.empty());
  CHECK(h.session.player_count() == 1);
  CHECK(h.session.stats().bad_password == 0);
}

TEST_CASE("join password: a wrong proof is refused with bad_password and closed") {
  Harness h(with_password("hunter2"));
  const JoinNonce nonce = challenge_for(h, 1, "Link");
  h.clear();
  h.receive(1, JoinProof{h.proof("hunter3", nonce)}, kChannelSession);
  auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::bad_password);
  CHECK(h.dropped == std::vector<ConnectionId>{1});
  CHECK(h.session.player_count() == 0);
  CHECK(h.session.stats().bad_password == 1);
  // A second try on the same connection goes nowhere: it is closing.
  h.clear();
  h.receive(1, JoinProof{h.proof("hunter2", nonce)}, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.player_count() == 0);
}

TEST_CASE("join password: a game with no password (the empty one's proof) is refused") {
  Harness h(with_password("hunter2"));
  const JoinNonce nonce = challenge_for(h, 1, "Link");
  h.clear();
  h.receive(1, JoinProof{h.proof("", nonce)}, kChannelSession);
  auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::bad_password);
}

TEST_CASE("join password: no proof at all times out like a missing HELLO") {
  SessionConfig config = with_password("hunter2");
  config.hello_timeout_us = 1'000'000;
  Harness h(config);
  challenge_for(h, 1, "Link");
  h.receive(1, Chat{0, "let me in"}, kChannelSession);  // anything but a proof
  CHECK(h.session.stats().before_welcome == 1);
  h.now += 1'000'000;
  h.session.tick(h.now);
  h.collect();
  CHECK(std::find(h.dropped.begin(), h.dropped.end(), ConnectionId{1}) != h.dropped.end());
  CHECK(h.session.player_count() == 0);
}

TEST_CASE("join password: a recorded proof does not work on another connection") {
  Harness h(with_password("hunter2"));
  const JoinNonce first = challenge_for(h, 1, "Link");
  const JoinProofBytes recorded = h.proof("hunter2", first);
  h.receive(1, JoinProof{recorded}, kChannelSession);
  REQUIRE(h.session.player_count() == 1);

  // Someone who saw that proof on the wire sends it again on a connection of
  // their own: their nonce is a new one, so it does not match.
  const JoinNonce second = challenge_for(h, 2, "Ganon");
  CHECK(second != first);
  h.clear();
  h.receive(2, JoinProof{recorded}, kChannelSession);
  auto refused = only_message_to<Refused>(h, 2);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::bad_password);
  CHECK(h.session.player_count() == 1);
}

TEST_CASE("join password: one HELLO, one nonce: a second HELLO gets no new challenge") {
  Harness h(with_password("hunter2"));
  const JoinNonce nonce = challenge_for(h, 1, "Link");
  h.clear();
  h.receive(1, hello_from("Link"), kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().repeated_hello == 1);
  h.receive(1, JoinProof{h.proof("hunter2", nonce)}, kChannelSession);
  CHECK(h.session.player_count() == 1);
}

TEST_CASE("join password: a joined player's JOIN_PROOF is dropped, never relayed") {
  Harness h;  // the harness's own password, answered by it
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.receive(1, JoinProof{}, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().stray_proof == 1);
}

TEST_CASE("join password: a wrong protocol version is refused before any challenge") {
  Harness h(with_password("hunter2"));
  h.connect(1);
  Hello old = hello_from("Link");
  old.protocol_version = kProtocolVersion - 1;
  h.receive(1, old, kChannelSession);
  auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::wrong_protocol_version);
}

TEST_CASE("join password: the checks after it still run (a name taken is refused)") {
  Harness h(with_password("hunter2"));
  JoinNonce nonce = challenge_for(h, 1, "Link");
  h.receive(1, JoinProof{h.proof("hunter2", nonce)}, kChannelSession);
  REQUIRE(h.session.player_count() == 1);
  nonce = challenge_for(h, 2, "Link");
  h.clear();
  h.receive(2, JoinProof{h.proof("hunter2", nonce)}, kChannelSession);
  auto refused = only_message_to<Refused>(h, 2);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::name_taken);
}

TEST_CASE("join password: 5 wrong guesses from one address close it at connect for 60 s") {
  Harness h(with_password("hunter2"));
  const std::uint32_t guesser = 0x0A000001;
  const std::uint32_t friend_address = 0x0A000002;
  ConnectionId next = 1;
  for (int i = 0; i < 5; ++i) {
    const ConnectionId id = next++;
    h.connect(id, guesser);
    h.clear();
    h.receive(id, hello_from("Ganon"), kChannelSession);
    auto challenge = only_message_to<JoinChallenge>(h, id);
    REQUIRE(challenge);
    h.receive(id, JoinProof{h.proof("guess" + std::to_string(i), challenge->nonce)},
              kChannelSession);
  }
  CHECK(h.session.stats().bad_password == 5);
  CHECK(h.session.wrong_guess_addresses() == 1);

  // The 6th is closed at connect, before it can send anything, right password or not.
  CHECK_FALSE(h.session.connected(next++, h.now, guesser));
  CHECK(h.session.stats().password_blocked == 1);
  // Another address is not touched.
  JoinNonce nonce{};
  {
    const ConnectionId id = next++;
    h.connect(id, friend_address);
    h.clear();
    h.receive(id, hello_from("Zelda"), kChannelSession);
    auto challenge = only_message_to<JoinChallenge>(h, id);
    REQUIRE(challenge);
    h.receive(id, JoinProof{h.proof("hunter2", challenge->nonce)}, kChannelSession);
    CHECK(h.session.player_count() == 1);
  }

  // Still blocked just before the minute is over.
  h.now += 59'000'000;
  h.session.tick(h.now);
  h.collect();
  CHECK_FALSE(h.session.connected(next++, h.now, guesser));
  CHECK(h.session.stats().password_blocked == 2);

  // After 60 s the address may try again, and the right password gets in.
  h.now += 1'000'000;
  h.session.tick(h.now);
  h.collect();
  CHECK(h.session.wrong_guess_addresses() == 0);  // swept: the map does not keep it
  const ConnectionId id = next++;
  h.connect(id, guesser);
  h.clear();
  h.receive(id, hello_from("Link"), kChannelSession);
  auto challenge = only_message_to<JoinChallenge>(h, id);
  REQUIRE(challenge);
  nonce = challenge->nonce;
  h.clear();
  h.receive(id, JoinProof{h.proof("hunter2", nonce)}, kChannelSession);
  bool welcomed = false;
  for (const Sent& s : h.to(id)) welcomed = welcomed || std::holds_alternative<Welcome>(s.message);
  CHECK(welcomed);  // (Zelda timed out meanwhile: the fake clock jumped a minute)
}

TEST_CASE("join password: 4 wrong guesses, then a minute without, start the count again") {
  Harness h(with_password("hunter2"));
  const std::uint32_t address = 0x0A000003;
  ConnectionId next = 1;
  auto guess_wrong = [&] {
    const ConnectionId id = next++;
    h.connect(id, address);
    h.clear();
    h.receive(id, hello_from("Ganon"), kChannelSession);
    auto challenge = only_message_to<JoinChallenge>(h, id);
    REQUIRE(challenge);
    h.receive(id, JoinProof{h.proof("nope", challenge->nonce)}, kChannelSession);
  };
  for (int i = 0; i < 4; ++i) guess_wrong();
  h.now += 60'000'000;
  h.session.tick(h.now);
  h.collect();
  for (int i = 0; i < 4; ++i) guess_wrong();
  CHECK(h.session.connected(next++, h.now, address));
  CHECK(h.session.stats().password_blocked == 0);
}

TEST_CASE("the host's save: SAVE_ASK goes to the host, SAVE_PART from the host to the asker or everyone") {
  Harness h;
  h.join(1, "Link");  // the first player hosts
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();

  h.receive(2, SaveAsk{0}, kChannelSession);
  auto ask = only_message_to<SaveAsk>(h, 1);
  REQUIRE(ask.has_value());
  CHECK(ask->player_number == 2);
  CHECK(h.to(3).empty());
  CHECK(h.session.stats().save_asks == 1);
  h.clear();

  h.receive(1, SaveAsk{0}, kChannelSession);  // the host asks itself: refused
  CHECK(h.sent.empty());
  CHECK(h.session.stats().save_refused == 1);

  SavePart part;
  part.to_player = 2;
  part.transfer_id = 7;
  part.reason = SaveReason::join;
  part.total_size = 3;
  part.crc32 = 0x1234;
  part.data = {1, 2, 3};
  h.receive(1, part, kChannelSession);
  auto got = only_message_to<SavePart>(h, 2);
  REQUIRE(got.has_value());
  CHECK(got->player_number == 1);
  CHECK(got->data == part.data);
  CHECK(h.to(3).empty());
  h.clear();

  part.to_player = 0;
  part.reason = SaveReason::host_load;
  h.receive(1, part, kChannelSession);  // the host loaded a save: every guest
  CHECK(only_message_to<SavePart>(h, 2).has_value());
  CHECK(only_message_to<SavePart>(h, 3).has_value());
  CHECK(h.to(1).empty());
  CHECK(h.session.stats().save_parts == 2);
  h.clear();

  h.receive(2, part, kChannelSession);  // a guest cannot send a save
  CHECK(h.sent.empty());
  part.to_player = 4;  // nobody
  part.reason = SaveReason::join;
  h.receive(1, part, kChannelSession);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().save_refused == 3);
  h.clear();

  // The host's parts are outside its message budget (60 a second, burst 120):
  // a part dropped would leave the guest's transfer unfinished.
  part.to_player = 2;
  for (int i = 0; i < 200; ++i) h.receive(1, part, kChannelSession);
  CHECK(messages_to<SavePart>(h, 2).size() == 200);
  CHECK(h.session.stats().rate_limited == 0);
}

TEST_CASE("pvp: off by default, nothing told at join, hits dropped") {
  Harness h;
  CHECK_FALSE(h.session.pvp());
  h.join(1, "Link");
  h.join(2, "Zelda");
  CHECK(messages_of<PvpMode>(h, 1).empty());
  CHECK(messages_of<PvpMode>(h, 2).empty());
  h.clear();
  h.receive(1, PvpHit{0, 2, 1, 4, {1, 2, 3}, {0, 2, 3}}, kChannelOwnership);
  CHECK(h.sent.empty());
  CHECK(h.session.stats().pvp_hits_dropped == 1);
}

TEST_CASE("pvp: --pvp on is told at join, and hits go to their victim only") {
  SessionConfig config;
  config.pvp = true;
  Harness h(config);
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  CHECK(messages_of<PvpMode>(h, 2) == std::vector<PvpMode>{PvpMode{0, true}});
  h.clear();
  h.receive(1, PvpHit{0, 3, 1, 4, {1, 2, 3}, {0, 2, 3}}, kChannelOwnership);
  CHECK(h.to(2).empty());
  const auto hit = only_message_to<PvpHit>(h, 3);
  REQUIRE(hit);
  CHECK(hit->player_number == 1);
  CHECK(hit->amount == 4);
  CHECK(h.session.stats().pvp_hits_relayed == 1);
  // To a player who is not there: dropped.
  h.clear();
  h.receive(1, PvpHit{0, 4, 1, 4, {}, {}}, kChannelOwnership);
  CHECK(h.sent.empty());
  // The victim's answer goes to everyone else, with its sender written in.
  h.receive(3, PvpHurt{0, 1, 4, 8, 0}, kChannelOwnership);
  CHECK(only_message_to<PvpHurt>(h, 1) == PvpHurt{3, 1, 4, 8, 0});
  CHECK(only_message_to<PvpHurt>(h, 2) == PvpHurt{3, 1, 4, 8, 0});
  // A hurt by itself is nobody's.
  h.clear();
  h.receive(3, PvpHurt{0, 3, 4, 8, 0}, kChannelOwnership);
  CHECK(h.sent.empty());
}

TEST_CASE("pvp: only the host changes it, for everyone, and an empty session goes back") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.receive(2, PvpMode{0, true}, kChannelSession);  // a guest cannot
  CHECK_FALSE(h.session.pvp());
  CHECK(only_message_to<PvpMode>(h, 2) == PvpMode{0, false});
  CHECK(h.to(1).empty());
  CHECK(h.session.stats().pvp_mode_refused == 1);
  h.clear();
  h.receive(1, PvpMode{0, true}, kChannelSession);
  CHECK(h.session.pvp());
  CHECK(only_message_to<PvpMode>(h, 1) == PvpMode{1, true});
  CHECK(only_message_to<PvpMode>(h, 2) == PvpMode{1, true});
  // A newcomer learns it at join.
  h.clear();
  h.join(3, "Saria");
  CHECK(messages_of<PvpMode>(h, 3) == std::vector<PvpMode>{PvpMode{1, true}});
  // Everyone leaves: the next session starts from the server's own value.
  for (ConnectionId id : {1u, 2u, 3u}) h.session.disconnected(id, h.now);
  h.collect();
  CHECK_FALSE(h.session.pvp());
}

// Issue 151 (fishing-3): a fish goes to the game whose lure is nearest it.
namespace {
FishLure lure_at(std::uint16_t cm, std::uint8_t flags = 0) {
  return FishLure{0, {FishLureEntry{placed(7), cm, flags}}};
}
}  // namespace

TEST_CASE("issue 151: a fish is moved by the lures, not by who stands near it") {
  OwnerHarness h;
  h.stand(1, 50);
  h.stand(2, 5);  // the distance rule would give an enemy to 2 after 3 s
  h.receive(1, lure_at(kFishLureNone), kChannelOwnership);
  h.receive(2, lure_at(kFishLureNone), kChannelOwnership);
  h.run(4 * kSecond);
  CHECK(h.asks() == 0);
  // 2 casts 3 m from it; 1 has no lure in reach.
  h.receive(2, lure_at(300), kChannelOwnership);
  h.run(250'000);
  CHECK(h.asks() == 0);  // held 300 ms first
  h.run(500'000);
  REQUIRE(h.asks() == 1);
  CHECK(messages_to<HandoverAsk>(h, 1)[0] == HandoverAsk{placed(7), 0, 2});
  CHECK(h.session.stats().fish_to_lure == 1);
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  CHECK(h.session.object_owner(placed(7)) == owned(2, 1));
}

TEST_CASE("issue 151: the owner keeps a fish on its lure, or one the other lure is not clearly nearer") {
  auto asked = [](FishLure owners, FishLure others, bool fight = false) {
    OwnerHarness h;
    h.stand(1, 3);
    h.stand(2, 4);
    h.run(kSecond);
    if (fight) {  // the owner says it fights it: no fight lock for a fish
      ObjectPresent p = present_in("Field", {placed(7)});
      p.objects[0].flags = object_presence::awake | object_presence::in_fight;
      h.receive(1, p, kChannelOwnership);
    }
    h.receive(1, owners, kChannelOwnership);
    h.receive(2, others, kChannelOwnership);
    h.run(1'500'000);
    return h.asks() == 1;
  };
  CHECK_FALSE(asked(lure_at(50, fish_lure::hooked), lure_at(0, fish_lure::drawn)));  // hooked
  CHECK_FALSE(asked(lure_at(300), lure_at(250)));  // only 50 cm nearer
  CHECK(asked(lure_at(300), lure_at(150)));        // 1.5 m nearer
  CHECK(asked(lure_at(300), lure_at(350, fish_lure::drawn)));  // it swims at 2's lure
  // Review must-fix: 2's copy turns at a lure 3 m farther than 1's, which
  // the owner's fish circles before it bites: it stays (no ping-pong).
  CHECK_FALSE(asked(lure_at(100), lure_at(400, fish_lure::drawn)));
  // Only the owner's hooked counts: a late list from an old owner is distance.
  CHECK_FALSE(asked(lure_at(300), lure_at(250, fish_lure::hooked)));
  CHECK(asked(lure_at(300), lure_at(150, fish_lure::hooked)));
  CHECK_FALSE(asked(lure_at(100, fish_lure::drawn), lure_at(400, fish_lure::drawn)));
  CHECK(asked(lure_at(kFishLureNone), lure_at(550)));  // the owner has none in reach
  CHECK_FALSE(asked(lure_at(300), lure_at(kFishLureNone)));
  CHECK(asked(lure_at(300), lure_at(150), true));  // a fight lock does not hold a fish
}

TEST_CASE("issue 151: an old lure list counts as no lure") {
  SessionConfig config = quiet_config();
  config.fish_lure_cooldown_us = 3 * kSecond;
  OwnerHarness h(config);
  h.stand(1, 3);
  h.stand(2, 4);
  h.receive(2, lure_at(kFishLureNone), kChannelOwnership);
  h.run(3 * kSecond);
  // 1 owns it; 2 casts near it, then says nothing more for 3 s.
  h.receive(2, lure_at(100), kChannelOwnership);
  h.receive(1, lure_at(kFishLureNone), kChannelOwnership);
  h.run(750'000);
  REQUIRE(h.asks() == 1);
  h.receive(1, HandoverState{0, placed(7), 0, state_at(0, 12)}, kChannelOwnership);
  REQUIRE(h.session.object_owner(placed(7)) == owned(2, 1));
  // 2 owns it now. 1's lure is nearer, but its list is stale by the time
  // the 3 s cooldown is over: no ask.
  h.clear();
  h.receive(1, lure_at(50), kChannelOwnership);
  h.run(3'500'000);
  CHECK(messages_to<HandoverAsk>(h, 2).empty());
  // Said again, fresh: asked.
  h.receive(1, lure_at(50), kChannelOwnership);
  h.run(750'000);
  CHECK(messages_to<HandoverAsk>(h, 2).size() == 1);
}

TEST_CASE("issue 151: a hooked fish waits for its owner: a late list, a pause") {
  auto other_takes = [](bool pause) {
    OwnerHarness h;
    h.stand(1, 3);
    h.stand(2, 4);
    h.run(kSecond);
    h.receive(1, lure_at(25, fish_lure::hooked), kChannelOwnership);
    if (pause) h.receive(1, PlayerStatus{0, status::paused}, kChannelSession);
    // 1 says nothing more for 8 s (a network stall, or the pause menu);
    // 2's lure is right beside the fish all the while.
    for (int i = 0; i < 16; ++i) {
      h.receive(2, lure_at(0, fish_lure::drawn), kChannelOwnership);
      h.run(500'000);
    }
    return h.asks() != 0;
  };
  CHECK_FALSE(other_takes(false));
  CHECK_FALSE(other_takes(true));

  // Not hooked: a paused owner's fish goes to the lure near it.
  OwnerHarness h;
  h.stand(1, 3);
  h.stand(2, 40);
  h.run(kSecond);
  h.receive(1, lure_at(300), kChannelOwnership);
  h.receive(1, PlayerStatus{0, status::paused}, kChannelSession);
  for (int i = 0; i < 14; ++i) {
    h.receive(2, lure_at(400), kChannelOwnership);
    h.run(500'000);
  }
  REQUIRE(h.asks() == 1);
  CHECK(messages_to<HandoverAsk>(h, 1)[0] == HandoverAsk{placed(7), 0, 2});
}

TEST_CASE("a gimmick event start goes to the others in the sender's scene only, never kept (deku-dungeon-2)") {
  ObjectHarness h;
  h.receive(1, sample_in("InsideTheDekuTree"), kChannelPlayerSamples);
  h.receive(2, sample_in("InsideTheDekuTree"), kChannelPlayerSamples);
  h.receive(3, sample_in("KokiriForest"), kChannelPlayerSamples);
  h.clear();
  DungeonEvent e;
  e.player_number = 0;  // ours: the server writes the sender
  e.scene = "InsideTheDekuTree";
  e.name = "InsideTheDekuTree_Gimmick_RoomD_PlatformUp";
  h.receive(1, e, kChannelSession);
  const auto got = messages_to<DungeonEvent>(h, 2);
  REQUIRE(got.size() == 1);
  CHECK(got.front().player_number == 1);
  CHECK(got.front().name == e.name);
  CHECK(messages_to<DungeonEvent>(h, 1).empty());
  CHECK(messages_to<DungeonEvent>(h, 3).empty());
  // From a scene the sender is not in (late): dropped.
  h.clear();
  e.scene = "KokiriForest";
  h.receive(1, e, kChannelSession);
  CHECK(messages_to<DungeonEvent>(h, 3).empty());
  // Not kept: a player entering later gets nothing.
  h.clear();
  h.receive(3, sample_in("InsideTheDekuTree"), kChannelPlayerSamples);
  CHECK(messages_to<DungeonEvent>(h, 3).empty());
}

TEST_CASE("a platform clock goes to the others in the sender's scene only, never kept (platforms-b)") {
  ObjectHarness h;
  h.receive(1, sample_in("DodongoCavern"), kChannelPlayerSamples);
  h.receive(2, sample_in("DodongoCavern"), kChannelPlayerSamples);
  h.receive(3, sample_in("KokiriForest"), kChannelPlayerSamples);
  h.clear();
  PlatformClock p;
  p.player_number = 0;  // ours: the server writes the sender
  p.scene = "DodongoCavern";
  p.server_time_us = 5'000'000;
  p.ticks.push_back({0x7f36b7404e6b5890ull, (std::uint64_t{4} << 32) | 0x30, 2.5f, 1.0f, 0.0f});
  h.receive(1, p, kChannelObjectSamples);
  const auto got = messages_to<PlatformClock>(h, 2);
  REQUIRE(got.size() == 1);
  CHECK(got.front().player_number == 1);
  CHECK(got.front().ticks == p.ticks);
  CHECK(got.front().server_time_us == p.server_time_us);
  CHECK(messages_to<PlatformClock>(h, 1).empty());
  CHECK(messages_to<PlatformClock>(h, 3).empty());
  h.clear();
  p.scene = "KokiriForest";  // a scene the sender is not in (late): dropped
  h.receive(1, p, kChannelObjectSamples);
  CHECK(messages_to<PlatformClock>(h, 3).empty());
  h.clear();
  h.receive(3, sample_in("DodongoCavern"), kChannelPlayerSamples);
  CHECK(messages_to<PlatformClock>(h, 3).empty());  // not kept
  // The pause counters (platforms-b2) go the same way.
  h.clear();
  PlatformCounter k;
  k.scene = "DodongoCavern";
  k.server_time_us = 6'000'000;
  k.ticks.push_back({0xabcdefull, 3'000'000, 500'000, 4});
  h.receive(2, k, kChannelObjectSamples);
  const auto counters = messages_to<PlatformCounter>(h, 3);
  REQUIRE(counters.size() == 1);
  CHECK(counters.front().player_number == 2);
  CHECK(counters.front().ticks == k.ticks);
  CHECK(messages_to<PlatformCounter>(h, 1).size() == 1);  // all three are in DodongoCavern now
  CHECK(messages_to<PlatformCounter>(h, 2).empty());      // not back to the sender
}

// --- Encrypted traffic (docs/protocol.md "Encrypted traffic") ---

TEST_CASE("encrypted traffic: a right proof switches the connection to its keys first") {
  Harness h(with_password("hunter2"));
  h.connect(1);
  const Hello hello = hello_from("Link");
  h.session.received(1, kChannelSession, bytes_of(hello), h.now);
  // The challenge carries the server's key share.
  std::vector<Action> actions = h.session.take_actions();
  REQUIRE(actions.size() == 1);
  // A copy: `actions` is reassigned below.
  const SendAction challenge_send = std::get<SendAction>(actions[0]);
  Message decoded;
  REQUIRE(decode(challenge_send.bytes, decoded) == Error::none);
  const JoinChallenge challenge = std::get<JoinChallenge>(decoded);  // a copy: `decoded` is reused below
  REQUIRE(challenge.key_share.has_value());

  h.session.received(1, kChannelSession,
                     bytes_of(JoinProof{join_proof("hunter2", challenge.nonce, *hello.key_share, *challenge.key_share)}), h.now);
  actions = h.session.take_actions();
  REQUIRE(actions.size() >= 2);
  // The keys come before anything else, so WELCOME already goes sealed.
  const auto* encrypt = std::get_if<EncryptAction>(&actions[0]);
  REQUIRE(encrypt != nullptr);
  CHECK(encrypt->who == 1);
  REQUIRE(decode(std::get<SendAction>(actions[1]).bytes, decoded) == Error::none);
  CHECK(std::holds_alternative<Welcome>(decoded));

  // The game makes the same keys from what it sent and got.
  static const KeyPair game_keys = KeyPair::from_secret({7, 7, 7});
  const auto game = derive_traffic_keys(game_keys, *challenge.key_share, "hunter2",
                                        bytes_of(hello), challenge_send.bytes, false);
  REQUIRE(game.has_value());
  CHECK(game->send == encrypt->keys.receive);
  CHECK(game->receive == encrypt->keys.send);
  CHECK(game->send != game->receive);
}

TEST_CASE("encrypted traffic: a wrong proof gets no keys") {
  Harness h(with_password("hunter2"));
  const JoinNonce nonce = challenge_for(h, 1, "Link");
  h.clear();
  h.receive(1, JoinProof{h.proof("hunter3", nonce)}, kChannelSession);
  CHECK(h.encrypted.empty());
  const auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::bad_password);
}

TEST_CASE("encrypted traffic: a HELLO without a key share is refused before the challenge") {
  Harness h(with_password("hunter2"));
  h.connect(1);
  h.clear();
  Hello hello = hello_from("Link");
  hello.key_share.reset();  // an older game, or someone took it out on the way
  h.receive(1, hello, kChannelSession);
  CHECK_FALSE(only_message_to<JoinChallenge>(h, 1).has_value());
  const auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::wrong_protocol_version);
  CHECK(h.session.stats().no_key_share == 1);
  CHECK(h.session.player_count() == 0);
}

TEST_CASE("encrypted traffic: a session without a join password lets nobody in") {
  // coop_server does not start without one; the session refuses as well.
  Session open{SessionConfig{}};
  REQUIRE(open.connected(1, 0, std::nullopt));
  open.take_actions();
  open.received(1, kChannelSession, bytes_of(hello_from("Link")), 0);
  bool refused = false;
  for (const Action& a : open.take_actions()) {
    CHECK_FALSE(std::holds_alternative<EncryptAction>(a));
    if (const auto* send = std::get_if<SendAction>(&a)) {
      Message m;
      REQUIRE(decode(send->bytes, m) == Error::none);
      CHECK_FALSE(std::holds_alternative<Welcome>(m));
      CHECK_FALSE(std::holds_alternative<JoinChallenge>(m));
      if (const auto* r = std::get_if<Refused>(&m)) refused = r->reason == RefuseReason::bad_password;
    }
  }
  CHECK(refused);
  CHECK(open.player_count() == 0);
}

TEST_CASE("encrypted traffic: every join is keyed, and a HELLO without a key share is refused") {
  Harness h;  // the default test password, answered by the harness
  h.join(1, "Link");
  CHECK(h.encrypted == std::vector<ConnectionId>{1});
  Hello plain = hello_from("Zelda");
  plain.key_share.reset();  // an older game, or someone took it out on the way
  h.connect(2);
  h.clear();
  h.receive(2, plain, kChannelSession);
  CHECK(messages_to<Welcome>(h, 2).empty());
  const auto refused = only_message_to<Refused>(h, 2);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::wrong_protocol_version);
  CHECK(h.session.stats().no_key_share == 1);
  CHECK(h.encrypted == std::vector<ConnectionId>{1});
  CHECK(h.session.player_count() == 1);
}

TEST_CASE("encrypted traffic: a proof for other key shares (a relay in the middle) is refused") {
  Harness h(with_password("hunter2"));
  const JoinNonce nonce = challenge_for(h, 1, "Link");
  h.clear();
  // Right password and nonce, but made for the share someone in the middle
  // put in place of the game's: the server took the game's.
  const KeyPair middle = KeyPair::from_secret({9, 9, 9});
  h.receive(1, JoinProof{join_proof("hunter2", nonce, middle.share(), h.server_shares.at(nonce))},
            kChannelSession);
  CHECK(h.encrypted.empty());
  const auto refused = only_message_to<Refused>(h, 1);
  REQUIRE(refused);
  CHECK(refused->reason == RefuseReason::bad_password);
  CHECK(h.session.player_count() == 0);
  // And the old unbound proof is not taken either.
  const JoinNonce again = challenge_for(h, 2, "Zelda");
  h.clear();
  h.receive(2, JoinProof{join_proof("hunter2", again)}, kChannelSession);
  CHECK(only_message_to<Refused>(h, 2)->reason == RefuseReason::bad_password);
}

// 16 players (protocol 12): the cap, and who gets which stream
// (docs/protocol.md "Who gets which stream").
namespace {

PlayerSample sample_at(const std::string& scene, float x) {
  PlayerSample s;
  s.time = {1, 1};
  s.scene = scene;
  s.position = Vec3{x, 0.0f, 0.0f};
  return s;
}

PlayerPose a_pose() {
  PlayerPose p;
  p.skeleton = 7;
  PoseBone b;
  b.bone = 3;
  b.rotation = std::array<std::int16_t, 4>{0, 0, 0, 32767};
  p.bones.push_back(b);
  return p;
}

template <typename T>
std::size_t count_to(const Harness& h, ConnectionId id) {
  std::size_t n = 0;
  for (const auto& m : h.to(id)) n += std::holds_alternative<T>(m.message) ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("16 players: all take numbers 1 to 16, the 17th is told the server is full") {
  // 4 by default until the games' side takes 16; the protocol takes 16.
  CHECK(SessionConfig{}.max_players == 4);
  SessionConfig sixteen;
  sixteen.max_players = 16;
  Harness h(sixteen);
  for (ConnectionId id = 1; id <= 16; ++id) h.join(id, "p" + std::to_string(id));
  CHECK(h.session.player_count() == 16);
  h.clear();
  h.connect(17);
  h.receive(17, hello_from("p17"), kChannelSession);
  const auto refused = only_message_to<Refused>(h, 17);
  REQUIRE(refused.has_value());
  CHECK(refused->reason == RefuseReason::server_full);
  CHECK(h.session.player_count() == 16);
}

TEST_CASE("16 players: the 16th's WELCOME lists the 15 others") {
  SessionConfig sixteen;
  sixteen.max_players = 16;
  Harness h(sixteen);
  for (ConnectionId id = 1; id <= 15; ++id) h.join(id, "p" + std::to_string(id));
  h.clear();
  h.join(16, "p16");
  std::optional<Welcome> w;
  for (const auto& m : h.to(16)) {
    if (const auto* got = std::get_if<Welcome>(&m.message)) w = *got;
  }
  REQUIRE(w.has_value());
  CHECK(w->player_number == 16);
  CHECK(w->players.size() == 15);
}

TEST_CASE("max_players (coop_server --max-players) caps the session below the default") {
  SessionConfig two;
  two.max_players = 2;
  Harness h(two);
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.clear();
  h.connect(3);
  h.receive(3, hello_from("Saria"), kChannelSession);
  const auto refused = only_message_to<Refused>(h, 3);
  REQUIRE(refused.has_value());
  CHECK(refused->reason == RefuseReason::server_full);
}

TEST_CASE("interest: poses go to the same scene only, every one near, every 3rd far") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Near");
  h.join(3, "Far");
  h.join(4, "Elsewhere");
  h.receive(1, sample_at("Field", 0.0f), kChannelPlayerSamples);
  h.receive(2, sample_at("Field", 500.0f), kChannelPlayerSamples);
  h.receive(3, sample_at("Field", 9000.0f), kChannelPlayerSamples);
  h.receive(4, sample_at("Village", 0.0f), kChannelPlayerSamples);
  h.clear();
  const std::uint8_t channel = channel_of(Message{a_pose()});
  for (int i = 0; i < 30; ++i) {
    h.now += 33'000;
    h.receive(1, a_pose(), channel);
  }
  CHECK(count_to<PlayerPose>(h, 2) == 30);
  CHECK(count_to<PlayerPose>(h, 3) == 10);
  CHECK(count_to<PlayerPose>(h, 4) == 0);
  CHECK(h.session.stats().interest_skipped == 20 + 30);
}

TEST_CASE("interest: samples go everywhere, every 6th to another scene") {
  Harness h;
  h.join(1, "Link");
  h.join(2, "Here");
  h.join(3, "Elsewhere");
  h.receive(2, sample_at("Field", 100.0f), kChannelPlayerSamples);
  h.receive(3, sample_at("Village", 0.0f), kChannelPlayerSamples);
  h.clear();
  for (int i = 0; i < 60; ++i) {
    h.now += 16'000;
    h.receive(1, sample_at("Field", 0.0f), kChannelPlayerSamples);
  }
  CHECK(count_to<PlayerSample>(h, 2) == 60);
  CHECK(count_to<PlayerSample>(h, 3) == 10);
}

TEST_CASE("interest: the pose upload cap holds poses back, shared by the players") {
  SessionConfig capped;
  capped.pose_upload_bytes_per_second = 600;  // two receivers: 300 B/s each
  Harness h(capped);
  h.join(1, "Link");
  h.join(2, "Zelda");
  h.join(3, "Saria");
  h.clear();
  const std::uint8_t channel = channel_of(Message{a_pose()});
  std::vector<std::uint8_t> bytes;
  REQUIRE(encode(Message{a_pose()}, bytes) == Error::none);
  for (int i = 0; i < 60; ++i) {  // 2 s at 30 a second
    h.now += 33'333;
    h.receive(1, a_pose(), channel);
  }
  // About 300 B/s for 2 s each, plus the first one.
  const std::size_t most = 600 / bytes.size() + 2;
  REQUIRE(most < 60);  // the cap has to bite for this test to mean anything
  CHECK(count_to<PlayerPose>(h, 2) <= most);
  CHECK(count_to<PlayerPose>(h, 2) >= most / 2);
  CHECK(count_to<PlayerPose>(h, 3) == count_to<PlayerPose>(h, 2));
  CHECK(h.session.stats().upload_capped > 0);
}
