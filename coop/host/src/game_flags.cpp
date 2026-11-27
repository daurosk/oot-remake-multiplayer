#include "coop/host/game_flags.hpp"

#include "coop/host/game_flag_events.hpp"
#include "coop/host/game_items.hpp"
#include "coop/host/map_drop_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <span>

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
// UInt (type 20, M + 0x790): the Int layout 0x6c0 further on, as the getter
// 0x57a4e0 and the setter 0x57af30 read it (entries of 0x28, value at +0x1c,
// 12-byte queue slots {u32 value, u32 hash, u32 1}: unlike the Int setter,
// the UInt setter also stores 1 at +8, 0x57b018). Both fall back to a second
// table (slots at M + 0x800) for a hash not in this one; that is not read,
// so such a flag is not found here.
constexpr Table kUInt{0x790, 0x798, 0x7a0, 0x7ac, 0x28, 0x1c, 0x7b0, 0x7b8, 0x7c0, 12};
// String64 (type 16, M + 0x610): entries of 0x108, char* to the text at
// +0x68, s32 buffer size at +0x70 (0x3758110). Its write queue (M + 0x608 is
// what the push f_710376b9b0 takes): slot count at M + 0x630, slots of 0x58
// at M + 0x638 {char* text buffer of its own +0, s32 its size +8, u32 hash
// +0x50}, the word at M + 0x640. The applier (0xb99980) copies a slot's text
// into the entry's live text and its saved copy (+0xb8), as for a Bool.
// Enum (type 6, M + 0x250): entries of 0x38, value (the name's hash) at
// +0x1c, 8-byte queue slots {u32 value, u32 hash}, as the getter 0x7e73d0
// and the setter 0x3e0980 read them.
constexpr Table kEnum{0x250, 0x258, 0x260, 0x26c, 0x38, 0x1c, 0x270, 0x278, 0x280, 8};
// Float (type 4, M + 0x190): the Int layout 0xc0 further on, as the getter 0x607f70 and the setter 0xba6f80
// (f_7100ba6f80 in the recomp's main) read them: entries of 0x28, value (the float's bits) at +0x1c, 12-byte queue slots
// {f32 value, u32 hash}. The setter takes the value in s0 and the hash in w1 (the Int setter: value w1, hash w2).
constexpr Table kFloat{0x190, 0x198, 0x1a0, 0x1ac, 0x28, 0x1c, 0x1b0, 0x1b8, 0x1c0, 12};
constexpr Table kString64{0x610, 0x618, 0x620, 0x62c, 0x108, 0, 0, 0, 0, 0};
constexpr std::uint64_t kStringText = 0x68, kStringSize = 0x70;
constexpr std::uint64_t kStringQueueCap = 0x630, kStringQueueBuf = 0x638, kStringQueueWord = 0x640;
constexpr std::uint64_t kStringSlotSize = 0x58, kStringSlotText = 0, kStringSlotBytes = 8, kStringSlotHash = 0x50;
// The game's String64 buffers hold 64 bytes; more is not a String64.
constexpr std::uint32_t kMaxString64Bytes = 64;
// BoolArray: entries of 0x40, element count u32 at +0x20, bits at *(+0x28).
constexpr Table kBoolArray{0x70, 0x78, 0x80, 0x8c, 0x40, 0, 0, 0, 0, 0};
constexpr std::uint64_t kArraySize = 0x20, kArrayBits = 0x28;
// String64Array (type 17, M + 0x670: the type managers sit 0x60 apart from M + 0x10, save-format.md "Serializer";
// the serializer 0xba8100 walks it at 0xba92ac: entries of 0x90, file index at +0x14). Row 294: the parser 0xdfb0f4
// (via 0xdfb310) builds each entry's live array as u32 count at +0x70 and a pointer at +0x78 (saved copy +0x80/+0x88),
// elements of 0x50 bytes {char* text +0 (to the element's own buffer at +0xc), u32 capacity 0x40 at +8} (0xdfb400).
constexpr Table kString64Array{0x670, 0x678, 0x680, 0x68c, 0x90, 0, 0, 0, 0, 0};
constexpr std::uint64_t kStrArrayCount = 0x70, kStrArrayItems = 0x78;
constexpr std::uint64_t kStrItemSize = 0x50, kStrItemText = 0, kStrItemCap = 8;
// Row 294: the String64Array write queue, as the game's setter 0x3759470 (M, &{char* text}, hash, index) fills
// it and the applier 0xb99980 (at 0xb9a710) empties it into the live element: slot count M + 0x690, slots of 0x58
// at M + 0x698, word M + 0x6a0 (the other queues' word layout); a slot holds the text's buffer (+0, its size +8),
// the element index at +0x50 and the hash at +0x54 (the setter's `stp w3, w2, [slot, #0x50]`).
constexpr std::uint64_t kStrArrayQueueCap = 0x690, kStrArrayQueueBuf = 0x698, kStrArrayQueueWord = 0x6a0;
constexpr std::uint64_t kStrArraySlotIndex = 0x50, kStrArraySlotHash = 0x54;

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

