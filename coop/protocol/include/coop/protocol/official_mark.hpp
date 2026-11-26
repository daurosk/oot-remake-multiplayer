// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The official public game's mark (docs/official-game.md): the official server signs its listing with an Ed25519
// key only the owner holds; every kit has the public key built in and shows "Official public game" only for a room
// whose proof checks. The proof binds the relay it is listed on (as the kits name it: official_relay_name), the room
// name (in the relay's lower case), the listing's version and the time it was made, so it is good for that relay,
// room and version only and goes stale after kOfficialProofMaxAgeS: a copy taken off the list cannot mark a room on
// another relay or another room, and a replayed one stops working within minutes.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::protocol {

using OfficialSeed = std::array<std::uint8_t, 32>;  // the private key's seed (the key file: 64 hex digits)
using OfficialPublicKey = std::array<std::uint8_t, 32>;
using OfficialSignature = std::array<std::uint8_t, 64>;

// The kit's built-in public key (the pair made 2026-11-21; the seed is the owner's alone, never in the repo).
inline constexpr OfficialPublicKey kOfficialPublicKey = {
    0x6d, 0x7a, 0xb1, 0x53, 0x26, 0x06, 0xdf, 0xc0, 0x61, 0x79, 0x72, 0x01, 0x91, 0x8b, 0xc5, 0x77,
    0xce, 0xac, 0xda, 0x5b, 0x3e, 0xc7, 0x1d, 0xf0, 0x0c, 0x79, 0xe5, 0x9f, 0xda, 0xdf, 0xf5, 0xf3};

// How old (or how far ahead of the kit's clock) a proof may be, in seconds. The server signs a fresh one with every
// keepalive (5 s), so this only allows for clocks that are a little off.
inline constexpr std::uint64_t kOfficialProofMaxAgeS = 300;

struct OfficialProof {
  std::uint64_t time_s = 0;  // when it was signed: seconds since 1970 (UTC)
  OfficialSignature signature{};
  bool operator==(const OfficialProof&) const = default;
};

// A relay as the signed bytes name it: its host in lower case, ':', its port ("159.54.182.85:27530"), the same
// whether a player typed the port or not.
std::string official_relay_name(std::string_view host, std::uint16_t port);
// The bytes signed: "squadala-official-listing-v2", 0, the relay (official_relay_name), 0, the room in lower case, 0,
// version (u16, low byte first), time_s (u64, low byte first).
std::vector<std::uint8_t> official_message(std::string_view relay, std::string_view room, std::uint16_t version,
                                           std::uint64_t time_s);
OfficialPublicKey official_public_key(const OfficialSeed& seed);
OfficialProof official_sign(const OfficialSeed& seed, std::string_view relay, std::string_view room,
                            std::uint16_t version, std::uint64_t time_s);
// Whether `proof` is `key`'s for this relay, room and version, made no more than kOfficialProofMaxAgeS from `now_s`.
bool official_check(const OfficialPublicKey& key, std::string_view relay, std::string_view room,
                    std::uint16_t version, const OfficialProof& proof, std::uint64_t now_s);
// A key file's text: 64 hex digits (either case) with spaces or line ends around them; nothing when it is not.
std::optional<OfficialSeed> official_seed_from_text(std::string_view text);
std::string official_seed_text(const OfficialSeed& seed);
// The time now, as official_check wants it.
std::uint64_t official_now_s();

}  // namespace coop::protocol
