#pragma once

// A fake guest memory for the host tests: a sparse byte map. A read fails when
// any of its bytes was never written. Also lays out the game's structures at
// the addresses of coop/host/src/addresses.hpp.

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>

#include "../../coop/host/src/addresses.hpp"
#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"

namespace coop::host::test {

class FakeMemory final : public GameMemory {
 public:
  static constexpr std::uint64_t kBase = 0x7100000000ull;

  std::uint64_t main_base() const override { return kBase; }

  bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
    ++reads;
    for (std::size_t i = 0; i < out.size(); ++i) {
      const auto it = bytes_.find(address + i);
      if (it == bytes_.end()) return false;
      out[i] = it->second;
    }
    return true;
  }

  void put(std::uint64_t address, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) bytes_[address + i] = p[i];
  }
  void put_u8(std::uint64_t a, std::uint8_t v) { put(a, &v, 1); }
  void put_u16(std::uint64_t a, std::uint16_t v) { put_le(a, v, 2); }
  void put_u32(std::uint64_t a, std::uint32_t v) { put_le(a, v, 4); }
  void put_u64(std::uint64_t a, std::uint64_t v) { put_le(a, v, 8); }
  void put_f32(std::uint64_t a, float v) { put_u32(a, std::bit_cast<std::uint32_t>(v)); }
  void put_vec3(std::uint64_t a, const Vec3& v) {
    for (std::size_t i = 0; i < 3; ++i) put_f32(a + 4 * i, v[i]);
  }
  void erase(std::uint64_t address, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) bytes_.erase(address + i);
  }

  std::size_t reads = 0;

 private:
  void put_le(std::uint64_t a, std::uint64_t v, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) bytes_[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
  std::unordered_map<std::uint64_t, std::uint8_t> bytes_;
};

// Heap addresses of the fake game's objects.
struct Layout {
  std::uint64_t game_ui = 0x80000000;
  std::uint64_t scene = 0x80010000;
  std::uint64_t table = 0x80020000;
  std::uint64_t array = 0x80030000;
  std::uint64_t manager = 0x80040000;
  std::uint64_t link1 = 0x80050000;
  std::uint64_t holder1 = 0x80060000;
  std::uint64_t actor1 = 0x80070000;
  std::uint64_t link2 = 0x80080000;
  std::uint64_t holder2 = 0x80090000;
  std::uint64_t actor2 = 0x800a0000;
  std::uint64_t banc = 0x800b0000;
  std::uint64_t path = 0x800c0000;
  std::uint64_t events = 0x800d0000;
};

inline void put_matrix(FakeMemory& m, std::uint64_t actor, const Mat3& r) {
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = 0; j < 3; ++j) {
      m.put_f32(actor + addr::kActorRotation + 4 * (3 * i + j), r[i][j]);
    }
  }
}

inline void put_actor(FakeMemory& m, std::uint64_t link, std::uint64_t holder, std::uint64_t actor,
                      std::uint32_t id, std::uint32_t state, const Vec3& position) {
  m.put_u64(link + addr::kLinkHolder, holder);
  m.put_u32(link + addr::kLinkId, id);
  m.put_u64(holder + addr::kHolderActor, actor);
  m.put_u32(holder + addr::kHolderId, id);
  m.put_u32(actor + addr::kActorState, state);
  m.put_vec3(actor + addr::kActorPosition, position);
  put_matrix(m, actor, Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}});
}

inline void put_scene_path(FakeMemory& m, const Layout& l, const std::string& path,
                           std::uint64_t capacity = 64) {
  m.put_u64(l.banc + addr::kBancPathCapacity, capacity);
  std::string buffer = path;
  buffer.resize(capacity, '\0');
  m.put(l.path, buffer.data(), buffer.size());
}

// A whole, healthy game: an adult player at (1, 2, 3) facing +z (identity),
// in spot00, no event running.
inline Layout make_game(FakeMemory& m) {
  const Layout l;
  const auto base = FakeMemory::kBase;

  m.put_u64(base + addr::kGameUiModulePtr, l.game_ui);
  m.put_u16(l.game_ui + addr::kGameUiPoseFlags, addr::kGameUiPoseValidBit);
  m.put_vec3(l.game_ui + addr::kGameUiPosition, {1.0f, 2.0f, 3.0f});
  m.put_u8(l.game_ui + addr::kGameUiIsChild, 0);

  m.put_u64(base + addr::kSceneModulePtr, l.scene);
  m.put_u64(l.scene + addr::kSceneTable, l.table);
  m.put_u32(l.table + addr::kTableCount, 0x20);
  m.put_u64(l.table + addr::kTableArray, l.array);
  m.put_u64(l.array + addr::kPlayerManagerSlot, l.manager);
  m.put_u64(l.manager + addr::kManagerMustBeSet, 0x1234);
  m.put_u64(l.manager + addr::kManagerLinkFirst, l.link1);
  m.put_u64(l.manager + addr::kManagerLinkSecond, l.link2);
  put_actor(m, l.link1, l.holder1, l.actor1, 7, addr::kActorStatePresent, {1.0f, 2.0f, 3.0f});
  put_actor(m, l.link2, l.holder2, l.actor2, addr::kEmptyId, 0, {0.0f, 0.0f, 0.0f});

  m.put_u64(base + addr::kBancModulePtr, l.banc);
  m.put_u64(l.banc + addr::kBancMustBeSet, 0x5678);
  m.put_u64(l.banc + addr::kBancPathPtr, l.path);
  put_scene_path(m, l, "Work/Banc/Field/spot00.banc");

  m.put_u64(base + addr::kEventManagerPtr, l.events);
  m.put_u64(l.events + addr::kEventRunning, 0);
  return l;
}

}  // namespace coop::host::test