// GameFlags::use_calls: the writes go to the game's own setters, not into
// its queues.
std::atomic<GameFlagCalls*> g_calls{nullptr};

// GameFlags::on_int_written.
std::atomic<GameFlags::IntWritten> g_int_written{nullptr};

// The value a write still waiting for the game's setter gives this flag.
std::optional<std::uint32_t> waiting(GameFlagType type, std::uint64_t id) {
  const GameFlagCalls* calls = g_calls.load(std::memory_order_acquire);
  return calls ? calls->pending(type, id) : std::nullopt;
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

// Half the queue stays free for the game. Its own setters (0x608a10, a Bool
// set an actor makes every frame) abort the game when they find the queue
// full (0x34fc280), and enqueue runs on another thread at any point of the
// frame. Issue 175: a whole join snapshot written at once filled it. What
// is refused for that is written again on a later frame (FlagWatcher,
// FlagStore::busy).
std::uint32_t ours(std::uint32_t cap) { return cap / 2; }

// The queue at `word_at` has no room for us now.
bool no_room(GameMemory& m, std::uint64_t mgr, std::uint64_t cap_at, std::uint64_t word_at) {
  const auto cap = read_u32(m, mgr + cap_at);
  const auto word = read_u32(m, mgr + word_at);
  return cap && word && (*word & kTakenMask) >= ours(*cap);
}

// A queue slot taken for one write: its address and the word it was taken
// with (finish_slot checks the round).
struct TakenSlot {
  std::uint64_t at = 0;
  std::uint32_t word = 0;
};

// Takes a slot as the game's setters do (0x682b20), within our half.
std::optional<TakenSlot> take_slot(WritableGameMemory& m, std::uint64_t mgr, std::uint64_t cap_at,
                                   std::uint64_t buf_at, std::uint64_t word_at, std::uint64_t slot_size) {
  const auto cap = read_u32(m, mgr + cap_at);
  const auto buf = read_ptr(m, mgr + buf_at);
  if (!cap || !buf) return std::nullopt;
  const std::uint64_t word_addr = mgr + word_at;
  for (int t = 0; t < kCasTries; ++t) {
    const auto w = read_u32(m, word_addr);
    if (!w || (*w & kTakenMask) >= ours(*cap)) return std::nullopt;
    if (m.compare_exchange_u32(word_addr, *w, *w + 1) == w) {
      return TakenSlot{*buf + slot_size * (*w & kTakenMask), *w};
    }
  }
  return std::nullopt;
}

// Marks a written slot finished, so the applier takes it.
bool finish_slot(WritableGameMemory& m, std::uint64_t mgr, std::uint64_t word_at, const TakenSlot& slot) {
  const std::uint64_t word_addr = mgr + word_at;
  for (int t = 0; t < kCasTries; ++t) {
    const auto w = read_u32(m, word_addr);
    if (!w) return false;
    // The applier ran since the slot was taken: the slot may be lost.
    if ((*w >> 28) != (slot.word >> 28)) return false;
    const std::uint32_t next = (*w & kFinishedMask) == kFinishedMask ? (*w & ~kFinishedMask)
                                                                     : *w + kFinishedOne;
    if (m.compare_exchange_u32(word_addr, *w, next) == w) return true;
  }
  return false;
}

// Takes a slot, writes it, marks it finished: the game's setter (0x682b20).
// `word8`: a u32 the setter also stores at slot + 8 (the UInt setter stores 1).
bool enqueue(WritableGameMemory& m, std::uint64_t mgr, std::uint64_t cap_at,
             std::uint64_t buf_at, std::uint64_t word_at, std::uint64_t slot_size,
             std::uint32_t value, std::uint32_t id, bool value_is_byte,
             std::optional<std::uint32_t> word8 = std::nullopt) {
  const auto slot = take_slot(m, mgr, cap_at, buf_at, word_at, slot_size);
  if (!slot) return false;
  const std::uint64_t at = slot->at;
  std::array<std::uint8_t, 4> id_bytes{}, value_bytes{};
  std::memcpy(id_bytes.data(), &id, 4);  // the guest is little-endian, as is every host we build for
  std::memcpy(value_bytes.data(), &value, 4);
  if (!m.write(at + 4, id_bytes)) return false;
  if (!m.write(at, std::span(value_bytes).first(value_is_byte ? 1 : 4))) return false;
  if (word8) {
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &*word8, 4);
    if (!m.write(at + 8, bytes)) return false;
  }
  return finish_slot(m, mgr, word_at, *slot);
}

