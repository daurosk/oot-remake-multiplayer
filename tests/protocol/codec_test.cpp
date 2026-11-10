#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>  // doctest prints string_view through it
#include <string>
#include <utility>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "samples.hpp"

using namespace coop::protocol;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes must_encode(const Message& m) {
  Bytes out;
  REQUIRE(encode(m, out) == Error::none);
  return out;
}

Error decode_bytes(const Bytes& b) {
  Message m;
  return decode(b, m);
}

void put_u16(Bytes& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v));
  b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void set_u16(Bytes& b, std::size_t at, std::uint16_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// Rewrites the header length after the body was changed by hand.
void fix_length(Bytes& b) { set_u16(b, 2, static_cast<std::uint16_t>(b.size() - kHeaderSize)); }

Bytes encoded_sample(const PlayerSample& s) { return must_encode(Message{s}); }

}  // namespace

TEST_CASE("every message survives a round trip") {
  for (const Message& m : test::every_message()) {
    CAPTURE(m.index());
    const Bytes b = must_encode(m);
    Message back;
    REQUIRE(decode(b, back) == Error::none);
    CHECK(back == m);
    CHECK(type_of(back) == type_of(m));
  }
}

TEST_CASE("the header is little-endian type then body length") {
  const Bytes b = must_encode(Ping{0x0102030405060708ull});
  REQUIRE(b.size() == kHeaderSize + 8);
  CHECK(b[0] == 6);  // PING
  CHECK(b[1] == 0);
  CHECK(b[2] == 8);
  CHECK(b[3] == 0);
  CHECK(b[4] == 0x08);  // lowest byte first, whatever the host
  CHECK(b[11] == 0x01);
}

TEST_CASE("floats are IEEE-754 little-endian") {
  PlayerSample s;
  s.position = Vec3{1.0f, 0.0f, 0.0f};
  const Bytes b = encoded_sample(s);
  // header 4, player 1, time component 4+12, position header 4
  const std::size_t at = 4 + 1 + 16 + 4;
  CHECK(b[at + 0] == 0x00);
  CHECK(b[at + 1] == 0x00);
  CHECK(b[at + 2] == 0x80);
  CHECK(b[at + 3] == 0x3F);
}

TEST_CASE("channels follow the transport table") {
  CHECK(channel_of(Hello{}) == kChannelSession);
  CHECK(channel_of(Chat{}) == kChannelSession);
  CHECK(channel_of(PlayerStatus{}) == kChannelSession);
  CHECK(channel_of(PlayerSample{}) == kChannelPlayerSamples);
}

TEST_CASE("an unknown sample component is skipped by its length") {
  Bytes b = encoded_sample(test::full_sample());
  // Append component 999 with 5 bytes of data, and component 0 with none.
  put_u16(b, 999);
  put_u16(b, 5);
  for (int i = 0; i < 5; ++i) b.push_back(0xAB);
  put_u16(b, 0);
  put_u16(b, 0);
  fix_length(b);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(back == Message{test::full_sample()});
}

TEST_CASE("an unknown message type is reported, not decoded") {
  Bytes b;
  put_u16(b, 500);
  put_u16(b, 2);
  b.push_back(1);
  b.push_back(2);
  CHECK(decode_bytes(b) == Error::unknown_type);
}

TEST_CASE("bad lengths are refused") {
  SUBCASE("header length past the end") {
    Bytes b = must_encode(Ping{1});
    set_u16(b, 2, 9);
    CHECK(decode_bytes(b) == Error::truncated);
  }
  SUBCASE("header length short of the end") {
    Bytes b = must_encode(Ping{1});
    b.push_back(0);
    CHECK(decode_bytes(b) == Error::trailing_bytes);
  }
  SUBCASE("body longer than the message needs") {
    Bytes b = must_encode(Ping{1});
    b.push_back(0);
    fix_length(b);
    CHECK(decode_bytes(b) == Error::trailing_bytes);
  }
  SUBCASE("component length past the body") {
    Bytes b = encoded_sample(test::full_sample());
    put_u16(b, 999);
    put_u16(b, 50);
    b.push_back(0);
    fix_length(b);
    CHECK(decode_bytes(b) == Error::bad_length);
  }
  SUBCASE("known component with the wrong length") {
    PlayerSample s;
    s.age = Age::child;
    Bytes b = encoded_sample(s);
    // The age component is last: id, length 1, value. Make it length 2.
    const std::size_t len_at = b.size() - 3;
    set_u16(b, len_at, 2);
    b.push_back(0);
    fix_length(b);
    CHECK(decode_bytes(b) == Error::bad_length);
    // And length 0.
    b.resize(b.size() - 2);
    set_u16(b, len_at, 0);
    fix_length(b);
    CHECK(decode_bytes(b) == Error::bad_length);
  }
  SUBCASE("string length past the body") {
    Bytes b = must_encode(Chat{0, "abc"});
    set_u16(b, kHeaderSize + 1, 4);
    CHECK(decode_bytes(b) == Error::bad_length);
  }
  SUBCASE("string over its maximum") {
    Bytes b;
    put_u16(b, static_cast<std::uint16_t>(MessageType::chat));
    put_u16(b, 0);
    b.push_back(0);
    put_u16(b, 201);
    for (int i = 0; i < 201; ++i) b.push_back('a');
    fix_length(b);
    CHECK(decode_bytes(b) == Error::string_too_long);
  }
}

TEST_CASE("truncated buffers are refused at every length") {
  for (const Message& m : test::every_message()) {
    const Bytes full = must_encode(m);
    for (std::size_t n = 0; n < full.size(); ++n) {
      CAPTURE(m.index());
      CAPTURE(n);
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
      CHECK(decode_bytes(cut) == Error::truncated);
      // Also cut, with the header length fixed to match: the body is short.
      if (n >= kHeaderSize) {
        Bytes fixed = cut;
        fix_length(fixed);
        Message back;
        const Error e = decode(fixed, back);
        // A TEST_PAYLOAD filler or an optional sample component can be cut
        // off cleanly, and so can any PLAYER_EQUIP component; anything else must
        // be refused.
        if (e == Error::none) {
          const bool may_shrink = std::holds_alternative<TestPayload>(m) ||
                                  std::holds_alternative<PlayerSample>(m) ||
                                  std::holds_alternative<PlayerEquip>(m) ||
                                  std::holds_alternative<HandoverState>(m) ||
                                  std::holds_alternative<ObjectSpawn>(m) ||
                                  std::holds_alternative<ObjectSample>(m);
          CHECK(may_shrink);
        }
      }
    }
  }
}

TEST_CASE("values out of range are refused") {
  CHECK(encode(Message{Welcome{0, 0, 60, {}}}) == std::nullopt);
  CHECK(encode(Message{Welcome{5, 0, 60, {}}}) == std::nullopt);
  CHECK(encode(Message{PlayerJoined{1, ""}}) == std::nullopt);
  CHECK(encode(Message{Chat{5, "x"}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, std::string(201, 'a')}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, std::string(200, 'a')}}) != std::nullopt);
  CHECK(encode(Message{Hello{0, "b", "g", "v", std::string(17, 'n')}}) == std::nullopt);
  // 16 characters of 3 bytes each is still 16 characters.
  std::string stars;
  for (int i = 0; i < 16; ++i) stars += "\xE2\x98\x85";
  CHECK(encode(Message{Hello{0, "b", "g", "v", stars}}) != std::nullopt);

  PlayerSample s;
  s.position = Vec3{0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f};
  CHECK(encode(Message{s}) == std::nullopt);
  s.position.reset();
  s.velocity = Vec3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
  CHECK(encode(Message{s}) == std::nullopt);

  SUBCASE("non-finite float on the wire") {
    PlayerSample ok;
    ok.position = Vec3{0.0f, 0.0f, 0.0f};
    Bytes b = encoded_sample(ok);
    // Last float's top bytes to 0x7F80: +infinity.
    b[b.size() - 2] = 0x80;
    b[b.size() - 1] = 0x7F;
    CHECK(decode_bytes(b) == Error::bad_value);
  }
  SUBCASE("age 2 on the wire") {
    PlayerSample ok;
    ok.age = Age::child;
    Bytes b = encoded_sample(ok);
    b.back() = 2;
    CHECK(decode_bytes(b) == Error::bad_value);
  }
  SUBCASE("player 5 on the wire") {
    Bytes b = must_encode(PlayerLeft{1, LeaveReason::left});
    b[kHeaderSize] = 5;
    CHECK(decode_bytes(b) == Error::bad_value);
  }
  SUBCASE("bad UTF-8 on the wire") {
    Bytes b = must_encode(Chat{0, "ab"});
    b.back() = 0xFF;
    CHECK(decode_bytes(b) == Error::bad_utf8);
    b.back() = 0xC0;  // lead byte with no continuation
    CHECK(decode_bytes(b) == Error::bad_utf8);
  }
  SUBCASE("overlong and surrogate UTF-8") {
    CHECK(encode(Message{Chat{0, "\xC0\xAF"}}) == std::nullopt);
    CHECK(encode(Message{Chat{0, "\xED\xA0\x80"}}) == std::nullopt);
    CHECK(encode(Message{Chat{0, "\xF4\x90\x80\x80"}}) == std::nullopt);
  }
}

