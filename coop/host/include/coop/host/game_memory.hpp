#pragma once

// What the emulator (or later the recomp) gives the host library: a way to
// read guest memory. No game knowledge here; the addresses are in
// coop/host/src/addresses.hpp.

#include <cstdint>
#include <optional>
#include <span>

namespace coop::host {

class GameMemory {
 public:
  virtual ~GameMemory() = default;

  // Where the game's main module is mapped in guest memory (0x7100000000 in
  // the logs seen so far).
  virtual std::uint64_t main_base() const = 0;

  // Copies out.size() bytes from guest address `address` into `out`. False
  // when any of them cannot be read (unmapped, out of range); `out` is then
  // unspecified. Must not throw.
  virtual bool read(std::uint64_t address, std::span<std::uint8_t> out) = 0;
};

// Little-endian reads on top of GameMemory. Nothing on a failed read.
std::optional<std::uint8_t> read_u8(GameMemory& memory, std::uint64_t address);
std::optional<std::uint16_t> read_u16(GameMemory& memory, std::uint64_t address);
std::optional<std::uint32_t> read_u32(GameMemory& memory, std::uint64_t address);
std::optional<std::uint64_t> read_u64(GameMemory& memory, std::uint64_t address);
std::optional<float> read_f32(GameMemory& memory, std::uint64_t address);
// A guest pointer: as read_u64, but also nothing when it is null.
std::optional<std::uint64_t> read_ptr(GameMemory& memory, std::uint64_t address);

}  // namespace coop::host
