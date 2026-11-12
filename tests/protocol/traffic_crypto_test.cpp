// Traffic encryption (traffic_crypto.hpp, docs/protocol.md "Encrypted
// traffic"): key derivation, sealing, and what must not open.
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/traffic_crypto.hpp"

using namespace coop::protocol;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::array<std::uint8_t, 32> from_hex(const char* text) {
  std::array<std::uint8_t, 32> out{};
  auto digit = [](char c) {
    return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint8_t>((digit(text[2 * i]) << 4) | digit(text[2 * i + 1]));
  }
  return out;
}

const Bytes kHello = {1, 0, 9, 0, 'h', 'e', 'l', 'l', 'o'};
const Bytes kChallenge = {191, 0, 4, 0, 'n', 'o', 'n', 'c'};

struct Pair {
  TrafficKeys client;
  TrafficKeys server;
};

Pair keys_for(const std::string& client_password, const std::string& server_password,
              const Bytes& client_hello = kHello, const Bytes& server_hello = kHello) {
  const KeyPair game = KeyPair::generate();
  const KeyPair host = KeyPair::generate();
  auto c = derive_traffic_keys(game, host.share(), client_password, client_hello, kChallenge,
                               false);
  auto s = derive_traffic_keys(host, game.share(), server_password, server_hello, kChallenge,
                               true);
  REQUIRE(c.has_value());
  REQUIRE(s.has_value());
  return {*c, *s};
}

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

}  // namespace

