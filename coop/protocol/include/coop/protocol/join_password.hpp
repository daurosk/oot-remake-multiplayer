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
#include <string>
#include <string_view>

namespace coop::protocol {

inline constexpr std::size_t kJoinNonceBytes = 32;
inline constexpr std::size_t kJoinProofBytes = 32;
// Longest password, in bytes. HMAC takes any length; this only keeps
// coop-options.ini and the command line sane.
inline constexpr std::size_t kMaxJoinPasswordBytes = 128;
// The same-network key (docs/protocol.md "Same network"): a game joining a
// private address with no password typed answers with this one. A server
// takes it only when its host lets the same network in without the password
// (SessionConfig::same_network_open), and only from a private address
// (10/8, 172.16/12, 192.168/16, 169.254/16), never from 127/8, where a
// relay's or a helper's connections come from. It is no secret: the traffic
// is still sealed with fresh keys, it is only not a password.
inline constexpr std::string_view kSameNetworkPassword = "squadala-same-network";

// A public game (0.11.8, docs/play-online.md "Public games"): a host who lists the room on the relay's public list
// hosts it with no password to type. Its join password is this one, made from the room name every joiner sees on
// the list (kPublicPasswordTag + the name in lower case), so the join proof and the traffic keys work exactly as
// with any password: the traffic is still sealed with fresh keys per connection, and only who may join changes
// (anyone who knows the room). A private game's password may never have this shape (is_public_join_password), so
// a public room's password never opens a private one.
inline constexpr std::string_view kPublicPasswordTag = "squadala-public-game:";
std::string public_join_password(std::string_view room);
bool is_public_join_password(std::string_view password);

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

// The proof with traffic encryption (docs/protocol.md "Encrypted traffic"):
// HMAC-SHA256 keyed with the password over the nonce, then the game's and
// the server's X25519 key shares as they went in HELLO and JOIN_CHALLENGE.
// A proof relayed by someone who swapped the shares does not match.
JoinProofBytes join_proof(std::string_view password, const JoinNonce& nonce,
                          const std::array<std::uint8_t, 32>& client_share,
                          const std::array<std::uint8_t, 32>& server_share);

// Compares two proofs in a time that does not depend on where they differ.
bool same_proof(const JoinProofBytes& a, const JoinProofBytes& b);

// 32 fresh bytes from the OS's cryptographic random source, called directly
// (BCryptGenRandom, getrandom, arc4random_buf): nonces, rejoin tokens and the
// X25519 secrets (traffic_crypto.hpp). Stops the program if there is none.
JoinNonce random_nonce();

}  // namespace coop::protocol
