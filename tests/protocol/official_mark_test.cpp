// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include "coop/protocol/messages.hpp"
#include "coop/protocol/official_mark.hpp"

using namespace coop::protocol;

namespace {

// RFC 8032 7.1 TEST 1 (a published test key, never the official one).
OfficialSeed test_seed() {
  return *official_seed_from_text("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
}

}  // namespace

TEST_CASE("official mark: Ed25519 as RFC 8032 has it (the test key's public key)") {
  const OfficialPublicKey pub = official_public_key(test_seed());
  CHECK(official_seed_text(OfficialSeed(pub)) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  CHECK(pub != kOfficialPublicKey);
}

TEST_CASE("official mark: a proof checks for its room and version only, with the key it was made with, while fresh") {
  const OfficialSeed seed = test_seed();
  const OfficialPublicKey pub = official_public_key(seed);
  const std::uint16_t version = public_listing_version();
  const std::uint64_t t = 1'795'000'000;
  const std::string relay = official_relay_name("159.54.182.85", 27530);
  const OfficialProof proof = official_sign(seed, relay, "squadala", version, t);
  // Valid (the room's case does not matter: the relay compares names in lower case).
  CHECK(official_check(pub, relay, "squadala", version, proof, t));
  CHECK(official_check(pub, relay, "Squadala", version, proof, t + 10));
  // Wrong key: the kits' built-in one, or any other.
  CHECK_FALSE(official_check(kOfficialPublicKey, relay, "squadala", version, proof, t));
  OfficialSeed other = seed;
  other[0] ^= 1;
  CHECK_FALSE(official_check(official_public_key(other), relay, "squadala", version, proof, t));
  // Tampered: another room name, another version, another time, a changed signature byte.
  CHECK_FALSE(official_check(pub, relay, "squadala2", version, proof, t));
  CHECK_FALSE(official_check(pub, relay, "forest-4821", version, proof, t));
  CHECK_FALSE(official_check(pub, relay, "squadala", static_cast<std::uint16_t>(version + 1), proof, t));
  CHECK_FALSE(official_check(pub, relay, "squadala", kProtocolVersion, proof, t));   // (the private listing's version)
  OfficialProof moved = proof;
  moved.time_s += 1;
  CHECK_FALSE(official_check(pub, relay, "squadala", version, moved, t));
  OfficialProof flipped = proof;
  flipped.signature[17] ^= 0x40;
  CHECK_FALSE(official_check(pub, relay, "squadala", version, flipped, t));
  // Passed on through another relay (an attacker's relay lists 'squadala' with fresh proofs fetched from ours): the
  // relay is in the signed bytes, so the proof does not check there; the relay's name is compared in lower case.
  CHECK_FALSE(official_check(pub, official_relay_name("203.0.113.9", 27530), "squadala", version, proof, t));
  CHECK_FALSE(official_check(pub, official_relay_name("159.54.182.85", 27531), "squadala", version, proof, t));
  CHECK_FALSE(official_check(pub, "", "squadala", version, proof, t));
  CHECK(official_relay_name("Relay.Example.NET", 27530) == "relay.example.net:27530");
  // Replayed: an old proof stops checking after kOfficialProofMaxAgeS; one from the future too.
  CHECK(official_check(pub, relay, "squadala", version, proof, t + kOfficialProofMaxAgeS));
  CHECK_FALSE(official_check(pub, relay, "squadala", version, proof, t + kOfficialProofMaxAgeS + 1));
  CHECK_FALSE(official_check(pub, relay, "squadala", version, proof, t + 86'400));
  CHECK_FALSE(official_check(pub, relay, "squadala", version, proof, t - kOfficialProofMaxAgeS - 1));
}

TEST_CASE("official mark: the key file is 64 hex digits and nothing else") {
  const OfficialSeed seed = test_seed();
  CHECK(official_seed_from_text(official_seed_text(seed) + "\n") == seed);
  CHECK(official_seed_from_text("  9D61B19DEFFD5A60BA844AF492EC2CC44449C5697B326919703BAC031CAE7F60\r\n") == seed);
  CHECK_FALSE(official_seed_from_text("").has_value());
  CHECK_FALSE(official_seed_from_text(official_seed_text(seed).substr(1)).has_value());
  CHECK_FALSE(official_seed_from_text(official_seed_text(seed) + "0").has_value());
  CHECK_FALSE(official_seed_from_text("zz" + official_seed_text(seed).substr(2)).has_value());
  CHECK_FALSE(official_seed_from_text("9d61 b19d").has_value());
}
