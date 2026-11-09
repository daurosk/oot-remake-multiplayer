#include "coop/host/game_flags.hpp"

#include <array>
#include <bit>
#include <cstring>

// Offsets: docs/flags.md, read from main of build e0f3ccfaf50065ae.
namespace coop::host {
namespace {

// Pointer to gmd::GameDataMgr, at main + 0x4e842a8 (GOT slot 0x4c7e180).
constexpr std::uint64_t kManagerPtr = 0x4e842a8;

// A hashed type table: entries, an open-addressing hash table of {u32 hash,
// u32 entry index} slots, and a write queue. Offsets from the manager.
struct Table {
  std::uint64_t count, entries, slots, capacity;  // u32, ptr, ptr, u32
  std::uint64_t stride, value;                    // entry size, value offset
  std::uint64_t queue_cap, queue_buf, queue_word;  // u32, ptr, u32 packed
  std::uint64_t slot_size;                         // queue slot size
};
constexpr Table kBool{0x10, 0x18, 0x20, 0x2c, 0x18, 0x16, 0x30, 0x38, 0x40, 8};
constexpr Table kInt{0xd0, 0xd8, 0xe0, 0xec, 0x28, 0x1c, 0xf0, 0xf8, 0x100, 12};
// BoolArray: entries of 0x40, element count u32 at +0x20, bits at *(+0x28).
constexpr Table kBoolArray{0x70, 0x78, 0x80, 0x8c, 0x40, 0, 0, 0, 0, 0};
constexpr std::uint64_t kArraySize = 0x20, kArrayBits = 0x28;

// 64-bit keys: a sorted array of 16-byte records {u64 key, u32 file mask,
// u8 value, s8 file index, u8 flags}, and a queue of {u8 value, u32 record
// index} slots, empty = index 0xffffffff.
constexpr std::uint64_t kKeyCount = 0xc58, kKeyRecords = 0xc60, kKeyQueueCap = 0xc68,
                        kKeyQueueBuf = 0xc70, kKeyQueueWord = 0xc78;
constexpr std::uint64_t kKeyRecordSize = 0x10, kKeyValue = 0xc;

// Queue word: bits 0-19 slots taken, 20-27 slots finished, 28-31 a round
// the applier (0xb99980, 0xba24b0) moves on each time it empties the queue.
constexpr std::uint32_t kTakenMask = 0xfffff, kFinishedMask = 0xff00000,
                        kFinishedOne = 0x100000;
constexpr int kCasTries = 64;

std::optional<std::uint64_t> manager(GameMemory& m) {
  return read_ptr(m, m.main_base() + kManagerPtr);
}

// The entry index of `hash` in table `t`, as 0x833920 probes it.
std::optional<std::uint32_t> find(GameMemory& m, std::uint64_t mgr, const Table& t,
                                  std::uint32_t hash) {
  if (hash == 0) return std::nullopt;
  const auto slots = read_ptr(m, mgr + t.slots);
  const auto cap = read_u32(m, mgr + t.capacity);
  const auto count = read_u32(m, mgr + t.count);
  if (!slots || !cap || !count || *cap == 0) return std::nullopt;
  const std::uint32_t start = hash % *cap;
  std::uint32_t i = start;
  do {
    const auto h = read_u32(m, *slots + 8ull * i);
    if (!h || *h == 0) return std::nullopt;
    if (*h == hash) {
      const auto index = read_u32(m, *slots + 8ull * i + 4);
      if (!index || *index >= *count) return std::nullopt;
      return *index;
    }
    i = i + 1 >= *cap ? 0 : i + 1;
  } while (i != start);
  return std::nullopt;
}

// The record index of `key`, by binary search as 0x838960 does.
std::optional<std::uint32_t> find_key(GameMemory& m, std::uint64_t mgr, std::uint64_t key) {
  if (key == 0) return std::nullopt;
  const auto count = read_u32(m, mgr + kKeyCount);
  const auto records = read_ptr(m, mgr + kKeyRecords);
  if (!count || !records || *count == 0 || *count > 0x7fffffff) return std::nullopt;
  std::int64_t lo = 0, hi = static_cast<std::int64_t>(*count) - 1;
  while (lo <= hi) {
    const std::int64_t mid = (lo + hi) / 2;
    const auto k = read_u64(m, *records + kKeyRecordSize * mid);
    if (!k) return std::nullopt;
    if (*k == key) return static_cast<std::uint32_t>(mid);
    if (*k < key) lo = mid + 1; else hi = mid - 1;
  }
  return std::nullopt;
}

// The newest queued value for `id` (a hash, or a record index for 64-bit
// keys), whose slot holds `id` at +4 and the value at +0.
std::optional<std::uint32_t> queued(GameMemory& m, std::uint64_t mgr, std::uint64_t cap_at,
                                    std::uint64_t buf_at, std::uint64_t word_at,
                                    std::uint64_t slot_size, std::uint32_t id) {
  const auto cap = read_u32(m, mgr + cap_at);
  const auto buf = read_ptr(m, mgr + buf_at);
  const auto word = read_u32(m, mgr + word_at);
  if (!cap || !buf || !word) return std::nullopt;
  std::uint32_t taken = *word & kTakenMask;
  if (taken > *cap) taken = *cap;
  for (std::uint32_t i = taken; i-- > 0;) {
    const auto at = *buf + slot_size * i;
    if (read_u32(m, at + 4) == id) return read_u32(m, at);
  }
  return std::nullopt;
}

// Takes a slot, writes it, marks it finished: the game's setter (0x682b20).
bool enqueue(WritableGameMemory& m, std::uint64_t mgr, std::uint64_t cap_at,
             std::uint64_t buf_at, std::uint64_t word_at, std::uint64_t slot_size,
             std::uint32_t value, std::uint32_t id, bool value_is_byte) {
  const auto cap = read_u32(m, mgr + cap_at);
  const auto buf = read_ptr(m, mgr + buf_at);
  if (!cap || !buf) return false;
  const std::uint64_t word_addr = mgr + word_at;
  std::optional<std::uint32_t> taken_word;
  for (int t = 0; t < kCasTries && !taken_word; ++t) {
    const auto w = read_u32(m, word_addr);
    if (!w || (*w & kTakenMask) >= *cap) return false;
    if (m.compare_exchange_u32(word_addr, *w, *w + 1) == w) taken_word = *w;
  }
  if (!taken_word) return false;
  const std::uint64_t at = *buf + slot_size * (*taken_word & kTakenMask);
  std::array<std::uint8_t, 4> id_bytes{}, value_bytes{};
  std::memcpy(id_bytes.data(), &id, 4);  // the guest is little-endian, as is every host we build for
  std::memcpy(value_bytes.data(), &value, 4);
  if (!m.write(at + 4, id_bytes)) return false;
  if (!m.write(at, std::span(value_bytes).first(value_is_byte ? 1 : 4))) return false;
  for (int t = 0; t < kCasTries; ++t) {
    const auto w = read_u32(m, word_addr);
    if (!w) return false;
    // The applier ran since the slot was taken: the slot may be lost.
    if ((*w >> 28) != (*taken_word >> 28)) return false;
    const std::uint32_t next = (*w & kFinishedMask) == kFinishedMask ? (*w & ~kFinishedMask)
                                                                     : *w + kFinishedOne;
    if (m.compare_exchange_u32(word_addr, *w, next) == w) return true;
  }
  return false;
}

}  // namespace

std::optional<bool> GameFlags::get_bool(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kBool, hash);
  if (!index) return std::nullopt;
  if (const auto q = queued(memory_, *mgr, kBool.queue_cap, kBool.queue_buf, kBool.queue_word,
                            kBool.slot_size, hash))
    return (*q & 0xff) != 0;
  const auto entries = read_ptr(memory_, *mgr + kBool.entries);
  if (!entries) return std::nullopt;
  const auto v = read_u8(memory_, *entries + kBool.stride * *index + kBool.value);
  if (!v) return std::nullopt;
  return *v != 0;
}

