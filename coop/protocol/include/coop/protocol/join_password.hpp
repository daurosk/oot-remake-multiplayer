#pragma once

// The join password (protocol 9, docs/protocol.md "Join password"): the
// server sends a random nonce in JOIN_CHALLENGE, the game answers with
// HMAC-SHA256(password, nonce) in JOIN_PROOF. The password itself never goes
// on the wire, and a proof is good for one nonce only, so a recorded one
// cannot be sent again on another connection.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace coop::protocol {

inline constexpr std::size_t kJoinNonceBytes = 32;
inline constexpr std::size_t kJoinProofBytes = 32;
// Longest password, in bytes. HMAC takes any length; this only keeps
// coop-options.ini and the command line sane.
inline constexpr std::size_t kMaxJoinPasswordBytes = 128;

using Sha256Digest = std::array<std::uint8_t, 32>;
using JoinNonce = std::array<std::uint8_t, kJoinNonceBytes>;
using JoinProofBytes = std::array<std::uint8_t, kJoinProofBytes>;

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104).
Sha256Digest sha256(std::span<const std::uint8_t> data);
Sha256Digest hmac_sha256(std::span<const std::uint8_t> key,
                         std::span<const std::uint8_t> message);

// The answer to a JOIN_CHALLENGE: HMAC-SHA256 keyed with the password's
// UTF-8 bytes over the nonce.
JoinProofBytes join_proof(std::string_view password, const JoinNonce& nonce);

// Compares two proofs in a time that does not depend on where they differ.
bool same_proof(const JoinProofBytes& a, const JoinProofBytes& b);

// A fresh nonce from the system's random source (std::random_device, which
// is the OS's cryptographic generator on Windows, Linux and macOS).
JoinNonce random_nonce();

}  // namespace coop::protocol
