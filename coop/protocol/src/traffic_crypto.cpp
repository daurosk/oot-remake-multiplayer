#include "coop/protocol/traffic_crypto.hpp"

#include <cstring>
#include <utility>

#include "coop/protocol/join_password.hpp"
#include "monocypher.h"

namespace coop::protocol {

namespace {

constexpr char kContext[] = "oot-coop traffic v1";
constexpr char kClientToServer[] = "client to server";
constexpr char kServerToClient[] = "server to client";
constexpr std::size_t kWindowBits = 64;

void update(crypto_blake2b_ctx& ctx, std::span<const std::uint8_t> part) {
  // Each part is prefixed with its length, so no two inputs hash the same.
  std::uint8_t length[4];
  const auto n = static_cast<std::uint32_t>(part.size());
  for (int i = 0; i < 4; ++i) length[i] = static_cast<std::uint8_t>(n >> (8 * i));
  crypto_blake2b_update(&ctx, length, sizeof length);
  crypto_blake2b_update(&ctx, part.data(), part.size());
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::array<std::uint8_t, 32> expand(const std::array<std::uint8_t, 32>& prk, const char* label) {
  std::array<std::uint8_t, 32> key{};
  crypto_blake2b_keyed(key.data(), key.size(), prk.data(), prk.size(),
                       reinterpret_cast<const std::uint8_t*>(label), std::strlen(label));
  return key;
}

constexpr char kPlayerKeyLabel[] = "oot-coop player key v1";

void nonce_for(std::uint8_t channel, std::uint64_t counter, std::uint8_t (&nonce)[24]) {
  std::memset(nonce, 0, sizeof nonce);
  for (int i = 0; i < 8; ++i) nonce[i] = static_cast<std::uint8_t>(counter >> (8 * i));
  nonce[8] = channel;
}

}  // namespace

// ---------------------------------------------------------------- KeyPair

KeyPair KeyPair::generate() { return from_secret(random_nonce()); }

KeyPair KeyPair::from_secret(const std::array<std::uint8_t, 32>& secret) {
  KeyPair pair;
  pair.secret_ = secret;
  crypto_x25519_public_key(pair.public_.data(), pair.secret_.data());
  return pair;
}

KeyPair::KeyPair(KeyPair&& other) noexcept : secret_(other.secret_), public_(other.public_) {
  crypto_wipe(other.secret_.data(), other.secret_.size());
}

KeyPair& KeyPair::operator=(KeyPair&& other) noexcept {
  if (this != &other) {
    secret_ = other.secret_;
    public_ = other.public_;
    crypto_wipe(other.secret_.data(), other.secret_.size());
  }
  return *this;
}

SealedPlayerKey seal_player_key(const std::array<std::uint8_t, 32>& client_to_server, const PlayerKey& key) {
  std::array<std::uint8_t, 32> own = expand(client_to_server, kPlayerKeyLabel);
  const std::uint8_t nonce[24] = {};
  SealedPlayerKey sealed{};
  crypto_aead_lock(sealed.data(), sealed.data() + kPlayerKeyBytes, own.data(), nonce, nullptr, 0, key.data(),
                   key.size());
  crypto_wipe(own.data(), own.size());
  return sealed;
}

std::optional<PlayerKey> open_player_key(const std::array<std::uint8_t, 32>& client_to_server,
                                         const SealedPlayerKey& sealed) {
  std::array<std::uint8_t, 32> own = expand(client_to_server, kPlayerKeyLabel);
  const std::uint8_t nonce[24] = {};
  PlayerKey key{};
  const int bad = crypto_aead_unlock(key.data(), sealed.data() + kPlayerKeyBytes, own.data(), nonce, nullptr, 0,
                                     sealed.data(), kPlayerKeyBytes);
  crypto_wipe(own.data(), own.size());
  if (bad != 0) return std::nullopt;
  return key;
}

KeyPair::~KeyPair() { crypto_wipe(secret_.data(), secret_.size()); }

std::optional<std::array<std::uint8_t, 32>> KeyPair::shared_with(const KeyShare& peer) const {
  std::array<std::uint8_t, 32> shared{};
  crypto_x25519(shared.data(), secret_.data(), peer.data());
  const std::array<std::uint8_t, 32> zero{};
  if (crypto_verify32(shared.data(), zero.data()) == 0) return std::nullopt;
  return shared;
}

// ---------------------------------------------------------------- keys

void TrafficKeys::wipe() {
  crypto_wipe(send.data(), send.size());
  crypto_wipe(receive.data(), receive.size());
}

std::optional<TrafficKeys> derive_traffic_keys(const KeyPair& own, const KeyShare& peer,
                                               std::string_view password,
                                               std::span<const std::uint8_t> hello,
                                               std::span<const std::uint8_t> challenge,
                                               bool is_server) {
  auto shared = own.shared_with(peer);
  if (!shared) return std::nullopt;

  std::array<std::uint8_t, 32> prk{};
  crypto_blake2b_ctx ctx;
  crypto_blake2b_keyed_init(&ctx, prk.size(), shared->data(), shared->size());
  update(ctx, bytes_of(kContext));
  update(ctx, bytes_of(password));
  update(ctx, hello);
  update(ctx, challenge);
  crypto_blake2b_final(&ctx, prk.data());
  crypto_wipe(shared->data(), shared->size());

  TrafficKeys keys;
  const auto c2s = expand(prk, kClientToServer);
  const auto s2c = expand(prk, kServerToClient);
  crypto_wipe(prk.data(), prk.size());
  keys.send = is_server ? s2c : c2s;
  keys.receive = is_server ? c2s : s2c;
  return keys;
}

// ---------------------------------------------------------------- cipher

TrafficCipher::TrafficCipher(const TrafficKeys& keys) : keys_(keys) {}

TrafficCipher::~TrafficCipher() { keys_.wipe(); }

std::optional<std::vector<std::uint8_t>> TrafficCipher::seal(
    std::uint8_t channel, std::span<const std::uint8_t> plain) {
  if (channel >= kSealChannels) return std::nullopt;
  const std::uint64_t counter = ++next_[channel];
  std::uint8_t nonce[24];
  nonce_for(channel, counter, nonce);

  std::vector<std::uint8_t> out(kSealCounterBytes + plain.size() + kSealTagBytes);
  for (std::size_t i = 0; i < kSealCounterBytes; ++i) {
    out[i] = static_cast<std::uint8_t>(counter >> (8 * i));
  }
  std::uint8_t* text = out.data() + kSealCounterBytes;
  std::uint8_t* mac = text + plain.size();
  crypto_aead_lock(text, mac, keys_.send.data(), nonce, nullptr, 0, plain.data(), plain.size());
  return out;
}

bool TrafficCipher::take(Window& w, std::uint64_t counter) {
  if (counter > w.highest) {
    const std::uint64_t shift = counter - w.highest;
    w.seen = shift >= kWindowBits ? 0 : w.seen << shift;
    w.seen |= 1;
    w.highest = counter;
    return true;
  }
  const std::uint64_t back = w.highest - counter;
  if (back >= kWindowBits) return false;
  const std::uint64_t bit = std::uint64_t{1} << back;
  if (w.seen & bit) return false;
  w.seen |= bit;
  return true;
}

OpenError TrafficCipher::open(std::uint8_t channel, std::span<const std::uint8_t> sealed,
                              std::vector<std::uint8_t>& plain) {
  if (channel >= kSealChannels) return OpenError::bad_channel;
  if (sealed.size() < kSealOverheadBytes) return OpenError::too_short;
  std::uint64_t counter = 0;
  for (std::size_t i = 0; i < kSealCounterBytes; ++i) {
    counter |= std::uint64_t{sealed[i]} << (8 * i);
  }
  Window& w = windows_[channel];
  // Counters start at 1. Checked before the tag (cheap), taken only after
  // it, so a forged packet cannot use up a counter.
  if (counter == 0) return OpenError::replayed;
  if (counter <= w.highest) {
    const std::uint64_t back = w.highest - counter;
    if (back >= kWindowBits || (w.seen & (std::uint64_t{1} << back))) {
      return OpenError::replayed;
    }
  }
  const std::size_t text_size = sealed.size() - kSealOverheadBytes;
  const std::uint8_t* text = sealed.data() + kSealCounterBytes;
  const std::uint8_t* mac = text + text_size;
  std::uint8_t nonce[24];
  nonce_for(channel, counter, nonce);
  plain.resize(text_size);
  if (crypto_aead_unlock(plain.data(), mac, keys_.receive.data(), nonce, nullptr, 0, text,
                         text_size) != 0) {
    plain.clear();
    return OpenError::bad_tag;
  }
  take(w, counter);
  return OpenError::none;
}

}  // namespace coop::protocol