std::optional<std::int32_t> GameFlags::get_int(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kInt, hash);
  if (!index) return std::nullopt;
  if (const auto q = queued(memory_, *mgr, kInt.queue_cap, kInt.queue_buf, kInt.queue_word,
                            kInt.slot_size, hash))
    return static_cast<std::int32_t>(*q);
  const auto entries = read_ptr(memory_, *mgr + kInt.entries);
  if (!entries) return std::nullopt;
  const auto v = read_u32(memory_, *entries + kInt.stride * *index + kInt.value);
  if (!v) return std::nullopt;
  return static_cast<std::int32_t>(*v);
}

std::optional<bool> GameFlags::get_key64(std::uint64_t key) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find_key(memory_, *mgr, key);
  if (!index) return std::nullopt;
  if (const auto q = queued(memory_, *mgr, kKeyQueueCap, kKeyQueueBuf, kKeyQueueWord, 8, *index))
    return (*q & 0xff) != 0;
  const auto records = read_ptr(memory_, *mgr + kKeyRecords);
  if (!records) return std::nullopt;
  const auto v = read_u8(memory_, *records + kKeyRecordSize * *index + kKeyValue);
  if (!v) return std::nullopt;
  return *v != 0;
}

std::optional<bool> GameFlags::get_bool_array(std::uint32_t hash, std::uint32_t index) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto entry_index = find(memory_, *mgr, kBoolArray, hash);
  if (!entry_index) return std::nullopt;
  const auto entries = read_ptr(memory_, *mgr + kBoolArray.entries);
  if (!entries) return std::nullopt;
  const std::uint64_t entry = *entries + kBoolArray.stride * *entry_index;
  const auto size = read_u32(memory_, entry + kArraySize);
  const auto bits = read_ptr(memory_, entry + kArrayBits);
  if (!size || !bits || index >= *size) return std::nullopt;
  const auto word = read_u32(memory_, *bits + 4ull * (index / 32));
  if (!word) return std::nullopt;
  return ((*word >> (index % 32)) & 1) != 0;
}

