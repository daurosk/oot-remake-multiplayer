#pragma once
// Invite and answer codes for hosting without router setup or a server of
// ours (docs/play-online.md, "Host.bat and an invite code"; issue 165).
//
// INVITE CODE (the host's server makes it, the host sends it to friends):
// the host's internet address and port and the join password, a version and
// a checksum, as letters in groups of five. Join.bat takes it. It holds the
// password, so it is never written to a log.
//
// ANSWER CODE (Join.bat's helper makes it when the direct way does not
// answer; the friend sends it back to the host): the friend's internet
// address and port as a public STUN server saw them, a random nonce and the
// time, signed with a key made from the join password, so the host's server
// only ever sends punch datagrams toward an address that someone who knows
// the password named.
//
// Pure code: no network, so the tests can feed it anything.

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/join_password.hpp"

namespace coop::server::nat {

// An IPv4 address (first part in the highest byte) and a port.
struct Addr {
  std::uint32_t ip = 0;
  std::uint16_t port = 0;
  auto operator<=>(const Addr&) const = default;
};
// 203.0.113.5:27510
std::string show(Addr a);
// Only real internet addresses can be in a code: not private, CGNAT,
// loopback, link-local, multicast or unset.
bool is_public(std::uint32_t ip);

// The letters of the codes (the relay's: no 0/O, 1/I that look alike).
inline constexpr std::string_view kLetters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

// Bytes as letters, 5 bits each, the last one padded with zero bits.
std::string to_letters(std::span<const std::uint8_t> bytes);
// Letters back to `byte_count` bytes: needs exactly the letters to_letters
// makes for that many bytes, and zero padding bits.
std::optional<std::vector<std::uint8_t>> from_letters(std::string_view letters, std::size_t byte_count);
// What someone pasted ("abcde-fghjk", " ABCDE FGHJK. ") as bare letters:
// upper case, without spaces, dashes and dots; nothing if another character
// is in it.
std::optional<std::string> clean_letters(std::string_view typed);
// Groups of five letters joined by '-'.
std::string grouped(std::string_view letters);

// --- The join password Host.bat makes ---
inline constexpr std::size_t kPasswordLetters = 20;  // 100 random bits
// 20 letters from 20 random values (each used mod 32).
std::string make_password(const std::function<std::uint32_t()>& random);
// The system's random source (random_device: the OS's generator).
std::string make_password();
bool is_made_password(std::string_view password);

// The key every punch and answer is signed with: HMAC-SHA256 keyed with the
// password over a fixed label, so a punch key never works as a join proof.
protocol::Sha256Digest punch_key(std::string_view password);

enum class CodeError {
  kOk,
  kNotACode,     // other characters, or not the length of any code
  kBadChecksum,  // a letter was mistyped or is missing
  kWrongKind,    // an answer code where an invite belongs, or the other way
  kNewerCode,    // made by a newer co-op version
  kMalformed,    // checksum fine but the inside does not make sense
  kBadSignature, // an answer code not made with this join password
  kStale,        // an answer code older than 10 minutes (or from the future)
  kNotPublic,    // the address in it is not an internet address
};
// One line for a person ("This is not an invite code: ...").
std::string explain(CodeError error);

// --- Invite code ---
struct Invite {
  Addr host;
  std::string password;  // 1 to 128 bytes, no spaces
};
inline constexpr std::uint8_t kInviteMade = 0x11;  // version 1, a made password
inline constexpr std::uint8_t kInviteText = 0x12;  // version 1, the host's own password
// Nothing when the password is empty, too long or has a space.
std::optional<std::string> make_invite(const Invite& invite);
struct InviteRead {
  CodeError error = CodeError::kNotACode;
  Invite invite;
};
InviteRead read_invite(std::string_view typed);
// Cleaned length of an invite (or answer) code: decides what Join.bat got.
bool looks_like_code(std::string_view typed);

// --- Answer code ---
inline constexpr std::uint8_t kAnswerV1 = 0x21;
inline constexpr std::size_t kNonceBytes = 8;
using Nonce = std::array<std::uint8_t, kNonceBytes>;
struct Answer {
  Addr guest;               // the friend's address as the STUN server saw it
  Nonce nonce{};            // the friend's helper's, new each time
  std::uint32_t time = 0;   // Unix seconds (low 32 bits) when it was made
};
inline constexpr std::uint32_t kAnswerMaxAgeSeconds = 600;
std::string make_answer(const Answer& answer, std::string_view password);
struct AnswerRead {
  CodeError error = CodeError::kNotACode;
  Answer answer;
};
AnswerRead read_answer(std::string_view typed, std::string_view password, std::uint64_t unix_now);

}  // namespace coop::server::nat