TEST_CASE("traffic crypto: X25519 is Monocypher's, as in RFC 7748 section 6.1") {
  const auto alice = KeyPair::from_secret(
      from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"));
  CHECK(alice.share() ==
        from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
  const auto bob = KeyPair::from_secret(
      from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"));
  CHECK(bob.share() ==
        from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
  const auto ab = alice.shared_with(bob.share());
  REQUIRE(ab.has_value());
  CHECK(*ab == from_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
  CHECK(bob.shared_with(alice.share()) == ab);
}

TEST_CASE("traffic crypto: both ends make the same keys, one per direction") {
  const Pair p = keys_for("hunter2", "hunter2");
  CHECK(p.client.send == p.server.receive);
  CHECK(p.client.receive == p.server.send);
  CHECK(p.client.send != p.client.receive);
}

TEST_CASE("traffic crypto: another password or a changed HELLO gives other keys") {
  const Pair wrong = keys_for("hunter2", "hunter3");
  CHECK(wrong.client.send != wrong.server.receive);
  Bytes changed = kHello;
  changed.back() ^= 1;
  const Pair tampered = keys_for("hunter2", "hunter2", kHello, changed);
  CHECK(tampered.client.send != tampered.server.receive);
}

TEST_CASE("traffic crypto: a low-order key share is refused") {
  const KeyPair own = KeyPair::generate();
  const KeyShare zero{};  // every secret gives the all-zero result
  CHECK_FALSE(own.shared_with(zero).has_value());
  CHECK_FALSE(derive_traffic_keys(own, zero, "pw", kHello, kChallenge, true).has_value());
  KeyShare one{};
  one[0] = 1;  // also a low-order point
  CHECK_FALSE(own.shared_with(one).has_value());
}

TEST_CASE("traffic crypto: seal and open, on every channel, any size") {
  const Pair p = keys_for("pw", "pw");
  TrafficCipher game(p.client);
  TrafficCipher host(p.server);
  for (std::uint8_t channel = 0; channel < 5; ++channel) {
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{100},
                             std::size_t{1100}, std::size_t{4 + 0xFFFF}}) {
      Bytes plain(size);
      for (std::size_t i = 0; i < size; ++i) {
        plain[i] = static_cast<std::uint8_t>(i * 31 + channel);
      }
      auto sealed = game.seal(channel, plain);
      REQUIRE(sealed.has_value());
      CHECK(sealed->size() == size + kSealOverheadBytes);
      if (size >= 16) {
        // The message does not show through.
        CHECK(Bytes(sealed->begin() + 8, sealed->begin() + 24) !=
              Bytes(plain.begin(), plain.begin() + 16));
      }
      Bytes opened;
      REQUIRE(host.open(channel, *sealed, opened) == OpenError::none);
      CHECK(opened == plain);
    }
  }
  // And the other way.
  auto back = host.seal(0, text("WELCOME"));
  REQUIRE(back.has_value());
  Bytes opened;
  REQUIRE(game.open(0, *back, opened) == OpenError::none);
  CHECK(opened == text("WELCOME"));
  // A packet does not open with its own sender's receive key (reflection).
  auto own = game.seal(1, text("x"));
  CHECK(game.open(1, *own, opened) == OpenError::bad_tag);
  CHECK_FALSE(game.seal(kSealChannels, text("x")).has_value());
}

TEST_CASE("traffic crypto: any changed byte, another channel or another key does not open") {
  const Pair p = keys_for("pw", "pw");
  TrafficCipher game(p.client);
  const Bytes plain = text("CHAT: meet me at the Deku Tree");
  const Bytes sealed = *game.seal(0, plain);
  for (std::size_t i = 0; i < sealed.size(); ++i) {
    for (std::uint8_t bit : {std::uint8_t{1}, std::uint8_t{0x80}}) {
      TrafficCipher host(p.server);  // fresh, so no counter is taken yet
      Bytes bad = sealed;
      bad[i] ^= bit;
      Bytes opened;
      CHECK(host.open(0, bad, opened) != OpenError::none);
      CHECK(opened.empty());
    }
  }
  {
    TrafficCipher host(p.server);
    Bytes opened;
    CHECK(host.open(1, sealed, opened) == OpenError::bad_tag);  // moved to channel 1
    CHECK(host.open(0, Bytes(sealed.begin(), sealed.end() - 1), opened) == OpenError::bad_tag);
    CHECK(host.open(0, Bytes(23), opened) == OpenError::too_short);
    CHECK(host.open(kSealChannels, sealed, opened) == OpenError::bad_channel);
    // The forged ones did not use up its counter: the real packet still opens.
    CHECK(host.open(0, sealed, opened) == OpenError::none);
    CHECK(opened == plain);
  }
  {
    const Pair other = keys_for("pw", "pw");  // another connection's keys
    TrafficCipher host(other.server);
    Bytes opened;
    CHECK(host.open(0, sealed, opened) == OpenError::bad_tag);
  }
  {
    const Pair wrong = keys_for("pw", "not pw");
    TrafficCipher a(wrong.client);
    TrafficCipher b(wrong.server);
    Bytes opened;
    CHECK(b.open(0, *a.seal(0, plain), opened) == OpenError::bad_tag);
  }
}

TEST_CASE("traffic crypto: a packet sent again is dropped, late ones within the window open") {
  const Pair p = keys_for("pw", "pw");
  TrafficCipher game(p.client);
  TrafficCipher host(p.server);
  std::vector<Bytes> sent;
  for (int i = 0; i < 200; ++i) sent.push_back(*game.seal(3, text("sample " + std::to_string(i))));
  Bytes opened;
  REQUIRE(host.open(3, sent[0], opened) == OpenError::none);
  CHECK(host.open(3, sent[0], opened) == OpenError::replayed);
  // Unreliable: 100 first, then some older ones that were late.
  REQUIRE(host.open(3, sent[100], opened) == OpenError::none);
  CHECK(host.open(3, sent[99], opened) == OpenError::none);
  CHECK(host.open(3, sent[40], opened) == OpenError::none);  // 60 back: in the window
  CHECK(host.open(3, sent[40], opened) == OpenError::replayed);
  CHECK(host.open(3, sent[30], opened) == OpenError::replayed);  // 70 back: too old
  CHECK(host.open(3, sent[100], opened) == OpenError::replayed);
  // A jump far ahead: the old ones are all too old now.
  REQUIRE(host.open(3, sent[199], opened) == OpenError::none);
  CHECK(host.open(3, sent[101], opened) == OpenError::replayed);
  CHECK(host.open(3, sent[150], opened) == OpenError::none);
  // Counters are per channel: channel 0 has its own.
  const Bytes first = *game.seal(0, text("chat"));
  CHECK(host.open(0, first, opened) == OpenError::none);
  // A counter of 0 is never sent.
  Bytes zero = *game.seal(0, text("chat"));
  for (int i = 0; i < 8; ++i) zero[i] = 0;
  CHECK(host.open(0, zero, opened) == OpenError::replayed);
}

TEST_CASE("traffic crypto: the key share rides at the end of HELLO and JOIN_CHALLENGE") {
  Hello hello;
  hello.build_id = "lichii";
  hello.game_build_id = "e0f3ccfaf50065ae";
  hello.game_version = "1.1.1";
  hello.nickname = "Link";
  const auto plain = encode(hello);
  REQUIRE(plain.has_value());
  hello.key_share = KeyPair::generate().share();
  const auto with_key = encode(hello);
  REQUIRE(with_key.has_value());
  CHECK(with_key->size() == plain->size() + kKeyShareBytes);
  Message back;
  REQUIRE(decode(*with_key, back) == Error::none);
  CHECK(std::get<Hello>(back) == hello);
  REQUIRE(decode(*plain, back) == Error::none);
  CHECK_FALSE(std::get<Hello>(back).key_share.has_value());
  // Protocol 11: the previous number after the key share, or alone.
  hello.previous_number = 2;
  const auto with_number = encode(hello);
  REQUIRE(with_number.has_value());
  CHECK(with_number->size() == with_key->size() + 1);
  REQUIRE(decode(*with_number, back) == Error::none);
  CHECK(std::get<Hello>(back) == hello);
  Hello no_key = hello;
  no_key.key_share.reset();
  const auto number_only = encode(no_key);
  REQUIRE(number_only.has_value());
  REQUIRE(decode(*number_only, back) == Error::none);
  CHECK(std::get<Hello>(back) == no_key);
  hello.previous_number = 5;  // not a player number
  CHECK_FALSE(encode(hello).has_value());
  hello.previous_number.reset();
  // Anything else after the nickname is still refused (a lone byte 0 is no player number).
  Bytes odd = *plain;
  odd.push_back(0);
  odd[2] = static_cast<std::uint8_t>(odd.size() - 4);
  odd[3] = 0;
  CHECK(decode(odd, back) != Error::none);

  JoinChallenge challenge;
  challenge.nonce[3] = 7;
  challenge.key_share = KeyPair::generate().share();
  const auto c = encode(challenge);
  REQUIRE(c.has_value());
  CHECK(c->size() == 4 + kJoinNonceBytes + kKeyShareBytes);
  REQUIRE(decode(*c, back) == Error::none);
  CHECK(std::get<JoinChallenge>(back) == challenge);
}

TEST_CASE("traffic crypto: cost per packet") {
  const Pair p = keys_for("pw", "pw");
  TrafficCipher game(p.client);
  TrafficCipher host(p.server);
  const Bytes sample(120, 0x5A);  // about a PLAYER_SAMPLE
  constexpr int kPackets = 20000;
  Bytes opened;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kPackets; ++i) {
    auto sealed = game.seal(3, sample);
    REQUIRE(host.open(3, *sealed, opened) == OpenError::none);
  }
  const double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - start).count() / kPackets;
  MESSAGE("seal + open of a 120-byte packet: " << us << " us");
  // A game sends and gets a few hundred packets a second; even a debug or
  // sanitizer build must stay far under a millisecond each.
  CHECK(us < 500.0);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 50; ++i) (void)keys_for("pw", "pw");
  const double handshake_us = std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - t0).count() / 50;
  MESSAGE("key exchange, both ends (once per join): " << handshake_us << " us");
  CHECK(handshake_us < 200000.0);
}
