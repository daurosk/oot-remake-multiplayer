#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <ostream>  // doctest prints string_view through it
#include <string>
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
        // off cleanly; anything else must be refused.
        if (e == Error::none) {
          const bool may_shrink = std::holds_alternative<TestPayload>(m) ||
                                  std::holds_alternative<PlayerSample>(m);
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
                        MessageType::player_joined, MessageType::player_left}) {
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