// A String64 text we may write: at most 63 bytes of printable ASCII.
bool string64_text_ok(std::string_view text) {
  if (text.size() + 1 > kMaxString64Bytes) return false;
  for (const char ch : text) {
    if (ch < 0x20 || ch > 0x7e) return false;
  }
  return true;
}

// A String64 write into the game's queue, as its push f_710376b9b0 makes it:
// the hash at +0x50 and the text copied into the slot's own buffer (the push
// cuts a long text at the buffer's size - 1; here one that does not fit is
// refused).
bool enqueue_string(WritableGameMemory& m, std::uint64_t mgr, std::uint32_t hash, std::string_view text) {
  const auto slot = take_slot(m, mgr, kStringQueueCap, kStringQueueBuf, kStringQueueWord, kStringSlotSize);
  if (!slot) return false;
  const auto buffer = read_ptr(m, slot->at + kStringSlotText);
  const auto size = read_u32(m, slot->at + kStringSlotBytes);
  std::array<std::uint8_t, 4> hash_bytes{};
  std::memcpy(hash_bytes.data(), &hash, 4);
  std::array<std::uint8_t, kMaxString64Bytes> bytes{};
  std::copy(text.begin(), text.end(), bytes.begin());
  // A taken slot is finished even when it could not be filled (the applier
  // stops at the first unfinished one); its hash stays 0, a flag it skips.
  const bool fits = buffer && size && *size >= text.size() + 1;
  const bool wrote = fits &&
                     m.write(*buffer, std::span<const std::uint8_t>(bytes.data(), text.size() + 1)) &&
                     m.write(slot->at + kStringSlotHash, hash_bytes);
  return finish_slot(m, mgr, kStringQueueWord, *slot) && wrote;
}

// The newest text queued for `hash` in the game's String64 queue.
std::optional<std::string> queued_string(GameMemory& m, std::uint64_t mgr, std::uint32_t hash) {
  const auto cap = read_u32(m, mgr + kStringQueueCap);
  const auto buf = read_ptr(m, mgr + kStringQueueBuf);
  const auto word = read_u32(m, mgr + kStringQueueWord);
  if (!cap || !buf || !word) return std::nullopt;
  std::uint32_t taken = *word & kTakenMask;
  if (taken > *cap) taken = *cap;
  for (std::uint32_t i = taken; i-- > 0;) {
    const std::uint64_t at = *buf + kStringSlotSize * i;
    if (read_u32(m, at + kStringSlotHash) != hash) continue;
    const auto text = read_ptr(m, at + kStringSlotText);
    const auto size = read_u32(m, at + kStringSlotBytes);
    if (!text || !size || *size == 0) return std::nullopt;
    const std::size_t n = std::min<std::size_t>(*size, kMaxString64Bytes);
    std::array<std::uint8_t, kMaxString64Bytes> bytes{};
    if (!m.read(*text, std::span(bytes.data(), n))) return std::nullopt;
    std::string out;
    for (std::size_t k = 0; k < n && bytes[k] != 0; ++k) out.push_back(static_cast<char>(bytes[k]));
    if (!string64_text_ok(out)) return std::nullopt;
    return out;
  }
  return std::nullopt;
}

