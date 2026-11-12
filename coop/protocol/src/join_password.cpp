#include "coop/protocol/join_password.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <exception>

// The OS's cryptographic random source, called directly (std::random_device
// is not promised to be one).
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <sys/random.h>
#endif

namespace coop::protocol {
namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

class Sha256 {
 public:
  void add(std::span<const std::uint8_t> data) {
    for (std::uint8_t b : data) {
      block_[used_++] = b;
      if (used_ == block_.size()) {
        compress();
        used_ = 0;
      }
    }
    length_ += data.size();
  }

  Sha256Digest finish() {
    const std::uint64_t bits = length_ * 8;
    const std::uint8_t one = 0x80;
    add({&one, 1});
    const std::uint8_t zero = 0;
    while (used_ != 56) add({&zero, 1});
    std::array<std::uint8_t, 8> tail{};
    for (int i = 0; i < 8; ++i) tail[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    add(tail);
    Sha256Digest out{};
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * j));
    }
    return out;
  }

 private:
  void compress() {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
      w[i] = (std::uint32_t{block_[4 * i]} << 24) | (std::uint32_t{block_[4 * i + 1]} << 16) |
             (std::uint32_t{block_[4 * i + 2]} << 8) | std::uint32_t{block_[4 * i + 3]};
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
      const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  std::array<std::uint32_t, 8> h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> block_{};
  std::size_t used_ = 0;
  std::uint64_t length_ = 0;
};

}  // namespace

Sha256Digest sha256(std::span<const std::uint8_t> data) {
  Sha256 s;
  s.add(data);
  return s.finish();
}

Sha256Digest hmac_sha256(std::span<const std::uint8_t> key,
                         std::span<const std::uint8_t> message) {
  std::array<std::uint8_t, 64> k{};
  if (key.size() > k.size()) {
    const Sha256Digest d = sha256(key);
    std::copy(d.begin(), d.end(), k.begin());
  } else {
    std::copy(key.begin(), key.end(), k.begin());
  }
  std::array<std::uint8_t, 64> pad{};
  for (std::size_t i = 0; i < k.size(); ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
  Sha256 inner;
  inner.add(pad);
  inner.add(message);
  const Sha256Digest inner_digest = inner.finish();
  for (std::size_t i = 0; i < k.size(); ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
  Sha256 outer;
  outer.add(pad);
  outer.add(inner_digest);
  return outer.finish();
}

JoinProofBytes join_proof(std::string_view password, const JoinNonce& nonce) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(password.data());
  return hmac_sha256({bytes, password.size()}, nonce);
}

JoinProofBytes join_proof(std::string_view password, const JoinNonce& nonce,
                          const std::array<std::uint8_t, 32>& client_share,
                          const std::array<std::uint8_t, 32>& server_share) {
  std::array<std::uint8_t, kJoinNonceBytes + 64> message{};
  std::copy(nonce.begin(), nonce.end(), message.begin());
  std::copy(client_share.begin(), client_share.end(), message.begin() + kJoinNonceBytes);
  std::copy(server_share.begin(), server_share.end(), message.begin() + kJoinNonceBytes + 32);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(password.data());
  return hmac_sha256({bytes, password.size()}, message);
}

bool same_proof(const JoinProofBytes& a, const JoinProofBytes& b) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

JoinNonce random_nonce() {
  JoinNonce nonce{};
#if defined(_WIN32)
  if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    std::terminate();  // no random source: a guessable key is worse than stopping
  }
#elif defined(__APPLE__)
  arc4random_buf(nonce.data(), nonce.size());
#else
  std::size_t got = 0;
  while (got < nonce.size()) {
    const ssize_t n = getrandom(nonce.data() + got, nonce.size() - got, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::terminate();  // no random source: a guessable key is worse than stopping
    }
    got += static_cast<std::size_t>(n);
  }
#endif
  return nonce;
}

}  // namespace coop::protocol