TEST_CASE("a WELCOME a client could trip over is refused") {
  // Tick rate 0 would make a client divide by zero.
  CHECK(encode(Message{Welcome{1, 0, 0, {}}}) == std::nullopt);
  // The same player twice, or the new player in the list of those already in.
  CHECK(encode(Message{Welcome{1, 0, 60, {{2, "a"}, {2, "b"}}}}) == std::nullopt);
  CHECK(encode(Message{Welcome{1, 0, 60, {{1, "a"}}}}) == std::nullopt);

  SUBCASE("on the wire") {
    Bytes b = must_encode(Welcome{1, 0, 60, {{2, "a"}, {3, "b"}}});
    // header 4, player 1, time 8, tick 2, count 1, then (number, u16 len, 'a')
    const std::size_t first = kHeaderSize + 1 + 8 + 2 + 1;
    const std::size_t second = first + 1 + 2 + 1;
    Bytes dup = b;
    dup[second] = 2;
    CHECK(decode_bytes(dup) == Error::bad_value);
    Bytes self = b;
    self[first] = 1;
    CHECK(decode_bytes(self) == Error::bad_value);
    Bytes zero_tick = b;
    zero_tick[kHeaderSize + 1 + 8] = 0;
    CHECK(decode_bytes(zero_tick) == Error::bad_value);
  }
}

TEST_CASE("control characters are refused in every string") {
  CHECK(encode(Message{Chat{0, std::string("a\0b", 3)}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, "line\nbreak"}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, "esc\x1B[2J"}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, "del\x7F"}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, "c1 \xC2\x9B"}}) == std::nullopt);  // U+009B
  CHECK(encode(Message{PlayerJoined{1, "Na\tvi"}}) == std::nullopt);
  CHECK(encode(Message{Chat{0, "plain \xC2\xA0 text"}}) != std::nullopt);  // U+00A0

  Bytes b = must_encode(PlayerJoined{1, "Navi"});
  b.back() = 0x00;  // "Nav\0" on the wire
  CHECK(decode_bytes(b) == Error::bad_value);
}

TEST_CASE("a sample needs its time component, once") {
  SUBCASE("missing") {
    Bytes b;
    put_u16(b, static_cast<std::uint16_t>(MessageType::player_sample));
    put_u16(b, 0);
    b.push_back(0);
    fix_length(b);
    CHECK(decode_bytes(b) == Error::missing_component);
  }
  SUBCASE("twice") {
    PlayerSample s;
    Bytes b = encoded_sample(s);
    const Bytes time_component(b.begin() + kHeaderSize + 1, b.end());
    b.insert(b.end(), time_component.begin(), time_component.end());
    fix_length(b);
    CHECK(decode_bytes(b) == Error::duplicate_component);
  }
}

TEST_CASE("every refuse reason has its number on the wire and round trips") {
  const std::pair<RefuseReason, std::uint8_t> reasons[] = {
      {RefuseReason::wrong_protocol_version, 1}, {RefuseReason::wrong_game_build, 2},
      {RefuseReason::server_full, 3},            {RefuseReason::name_taken, 4},
      {RefuseReason::bad_name, 5}};
  for (const auto& [reason, number] : reasons) {
    Bytes b = must_encode(Refused{reason});
    REQUIRE(b.size() == 5);
    CHECK(b.back() == number);
    Message m;
    REQUIRE(decode(b, m) == Error::none);
    CHECK(std::get<Refused>(m).reason == reason);
  }
}

TEST_CASE("unknown reasons and status bits are kept, not refused") {
  Bytes b = must_encode(Refused{RefuseReason::name_taken});
  b.back() = 200;
  Message m;
  REQUIRE(decode(b, m) == Error::none);
  CHECK(static_cast<int>(std::get<Refused>(m).reason) == 200);

  const Bytes s = must_encode(PlayerStatus{1, 0xFF});
  REQUIRE(decode(s, m) == Error::none);
  CHECK(std::get<PlayerStatus>(m).flags == 0xFF);
}

TEST_CASE("the protocol version check") {
  Hello hello{kProtocolVersion, "recomp 0.1", "e0f3ccfaf50065ae", "1.1.1", "Link"};
  CHECK(check_hello(hello, "e0f3ccfaf50065ae") == std::nullopt);
  CHECK(check_hello(hello, "0000000000000000") == RefuseReason::wrong_game_build);

  hello.protocol_version = kProtocolVersion + 1;
  CHECK(check_hello(hello, "e0f3ccfaf50065ae") == RefuseReason::wrong_protocol_version);

  // A HELLO from a newer protocol whose layout this build cannot read still
  // gives up its version, so the server can refuse it with the right reason.
  Bytes future;
  put_u16(future, static_cast<std::uint16_t>(MessageType::hello));
  put_u16(future, 0);
  put_u16(future, 7);
  future.push_back(0xEE);  // something this build cannot read
  fix_length(future);
  CHECK(decode_bytes(future) != Error::none);
  CHECK(hello_protocol_version(future) == std::optional<std::uint16_t>{7});

  CHECK(hello_protocol_version(must_encode(hello)) ==
        std::optional<std::uint16_t>{kProtocolVersion + 1});
  CHECK(hello_protocol_version(must_encode(Ping{1})) == std::nullopt);
  CHECK(hello_protocol_version(Bytes{1, 0, 2, 0, 9}) == std::nullopt);
}