// Row 294: a String64Array element write into the game's queue, as its setter 0x3759470 makes it (above).
bool enqueue_string_array(WritableGameMemory& m, std::uint64_t mgr, std::uint32_t hash, std::uint32_t index,
                          std::string_view text) {
  const auto slot = take_slot(m, mgr, kStrArrayQueueCap, kStrArrayQueueBuf, kStrArrayQueueWord, kStringSlotSize);
  if (!slot) return false;
  const auto buffer = read_ptr(m, slot->at + kStringSlotText);
  const auto size = read_u32(m, slot->at + kStringSlotBytes);
  std::array<std::uint8_t, 8> where{};
  std::memcpy(where.data(), &index, 4);
  std::memcpy(where.data() + 4, &hash, 4);
  std::array<std::uint8_t, kMaxString64Bytes> bytes{};
  std::copy(text.begin(), text.end(), bytes.begin());
  // A taken slot is finished even when it could not be filled; its hash stays 0, which the applier stops at
  // (as enqueue_string).
  const bool fits = buffer && size && *size >= text.size() + 1;
  const bool wrote = fits &&
                     m.write(*buffer, std::span<const std::uint8_t>(bytes.data(), text.size() + 1)) &&
                     m.write(slot->at + kStrArraySlotIndex, where);
  return finish_slot(m, mgr, kStrArrayQueueWord, *slot) && wrote;
}

// The newest text queued for element `index` of `hash` in the game's String64Array queue.
std::optional<std::string> queued_string_array(GameMemory& m, std::uint64_t mgr, std::uint32_t hash,
                                               std::uint32_t index) {
  const auto cap = read_u32(m, mgr + kStrArrayQueueCap);
  const auto buf = read_ptr(m, mgr + kStrArrayQueueBuf);
  const auto word = read_u32(m, mgr + kStrArrayQueueWord);
  if (!cap || !buf || !word) return std::nullopt;
  const std::uint32_t taken = std::min(*word & kTakenMask, *cap);
  for (std::uint32_t i = taken; i-- > 0;) {
    const std::uint64_t at = *buf + kStringSlotSize * i;
    if (read_u32(m, at + kStrArraySlotHash) != hash || read_u32(m, at + kStrArraySlotIndex) != index) continue;
    const auto text = read_ptr(m, at + kStringSlotText);
    const auto size = read_u32(m, at + kStringSlotBytes);
    if (!text || !size) return std::nullopt;
    const std::size_t n = std::min<std::size_t>(*size, kMaxString64Bytes);
    std::array<std::uint8_t, kMaxString64Bytes> bytes{};
    if (!m.read(*text, std::span(bytes.data(), n))) return std::nullopt;
    std::string out;
    for (std::size_t k = 0; k < n && bytes[k] != 0; ++k) out.push_back(static_cast<char>(bytes[k]));
    if (!string64_text_ok(out)) return std::nullopt;
    return out;
  }
  return std::nullopt;
}

}  // namespace

std::optional<bool> GameFlags::get_bool(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kBool, hash);
  if (!index) return std::nullopt;
  if (const auto w = waiting(GameFlagType::boolean, hash)) return (*w & 0xff) != 0;
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
  if (const auto w = waiting(GameFlagType::int32, hash)) return static_cast<std::int32_t>(*w);
  if (const auto q = queued(memory_, *mgr, kInt.queue_cap, kInt.queue_buf, kInt.queue_word,
                            kInt.slot_size, hash))
    return static_cast<std::int32_t>(*q);
  const auto entries = read_ptr(memory_, *mgr + kInt.entries);
  if (!entries) return std::nullopt;
  const auto v = read_u32(memory_, *entries + kInt.stride * *index + kInt.value);
  if (!v) return std::nullopt;
  return static_cast<std::int32_t>(*v);
}

std::optional<std::int32_t> GameFlags::get_int_live(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kInt, hash);
  if (!index) return std::nullopt;
  const auto entries = read_ptr(memory_, *mgr + kInt.entries);
  if (!entries) return std::nullopt;
  const auto v = read_u32(memory_, *entries + kInt.stride * *index + kInt.value);
  if (!v) return std::nullopt;
  return static_cast<std::int32_t>(*v);
}

std::optional<float> GameFlags::get_float(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kFloat, hash);
  if (!index) return std::nullopt;
  if (const auto w = waiting(GameFlagType::float32, hash)) return std::bit_cast<float>(*w);
  if (const auto q = queued(memory_, *mgr, kFloat.queue_cap, kFloat.queue_buf, kFloat.queue_word,
                            kFloat.slot_size, hash))
    return std::bit_cast<float>(*q);
  const auto entries = read_ptr(memory_, *mgr + kFloat.entries);
  if (!entries) return std::nullopt;
  const auto v = read_u32(memory_, *entries + kFloat.stride * *index + kFloat.value);
  if (!v) return std::nullopt;
  return std::bit_cast<float>(*v);
}