bool GameFlags::set_bool(WritableGameMemory& m, std::uint32_t hash, bool value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kBool, hash)) return false;
  return enqueue(m, *mgr, kBool.queue_cap, kBool.queue_buf, kBool.queue_word, kBool.slot_size,
                 value ? 1 : 0, hash, true);
}

bool GameFlags::set_int(WritableGameMemory& m, std::uint32_t hash, std::int32_t value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kInt, hash)) return false;
  return enqueue(m, *mgr, kInt.queue_cap, kInt.queue_buf, kInt.queue_word, kInt.slot_size,
                 static_cast<std::uint32_t>(value), hash, false);
}

bool GameFlags::set_key64(WritableGameMemory& m, std::uint64_t key, bool value) {
  const auto mgr = manager(m);
  if (!mgr) return false;
  const auto index = find_key(m, *mgr, key);
  if (!index) return false;
  return enqueue(m, *mgr, kKeyQueueCap, kKeyQueueBuf, kKeyQueueWord, 8, value ? 1 : 0, *index,
                 true);
}

std::optional<protocol::FlagEntry> GameFlagStore::read(FlagKey key) {
  GameFlags flags(memory_);
  protocol::FlagEntry e{key, protocol::FlagKind::boolean, 0};
  if (key.kind == FlagKeyKind::key64) {
    if (key.table != 0) return std::nullopt;  // the game has one table
    const auto v = flags.get_key64(key.key);
    if (!v) return std::nullopt;
    e.value = *v ? 1 : 0;
    return e;
  }
  if (key.kind != FlagKeyKind::named) return std::nullopt;
  if (key.index > 0) {
    const auto v = flags.get_bool_array(key.hash, key.index);
    if (!v) return std::nullopt;
    e.value = *v ? 1 : 0;
    return e;
  }
  if (const auto b = flags.get_bool(key.hash)) {
    e.value = *b ? 1 : 0;
    return e;
  }
  if (const auto i = flags.get_int(key.hash)) {
    e.kind = protocol::FlagKind::int32;
    e.value = static_cast<std::uint32_t>(*i);
    return e;
  }
  return std::nullopt;
}

bool GameFlagStore::write(const protocol::FlagEntry& entry) {
  const FlagKey& key = entry.key;
  if (key.kind == FlagKeyKind::key64) {
    if (key.table != 0 || entry.kind != protocol::FlagKind::boolean || entry.value > 1) {
      return false;
    }
    return GameFlags::set_key64(memory_, key.key, entry.value != 0);
  }
  if (key.kind != FlagKeyKind::named || key.index != 0) return false;
  switch (entry.kind) {
    case protocol::FlagKind::boolean:
      if (entry.value > 1) return false;
      return GameFlags::set_bool(memory_, key.hash, entry.value != 0);
    case protocol::FlagKind::int32:
      return GameFlags::set_int(memory_, key.hash, static_cast<std::int32_t>(entry.value));
    default:
      return false;
  }
}

}  // namespace coop::host