TEST_CASE("encode leaves the buffer alone on an error") {
  Bytes out{9, 9};
  CHECK(encode(Message{Chat{9, "x"}}, out) == Error::bad_value);
  CHECK(out == Bytes{9, 9});

  TestPayload big;
  big.filler.assign(70'000, 0);
  CHECK(encode(Message{big}, out) == Error::too_large);
  CHECK(out == Bytes{9, 9});

  TestPayload biggest;
  biggest.filler.assign(65'535 - 4, 0);
  CHECK(encode(Message{biggest}, out) == Error::none);
  CHECK(out.size() == 2 + kHeaderSize + 65'535);
}

TEST_CASE("every error has a name") {
  for (int e = 0; e <= static_cast<int>(Error::wrong_sender); ++e) {
    CHECK(to_string(static_cast<Error>(e)) != "unknown error");
  }
}

TEST_CASE("channel of a message type") {
  CHECK(channel_of(MessageType::hello) == kChannelSession);
  CHECK(channel_of(MessageType::player_sample) == kChannelPlayerSamples);
  CHECK(channel_of(MessageType::test_payload) == kChannelPlayerSamples);
  for (const Message& m : test::every_message()) {
    CHECK(channel_of(type_of(m)) == channel_of(m));
    CHECK(allowed_on_channel(type_of(m), channel_of(m)));
  }
  CHECK(allowed_on_channel(MessageType::test_payload, kChannelSession));
  CHECK_FALSE(allowed_on_channel(MessageType::chat, kChannelPlayerSamples));
  CHECK_FALSE(allowed_on_channel(MessageType::player_sample, kChannelSession));
  CHECK_FALSE(allowed_on_channel(MessageType::hello, kChannelCount));
  CHECK_FALSE(allowed_on_channel(static_cast<MessageType>(999), kChannelSession));
}

TEST_CASE("who may send what") {
  CHECK(may_send(Sender::client, MessageType::hello));
  CHECK_FALSE(may_send(Sender::server, MessageType::hello));
  for (MessageType t : {MessageType::welcome, MessageType::refused,
                        MessageType::player_joined, MessageType::player_left,
                        MessageType::host}) {
    CHECK(may_send(Sender::server, t));
    CHECK_FALSE(may_send(Sender::client, t));
  }
  for (MessageType t : {MessageType::ping, MessageType::pong, MessageType::player_status,
                        MessageType::chat, MessageType::player_sample,
                        MessageType::test_payload}) {
    CHECK(may_send(Sender::client, t));
    CHECK(may_send(Sender::server, t));
  }
  CHECK_FALSE(may_send(Sender::client, static_cast<MessageType>(999)));
}

TEST_CASE("check_received checks direction, channel and player field") {
  CHECK(check_received(Chat{0, "hi"}, Sender::client, kChannelSession) == Error::none);
  CHECK(check_received(Chat{2, "hi"}, Sender::server, kChannelSession) == Error::none);
  // A client may not claim to be someone; the server always says who.
  CHECK(check_received(Chat{2, "hi"}, Sender::client, kChannelSession) == Error::wrong_sender);
  CHECK(check_received(Chat{0, "hi"}, Sender::server, kChannelSession) == Error::wrong_sender);
  CHECK(check_received(PlayerStatus{3, 0}, Sender::client, kChannelSession) ==
        Error::wrong_sender);
  CHECK(check_received(test::full_sample(), Sender::client, kChannelPlayerSamples) ==
        Error::wrong_sender);
  // Only the server sends these.
  CHECK(check_received(Welcome{1, 0, 60, {}}, Sender::client, kChannelSession) ==
        Error::wrong_sender);
  CHECK(check_received(Hello{}, Sender::server, kChannelSession) == Error::wrong_sender);
  // Wrong channel.
  CHECK(check_received(Chat{0, "hi"}, Sender::client, kChannelPlayerSamples) ==
        Error::wrong_channel);
  CHECK(check_received(PlayerSample{}, Sender::client, kChannelSession) == Error::wrong_channel);
  CHECK(check_received(TestPayload{}, Sender::client, kChannelSession) == Error::none);
  CHECK(check_received(Ping{}, Sender::server, kChannelSession) == Error::none);
}

TEST_CASE("the server stamps the sender on a relayed message") {
  Message m = Chat{0, "hi"};
  CHECK(sender_of(m) == std::optional<std::uint8_t>{0});
  CHECK(set_sender(m, 3));
  CHECK(std::get<Chat>(m).player_number == 3);
  CHECK(sender_of(m) == std::optional<std::uint8_t>{3});
  CHECK_FALSE(set_sender(m, 0));
  CHECK_FALSE(set_sender(m, 5));
  CHECK(std::get<Chat>(m).player_number == 3);

  Message s = PlayerSample{};
  CHECK(set_sender(s, 4));
  CHECK(std::get<PlayerSample>(s).player_number == 4);
  Message st = PlayerStatus{};
  CHECK(set_sender(st, 1));
  CHECK(std::get<PlayerStatus>(st).player_number == 1);

  Message p = Ping{};
  CHECK_FALSE(set_sender(p, 1));
  CHECK(sender_of(p) == std::nullopt);
}

// ---------------------------------------------------------------- stage 1 steps 3 and 4

TEST_CASE("join partner and flag messages have their numbers, channels and senders") {
  CHECK(static_cast<int>(MessageType::join_partner) == 12);
  CHECK(static_cast<int>(MessageType::flag_set) == 13);
  CHECK(static_cast<int>(MessageType::flag_state) == 14);
  CHECK(static_cast<int>(MessageType::flag_snapshot) == 15);
  CHECK(channel_of(MessageType::join_partner) == kChannelSession);
  for (MessageType t : {MessageType::flag_set, MessageType::flag_state,
                        MessageType::flag_snapshot}) {
    CHECK(channel_of(t) == kChannelProgress);
    CHECK(allowed_on_channel(t, kChannelProgress));
    CHECK_FALSE(allowed_on_channel(t, kChannelSession));
    CHECK(may_send(Sender::client, t));
    CHECK(may_send(Sender::server, t));
  }
  CHECK_FALSE(allowed_on_channel(MessageType::join_partner, kChannelProgress));
  CHECK(may_send(Sender::client, MessageType::join_partner));
  CHECK(may_send(Sender::server, MessageType::join_partner));
}

TEST_CASE("join partner layout on the wire") {
  const Bytes ask = must_encode(test::join_messages()[0]);
  // player 0, other 2, request id 7, kind 0, nothing more.
  CHECK(ask == Bytes{12, 0, 5, 0, 0, 2, 7, 0, 0});
  const Bytes no = must_encode(test::join_messages()[2]);
  CHECK(no == Bytes{12, 0, 6, 0, 0, 3, 8, 0, 2, 3});
  const Bytes yes = must_encode(test::join_messages()[1]);
  // header, 5 fixed bytes, scene (2 + 6), 7 floats.
  CHECK(yes.size() == kHeaderSize + 5 + 8 + 28);
}

TEST_CASE("bad join partner messages are refused") {
  auto refused = [](const JoinPartner& j) { return encode(Message{j}) == std::nullopt; };
  JoinPartner j = test::join_messages()[0];
  j.other_player = 0;
  CHECK(refused(j));
  j.other_player = 5;
  CHECK(refused(j));
  j = test::join_messages()[0];
  j.player_number = 2;  // relayed: asks to join itself
  CHECK(refused(j));
  j = test::join_messages()[0];
  j.target = JoinTarget{"spot00", {}, {}};  // only an accept carries a place
  CHECK(refused(j));
  j = test::join_messages()[1];
  j.target.reset();
  CHECK(refused(j));
  j = test::join_messages()[1];
  j.target->scene = "";
  CHECK(refused(j));
  j = test::join_messages()[1];
  j.target->position[1] = std::numeric_limits<float>::infinity();
  CHECK(refused(j));
  j = test::join_messages()[1];
  j.target->rotation[3] = std::numeric_limits<float>::quiet_NaN();
  CHECK(refused(j));
  j = test::join_messages()[0];
  j.kind = static_cast<JoinKind>(3);
  CHECK(refused(j));

  // The same on decode, from hand-made bytes.
  Bytes b = must_encode(test::join_messages()[0]);
  b[kHeaderSize + 4] = 3;  // unknown kind: the rest cannot be read
  CHECK(decode_bytes(b) == Error::bad_value);
  b = must_encode(test::join_messages()[0]);
  b[kHeaderSize + 1] = 0;
  CHECK(decode_bytes(b) == Error::bad_value);
  b = must_encode(test::join_messages()[0]);
  b[kHeaderSize] = 2;  // from player 2, naming player 2
  CHECK(decode_bytes(b) == Error::bad_value);
  b = must_encode(test::join_messages()[0]);
  b.push_back(0);  // a request has nothing after its kind
  fix_length(b);
  CHECK(decode_bytes(b) == Error::trailing_bytes);
  b = must_encode(test::join_messages()[1]);
  set_u16(b, kHeaderSize + 5, 0);  // an empty scene
  b.erase(b.begin() + kHeaderSize + 7, b.begin() + kHeaderSize + 13);
  fix_length(b);
  CHECK(decode_bytes(b) == Error::bad_value);
}

TEST_CASE("an unknown join refuse reason is kept") {
  JoinPartner j = test::join_messages()[2];
  j.reason = static_cast<JoinRefuseReason>(200);
  const Bytes b = must_encode(j);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(static_cast<int>(std::get<JoinPartner>(back).reason) == 200);
}

TEST_CASE("join partner is relayed with the asker stamped in") {
  Message m = test::join_messages()[0];  // player 0 asks to join player 2
  CHECK(check_received(m, Sender::client, kChannelSession) == Error::none);
  CHECK(check_received(m, Sender::server, kChannelSession) == Error::wrong_sender);
  CHECK_FALSE(set_sender(m, 2));  // player 2 asking to join itself
  CHECK(std::get<JoinPartner>(m).player_number == 0);
  CHECK(set_sender(m, 1));
  CHECK(check_received(m, Sender::server, kChannelSession) == Error::none);
  const Bytes b = must_encode(m);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<JoinPartner>(back).player_number == 1);
  CHECK(std::get<JoinPartner>(back).other_player == 2);
}

TEST_CASE("flag entry layout on the wire") {
  const Bytes b = must_encode(FlagSet{0, {{0x04030201u, 0x0605, FlagKind::int32, 0x0A090807u}}});
  CHECK(b == Bytes{13, 0, 15, 0, 0, 1, 0, 0, 1, 2, 3, 4, 5, 6, 1, 7, 8, 9, 10});
  const Bytes s = must_encode(FlagSnapshot{1, 2, {}});
  CHECK(s == Bytes{15, 0, 6, 0, 1, 0, 2, 0, 0, 0});
  // A 64-bit key: key kind 1, u64 key, u32 table, then kind and value.
  const Bytes k = must_encode(FlagState{
      0, {{FlagKey::key64_flag(0x0807060504030201ull, 0x0C0B0A09u), FlagKind::boolean, 1}}});
  CHECK(k == Bytes{14, 0, 21, 0, 0, 1, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0,
                   1, 0, 0, 0});
  CHECK(k.size() == kHeaderSize + 3 + kFlagEntryKey64Size);
}

TEST_CASE("64-bit-key flags") {
  auto refused = [](const Message& m) { return encode(m) == std::nullopt; };
  const FlagKey chest = FlagKey::key64_flag(0x2fa4c1d09b7e6a35ull);
  CHECK(chest.kind == FlagKeyKind::key64);
  CHECK(chest.hash == 0);
  CHECK(chest.index == 0);
  CHECK(chest.table == 0);
  CHECK(valid_flag({chest, FlagKind::boolean, 1}));
  CHECK(valid_flag({0x1234u, 0, FlagKind::int32, 5}));

  // Always a bool, 0 or 1.
  CHECK(refused(FlagSet{0, {{chest, FlagKind::boolean, 2}}}));
  CHECK(refused(FlagSet{0, {{chest, FlagKind::int32, 1}}}));
  CHECK(refused(FlagSet{0, {{chest, FlagKind::uint32, 1}}}));
  CHECK(refused(FlagSet{0, {{chest, FlagKind::float32, 0}}}));
  // The fields of the other kind stay 0, so one flag has one key.
  FlagKey stray = chest;
  stray.hash = 1;
  CHECK(refused(FlagSet{0, {{stray, FlagKind::boolean, 1}}}));
  stray = chest;
  stray.index = 1;
  CHECK(refused(FlagSet{0, {{stray, FlagKind::boolean, 1}}}));
  FlagKey named{0x10u, 0};
  named.key = 5;
  CHECK(refused(FlagSet{0, {{named, FlagKind::boolean, 1}}}));
  named = FlagKey{0x10u, 0};
  named.table = 5;
  CHECK(refused(FlagSet{0, {{named, FlagKind::boolean, 1}}}));
  FlagKey unknown = chest;
  unknown.kind = static_cast<FlagKeyKind>(2);
  CHECK(refused(FlagSet{0, {{unknown, FlagKind::boolean, 1}}}));

  // The same key twice is refused; the same key in another table, or a
  // named flag with the same number, is another flag.
  CHECK(refused(FlagState{0, {{chest, FlagKind::boolean, 1}, {chest, FlagKind::boolean, 0}}}));
  CHECK(split_snapshot(std::vector<FlagEntry>{{chest, FlagKind::boolean, 1},
                                              {chest, FlagKind::boolean, 0}}) == std::nullopt);
  CHECK_FALSE(refused(FlagState{0, {{chest, FlagKind::boolean, 1},
                                    {FlagKey::key64_flag(chest.key, 7), FlagKind::boolean, 1},
                                    {FlagKey::key64_flag(1), FlagKind::boolean, 0},
                                    {1u, 0, FlagKind::boolean, 0}}}));

  // Named and 64-bit keys mixed in one list survive a round trip.
  const FlagSnapshot mixed{0, 1, {{0x99u, 3, FlagKind::int32, 7}, {chest, FlagKind::boolean, 1}}};
  Message back;
  REQUIRE(decode(must_encode(mixed), back) == Error::none);
  CHECK(std::get<FlagSnapshot>(back) == mixed);

  SUBCASE("on decode") {
    Bytes b = must_encode(FlagSet{0, {{chest, FlagKind::boolean, 1}}});
    const std::size_t entry = kHeaderSize + 1 + 2;
    b[entry] = 2;  // an unknown key kind: the rest cannot be read
    CHECK(decode_bytes(b) == Error::bad_value);
    b[entry] = 1;
    b[entry + 13] = 1;  // kind int32
    CHECK(decode_bytes(b) == Error::bad_value);
    b[entry + 13] = 0;
    b[entry + 14] = 2;  // a boolean of 2
    CHECK(decode_bytes(b) == Error::bad_value);
    b[entry + 14] = 1;
    CHECK(decode_bytes(b) == Error::none);
    // A key64 entry cut short: the count says one, the bytes hold less.
    Bytes cut = b;
    cut.resize(cut.size() - 3);
    fix_length(cut);
    CHECK(decode_bytes(cut) == Error::truncated);
    // A named entry read as a key64 one runs past the body.
    Bytes named_as64 = must_encode(FlagSet{0, {{1u, 0, FlagKind::boolean, 1}}});
    named_as64[entry] = 1;
    CHECK(decode_bytes(named_as64) == Error::truncated);
  }
}

TEST_CASE("count deltas: FLAG_SET only, named keys only") {
  auto refused = [](const Message& m) { return encode(m) == std::nullopt; };
  const FlagEntry minus_one{0x90237092u, 0, FlagKind::count_delta, 0xFFFFFFFFu};
  CHECK(static_cast<int>(FlagKind::count_delta) == 4);
  CHECK(valid_flag(minus_one));
  CHECK_FALSE(valid_flag({FlagKey::key64_flag(5), FlagKind::count_delta, 1}));

  // Layout: kind 4, then the change as s32.
  const Bytes b = must_encode(FlagSet{0, {minus_one}});
  CHECK(b == Bytes{13, 0, 15, 0, 0, 1, 0, 0, 0x92, 0x70, 0x23, 0x90, 0, 0, 4, 0xFF, 0xFF,
                   0xFF, 0xFF});
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<FlagSet>(back) == FlagSet{0, {minus_one}});

  // An answer or a snapshot carries totals, never a change.
  CHECK(refused(FlagState{0, {minus_one}}));
  CHECK(refused(FlagSnapshot{0, 1, {minus_one}}));
  CHECK(refused(FlagSet{0, {{FlagKey::key64_flag(5), FlagKind::count_delta, 1}}}));
  // A delta and a total of the same key in one list is still the key twice.
  CHECK(refused(FlagSet{0, {minus_one, {0x90237092u, 0, FlagKind::int32, 2}}}));

  SUBCASE("on decode") {
    Bytes s = must_encode(FlagState{0, {{0x90237092u, 0, FlagKind::int32, 2}}});
    const std::size_t kind = kHeaderSize + 1 + 2 + 7;
    REQUIRE(s[kind] == 1);
    s[kind] = 4;
    CHECK(decode_bytes(s) == Error::bad_value);
    Bytes p = must_encode(FlagSnapshot{0, 1, {{0x90237092u, 0, FlagKind::int32, 2}}});
    p[kHeaderSize + 4 + 2 + 7] = 4;
    CHECK(decode_bytes(p) == Error::bad_value);
    s[0] = 13;  // the same bytes as FLAG_SET are fine
    CHECK(decode_bytes(s) == Error::none);
  }
}

TEST_CASE("bad flag messages are refused") {
  auto refused = [](const Message& m) { return encode(m) == std::nullopt; };
  CHECK(refused(FlagSet{0, {}}));  // a change of nothing
  CHECK(refused(FlagState{0, {}}));
  CHECK(refused(FlagSet{5, {test::flag_entries()[0]}}));
  CHECK(refused(FlagSet{0, {{1, 0, FlagKind::boolean, 2}}}));
  CHECK(refused(FlagSet{0, {{1, 0, static_cast<FlagKind>(5), 0}}}));
  CHECK(refused(FlagSet{0, {{1, 0, FlagKind::float32, 0x7F800000u}}}));  // infinity
  CHECK(refused(FlagSet{0, {{1, 0, FlagKind::float32, 0x7FC00000u}}}));  // NaN
  CHECK(refused(FlagState{0, {{1, 2, FlagKind::int32, 5}, {1, 2, FlagKind::int32, 6}}}));
  CHECK_FALSE(refused(FlagState{0, {{1, 2, FlagKind::int32, 5}, {1, 3, FlagKind::int32, 6}}}));
  CHECK(refused(FlagSnapshot{0, 0, {}}));
  CHECK(refused(FlagSnapshot{3, 3, {}}));

  SUBCASE("on decode") {
    Bytes b = must_encode(FlagSet{0, {{1, 0, FlagKind::boolean, 1}}});
    b[kHeaderSize + 1 + 2 + 7] = 5;  // kind (4 is a count delta)
    CHECK(decode_bytes(b) == Error::bad_value);
    b[kHeaderSize + 1 + 2 + 7] = 0;
    b[kHeaderSize + 1 + 2 + 8] = 2;  // a boolean of 2
    CHECK(decode_bytes(b) == Error::bad_value);
    b = must_encode(FlagSet{0, {{1, 0, FlagKind::boolean, 1}}});
    set_u16(b, kHeaderSize + 1, 0);  // count 0, an entry left over
    CHECK(decode_bytes(b) == Error::bad_value);
    set_u16(b, kHeaderSize + 1, 2);  // count past the body
    CHECK(decode_bytes(b) == Error::truncated);
    b = must_encode(FlagSet{0, {{1, 0, FlagKind::boolean, 1}}});
    b.push_back(0);  // part of an entry more than the count says
    fix_length(b);
    CHECK(decode_bytes(b) == Error::trailing_bytes);
    // The same flag twice.
    b = must_encode(FlagSet{0, {{1, 0, FlagKind::boolean, 1}, {2, 0, FlagKind::boolean, 1}}});
    b[kHeaderSize + 1 + 2 + kFlagEntrySize + 1] = 1;  // the second hash, past its key kind
    CHECK(decode_bytes(b) == Error::bad_value);
    // A part number past the part count, and a part count of 0.
    b = must_encode(FlagSnapshot{1, 2, {}});
    b[kHeaderSize] = 2;
    CHECK(decode_bytes(b) == Error::bad_value);
    b[kHeaderSize] = 0;
    b[kHeaderSize + 2] = 0;
    CHECK(decode_bytes(b) == Error::bad_value);
  }
}

namespace {
std::vector<FlagEntry> many_flags(std::size_t n) {
  std::vector<FlagEntry> flags(n);
  for (std::size_t i = 0; i < n; ++i) {
    flags[i] = {static_cast<std::uint32_t>(i * 2654435761u), static_cast<std::uint16_t>(i),
                FlagKind::int32, static_cast<std::uint32_t>(i)};
  }
  return flags;
}
}  // namespace

TEST_CASE("a flag message holds at most kMaxFlagEntries") {
  const auto full = many_flags(kMaxFlagEntries);
  const Bytes b = must_encode(FlagSnapshot{0, 1, full});
  CHECK(b.size() == kHeaderSize + 6 + kMaxFlagEntries * kFlagEntrySize);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<FlagSnapshot>(back).flags == full);

  const auto over = many_flags(kMaxFlagEntries + 1);
  Bytes out;
  CHECK(encode(FlagSnapshot{0, 1, over}, out) == Error::too_large);
  CHECK(encode(FlagState{0, over}, out) == Error::too_large);
  CHECK(encode(FlagSet{0, over}, out) == Error::too_large);
  CHECK(out.empty());

  // A full message of 64-bit keys still fits the u16 body length.
  std::vector<FlagEntry> keys(kMaxFlagEntries);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    keys[i] = {FlagKey::key64_flag(0xffff'0000'0000'0000ull + i), FlagKind::boolean, 1};
  }
  const Bytes k = must_encode(FlagSnapshot{0, 1, keys});
  CHECK(k.size() == kHeaderSize + 6 + kMaxFlagEntries * kFlagEntryKey64Size);
  CHECK(k.size() - kHeaderSize <= 0xffffu);
  REQUIRE(decode(k, back) == Error::none);
  CHECK(std::get<FlagSnapshot>(back).flags == keys);

  // A hand-made oversize snapshot: one entry past the limit, all there.
  Bytes big;
  put_u16(big, static_cast<std::uint16_t>(MessageType::flag_snapshot));
  put_u16(big, 0);
  put_u16(big, 0);
  put_u16(big, 1);
  put_u16(big, static_cast<std::uint16_t>(kMaxFlagEntries + 1));
  big.resize(big.size() + (kMaxFlagEntries + 1) * kFlagEntrySize, 0);
  fix_length(big);
  CHECK(decode_bytes(big) == Error::bad_value);
}

TEST_CASE("a large flag table is split into snapshot parts") {
  const auto table = many_flags(kMaxFlagEntries * 2 + 5);
  const auto parts = split_snapshot(table);
  REQUIRE(parts.has_value());
  REQUIRE(parts->size() == 3);
  std::vector<FlagEntry> joined;
  for (std::size_t i = 0; i < parts->size(); ++i) {
    const FlagSnapshot& p = (*parts)[i];
    CHECK(p.part == i);
    CHECK(p.part_count == 3);
    const Bytes b = must_encode(p);
    Message back;
    REQUIRE(decode(b, back) == Error::none);
    CHECK(std::get<FlagSnapshot>(back) == p);
    joined.insert(joined.end(), p.flags.begin(), p.flags.end());
  }
  CHECK(joined == table);
  CHECK((*parts)[2].flags.size() == 5);

  const auto empty = split_snapshot({});
  REQUIRE(empty.has_value());
  REQUIRE(empty->size() == 1);
  CHECK((*empty)[0] == FlagSnapshot{0, 1, {}});

  auto twice = many_flags(10);
  twice.push_back(twice[3]);
  CHECK_FALSE(split_snapshot(twice).has_value());
}

TEST_CASE("flag hash is Murmur3 x86 32-bit with seed 0") {
  CHECK(flag_hash("") == 0u);
  CHECK(flag_hash("hello") == 0x248bfa47u);
  CHECK(flag_hash("The quick brown fox jumps over the lazy dog") == 0x2e4ff723u);
}

TEST_CASE("flag set and state carry the relaying player") {
  Message m = FlagSet{0, test::flag_entries()};
  CHECK(check_received(m, Sender::client, kChannelProgress) == Error::none);
  CHECK(check_received(m, Sender::client, kChannelSession) == Error::wrong_channel);
  CHECK(set_sender(m, 4));
  CHECK(check_received(m, Sender::server, kChannelProgress) == Error::none);
  Message st = FlagState{0, test::flag_entries()};
  CHECK(set_sender(st, 1));
  CHECK(sender_of(st) == std::optional<std::uint8_t>{1});
  Message snap = FlagSnapshot{0, 1, {}};
  CHECK(sender_of(snap) == std::nullopt);
  CHECK_FALSE(set_sender(snap, 1));
  CHECK(check_received(snap, Sender::server, kChannelProgress) == Error::none);
}

namespace {
// A reader of protocol version 0 as it was before types 12 to 15: it knows
// types 1 to 11 only, and reads the header of anything else to skip it.
Error stage0_decode(const Bytes& b, Message& out) {
  if (b.size() < kHeaderSize) return Error::truncated;
  const auto type = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
  if (type < 1 || type > static_cast<std::uint16_t>(MessageType::test_payload)) {
    const auto length = static_cast<std::size_t>(b[2] | (b[3] << 8));
    if (length != b.size() - kHeaderSize) return Error::truncated;
    return Error::unknown_type;
  }
  return decode(b, out);
}
}  // namespace

TEST_CASE("a reader without the new types skips them") {
  std::vector<Message> fresh;
  for (const JoinPartner& j : test::join_messages()) fresh.push_back(j);
  fresh.push_back(FlagSet{0, test::flag_entries()});
  fresh.push_back(FlagState{2, test::flag_entries()});
  fresh.push_back(FlagSnapshot{0, 1, many_flags(kMaxFlagEntries)});
  for (const Message& m : fresh) {
    const Bytes b = must_encode(m);
    // Numbered after every stage 0 type, so an old dispatch never claims it,
    // and its header length says exactly where it ends.
    const auto type = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    CHECK(type > static_cast<std::uint16_t>(MessageType::test_payload));
    Message old;
    CHECK(stage0_decode(b, old) == Error::unknown_type);
  }
  // A stage 0 message still reads the same.
  Message back;
  CHECK(stage0_decode(must_encode(Chat{0, "hi"}), back) == Error::none);
  // And this build in turn skips a type newer than itself.
  Bytes future;
  put_u16(future, 39);
  put_u16(future, 0);
  CHECK(decode_bytes(future) == Error::unknown_type);
}

TEST_CASE("host names the hosting player, from the server on the progress channel") {
  CHECK(static_cast<int>(MessageType::host) == 16);
  CHECK(channel_of(MessageType::host) == kChannelProgress);
  CHECK_FALSE(allowed_on_channel(MessageType::host, kChannelSession));
  CHECK(may_send(Sender::server, MessageType::host));
  CHECK_FALSE(may_send(Sender::client, MessageType::host));
  const Bytes b = must_encode(Host{3});
  CHECK(b == Bytes{16, 0, 1, 0, 3});
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<Host>(back) == Host{3});
  CHECK(check_received(back, Sender::server, kChannelProgress) == Error::none);
  CHECK(check_received(back, Sender::client, kChannelProgress) == Error::wrong_sender);
  CHECK(sender_of(back) == std::nullopt);
  Bytes out;
  CHECK(encode(Host{0}, out) == Error::bad_value);
  CHECK(encode(Host{5}, out) == Error::bad_value);
  CHECK(decode_bytes(Bytes{16, 0, 1, 0, 0}) == Error::bad_value);
  CHECK(decode_bytes(Bytes{16, 0, 2, 0, 1, 0}) == Error::trailing_bytes);
  // A stage 0 reader skips it like the other new types.
  Message old;
  CHECK(stage0_decode(b, old) == Error::unknown_type);
}