std::optional<std::uint32_t> GameFlags::get_uint(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kUInt, hash);
  if (!index) return std::nullopt;
  if (const auto w = waiting(GameFlagType::uint32, hash)) return *w;
  if (const auto q = queued(memory_, *mgr, kUInt.queue_cap, kUInt.queue_buf, kUInt.queue_word,
                            kUInt.slot_size, hash))
    return *q;
  const auto entries = read_ptr(memory_, *mgr + kUInt.entries);
  if (!entries) return std::nullopt;
  return read_u32(memory_, *entries + kUInt.stride * *index + kUInt.value);
}

std::optional<std::uint32_t> GameFlags::get_enum(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find(memory_, *mgr, kEnum, hash);
  if (!index) return std::nullopt;
  if (const auto w = waiting(GameFlagType::enum32, hash)) return *w;
  if (const auto q = queued(memory_, *mgr, kEnum.queue_cap, kEnum.queue_buf, kEnum.queue_word,
                            kEnum.slot_size, hash))
    return *q;
  const auto entries = read_ptr(memory_, *mgr + kEnum.entries);
  if (!entries) return std::nullopt;
  return read_u32(memory_, *entries + kEnum.stride * *index + kEnum.value);
}

std::optional<bool> GameFlags::get_key64(std::uint64_t key) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto index = find_key(memory_, *mgr, key);
  if (!index) return std::nullopt;
  if (const auto w = waiting(GameFlagType::key64, key)) return (*w & 0xff) != 0;
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

std::optional<std::string> GameFlags::get_string64_array(std::uint32_t hash, std::uint32_t index) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto entry_index = find(memory_, *mgr, kString64Array, hash);
  if (!entry_index) return std::nullopt;
  const auto entries = read_ptr(memory_, *mgr + kString64Array.entries);
  if (!entries) return std::nullopt;
  const std::uint64_t entry = *entries + kString64Array.stride * *entry_index;
  const auto count = read_u32(memory_, entry + kStrArrayCount);
  const auto items = read_ptr(memory_, entry + kStrArrayItems);
  if (!count || !items || index >= *count) return std::nullopt;
  if (auto text = queued_string_array(memory_, *mgr, hash, index)) return text;  // ours, not applied yet
  const std::uint64_t item = *items + kStrItemSize * index;
  const auto text = read_ptr(memory_, item + kStrItemText);
  const auto cap = read_u32(memory_, item + kStrItemCap);
  if (!text || !cap || *cap == 0 || *cap > kMaxString64Bytes) return std::nullopt;
  std::array<std::uint8_t, kMaxString64Bytes> bytes{};
  if (!memory_.read(*text, std::span(bytes.data(), *cap))) return std::nullopt;
  std::string out;
  for (std::uint32_t i = 0; i < *cap; ++i) {
    if (bytes[i] == 0) return out;
    if (bytes[i] < 0x20 || bytes[i] > 0x7e) return std::nullopt;
    out.push_back(static_cast<char>(bytes[i]));
  }
  return std::nullopt;  // not terminated within the buffer
}

std::optional<std::string> GameFlags::get_string64(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto entry_index = find(memory_, *mgr, kString64, hash);
  if (!entry_index) return std::nullopt;
  // A write still waiting for the game's setter, then one in its queue, as the other getters.
  if (const GameFlagCalls* calls = g_calls.load(std::memory_order_acquire)) {
    if (auto text = calls->pending_string(hash)) return text;
  }
  if (auto text = queued_string(memory_, *mgr, hash)) return text;
  const auto entries = read_ptr(memory_, *mgr + kString64.entries);
  if (!entries) return std::nullopt;
  const std::uint64_t entry = *entries + kString64.stride * *entry_index;
  const auto text = read_ptr(memory_, entry + kStringText);
  const auto size = read_u32(memory_, entry + kStringSize);
  // The game ends the text at size - 1 itself; a size of 0 or a huge one
  // is not a String64 buffer.
  if (!text || !size || *size == 0 || *size > kMaxString64Bytes) return std::nullopt;
  std::array<std::uint8_t, kMaxString64Bytes> bytes{};
  if (!memory_.read(*text, std::span(bytes.data(), *size))) return std::nullopt;
  std::string out;
  for (std::uint32_t i = 0; i < *size; ++i) {
    if (bytes[i] == 0) return out;
    if (bytes[i] < 0x20 || bytes[i] > 0x7e) return std::nullopt;
    out.push_back(static_cast<char>(bytes[i]));
  }
  return std::nullopt;  // not terminated within the buffer
}

