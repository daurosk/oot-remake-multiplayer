#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "samples.hpp"

using namespace coop::protocol;
using Bytes = std::vector<std::uint8_t>;

namespace {

// Decodes from a heap block of exactly the input's size, so a read past the
// end lands outside the allocation where debug heaps and sanitizers see it.
// A buffer that decodes must also encode again and decode to the same message.
void decode_exact(const Bytes& input) {
  const std::size_t n = input.size();
  auto block = std::make_unique<std::uint8_t[]>(n == 0 ? 1 : n);
  for (std::size_t i = 0; i < n; ++i) block[i] = input[i];
  Message m;
  const Error e = decode(std::span<const std::uint8_t>(block.get(), n), m);
  if (e == Error::none) {
    Bytes again;
    REQUIRE(encode(m, again) == Error::none);
    Message back;
    REQUIRE(decode(again, back) == Error::none);
    CHECK(back == m);
  }
}

}  // namespace

TEST_CASE("random and mutated buffers never crash the decoder") {
  std::mt19937 rng(0xC0FFEEu);  // fixed seed: a failure is reproducible
  auto pick = [&rng](std::size_t below) {
    return std::uniform_int_distribution<std::size_t>(0, below - 1)(rng);
  };
  auto byte = [&rng] {
    return static_cast<std::uint8_t>(std::uniform_int_distribution<int>(0, 255)(rng));
  };

  std::vector<Bytes> seeds;
  for (const Message& m : test::every_message()) {
    Bytes b;
    REQUIRE(encode(m, b) == Error::none);
    seeds.push_back(b);
  }

  int decoded = 0;
  for (int i = 0; i < 5000; ++i) {
    Bytes b;
    if (i % 4 == 0) {
      // Pure noise, sometimes behind a plausible header.
      b.resize(pick(80));
      for (auto& x : b) x = byte();
      if (b.size() >= kHeaderSize && pick(2) == 0) {
        b[0] = static_cast<std::uint8_t>(1 + pick(11));
        b[1] = 0;
        b[2] = static_cast<std::uint8_t>(b.size() - kHeaderSize);
        b[3] = 0;
      }
    } else {
      b = seeds[pick(seeds.size())];
      const std::size_t edits = 1 + pick(4);
      for (std::size_t k = 0; k < edits; ++k) {
        switch (pick(4)) {
          case 0:  // flip a byte
            if (!b.empty()) b[pick(b.size())] = byte();
            break;
          case 1:  // cut the end off
            if (!b.empty()) b.resize(pick(b.size()));
            break;
          case 2:  // insert a byte
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(pick(b.size() + 1)), byte());
            break;
          case 3:  // keep the header length honest so the body gets parsed
            if (b.size() >= kHeaderSize) {
              const std::size_t len = b.size() - kHeaderSize;
              b[2] = static_cast<std::uint8_t>(len);
              b[3] = static_cast<std::uint8_t>(len >> 8);
            }
            break;
        }
      }
    }
    Message m;
    if (decode(b, m) == Error::none) ++decoded;
    decode_exact(b);
    (void)hello_protocol_version(b);
  }
  // Some mutated buffers should still be valid, or the mutations are too harsh
  // to reach the deeper decoding paths.
  CHECK(decoded > 100);
}