TEST_CASE("player equip: number, channel, senders and layout") {
  CHECK(static_cast<int>(MessageType::player_equip) == 17);
  CHECK(channel_of(MessageType::player_equip) == kChannelSession);
  CHECK_FALSE(allowed_on_channel(MessageType::player_equip, kChannelPlayerSamples));
  CHECK(may_send(Sender::client, MessageType::player_equip));
  CHECK(may_send(Sender::server, MessageType::player_equip));

  PlayerEquip m;
  m.equipment.weapon = "Ab";
  m.equipment.armor = "";
  m.equipment.drawn = 0x81;
  const Bytes b = must_encode(m);
  // type 17, length; player 0; weapon (1) "Ab"; armor (3) ""; drawn (9) 0x81.
  CHECK(b == Bytes{17, 0, 20, 0, 0,
                   1, 0, 4, 0, 2, 0, 'A', 'b',
                   3, 0, 2, 0, 0, 0,
                   9, 0, 1, 0, 0x81});
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<PlayerEquip>(back) == m);
  CHECK(check_received(back, Sender::client, kChannelSession) == Error::none);
  CHECK(check_received(back, Sender::server, kChannelSession) == Error::wrong_sender);
  CHECK(check_received(back, Sender::client, kChannelProgress) == Error::wrong_channel);
  REQUIRE(set_sender(back, 2));
  CHECK(sender_of(back) == 2);
  CHECK(check_received(back, Sender::server, kChannelSession) == Error::none);

  const PlayerEquip full = test::equip_message();
  REQUIRE(decode(must_encode(full), back) == Error::none);
  CHECK(std::get<PlayerEquip>(back) == full);
  // Nothing known is also a message: the player field alone.
  CHECK(must_encode(PlayerEquip{}) == Bytes{17, 0, 1, 0, 0});
}