bool GameFlags::set_string64(WritableGameMemory& m, std::uint32_t hash, std::string_view text) {
  if (!string64_text_ok(text)) return false;
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kString64, hash)) return false;
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire)) return calls->put_string(hash, text);
  return enqueue_string(m, *mgr, hash, text);
}

bool GameFlags::set_string64_array(WritableGameMemory& m, std::uint32_t hash, std::uint32_t index,
                                   std::string_view text) {
  if (!string64_text_ok(text)) return false;
  const auto mgr = manager(m);
  if (!mgr) return false;
  const auto entry_index = find(m, *mgr, kString64Array, hash);
  if (!entry_index) return false;
  const auto entries = read_ptr(m, *mgr + kString64Array.entries);
  if (!entries) return false;
  // The setter's own bound: the element must be below the live count (entry +0x70).
  const auto count = read_u32(m, *entries + kString64Array.stride * *entry_index + kStrArrayCount);
  if (!count || index >= *count) return false;
  return enqueue_string_array(m, *mgr, hash, index, text);
}

bool GameFlags::set_string64_now(WritableGameMemory& m, std::uint32_t hash,
                                 std::string_view text) {
  if (!string64_text_ok(text)) return false;
  const auto mgr = manager(m);
  if (!mgr) return false;
  const auto index = find(m, *mgr, kString64, hash);
  if (!index) return false;
  const auto entries = read_ptr(m, *mgr + kString64.entries);
  if (!entries) return false;
  const std::uint64_t entry = *entries + kString64.stride * *index;
  const auto buffer = read_ptr(m, entry + kStringText);
  const auto size = read_u32(m, entry + kStringSize);
  if (!buffer || !size || *size < text.size() + 1 || *size > kMaxString64Bytes) return false;
  std::array<std::uint8_t, kMaxString64Bytes> bytes{};
  std::copy(text.begin(), text.end(), bytes.begin());
  return m.write(*buffer, std::span<const std::uint8_t>(bytes.data(), text.size() + 1));
}

bool GameFlags::set_int_now(WritableGameMemory& m, std::uint32_t hash, std::int32_t value) {
  const auto mgr = manager(m);
  if (!mgr) return false;
  const auto index = find(m, *mgr, kInt, hash);
  if (!index) return false;
  const auto entries = read_ptr(m, *mgr + kInt.entries);
  if (!entries) return false;
  const auto v = static_cast<std::uint32_t>(value);
  const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                                          static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
  return m.write(*entries + kInt.stride * *index + kInt.value, std::span<const std::uint8_t>(bytes.data(), 4));
}

std::optional<GameFlags::QueueSizes> GameFlags::queue_sizes() {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  const auto b = read_u32(memory_, *mgr + kBool.queue_cap);
  const auto i = read_u32(memory_, *mgr + kInt.queue_cap);
  const auto u = read_u32(memory_, *mgr + kUInt.queue_cap);
  const auto e = read_u32(memory_, *mgr + kEnum.queue_cap);
  const auto k = read_u32(memory_, *mgr + kKeyQueueCap);
  if (!b || !i || !u || !e || !k || *b == 0) return std::nullopt;  // 0: the tables are not made yet
  return QueueSizes{*b, *i, *u, *e, *k};
}

void GameFlags::use_calls(GameFlagCalls* calls) { g_calls.store(calls, std::memory_order_release); }
void GameFlags::on_int_written(IntWritten callback) {
  g_int_written.store(callback, std::memory_order_release);
}

bool GameFlags::setter_has_room(GameMemory& m, GameFlagType type) {
  const auto mgr = manager(m);
  if (!mgr) return false;
  switch (type) {
    case GameFlagType::boolean: return !no_room(m, *mgr, kBool.queue_cap, kBool.queue_word);
    case GameFlagType::int32: return !no_room(m, *mgr, kInt.queue_cap, kInt.queue_word);
    case GameFlagType::float32: return !no_room(m, *mgr, kFloat.queue_cap, kFloat.queue_word);
    case GameFlagType::enum32: return !no_room(m, *mgr, kEnum.queue_cap, kEnum.queue_word);
    case GameFlagType::uint32: return !no_room(m, *mgr, kUInt.queue_cap, kUInt.queue_word);
    case GameFlagType::string64: return !no_room(m, *mgr, kStringQueueCap, kStringQueueWord);
    case GameFlagType::key64: return !no_room(m, *mgr, kKeyQueueCap, kKeyQueueWord);
  }
  return false;
}

