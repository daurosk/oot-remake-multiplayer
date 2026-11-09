#include "coop/host/game_memory.hpp"

#include <array>
#include <bit>

namespace coop::host {
namespace {

template <typename T>
std::optional<T> read_le(GameMemory& memory, std::uint64_t address) {
  std::array<std::uint8_t, sizeof(T)> bytes{};
  if (!memory.read(address, bytes)) return std::nullopt;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  }
  return static_cast<T>(value);
}

}  // namespace

std::optional<std::uint8_t> read_u8(GameMemory& memory, std::uint64_t address) {
  return read_le<std::uint8_t>(memory, address);
}
std::optional<std::uint16_t> read_u16(GameMemory& memory, std::uint64_t address) {
  return read_le<std::uint16_t>(memory, address);
}
std::optional<std::uint32_t> read_u32(GameMemory& memory, std::uint64_t address) {
  return read_le<std::uint32_t>(memory, address);
}
std::optional<std::uint64_t> read_u64(GameMemory& memory, std::uint64_t address) {
  return read_le<std::uint64_t>(memory, address);
}
std::optional<float> read_f32(GameMemory& memory, std::uint64_t address) {
  const auto bits = read_u32(memory, address);
  if (!bits) return std::nullopt;
  return std::bit_cast<float>(*bits);
}
std::optional<std::uint64_t> read_ptr(GameMemory& memory, std::uint64_t address) {
  const auto value = read_u64(memory, address);
  if (!value || *value == 0) return std::nullopt;
  return value;
}

}  // namespace coop::host