TEST_CASE("player equip: unknown components are skipped, bad ones refused") {
  Message back;
  // An unknown component 200 between two known ones.
  CHECK(decode(Bytes{17, 0, 18, 0, 0, 2, 0, 3, 0, 1, 0, 'x', 200, 0, 1, 0, 7, 9, 0, 1, 0, 2},
               back) == Error::none);
  CHECK(std::get<PlayerEquip>(back).equipment.shield == "x");
  CHECK(std::get<PlayerEquip>(back).equipment.drawn == 2);
  // The held item (10).
  CHECK(decode(Bytes{17, 0, 8, 0, 0, 10, 0, 3, 0, 1, 0, 's'}, back) == Error::none);
  CHECK(std::get<PlayerEquip>(back).equipment.held == "s");
  // The other hand slots (11 to 15), each a string like the held item.
  CHECK(decode(Bytes{17, 0, 35, 0, 0, 11, 0, 3, 0, 1, 0, 'a', 12, 0, 3, 0, 1, 0, 'b',
                     13, 0, 3, 0, 1, 0, 'c', 14, 0, 3, 0, 1, 0, 'd', 15, 0, 2, 0, 0, 0},
               back) == Error::none);
  CHECK(std::get<PlayerEquip>(back).equipment.weapon_slot == "a");
  CHECK(std::get<PlayerEquip>(back).equipment.sub_weapon == "b");
  CHECK(std::get<PlayerEquip>(back).equipment.third_weapon == "c");
  CHECK(std::get<PlayerEquip>(back).equipment.tool == "d");
  CHECK(std::get<PlayerEquip>(back).equipment.sub_tool == "");
  CHECK_FALSE(std::get<PlayerEquip>(back).equipment.held);
  CHECK_FALSE(std::get<PlayerEquip>(back).equipment.weapon);
  // The same component twice.
  CHECK(decode_bytes(Bytes{17, 0, 11, 0, 0, 9, 0, 1, 0, 1, 9, 0, 1, 0, 1}) ==
        Error::duplicate_component);
  // Drawn of two bytes, and a name whose string runs past its component.
  CHECK(decode_bytes(Bytes{17, 0, 7, 0, 0, 9, 0, 2, 0, 1, 1}) == Error::bad_length);
  CHECK(decode_bytes(Bytes{17, 0, 8, 0, 0, 1, 0, 3, 0, 5, 0, 'a'}) == Error::bad_length);
  // A component longer than the body.
  CHECK(decode_bytes(Bytes{17, 0, 6, 0, 0, 1, 0, 9, 0, 0}) == Error::bad_length);
  // Player 5, a control character, a name over 64 characters.
  CHECK(decode_bytes(Bytes{17, 0, 1, 0, 5}) == Error::bad_value);
  CHECK(decode_bytes(Bytes{17, 0, 8, 0, 0, 1, 0, 3, 0, 1, 0, 0x01}) == Error::bad_value);
  Bytes out;
  PlayerEquip big;
  big.equipment.ocarina = std::string(kMaxEquipNameChars + 1, 'o');
  CHECK(encode(big, out) == Error::string_too_long);
  big.equipment.ocarina = std::string(kMaxEquipNameChars, 'o');
  CHECK(encode(big, out) == Error::none);
  CHECK(encode(PlayerEquip{5, {}}, out) == Error::bad_value);
  // A stage 0 reader skips it like the other new types.
  Message old;
  CHECK(stage0_decode(must_encode(test::equip_message()), old) == Error::unknown_type);
}