std::optional<std::uint64_t> GameFlags::manager_address(GameMemory& m) { return manager(m); }

std::size_t queued_flag_writes(GameMemory& m, std::uint64_t mgr,
                               const std::function<void(const GameFlagWrite&)>& sink) {
  if (mgr == 0) {
    const auto found = manager(m);
    if (!found) return 0;
    mgr = *found;
  }
  std::size_t n = 0;
  // One queue: the slots taken so far, as the applier is about to walk them.
  const auto walk = [&](std::uint64_t cap_at, std::uint64_t buf_at, std::uint64_t word_at,
                        std::uint64_t slot_size, const auto& each) {
    const auto cap = read_u32(m, mgr + cap_at);
    const auto word = read_u32(m, mgr + word_at);
    if (!cap || !word) return;
    const std::uint32_t taken = (std::min)(*word & kTakenMask, *cap);
    if (taken == 0) return;
    const auto buf = read_ptr(m, mgr + buf_at);
    if (!buf) return;
    for (std::uint32_t i = 0; i < taken; ++i) {
      const auto value = read_u32(m, *buf + slot_size * i);
      const auto id = read_u32(m, *buf + slot_size * i + 4);
      if (value && id) each(*value, *id);
    }
  };
  const auto named = [&](const Table& t, GameFlagType type, bool byte) {
    walk(t.queue_cap, t.queue_buf, t.queue_word, t.slot_size, [&](std::uint32_t value, std::uint32_t hash) {
      if (hash == 0) return;  // an empty slot
      sink(GameFlagWrite{type, hash, byte ? ((value & 0xff) != 0 ? 1u : 0u) : value});
      ++n;
    });
  };
  named(kBool, GameFlagType::boolean, true);
  named(kInt, GameFlagType::int32, false);
  named(kEnum, GameFlagType::enum32, false);
  named(kUInt, GameFlagType::uint32, false);
  const auto count = read_u32(m, mgr + kKeyCount);
  const auto records = read_ptr(m, mgr + kKeyRecords);
  if (count && records) {
    walk(kKeyQueueCap, kKeyQueueBuf, kKeyQueueWord, 8, [&](std::uint32_t value, std::uint32_t index) {
      if (index >= *count) return;  // 0xffffffff: an empty slot
      const auto key = read_u64(m, *records + kKeyRecordSize * index);
      if (!key || *key == 0) return;
      sink(GameFlagWrite{GameFlagType::key64, *key, (value & 0xff) != 0 ? 1u : 0u});
      ++n;
    });
  }
  return n;
}

std::optional<int> GameFlags::find_type(std::uint32_t hash) {
  const auto mgr = manager(memory_);
  if (!mgr) return std::nullopt;
  for (int type = 0; type <= 25; ++type) {
    const std::uint64_t base = 0x10 + 0x60ull * static_cast<std::uint64_t>(type);
    const Table t{base, base + 8, base + 0x10, base + 0x1c, 0, 0, 0, 0, 0, 0};
    if (find(memory_, *mgr, t, hash)) return type;
  }
  return std::nullopt;
}

bool GameFlags::set_bool(WritableGameMemory& m, std::uint32_t hash, bool value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kBool, hash)) return false;
  bool ok = false;
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire)) {
    ok = calls->put({GameFlagType::boolean, hash, value ? 1u : 0u});
  } else {
    ok = enqueue(m, *mgr, kBool.queue_cap, kBool.queue_buf, kBool.queue_word, kBool.slot_size, value ? 1 : 0, hash,
                 true);
  }
  // (minimap-b: a map or compass written by us is in the drop log's line)
  if (ok) map_drop_log().note_our_write(FlagKey{hash, 0}, value ? 1u : 0u, MapDropBecause::now());
  return ok;
}

bool GameFlags::set_int(WritableGameMemory& m, std::uint32_t hash, std::int32_t value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kInt, hash)) return false;
  bool queued = false;
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire))
    queued = calls->put({GameFlagType::int32, hash, static_cast<std::uint32_t>(value)});
  else
    queued = enqueue(m, *mgr, kInt.queue_cap, kInt.queue_buf, kInt.queue_word, kInt.slot_size,
                     static_cast<std::uint32_t>(value), hash, false);
  if (queued) {
    if (const IntWritten written = g_int_written.load(std::memory_order_acquire)) written(hash, value);
  }
  return queued;
}

