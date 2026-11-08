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

// The server's handshake check of a decoded HELLO: the reason to refuse it,
// or nothing when its protocol version and game build match. Server full and
// name taken are for the server to decide.
std::optional<RefuseReason> check_hello(const Hello& hello,
                                        std::string_view expected_game_build_id);

}  // namespace coop::protocol
