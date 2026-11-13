#pragma once

// Traffic encryption (docs/protocol.md "Encrypted traffic"): with a join
// password, every packet after the password check is sealed with
// XChaCha20-Poly1305 under a key only the two ends can make.
//
// - Each side makes a fresh X25519 key pair per connection. The game sends
//   its public key at the end of HELLO, the server sends its own at the end
//   of JOIN_CHALLENGE.
// - Both ends mix the X25519 secret, the join password and the exact bytes
//   of that HELLO and JOIN_CHALLENGE (BLAKE2b) into one key per direction.
//   Someone without the password cannot make the keys, even after taking
//   part in the exchange; someone who only listens cannot either. Changing
//   any byte of HELLO or JOIN_CHALLENGE on the way gives the two ends other
//   keys, so nothing after it opens.
// - A sealed packet is the u64 packet counter, the encrypted message and a
//   16-byte tag. The nonce is the counter and the ENet channel, so a packet
//   moved to another channel does not open. Every counter is taken once per
//   channel (a 64-packet window for unreliable channels, which may drop and
//   reorder): a recorded packet sent again is dropped.
//
// The primitives are Monocypher's (cmake/monocypher.cmake); this file only
// puts them together.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace coop::protocol {

inline constexpr std::size_t kKeyShareBytes = 32;
using KeyShare = std::array<std::uint8_t, kKeyShareBytes>;

// Bytes a sealed packet has on top of its message: counter and tag.
inline constexpr std::size_t kSealCounterBytes = 8;
inline constexpr std::size_t kSealTagBytes = 16;
inline constexpr std::size_t kSealOverheadBytes = kSealCounterBytes + kSealTagBytes;
// ENet channels a channel state keeps counters for.
inline constexpr std::size_t kSealChannels = 8;

// One end's X25519 key pair for one connection. The secret is wiped when it
// is destroyed.
class KeyPair {
 public:
  // A fresh pair from the system's random source.
  static KeyPair generate();
  // Only for tests: a pair from a given secret.
  static KeyPair from_secret(const std::array<std::uint8_t, 32>& secret);

  KeyPair(const KeyPair&) = delete;
  KeyPair& operator=(const KeyPair&) = delete;
  KeyPair(KeyPair&& other) noexcept;
  KeyPair& operator=(KeyPair&& other) noexcept;
  ~KeyPair();

  const KeyShare& share() const { return public_; }
  // The raw X25519 secret shared with `peer`; empty for the all-zero one
  // (a low-order point, which an attacker could pick to fix the result).
  std::optional<std::array<std::uint8_t, 32>> shared_with(const KeyShare& peer) const;

 private:
  KeyPair() = default;
  std::array<std::uint8_t, 32> secret_{};
  KeyShare public_{};
};

struct TrafficKeys {
  std::array<std::uint8_t, 32> send{};
  std::array<std::uint8_t, 32> receive{};
  void wipe();
};

// The keys for one connection. `hello` and `challenge` are the whole packets
// as they went on the wire (header included). `is_server` picks which key
// is for sending. Empty when the other end's share is not a usable X25519
// key (the all-zero secret of a low-order point).
std::optional<TrafficKeys> derive_traffic_keys(const KeyPair& own, const KeyShare& peer,
                                               std::string_view password,
                                               std::span<const std::uint8_t> hello,
                                               std::span<const std::uint8_t> challenge,
                                               bool is_server);

// Why a packet did not open.
enum class OpenError : std::uint8_t {
  none,
  too_short,  // shorter than counter and tag
  bad_tag,    // wrong key, changed on the way, or another channel's
  replayed,   // its counter was taken already, or is older than the window
  bad_channel,
};

// Both directions of one connection once the keys are known.
class TrafficCipher {
 public:
  explicit TrafficCipher(const TrafficKeys& keys);
  TrafficCipher(const TrafficCipher&) = delete;
  TrafficCipher& operator=(const TrafficCipher&) = delete;
  ~TrafficCipher();

  // The sealed packet for `plain` on `channel`. Empty only for a channel at
  // or past kSealChannels.
  std::optional<std::vector<std::uint8_t>> seal(std::uint8_t channel,
                                                std::span<const std::uint8_t> plain);
  // The message inside a sealed packet that came on `channel`.
  OpenError open(std::uint8_t channel, std::span<const std::uint8_t> sealed,
                 std::vector<std::uint8_t>& plain);

 private:
  struct Window {
    std::uint64_t highest = 0;  // the highest counter opened, 0 for none yet
    std::uint64_t seen = 0;     // bit i: highest - i was opened
  };
  bool take(Window& w, std::uint64_t counter);

  TrafficKeys keys_;
  std::array<std::uint64_t, kSealChannels> next_{};  // counters start at 1
  std::array<Window, kSealChannels> windows_{};
};

}  // namespace coop::protocol