bool GameFlags::set_float(WritableGameMemory& m, std::uint32_t hash, float value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kFloat, hash)) return false;
  const auto bits = std::bit_cast<std::uint32_t>(value);
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire))
    return calls->put({GameFlagType::float32, hash, bits});
  return enqueue(m, *mgr, kFloat.queue_cap, kFloat.queue_buf, kFloat.queue_word, kFloat.slot_size, bits, hash,
                 false);
}

bool GameFlags::set_uint(WritableGameMemory& m, std::uint32_t hash, std::uint32_t value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kUInt, hash)) return false;
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire))
    return calls->put({GameFlagType::uint32, hash, value});
  return enqueue(m, *mgr, kUInt.queue_cap, kUInt.queue_buf, kUInt.queue_word, kUInt.slot_size,
                 value, hash, false, 1u);
}

bool GameFlags::set_enum(WritableGameMemory& m, std::uint32_t hash, std::uint32_t value) {
  const auto mgr = manager(m);
  if (!mgr || !find(m, *mgr, kEnum, hash)) return false;
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire))
    return calls->put({GameFlagType::enum32, hash, value});
  return enqueue(m, *mgr, kEnum.queue_cap, kEnum.queue_buf, kEnum.queue_word, kEnum.slot_size,
                 value, hash, false);
}

bool GameFlags::set_key64(WritableGameMemory& m, std::uint64_t key, bool value) {
  const auto mgr = manager(m);
  if (!mgr) return false;
  const auto index = find_key(m, *mgr, key);
  if (!index) return false;
  if (GameFlagCalls* calls = g_calls.load(std::memory_order_acquire))
    return calls->put({GameFlagType::key64, key, value ? 1u : 0u});
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
  // A shared UInt item only (a dungeon's floors on its map, issue 146): any
  // other UInt stays unread, so no other shared list starts carrying one.
  if (is_shared_uint_item(key.hash)) {
    if (const auto u = flags.get_uint(key.hash)) {
      e.kind = protocol::FlagKind::uint32;
      e.value = *u;
      return e;
    }
  }
  return std::nullopt;
}

bool GameFlagStore::busy(FlagKey key) {
  // With the game's setters (use_calls) nothing of ours goes into a queue
  // here: the writes wait in our own list, and the calc thread hands them
  // over as the queues have room.
  if (const GameFlagCalls* calls = g_calls.load(std::memory_order_acquire)) return calls->full();
  const auto mgr = manager(memory_);
  if (!mgr) return false;
  if (key.kind == FlagKeyKind::key64) return no_room(memory_, *mgr, kKeyQueueCap, kKeyQueueWord);
  if (key.kind != FlagKeyKind::named || key.index != 0) return false;
  if (find(memory_, *mgr, kBool, key.hash)) return no_room(memory_, *mgr, kBool.queue_cap, kBool.queue_word);
  if (find(memory_, *mgr, kInt, key.hash)) return no_room(memory_, *mgr, kInt.queue_cap, kInt.queue_word);
  if (is_shared_uint_item(key.hash) && find(memory_, *mgr, kUInt, key.hash))
    return no_room(memory_, *mgr, kUInt.queue_cap, kUInt.queue_word);
  return false;
}

bool GameFlagStore::write(const protocol::FlagEntry& entry) {
  const FlagKey& key = entry.key;
  // A value the game already holds (or has queued) takes no queue slot: a join
  // snapshot is mostly such values (issue 175). Only for what this store
  // writes: a BoolArray element is refused below whatever it holds.
  const bool writable = key.kind == FlagKeyKind::key64 ? key.table == 0
                                                       : key.kind == FlagKeyKind::named && key.index == 0;
  if (const auto now = writable ? read(key) : std::nullopt;
      now && now->kind == entry.kind && now->value == entry.value) {
    return true;
  }
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
    case protocol::FlagKind::uint32:
      // Through the game's UInt setter (0x57af30), as read above: shared UInt items only.
      return is_shared_uint_item(key.hash) && GameFlags::set_uint(memory_, key.hash, entry.value);
    default:
      return false;
  }
}

}  // namespace coop::host