TEST_CASE("merge_equipment keeps what a message leaves out") {
  Equipment before;
  before.weapon = "Weapon_Sword_Kokiri";
  before.shield = "Weapon_Shield_Deku";
  before.drawn = 1;
  Equipment now;
  now.shield = "";  // took the shield off
  now.drawn = 0;
  const Equipment e = merge_equipment(before, now);
  CHECK(e.weapon == "Weapon_Sword_Kokiri");
  CHECK(e.shield == "");
  CHECK(e.drawn == 0);
  CHECK_FALSE(e.boots);
}

TEST_CASE("object messages have their numbers, channels and senders") {
  CHECK(static_cast<int>(MessageType::object_present) == 18);
  CHECK(static_cast<int>(MessageType::damage) == 25);
  CHECK(channel_of(MessageType::object_sample) == kChannelObjectSamples);
  CHECK(channel_of(MessageType::damage) == kChannelOwnership);
  CHECK(channel_of(MessageType::object_owner) == kChannelOwnership);
  CHECK_FALSE(allowed_on_channel(MessageType::object_sample, kChannelOwnership));
  CHECK_FALSE(allowed_on_channel(MessageType::object_spawn, kChannelSession));
  CHECK_FALSE(may_send(Sender::client, MessageType::object_owner));
  CHECK_FALSE(may_send(Sender::client, MessageType::handover_ask));
  CHECK(may_send(Sender::client, MessageType::damage));
  Message d = Damage{0, test::kPlacedId, 1, 2, 1, {}};
  CHECK(check_received(d, Sender::client, kChannelOwnership) == Error::none);
  CHECK(set_sender(d, 3));
  CHECK(sender_of(d) == 3);
  CHECK(check_received(d, Sender::server, kChannelOwnership) == Error::none);
}

TEST_CASE("object id layout on the wire") {
  const Bytes b = must_encode(ObjectOwner{test::kMadeId, 2, 0x0102, 0x0a0b0c0d});
  REQUIRE(b.size() == kHeaderSize + 10 + 1 + 2 + 4);
  CHECK(b[4] == 1);  // made
  CHECK(b[5] == 2);  // maker
  CHECK(b[6] == 7);  // counter, little-endian
  CHECK(b[14] == 2);  // owner
  CHECK(b[15] == 0x02);
  CHECK(b[16] == 0x01);
}

TEST_CASE("bad object messages are refused") {
  Bytes out;
  // A placed id names no maker; a made id names one.
  CHECK(encode(ObjectOwner{{ObjectKind::placed, 1, 5}, 1, 0, 0}, out) == Error::bad_value);
  CHECK(encode(ObjectOwner{{ObjectKind::made, 0, 5}, 1, 0, 0}, out) == Error::bad_value);
  CHECK(encode(ObjectOwner{{ObjectKind::made, 5, 5}, 1, 0, 0}, out) == Error::bad_value);
  CHECK(encode(ObjectOwner{test::kPlacedId, 5, 0, 0}, out) == Error::bad_value);
  CHECK(encode(HandoverAsk{test::kPlacedId, 0, 0}, out) == Error::bad_value);
  // A spawn names its actor; a placed id names no maker (version 2).
  Bytes placed_spawn;
  CHECK(encode(ObjectSpawn{0, test::kPlacedId, "Enemy_Skulltula_Gold", 0, {}}, placed_spawn) ==
        Error::none);
  CHECK(encode(ObjectSpawn{0, {ObjectKind::placed, 1, 5}, "Enemy_Skulltula_Gold", 0, {}}, out) ==
        Error::bad_value);
  CHECK(encode(ObjectSpawn{0, test::kMadeId, "", 0, {}}, out) == Error::bad_value);
  CHECK(encode(ObjectSpawn{0, test::kMadeId, std::string(65, 'a'), 0, {}}, out) ==
        Error::string_too_long);
  CHECK(encode(Damage{0, test::kPlacedId, 0, -1, 0, {}}, out) == Error::bad_value);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK(encode(ObjectRemove{0, test::kMadeId, 0, RemoveReason::hit, {nan, 0, 0}}, out) ==
        Error::bad_value);
  CHECK(encode(ObjectRemove{0, test::kMadeId, 0, static_cast<RemoveReason>(5), {}}, out) ==
        Error::bad_value);
  ObjectSample s{0, test::kMadeId, 0, {}};
  s.state.fuse = nan;
  CHECK(encode(s, out) == Error::bad_value);
  // The same id twice in one presence list.
  ObjectPresent twice{0, "Scene", {{test::kPlacedId, 0}, {test::kPlacedId, 1}}};
  CHECK(encode(twice, out) == Error::bad_value);
  ObjectPresent many{0, "Scene", {}};
  for (std::uint64_t i = 0; i <= kMaxPresentObjects; ++i) many.objects.push_back({{ObjectKind::placed, 0, i}, 0});
  CHECK(encode(many, out) == Error::bad_value);
  CHECK(out.empty());

  // On the wire: kind 2, and a present list with a duplicate.
  Bytes b = must_encode(ObjectOwner{test::kPlacedId, 1, 0, 0});
  b[4] = 2;
  CHECK(decode_bytes(b) == Error::bad_value);
  Bytes p = must_encode(ObjectPresent{0, "S", {{test::kPlacedId, 0}, {test::kMadeId, 0}}});
  // Copy the first entry (11 bytes after u8 player, str "S", u16 count) over the second.
  const std::size_t first = kHeaderSize + 1 + 2 + 1 + 2;
  std::copy(p.begin() + first, p.begin() + first + 11, p.begin() + first + 11);
  CHECK(decode_bytes(p) == Error::bad_value);
}

TEST_CASE("an object sample needs its time component, and skips unknown ones") {
  Bytes b = must_encode(ObjectSample{0, test::kMadeId, 1, {}});
  // Unknown component 30 with 2 bytes: skipped.
  put_u16(b, 30);
  put_u16(b, 2);
  b.push_back(0xaa);
  b.push_back(0xbb);
  fix_length(b);
  Message m;
  REQUIRE(decode(b, m) == Error::none);
  // Drop the time component (4 + 12 bytes after player, id and epoch).
  Bytes no_time = must_encode(ObjectSample{0, test::kMadeId, 1, {}});
  const std::size_t at = kHeaderSize + 1 + 10 + 2;
  no_time.erase(no_time.begin() + at, no_time.begin() + at + 16);
  fix_length(no_time);
  CHECK(decode_bytes(no_time) == Error::missing_component);
}

TEST_CASE("player pose has its number, channel and layout") {
  CHECK(static_cast<int>(MessageType::player_pose) == 26);
  CHECK(channel_of(MessageType::player_pose) == kChannelPlayerSamples);
  CHECK_FALSE(allowed_on_channel(MessageType::player_pose, kChannelSession));
  CHECK(may_send(Sender::client, MessageType::player_pose));
  Message p = PlayerPose{0, {5, 6}, 9, {}};
  CHECK(check_received(p, Sender::client, kChannelPlayerSamples) == Error::none);
  CHECK(set_sender(p, 2));
  CHECK(sender_of(p) == 2);
  PoseBone bone{4, 0x10, std::array<std::int16_t, 4>{1, 2, 3, -1}, std::nullopt,
                std::array<std::int16_t, 3>{5, 6, 7}};
  const Bytes b = must_encode(PlayerPose{0, {5, 6}, 9, {bone}});
  // header, player, time 12, skeleton 4, count 2; bone 2, parts 1, flags 1, rotation 8, scale 6
  REQUIRE(b.size() == kHeaderSize + 1 + 12 + 4 + 2 + 4 + 8 + 6);
  CHECK(b[kHeaderSize + 19] == 4);     // the bone, little-endian
  CHECK(b[kHeaderSize + 21] == 0x05);  // rotation and scale
  CHECK(b[kHeaderSize + 22] == 0x10);  // flags
  CHECK(b[kHeaderSize + 29] == 0xff);  // w = -1
}

