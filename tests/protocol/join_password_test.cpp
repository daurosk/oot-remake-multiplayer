// The join password's hash, proof and messages (protocol 9).
#include <doctest/doctest.h>

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/join_password.hpp"

using namespace coop::protocol;

namespace {

std::string hex(const Sha256Digest& d) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (std::uint8_t b : d) {
    out += kDigits[b >> 4];
    out += kDigits[b & 15];
  }
  return out;
}

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("join password: SHA-256 matches the FIPS 180-4 examples") {
  CHECK(hex(sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(sha256(bytes("abc"))) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the padding needs a second block.
  CHECK(hex(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::vector<std::uint8_t> million(1'000'000, 'a');
  CHECK(hex(sha256(million)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("join password: HMAC-SHA256 matches RFC 4231") {
  // Test case 1.
  const std::vector<std::uint8_t> key1(20, 0x0b);
  CHECK(hex(hmac_sha256(key1, bytes("Hi There"))) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  // Test case 2: a short key.
  CHECK(hex(hmac_sha256(bytes("Jefe"), bytes("what do ya want for nothing?"))) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Test case 6: a key longer than a block is hashed first.
  const std::vector<std::uint8_t> key6(131, 0xaa);
  CHECK(hex(hmac_sha256(key6, bytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("join password: the proof depends on both the password and the nonce") {
  JoinNonce a{};
  JoinNonce b{};
  b[31] = 1;
  const JoinProofBytes p = join_proof("hunter2", a);
  CHECK(same_proof(p, join_proof("hunter2", a)));
  CHECK_FALSE(same_proof(p, join_proof("hunter3", a)));
  CHECK_FALSE(same_proof(p, join_proof("hunter2", b)));
  CHECK_FALSE(same_proof(p, join_proof("", a)));
  // It is HMAC keyed with the password over the nonce, nothing else.
  CHECK(p == hmac_sha256(bytes("hunter2"), a));
}

TEST_CASE("join password: nonces are fresh") {
  const JoinNonce a = random_nonce();
  const JoinNonce b = random_nonce();
  CHECK(a != b);
  CHECK(a != JoinNonce{});
}

TEST_CASE("join password: JOIN_CHALLENGE and JOIN_PROOF on the wire") {
  JoinNonce nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<std::uint8_t>(i);
  const auto wire = encode(JoinChallenge{nonce});
  REQUIRE(wire);
  REQUIRE(wire->size() == 4 + 32);
  CHECK((*wire)[0] == 191);
  CHECK((*wire)[1] == 0);
  CHECK((*wire)[2] == 32);
  CHECK((*wire)[3] == 0);
  CHECK((*wire)[4] == 0);
  CHECK((*wire)[35] == 31);

  Message back;
  REQUIRE(decode(*wire, back) == Error::none);
  CHECK(std::get<JoinChallenge>(back).nonce == nonce);

  // One byte short, or one too many: refused.
  std::vector<std::uint8_t> shorter(wire->begin(), wire->end() - 1);
  shorter[2] = 31;
  CHECK(decode(shorter, back) == Error::truncated);
  std::vector<std::uint8_t> longer = *wire;
  longer.push_back(0);
  longer[2] = 33;
  CHECK(decode(longer, back) == Error::trailing_bytes);

  const auto proof = encode(JoinProof{join_proof("x", nonce)});
  REQUIRE(proof);
  CHECK((*proof)[0] == 192);
  CHECK(proof->size() == 4 + 32);

  CHECK(may_send(Sender::server, MessageType::join_challenge));
  CHECK_FALSE(may_send(Sender::client, MessageType::join_challenge));
  CHECK(may_send(Sender::client, MessageType::join_proof));
  CHECK_FALSE(may_send(Sender::server, MessageType::join_proof));
  CHECK(allowed_on_channel(MessageType::join_challenge, kChannelSession));
  CHECK_FALSE(allowed_on_channel(MessageType::join_proof, kChannelProgress));
  CHECK(kProtocolVersion >= 9);
}
