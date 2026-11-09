#pragma once

// Encoder and decoder for the messages in messages.hpp.
//
// One buffer holds exactly one message: u16 type, u16 length, body.
// Everything is little-endian on the wire, whatever the host.
// The decoder never trusts its input: every length is checked against the
// bytes left, and a bad buffer gives an error, never a crash or a read past
// the end.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::protocol {

enum class Error : std::uint8_t {
  none = 0,
  truncated,            // fewer bytes than a header or field needs
  trailing_bytes,       // bytes left after the message or after a body
  bad_length,           // a length field that does not fit (component, string)
  string_too_long,      // a string over its maximum
  bad_utf8,             // a string that is not valid UTF-8
  bad_value,            // a value out of range (player number, enum, non-finite float)
  missing_component,    // PLAYER_SAMPLE without its time component
  duplicate_component,  // PLAYER_SAMPLE with a known component twice
  unknown_type,         // a message type this build does not know; skip it
  too_large,            // encode only: the message does not fit a u16 length
  wrong_channel,        // check_received: not a channel this type goes on
  wrong_sender,         // check_received: this side may not send it, or its
                        // player field is wrong for who sent it
};

std::string_view to_string(Error error);

// Appends the encoded message to `out`. On an error `out` is left as it was.
Error encode(const Message& message, std::vector<std::uint8_t>& out);

// Convenience: the encoded message, or nothing on an error.
std::optional<std::vector<std::uint8_t>> encode(const Message& message);

// Decodes exactly one message from `bytes`. On an error `out` is unspecified.
Error decode(std::span<const std::uint8_t> bytes, Message& out);

// Reads only the protocol version of a HELLO, even one whose remaining layout
// this build cannot read. Nothing when `bytes` is not a HELLO with at least
// the version in it.
std::optional<std::uint16_t> hello_protocol_version(
    std::span<const std::uint8_t> bytes);

// Checks a decoded message against where it came from, before acting on it:
// that side may send this type (may_send), on this channel
// (allowed_on_channel), and the player field of PLAYER_STATUS, CHAT,
// PLAYER_SAMPLE, JOIN_PARTNER, FLAG_SET and FLAG_STATE is 0 from a client and
// 1 to 4 from the server; a JOIN_PARTNER from the server also names a player
// other than its sender. The server still knows who the client is from the
// connection, never from the message. Who is host (FLAG_STATE) is for the
// server and client to check, not the codec.
Error check_received(const Message& message, Sender from, std::uint8_t channel);

// The player field of PLAYER_STATUS, CHAT, PLAYER_SAMPLE, JOIN_PARTNER,
// FLAG_SET or FLAG_STATE; nothing for the other messages.
std::optional<std::uint8_t> sender_of(const Message& message);

// For the server, before relaying a message with a player field (above): writes
// the sending player's number (1 to 4) into the player field. False, and the
// message unchanged, for any other message, a number out of range, or a
// JOIN_PARTNER whose other player is that same number.
bool set_sender(Message& message, std::uint8_t player);

// The server's handshake check of a decoded HELLO: the reason to refuse it,
// or nothing when its protocol version and game build match. Server full, name
// taken and bad name are for the server to decide.
std::optional<RefuseReason> check_hello(const Hello& hello,
                                        std::string_view expected_game_build_id);

// The hash of a flag name for FlagEntry: Murmur3 x86 32-bit, seed 0, over the
// name's bytes.
std::uint32_t flag_hash(std::string_view name);

// The rules for one flag entry: a known key kind with only its own fields
// set, a known value kind and a value it allows (a bool 0 or 1, an f32
// finite), and a key64 flag a bool.
bool valid_flag(const FlagEntry& entry);

// Splits a whole flag table into FLAG_SNAPSHOT parts of at most
// kMaxFlagEntries entries each, in order. An empty table gives one empty part.
// Nothing when the table needs more than 65535 parts or holds a key twice.
std::optional<std::vector<FlagSnapshot>> split_snapshot(
    std::span<const FlagEntry> flags);

}  // namespace coop::protocol