TEST_CASE("bad player poses are refused") {
  const auto bone = [](std::size_t n) {
    PoseBone b;
    b.bone = static_cast<std::uint16_t>(n);
    return b;
  };
  Bytes out;
  PlayerPose p;
  p.bones.resize(kMaxPoseBones + 1);
  for (std::size_t i = 0; i < p.bones.size(); ++i) p.bones[i].bone = static_cast<std::uint16_t>(i);
  CHECK(encode(p, out) == Error::bad_value);
  p.bones = {bone(2), bone(2)};  // not increasing
  CHECK(encode(p, out) == Error::bad_value);
  p.bones = {bone(kMaxPoseBones)};
  CHECK(encode(p, out) == Error::bad_value);
  p.bones = {bone(1)};
  p.bones[0].rotation = std::array<std::int16_t, 4>{std::numeric_limits<std::int16_t>::min(), 0, 0, 0};
  CHECK(encode(p, out) == Error::bad_value);
  // An unknown part bit is refused on decode.
  p.bones = {bone(1)};
  Bytes b = must_encode(p);
  b[kHeaderSize + 21] = 0x08;
  Message m;
  CHECK(decode(b, m) == Error::bad_value);
}

TEST_CASE("time of day has its number, channel and layout") {
  CHECK(static_cast<int>(MessageType::time_of_day) == 27);
  CHECK(channel_of(MessageType::time_of_day) == kChannelSession);
  CHECK_FALSE(allowed_on_channel(MessageType::time_of_day, kChannelPlayerSamples));
  CHECK(may_send(Sender::client, MessageType::time_of_day));
  CHECK(may_send(Sender::server, MessageType::time_of_day));
  Message t = TimeOfDay{0, 6 * 3600, time_of_day::jump};
  CHECK(check_received(t, Sender::client, kChannelSession) == Error::none);
  CHECK(set_sender(t, 2));
  CHECK(sender_of(t) == 2);
  const Bytes b = must_encode(TimeOfDay{0, 0x01020304 % kSecondsPerDay, 1});
  REQUIRE(b.size() == kHeaderSize + 1 + 4 + 1);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<TimeOfDay>(back).seconds == 0x01020304 % kSecondsPerDay);
}

TEST_CASE("bad times of day are refused") {
  Bytes out;
  CHECK(encode(TimeOfDay{0, kSecondsPerDay, 0}, out) == Error::bad_value);
  CHECK(encode(TimeOfDay{0, 10, 2}, out) == Error::bad_value);
  CHECK(encode(TimeOfDay{5, 10, 0}, out) == Error::bad_value);
  Bytes b = must_encode(TimeOfDay{0, 10, 0});
  b[kHeaderSize + 5] = 0x80;  // an unknown flag
  Message m;
  CHECK(decode(b, m) == Error::bad_value);
  b = must_encode(TimeOfDay{0, 10, 0});
  b[kHeaderSize + 4] = 0xff;  // seconds past one day
  CHECK(decode(b, m) == Error::bad_value);
}

TEST_CASE("object group has its number, channel and layout; only a client sends it") {
  CHECK(static_cast<int>(MessageType::object_group) == 29);
  CHECK(channel_of(MessageType::object_group) == kChannelOwnership);
  CHECK_FALSE(allowed_on_channel(MessageType::object_group, kChannelSession));
  CHECK(may_send(Sender::client, MessageType::object_group));
  CHECK_FALSE(may_send(Sender::server, MessageType::object_group));
  const ObjectId part{ObjectKind::placed, 0, 0x0102030405060708ull};
  const ObjectId boss{ObjectKind::made, 2, 9};
  const Message g = ObjectGroup{0, {{part, boss, object_group::boss}}};
  CHECK(check_received(g, Sender::client, kChannelOwnership) == Error::none);
  CHECK(check_received(g, Sender::server, kChannelOwnership) == Error::wrong_sender);
  const Bytes b = must_encode(g);
  // u8 player, u16 count, then member id, root id (10 bytes each), u8 flags.
  REQUIRE(b.size() == kHeaderSize + 3 + 21);
  CHECK(b[kHeaderSize + 1] == 1);
  CHECK(b[kHeaderSize + 3] == 0);      // member: placed
  CHECK(b[kHeaderSize + 5] == 0x08);   // its hash, little-endian
  CHECK(b[kHeaderSize + 13] == 1);     // root: made
  CHECK(b[kHeaderSize + 14] == 2);     // by player 2
  CHECK(b[kHeaderSize + 23] == object_group::boss);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(back == g);
}

TEST_CASE("bad object groups are refused") {
  const ObjectId a{ObjectKind::placed, 0, 1};
  const ObjectId c{ObjectKind::placed, 0, 2};
  Bytes out;
  CHECK(encode(ObjectGroup{0, {}}, out) == Error::bad_value);                    // empty
  CHECK(encode(ObjectGroup{0, {{a, c, 0}, {a, a, 0}}}, out) == Error::bad_value);  // a member twice
  CHECK(encode(ObjectGroup{0, {{a, c, 2}}}, out) == Error::bad_value);           // an unknown flag
  CHECK(encode(ObjectGroup{5, {{a, c, 0}}}, out) == Error::bad_value);
  CHECK(encode(ObjectGroup{0, {{a, ObjectId{ObjectKind::placed, 1, 2}, 0}}}, out) ==
        Error::bad_value);  // a placed id with a maker
  std::vector<ObjectGroupEntry> many;
  for (std::uint64_t i = 0; i <= kMaxGroupEntries; ++i) {
    many.push_back({ObjectId{ObjectKind::placed, 0, i}, c, 0});
  }
  CHECK(encode(ObjectGroup{0, many}, out) == Error::bad_value);
  many.pop_back();
  CHECK(encode(ObjectGroup{0, many}, out) == Error::none);
  CHECK(out.size() == kHeaderSize + 3 + 21 * kMaxGroupEntries);

  Message m;
  Bytes b = must_encode(ObjectGroup{0, {{a, c, 0}}});
  b[kHeaderSize + 23] = 0x80;  // an unknown flag
  CHECK(decode(b, m) == Error::bad_value);
  b = must_encode(ObjectGroup{0, {{a, c, 0}}});
  b[kHeaderSize + 1] = 2;  // two entries said, one sent
  CHECK(decode(b, m) == Error::truncated);
  b = must_encode(ObjectGroup{0, {{a, c, 0}}});
  b[kHeaderSize + 1] = 0;  // none said
  CHECK(decode(b, m) == Error::bad_value);
}

namespace {
GrassCut Cut() {
  GrassCut c;
  c.scene = "HyruleField_Child";
  c.position = {189.57f, 157.52f, 24.04f};
  c.radius = 0.525f;
  c.flags = 0x0c;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  c.sweep = {Vec3{189.0f, 157.9f, 24.5f}, Vec3{190.1f, 157.6f, 23.8f}, Vec3{nan, nan, nan}};
  return c;
}
}  // namespace

TEST_CASE("grass cuts have their number, channel and layout") {
  CHECK(static_cast<int>(MessageType::grass_cut) == 28);
  CHECK(kProtocolVersion == 6);
  CHECK(channel_of(MessageType::grass_cut) == kChannelSession);
  CHECK_FALSE(allowed_on_channel(MessageType::grass_cut, kChannelPlayerSamples));
  CHECK(may_send(Sender::client, MessageType::grass_cut));
  CHECK(may_send(Sender::server, MessageType::grass_cut));
  Message g = Cut();
  CHECK(check_received(g, Sender::client, kChannelSession) == Error::none);
  CHECK(set_sender(g, 2));
  CHECK(sender_of(g) == 2);
  const Bytes b = must_encode(Cut());
  REQUIRE(b.size() == kHeaderSize + 1 + 2 + 17 + 12 + 4 + 1 + 36);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<GrassCut>(back) == Cut());  // NaN comes back as NaN
  CHECK(std::isnan(std::get<GrassCut>(back).sweep[2][1]));
}

TEST_CASE("bad grass cuts are refused") {
  Bytes out;
  GrassCut c = Cut();
  c.scene.clear();
  CHECK(encode(c, out) == Error::bad_value);
  c = Cut();
  c.radius = 0.0f;
  CHECK(encode(c, out) == Error::bad_value);
  c.radius = kMaxGrassCutRadius * 2;
  CHECK(encode(c, out) == Error::bad_value);
  c = Cut();
  c.position[1] = std::numeric_limits<float>::quiet_NaN();
  CHECK(encode(c, out) == Error::bad_value);
  c = Cut();
  c.sweep[0][0] = std::numeric_limits<float>::infinity();
  CHECK(encode(c, out) == Error::bad_value);
  c = Cut();
  c.player_number = 5;
  CHECK(encode(c, out) == Error::bad_value);
}

namespace {
WorldBreak Broke() {
  WorldBreak b;
  b.scene = "KakarikoVillage_Child";
  b.hash = 0x8796851d6bbe37cull;
  b.position = {-16.6f, 30.7f, 4.0f};
  return b;
}
WorldMove Moved() {
  WorldMove v;
  v.scene = "ForestTemple";
  v.hash = 0x1122334455667788ull;
  v.time_us = 123'456'789;
  v.position = {1.0f, 2.0f, 3.0f};
  v.rotation = {0.0f, 0.70710678f, 0.0f, 0.70710678f};
  v.flags = world_move::carried;
  return v;
}
WorldPlace Rested() {
  WorldPlace p;
  p.scene = "ForestTemple";
  p.hash = 0x1122334455667788ull;
  p.position = {4.0f, 2.0f, 3.0f};
  return p;
}
}  // namespace

