#include "coop/host/as_vars.hpp"

#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace coop::host {

namespace {

// The AS's vtable entry at `slot` is main + `function` (the game's default code, which this file ports).
bool vt_is(GameMemory& memory, std::uint64_t object, std::uint64_t slot, std::uint64_t function) {
  const auto vt = read_ptr(memory, object);
  if (!vt) return false;
  const auto entry = read_u64(memory, *vt + slot);
  return entry && *entry == memory.main_base() + function;
}

// Each table clamps an index at or past its count to 0 (cmp w1, w9; csel ..., lo).
std::uint64_t clamp(std::uint32_t handle, std::uint32_t count) { return handle < count ? handle : 0; }

// Fields of the value lookup that differ between floats (0x37ba6f0) and bools (0x37ba7a0).
struct Layout {
  std::uint64_t vt_slot;
  std::uint64_t function;
  std::uint64_t source_flag_count, source_flags, source_count, source_array;  // in S = [AS + 0xf8]
  std::uint64_t link;      // L = [AS + link]
  std::uint64_t bound;     // B = [AS + bound]
  std::uint64_t own;       // [AS + own]: u32 count, then the values
  std::uint64_t stride;    // bytes a value
};
constexpr Layout kF32{as_addr::kVtF32Address, as_addr::kF32Address, 0xd8, 0xe0, 0xc8, 0xd0, 0x88, 0xb8, 0x28, 4};
constexpr Layout kBool{as_addr::kVtBoolAddress, as_addr::kBoolAddress, 0x108, 0x110, 0xf8, 0x100, 0x90, 0xc0, 0x30, 1};
constexpr std::uint64_t kSource = 0xf8;
constexpr std::uint64_t kLinkedAs = 0xd0;  // 4 linked AS pointers
constexpr int kMaxLinks = 8;               // the game follows links without a limit; a loop gives nothing here

// The address the game's value lookup gives (0x37ba6f0 / 0x37ba7a0, step by step).
std::optional<std::uint64_t> value_address(GameMemory& memory, std::uint64_t as, std::uint32_t handle,
                                           const Layout& l) {
  for (int hop = 0; hop <= kMaxLinks; ++hop) {
    if (!vt_is(memory, as, l.vt_slot, l.function)) return std::nullopt;
    const auto source = read_u64(memory, as + kSource);
    if (!source) return std::nullopt;
    if (*source != 0) {
      const auto n = read_u32(memory, *source + l.source_flag_count);
      const auto flags = read_u64(memory, *source + l.source_flags);
      if (!n || !flags) return std::nullopt;
      const auto flag = read_u8(memory, *flags + clamp(handle, *n));
      if (!flag) return std::nullopt;
      if (*flag == 1) {
        const auto count = read_u32(memory, *source + l.source_count);
        const auto array = read_u64(memory, *source + l.source_array);
        if (!count || !array) return std::nullopt;
        return handle < *count ? *array + l.stride * handle : *array;
      }
    }
    const auto link = read_u64(memory, as + l.link);
    if (!link) return std::nullopt;
    if (*link != 0) {
      const auto n = read_u32(memory, *link);
      if (!n) return std::nullopt;
      if (*n != 0) {
        const std::uint64_t entry = *link + 4 + 2 * clamp(handle, *n);
        const auto which = read_u8(memory, entry);
        if (!which) return std::nullopt;
        const auto index = static_cast<std::int8_t>(*which);
        if (index >= 0) {
          const auto sub = read_u8(memory, entry + 1);
          const auto next = read_ptr(memory, as + kLinkedAs + (index < 4 ? 8 * static_cast<std::uint64_t>(index) : 0));
          if (!sub || !next) return std::nullopt;
          as = *next;
          handle = *sub;
          continue;
        }
      }
    }
    const auto bound = read_u64(memory, as + l.bound);
    if (!bound) return std::nullopt;
    if (*bound != 0) {
      const auto n = read_u32(memory, *bound);
      if (!n) return std::nullopt;
      if (*n != 0) {
        const auto at = read_u64(memory, *bound + 8 + 8 * clamp(handle, *n));
        if (!at) return std::nullopt;
        if (*at != 0) return *at;
      }
    }
    const auto own = read_ptr(memory, as + l.own);
    if (!own) return std::nullopt;
    const auto count = read_u32(memory, *own);
    if (!count) return std::nullopt;
    return handle < *count ? *own + 4 + l.stride * handle : *own + 4;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::int32_t> as_var_handle(GameMemory& memory, std::uint64_t as, std::string_view name, bool boolean) {
  // 0x37b9e80 (floats) / 0x37ba140 (bools).
  if (!vt_is(memory, as, boolean ? as_addr::kVtBoolHandle : as_addr::kVtF32Handle,
             boolean ? as_addr::kBoolHandle : as_addr::kF32Handle)) {
    return std::nullopt;
  }
  const auto resource = read_ptr(memory, as + 0xf0);
  if (!resource) return std::nullopt;
  if (!vt_is(memory, *resource, boolean ? as_addr::kResourceVtBoolTable : as_addr::kResourceVtF32Table,
             boolean ? as_addr::kBoolHandleTable : as_addr::kF32HandleTable)) {
    return std::nullopt;
  }
  const auto file = read_ptr(memory, *resource + 0x10);
  if (!file) return std::nullopt;
  const auto table = read_ptr(memory, *file + 0x20);
  if (!table) return std::nullopt;
  const auto n = read_u16(memory, *table + (boolean ? 0x20 : 0x18));
  if (!n) return std::nullopt;
  if (*n == 0) return -1;
  const auto first = read_u16(memory, *table + (boolean ? 0x22 : 0x1a));
  const auto base = read_ptr(memory, *file + 0x8);
  if (!first || !base) return std::nullopt;
  const auto strings_at = read_u32(memory, *base + 0x3c);
  if (!strings_at) return std::nullopt;
  const std::uint64_t strings = *base + static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                            static_cast<std::int32_t>(*strings_at)));
  std::vector<std::uint8_t> text(name.size() + 1);
  for (std::uint32_t i = 0; i < *n; ++i) {
    const auto offset = read_u32(memory, *table + 0x38 + 4 * (static_cast<std::uint64_t>(*first) + i));
    if (!offset) return std::nullopt;
    if (!memory.read(strings + (*offset & 0xffffffu), std::span(text))) continue;
    if (text.back() == 0 && std::string_view(reinterpret_cast<const char*>(text.data()), name.size()) == name) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

std::optional<float> as_f32(GameMemory& memory, std::uint64_t as, std::int32_t handle) {
  if (handle < 0 || !vt_is(memory, as, as_addr::kVtF32Get, as_addr::kF32Get)) return std::nullopt;
  const auto at = value_address(memory, as, static_cast<std::uint32_t>(handle), kF32);
  if (!at) return std::nullopt;
  return read_f32(memory, *at);
}

std::optional<bool> as_bool(GameMemory& memory, std::uint64_t as, std::int32_t handle) {
  if (handle < 0 || !vt_is(memory, as, as_addr::kVtBoolGet, as_addr::kBoolGet)) return std::nullopt;
  const auto at = value_address(memory, as, static_cast<std::uint32_t>(handle), kBool);
  if (!at) return std::nullopt;
  const auto byte = read_u8(memory, *at);
  if (!byte) return std::nullopt;
  return (*byte & 1) != 0;  // the getter's callers test bit 0 (0x2e75fc8)
}

std::optional<protocol::AsVars> read_as_vars(GameMemory& memory, std::uint64_t as, AsVarsMemo& memo) {
  if (as == 0) return std::nullopt;
  const auto resource = read_ptr(memory, as + 0xf0);
  if (!resource) return std::nullopt;
  if (memo.as != as || memo.resource != *resource) {
    AsVarsMemo fresh;
    for (std::size_t i = 0; i < protocol::kAsVarFloatNames.size(); ++i) {
      const auto h = as_var_handle(memory, as, protocol::kAsVarFloatNames[i], false);
      if (!h) return std::nullopt;
      fresh.f32[i] = *h;
    }
    for (std::size_t i = 0; i < protocol::kAsVarBools.size(); ++i) {
      const auto h = as_var_handle(memory, as, protocol::kAsVarBools[i].name, true);
      if (!h) return std::nullopt;
      fresh.boolean[i] = *h;
    }
    fresh.as = as;
    fresh.resource = *resource;
    memo = fresh;
  }
  bool any = false;
  std::array<float, 3> f{};
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (memo.f32[i] < 0) continue;
    const auto v = as_f32(memory, as, memo.f32[i]);
    if (!v || !std::isfinite(*v)) return std::nullopt;
    f[i] = *v;
    any = true;
  }
  protocol::AsVars out{f[0], f[1], f[2], 0};
  for (std::size_t i = 0; i < protocol::kAsVarBools.size(); ++i) {
    if (memo.boolean[i] < 0) continue;
    const auto v = as_bool(memory, as, memo.boolean[i]);
    if (!v) return std::nullopt;
    if (*v) out.bits |= protocol::kAsVarBools[i].bit;
    any = true;
  }
  if (!any) return std::nullopt;
  return out;
}

}  // namespace coop::host
