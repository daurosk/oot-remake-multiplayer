// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/protocol/official_mark.hpp"

#include <cctype>
#include <chrono>

#include <monocypher-ed25519.h>
#include <monocypher.h>

namespace coop::protocol {

namespace {

constexpr std::string_view kTag = "squadala-official-listing-v2";

void put_lower(std::vector<std::uint8_t>& out, std::string_view text) {
  for (char c : text) out.push_back(static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c))));
}

std::array<std::uint8_t, 64> secret_key(const OfficialSeed& seed, OfficialPublicKey& pub) {
  std::array<std::uint8_t, 64> secret{};
  OfficialSeed copy = seed;  // (crypto_ed25519_key_pair wipes the seed it is given)
  crypto_ed25519_key_pair(secret.data(), pub.data(), copy.data());
  return secret;
}

}  // namespace

std::string official_relay_name(std::string_view host, std::uint16_t port) {
  std::string out;
  for (char c : host) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out + ":" + std::to_string(port);
}

std::vector<std::uint8_t> official_message(std::string_view relay, std::string_view room, std::uint16_t version,
                                           std::uint64_t time_s) {
  std::vector<std::uint8_t> out(kTag.begin(), kTag.end());
  out.push_back(0);
  put_lower(out, relay);
  out.push_back(0);
  put_lower(out, room);
  out.push_back(0);
  out.push_back(static_cast<std::uint8_t>(version & 0xffu));
  out.push_back(static_cast<std::uint8_t>(version >> 8));
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(time_s >> (8 * i)));
  return out;
}

OfficialPublicKey official_public_key(const OfficialSeed& seed) {
  OfficialPublicKey pub{};
  std::array<std::uint8_t, 64> secret = secret_key(seed, pub);
  crypto_wipe(secret.data(), secret.size());
  return pub;
}

OfficialProof official_sign(const OfficialSeed& seed, std::string_view relay, std::string_view room,
                            std::uint16_t version, std::uint64_t time_s) {
  OfficialPublicKey pub{};
  std::array<std::uint8_t, 64> secret = secret_key(seed, pub);
  const std::vector<std::uint8_t> message = official_message(relay, room, version, time_s);
  OfficialProof proof;
  proof.time_s = time_s;
  crypto_ed25519_sign(proof.signature.data(), secret.data(), message.data(), message.size());
  crypto_wipe(secret.data(), secret.size());
  return proof;
}

bool official_check(const OfficialPublicKey& key, std::string_view relay, std::string_view room,
                    std::uint16_t version, const OfficialProof& proof, std::uint64_t now_s) {
  const std::uint64_t age = now_s >= proof.time_s ? now_s - proof.time_s : proof.time_s - now_s;
  if (age > kOfficialProofMaxAgeS) return false;
  const std::vector<std::uint8_t> message = official_message(relay, room, version, proof.time_s);
  return crypto_ed25519_check(proof.signature.data(), key.data(), message.data(), message.size()) == 0;
}

std::optional<OfficialSeed> official_seed_from_text(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::nullopt;
  text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  if (text.size() != 64) return std::nullopt;
  OfficialSeed seed{};
  for (std::size_t i = 0; i < seed.size(); ++i) {
    int value = 0;
    for (std::size_t j = 0; j < 2; ++j) {
      const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[2 * i + j])));
      const int digit = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
      if (digit < 0) return std::nullopt;
      value = value * 16 + digit;
    }
    seed[i] = static_cast<std::uint8_t>(value);
  }
  return seed;
}

std::string official_seed_text(const OfficialSeed& seed) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::uint8_t b : seed) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15u]);
  }
  return out;
}

std::uint64_t official_now_s() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace coop::protocol