TEST_CASE("world messages have their numbers, channels and layouts") {
  CHECK(static_cast<int>(MessageType::world_break) == 32);
  CHECK(static_cast<int>(MessageType::world_move) == 33);
  CHECK(static_cast<int>(MessageType::world_place) == 34);
  CHECK(channel_of(MessageType::world_break) == kChannelSession);
  CHECK(channel_of(MessageType::world_place) == kChannelSession);
  CHECK(channel_of(MessageType::world_move) == kChannelObjectSamples);
  CHECK_FALSE(allowed_on_channel(MessageType::world_move, kChannelSession));
  CHECK_FALSE(allowed_on_channel(MessageType::world_break, kChannelObjectSamples));
  for (const Message& m : {Message{Broke()}, Message{Moved()}, Message{Rested()}}) {
    CHECK(may_send(Sender::client, type_of(m)));
    CHECK(may_send(Sender::server, type_of(m)));
    Message stamped = m;
    CHECK(check_received(m, Sender::client, channel_of(m)) == Error::none);
    CHECK(check_received(m, Sender::server, channel_of(m)) == Error::wrong_sender);
    CHECK(set_sender(stamped, 3));
    CHECK(sender_of(stamped) == 3);
    Message back;
    REQUIRE(decode(must_encode(stamped), back) == Error::none);
    CHECK(back == stamped);
  }
  CHECK(must_encode(Broke()).size() == kHeaderSize + 1 + 2 + 21 + 8 + 12 + 1);
  CHECK(must_encode(Moved()).size() == kHeaderSize + 1 + 2 + 12 + 8 + 8 + 12 + 16 + 1);
  CHECK(must_encode(Rested()).size() == kHeaderSize + 1 + 2 + 12 + 8 + 12 + 16);
}

TEST_CASE("bad world messages are refused") {
  Bytes out;
  WorldBreak b = Broke();
  b.hash = 0;
  CHECK(encode(b, out) == Error::bad_value);
  b.hash = ~std::uint64_t{0};
  CHECK(encode(b, out) == Error::bad_value);
  b = Broke();
  b.scene.clear();
  CHECK(encode(b, out) == Error::bad_value);
  b = Broke();
  b.flags = 2;
  CHECK(encode(b, out) == Error::bad_value);
  b = Broke();
  b.position[2] = std::numeric_limits<float>::infinity();
  CHECK(encode(b, out) == Error::bad_value);
  b = Broke();
  b.player_number = 5;
  CHECK(encode(b, out) == Error::bad_value);
  WorldMove v = Moved();
  v.rotation = {0.0f, 0.0f, 0.0f, 0.0f};
  CHECK(encode(v, out) == Error::bad_value);
  v = Moved();
  v.flags = 0x80;
  CHECK(encode(v, out) == Error::bad_value);
  WorldPlace p = Rested();
  p.rotation[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(encode(p, out) == Error::bad_value);
  p = Rested();
  p.hash = 0;
  CHECK(encode(p, out) == Error::bad_value);
  CHECK(out.empty());
}

TEST_CASE("a world hit has its number, channel and layout") {
  CHECK(static_cast<int>(MessageType::world_hit) == 35);
  WorldHit h;
  h.scene = "Kakariko";
  h.hash = 0x70;
  h.at = {1.0f, 2.0f, 3.0f};
  h.from = {1.5f, 2.0f, 4.0f};
  CHECK(channel_of(Message{h}) == kChannelSession);
  CHECK(check_received(Message{h}, Sender::client, kChannelSession) == Error::none);
  const Bytes b = must_encode(h);
  CHECK(b.size() == kHeaderSize + 1 + 2 + 8 + 8 + 12 + 12);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(std::get<WorldHit>(back) == h);
  Bytes out;
  h.from[1] = std::numeric_limits<float>::infinity();
  CHECK(encode(h, out) == Error::bad_value);
  h.from[1] = 0.0f;
  h.hash = 0;
  CHECK(encode(h, out) == Error::bad_value);
}

TEST_CASE("the drop messages have their numbers, channel and layout") {
  CHECK(static_cast<int>(MessageType::drop_spawn) == 36);
  CHECK(static_cast<int>(MessageType::drop_take) == 37);
  CHECK(static_cast<int>(MessageType::drop_ask) == 38);
  for (const MessageType type :
       {MessageType::drop_spawn, MessageType::drop_take, MessageType::drop_ask}) {
    CHECK(channel_of(type) == kChannelOwnership);
    CHECK_FALSE(allowed_on_channel(type, kChannelSession));
    CHECK(may_send(Sender::client, type));
    CHECK(may_send(Sender::server, type));
  }
  const ObjectId id{ObjectKind::made, 2, 7};
  Message spawn = DropSpawn{0, id, "Graveyard", "Obj_Heart", {1.0f, 2.0f, 3.0f}, 99, drop_spawn::owned};
  CHECK(check_received(spawn, Sender::client, kChannelOwnership) == Error::none);
  CHECK(set_sender(spawn, 2));
  CHECK(sender_of(spawn) == 2);
  const Bytes b = must_encode(spawn);
  // player, id, two strings, position, time, flags
  REQUIRE(b.size() == kHeaderSize + 1 + 10 + (2 + 9) + (2 + 9) + 12 + 8 + 1);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(back == spawn);
  CHECK(must_encode(DropTake{0, id, DropTakeReason::gone}).size() == kHeaderSize + 1 + 10 + 1);
  CHECK(must_encode(DropAsk{0, "Graveyard"}).size() == kHeaderSize + 1 + 2 + 9);
}

TEST_CASE("bad drop messages are refused") {
  const ObjectId made{ObjectKind::made, 2, 7};
  const ObjectId placed{ObjectKind::placed, 0, 7};
  Bytes out;
  CHECK(encode(DropSpawn{0, placed, "s", "Obj_Heart", {}, 0, 0}, out) == Error::bad_value);
  CHECK(encode(DropSpawn{0, made, "", "Obj_Heart", {}, 0, 0}, out) == Error::bad_value);
  CHECK(encode(DropSpawn{0, made, "s", "", {}, 0, 0}, out) == Error::bad_value);
  CHECK(encode(DropSpawn{0, made, "s", "Obj_Heart", {}, 0, 4}, out) == Error::bad_value);
  CHECK(encode(DropSpawn{0, made, "s", "Obj_Heart", {std::numeric_limits<float>::infinity(), 0, 0}, 0, 0},
               out) == Error::bad_value);
  CHECK(encode(DropSpawn{5, made, "s", "Obj_Heart", {}, 0, 0}, out) == Error::bad_value);
  CHECK(encode(DropTake{0, placed, DropTakeReason::gone}, out) == Error::bad_value);
  CHECK(encode(DropTake{0, made, static_cast<DropTakeReason>(2)}, out) == Error::bad_value);
  CHECK(encode(DropAsk{0, ""}, out) == Error::bad_value);
  Message m;
  Bytes b = must_encode(DropTake{0, made, DropTakeReason::gone});
  b.back() = 2;  // an unknown reason
  CHECK(decode(b, m) == Error::bad_value);
  b = must_encode(DropSpawn{0, made, "s", "Obj_Heart", {}, 0, 0});
  b.back() = 0x80;  // an unknown flag
  CHECK(decode(b, m) == Error::bad_value);
  b = must_encode(DropAsk{0, "s"});
  b.push_back(0);
  CHECK(decode(b, m) != Error::none);
}

TEST_CASE("boss room messages have their number, channel and layout") {
  CHECK(static_cast<int>(MessageType::boss_room) == 50);
  CHECK(channel_of(MessageType::boss_room) == kChannelSession);
  CHECK_FALSE(allowed_on_channel(MessageType::boss_room, kChannelOwnership));
  CHECK(may_send(Sender::client, MessageType::boss_room));
  CHECK(may_send(Sender::server, MessageType::boss_room));
  Message m = BossRoom{0, "ForestTemple", 3, BossState::intro_end};
  CHECK(check_received(m, Sender::client, kChannelSession) == Error::none);
  CHECK(set_sender(m, 2));
  CHECK(sender_of(m) == 2);
  const Bytes b = must_encode(m);
  REQUIRE(b.size() == kHeaderSize + 1 + (2 + 12) + 1 + 1);
  Message back;
  REQUIRE(decode(b, back) == Error::none);
  CHECK(back == m);
}

TEST_CASE("bad boss room messages are refused") {
  Bytes out;
  CHECK(encode(BossRoom{0, "", 0, BossState::in_room}, out) == Error::bad_value);
  CHECK(encode(BossRoom{0, "s", kMaxBossRoom + 1, BossState::in_room}, out) == Error::bad_value);
  CHECK(encode(BossRoom{0, "s", 0, static_cast<BossState>(0)}, out) == Error::bad_value);
  CHECK(encode(BossRoom{0, "s", 0, static_cast<BossState>(6)}, out) == Error::bad_value);
  CHECK(encode(BossRoom{5, "s", 0, BossState::left}, out) == Error::bad_value);
  Message m;
  Bytes b = must_encode(BossRoom{0, "s", 0, BossState::defeated});
  b.back() = 6;  // an unknown state
  CHECK(decode(b, m) == Error::bad_value);
  b = must_encode(BossRoom{0, "s", 0, BossState::defeated});
  b[b.size() - 2] = kMaxBossRoom + 1;
  CHECK(decode(b, m) == Error::bad_value);
}
