// Shared world flags in the emulator (coop_flags.h). Table layout:
// docs/flags.md.

#include "coop_flags.h"

#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "common/logging.h"
#include "core/memory.h"
#include "coop/host/game_items.hpp"

namespace CoopHook {
namespace {

// docs/flags.md: the manager pointer and the tables the debug scan reads.
constexpr u64 kManagerPtr = 0x4e842a8;
constexpr u64 kBoolTable = 0x10;
constexpr u64 kIntTable = 0xd0;
constexpr u64 kTableCount = 0x00, kTableEntries = 0x08, kTableSlots = 0x10,
              kTableSlotCount = 0x1c;
constexpr u64 kKeyCount = 0xc58, kKeyRecords = 0xc60, kKeyRecordSize = 0x10, kKeyValue = 0xc;

// Tables larger than this are taken as a misread, not scanned.
constexpr u32 kMaxEntries = 1u << 20;
// How many changes one debug scan logs (the scan runs every CoopEvery::kQuarterUs).
constexpr int kMaxLogged = 40;

std::optional<std::string> Env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out = value;
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    std::string out = value;
#endif
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

// A hex number, with or without 0x. Nothing on anything else.
std::optional<std::uint64_t> Hex(const char* name) {
    const auto text = Env(name);
    if (!text) {
        return std::nullopt;
    }
    std::string_view s = *text;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    if (s.empty() || s.size() > 16) {
        LOG_ERROR(Core, "coop: {} is not a hex number: {}", name, *text);
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            LOG_ERROR(Core, "coop: {} is not a hex number: {}", name, *text);
            return std::nullopt;
        }
        v = v << 4 | static_cast<std::uint64_t>(d);
    }
    return v;
}

std::string Value(const std::optional<bool>& v) {
    return v ? (*v ? "1" : "0") : "none";
}

} // namespace

bool EmulatorMemory::Valid(std::uint64_t address, std::size_t size) const {
    return address + size >= address && memory_.IsValidVirtualAddressRange(address, size);
}

bool EmulatorMemory::read(std::uint64_t address, std::span<std::uint8_t> out) {
    if (out.empty()) {
        return true;
    }
    if (!Valid(address, out.size())) {
        return false;
    }
    // The unsafe read skips GPU flushing: the player state and the flags are
    // CPU data, and the present thread must not wait on the GPU.
    return memory_.ReadBlockUnsafe(address, out.data(), out.size());
}

bool EmulatorMemory::write(std::uint64_t address, std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return true;
    }
    if (!Valid(address, data.size())) {
        return false;
    }
    return memory_.WriteBlockUnsafe(address, data.data(), data.size());
}

std::optional<std::uint32_t> EmulatorMemory::compare_exchange_u32(std::uint64_t address,
                                                                  std::uint32_t expected,
                                                                  std::uint32_t desired) {
    if ((address & 3) != 0 || !Valid(address, 4)) {
        return std::nullopt;
    }
    // WriteExclusive32 only says whether it stored. On a miss, the value read
    // after it is what was there; if a guest thread put `expected` back in
    // between, try again so a miss is never reported as a success.
    for (int i = 0; i < 16; ++i) {
        if (memory_.WriteExclusive32(address, desired, expected)) {
            return expected;
        }
        std::uint32_t now = 0;
        if (!memory_.ReadBlockUnsafe(address, &now, sizeof(now))) {
            return std::nullopt;
        }
        if (now != expected) {
            return now;
        }
    }
    return std::nullopt;
}

std::optional<coop::host::SharedFlags> LoadShared(std::string& error) {
    // The player's items (docs/items.md), unless LICHII_COOP_ITEMS=0.
    const auto items_env = Env("LICHII_COOP_ITEMS");
    const bool items = !items_env || *items_env != "0";
    std::vector<coop::host::FlagKey> keys;
    if (items) {
        keys = coop::host::item_shared_flags().keys();
        LOG_INFO(Core, "coop: sharing {} item flags", keys.size());
    }
    if (const auto path = Env("LICHII_COOP_FLAGS")) {
        std::string why;
        const auto file = coop::host::SharedFlags::load(*path, &why);
        if (!file) {
            error = "LICHII_COOP_FLAGS " + *path + ": " + why;
            return std::nullopt;
        }
        LOG_INFO(Core, "coop: sharing {} flags from {}", file->size(), *path);
        keys.insert(keys.end(), file->keys().begin(), file->keys().end());
    } else if (!items) {
        return std::nullopt;
    }
    return coop::host::SharedFlags(std::move(keys));
}

FlagDebug::FlagDebug()
    : watch_key_(Hex("LICHII_COOP_DEBUG_KEY64")), set_key_(Hex("LICHII_COOP_SET_KEY64")) {
    const auto debug = Env("LICHII_COOP_DEBUG_FLAGS");
    on_ = debug && *debug != "0";
    if (const auto b = Hex("LICHII_COOP_SET_BOOL"); b && *b != 0 && *b <= 0xffffffffu) {
        set_bool_ = static_cast<std::uint32_t>(*b);
    }
    if (const auto b = Hex("LICHII_COOP_DEBUG_BOOL"); b && *b != 0 && *b <= 0xffffffffu) {
        watch_bool_ = static_cast<std::uint32_t>(*b);
    }
    if (const auto after = Env("LICHII_COOP_SET_AFTER")) {
        set_after_ = std::chrono::seconds(std::strtoul(after->c_str(), nullptr, 10));
    }
    if (on_ || watch_key_ || watch_bool_ || set_key_ || set_bool_) {
        LOG_INFO(Core, "coop: flags debug {}{}{}{}{}{}", on_ ? "on" : "off",
                 watch_key_ ? fmt::format(", watching key {:#x}", *watch_key_) : "",
                 watch_bool_ ? fmt::format(", watching bool {:#010x}", *watch_bool_) : "",
                 set_after_.count() > 0 ? fmt::format(", setting after {} s", set_after_.count())
                                        : "",
                 set_key_ ? fmt::format(", will set key {:#x}", *set_key_) : "",
                 set_bool_ ? fmt::format(", will set bool {:#010x}", *set_bool_) : "");
    }
}

void FlagDebug::Reset() {
    bools_ = {};
    ints_ = {};
    keys_ = {};
    key_set_ = false;
    bool_set_ = false;
    watch_last_.reset();
    watch_bool_last_.reset();
    up_at_.reset();
    scan_.Reset();
    relog_.Reset();
}

void FlagDebug::Frame(EmulatorMemory& memory) {
    if (!on_ && !watch_key_ && !watch_bool_ && !set_key_ && !set_bool_) {
        return;
    }
    const auto now_us = CoopEvery::NowUs();
    if (!scan_.Due(now_us)) {
        return;
    }
    const bool relog = relog_.Due(now_us);  // a watched value is logged every 5 s
    coop::host::GameFlags flags(memory);
    if (watch_key_) {
        const auto now = flags.get_key64(*watch_key_);
        if (!watch_last_ || *watch_last_ != now || relog) {
            LOG_INFO(Core, "coop: flags debug: get_key64({:#x}) = {}{}", *watch_key_, Value(now),
                     watch_last_ && *watch_last_ != now
                         ? " (was " + Value(*watch_last_) + ")"
                         : "");
            watch_last_ = now;
        }
    }
    if (watch_bool_) {
        const auto now = flags.get_bool(*watch_bool_);
        if (!watch_bool_last_ || *watch_bool_last_ != now || relog) {
            LOG_INFO(Core, "coop: flags debug: get_bool({:#010x}) = {}{}", *watch_bool_,
                     Value(now),
                     watch_bool_last_ && *watch_bool_last_ != now
                         ? " (was " + Value(*watch_bool_last_) + ")"
                         : "");
            watch_bool_last_ = now;
        }
    }
    Sets(memory);
    if (on_) {
        Scan(memory);
    }
}

void FlagDebug::Sets(EmulatorMemory& memory) {
    if ((!set_key_ || key_set_) && (!set_bool_ || bool_set_)) {
        return;
    }
    // The game is up once the flag manager reads; LICHII_COOP_SET_AFTER
    // waits from then.
    if (!coop::host::read_ptr(memory, memory.main_base() + kManagerPtr)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!up_at_) {
        up_at_ = now;
    }
    if (now - *up_at_ < set_after_) {
        return;
    }
    coop::host::GameFlags flags(memory);
    if (set_key_ && !key_set_) {
        const auto before = flags.get_key64(*set_key_);
        // Wait for the manager and the key: the first frames run before the
        // GameData module is up.
        if (before) {
            key_set_ = coop::host::GameFlags::set_key64(memory, *set_key_, true);
            LOG_INFO(Core, "coop: flags debug: set_key64({:#x}, 1) {} (before {}, now {})",
                     *set_key_, key_set_ ? "queued" : "failed, trying again", Value(before),
                     Value(flags.get_key64(*set_key_)));
        }
    }
    if (set_bool_ && !bool_set_) {
        const auto before = flags.get_bool(*set_bool_);
        if (before) {
            bool_set_ = coop::host::GameFlags::set_bool(memory, *set_bool_, true);
            LOG_INFO(Core, "coop: flags debug: set_bool({:#010x}, 1) {} (before {}, now {})",
                     *set_bool_, bool_set_ ? "queued" : "failed, trying again", Value(before),
                     Value(flags.get_bool(*set_bool_)));
        }
    }
}

void FlagDebug::Scan(EmulatorMemory& memory) {
    const auto manager = coop::host::read_ptr(memory, memory.main_base() + kManagerPtr);
    if (!manager) {
        return;
    }
    ScanNamed(memory, *manager, "bool", kBoolTable, 0x18, 0x16, 1, bools_);
    ScanNamed(memory, *manager, "int", kIntTable, 0x28, 0x1c, 4, ints_);
    ScanKeys(memory, *manager);
}

// A named table: the values by entry index, and the hash slots to name them.
void FlagDebug::ScanNamed(EmulatorMemory& memory, u64 manager, const char* name, u64 table,
                          std::size_t entry_size, std::size_t value_offset,
                          std::size_t value_size, Table& last) {
    const u64 t = manager + table;
    const auto count = coop::host::read_u32(memory, t + kTableCount);
    const auto entries = coop::host::read_ptr(memory, t + kTableEntries);
    if (!count || !entries || *count == 0 || *count > kMaxEntries) {
        return;
    }
    std::vector<std::uint8_t> raw(std::size_t(*count) * entry_size);
    if (!memory.read(*entries, raw)) {
        return;
    }
    std::vector<std::uint8_t> values(std::size_t(*count) * value_size);
    for (std::size_t i = 0; i < *count; ++i) {
        std::memcpy(&values[i * value_size], &raw[i * entry_size + value_offset], value_size);
    }
    if (!last.known || last.bytes.size() != values.size()) {
        LOG_INFO(Core, "coop: flags debug: {} table, {} entries", name, *count);
        last.known = true;
        last.bytes = std::move(values);
        return;
    }
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < *count; ++i) {
        if (std::memcmp(&values[i * value_size], &last.bytes[i * value_size], value_size) != 0) {
            changed.push_back(i);
        }
    }
    if (!changed.empty()) {
        // Name the entries through the hash slots: {u32 hash, u32 index}.
        std::unordered_map<std::uint32_t, std::uint32_t> hash_of;
        const auto slots = coop::host::read_ptr(memory, t + kTableSlots);
        const auto slot_count = coop::host::read_u32(memory, t + kTableSlotCount);
        if (slots && slot_count && *slot_count <= kMaxEntries * 4) {
            std::vector<std::uint8_t> s(std::size_t(*slot_count) * 8);
            if (memory.read(*slots, s)) {
                for (std::size_t i = 0; i < *slot_count; ++i) {
                    std::uint32_t h, idx;
                    std::memcpy(&h, &s[i * 8], 4);
                    std::memcpy(&idx, &s[i * 8 + 4], 4);
                    if (h != 0) {
                        hash_of[idx] = h;
                    }
                }
            }
        }
        int logged = 0;
        for (const auto i : changed) {
            if (logged++ == kMaxLogged) {
                LOG_INFO(Core, "coop: flags debug: {} more {} changes", changed.size() - kMaxLogged,
                         name);
                break;
            }
            std::uint32_t was = 0, now = 0;
            std::memcpy(&was, &last.bytes[i * value_size], value_size);
            std::memcpy(&now, &values[i * value_size], value_size);
            const auto h = hash_of.find(static_cast<std::uint32_t>(i));
            const std::uint32_t hash = h == hash_of.end() ? 0u : h->second;
            const auto* item = coop::host::find_item(hash);
            LOG_INFO(Core, "coop: flags debug: {} {:#010x} (entry {}) {} -> {}{}{}", name, hash, i,
                     static_cast<std::int32_t>(was), static_cast<std::int32_t>(now),
                     item ? " item " : "", item ? item->name : std::string_view{});
        }
    }
    last.bytes = std::move(values);
}

// The 64-bit keys: sorted records of {u64 key, ..., u8 value at +0xc}.
void FlagDebug::ScanKeys(EmulatorMemory& memory, u64 manager) {
    const auto count = coop::host::read_u32(memory, manager + kKeyCount);
    const auto records = coop::host::read_ptr(memory, manager + kKeyRecords);
    if (!count || !records || *count == 0 || *count > kMaxEntries) {
        return;
    }
    std::vector<std::uint8_t> raw(std::size_t(*count) * kKeyRecordSize);
    if (!memory.read(*records, raw)) {
        return;
    }
    if (!keys_.known || keys_.bytes.size() != raw.size()) {
        LOG_INFO(Core, "coop: flags debug: key64 table, {} records", *count);
        keys_.known = true;
        keys_.bytes = std::move(raw);
        return;
    }
    coop::host::GameFlags flags(memory);
    int logged = 0;
    std::size_t more = 0;
    for (std::size_t i = 0; i < *count; ++i) {
        const auto at = i * kKeyRecordSize;
        const u8 was = keys_.bytes[at + kKeyValue], now = raw[at + kKeyValue];
        if (was == now) {
            continue;
        }
        if (logged == kMaxLogged) {
            ++more;
            continue;
        }
        ++logged;
        std::uint64_t key, old_key;
        std::memcpy(&key, &raw[at], 8);
        std::memcpy(&old_key, &keys_.bytes[at], 8);
        LOG_INFO(Core, "coop: flags debug: key64 {:#x} (record {}) {} -> {}{}, get_key64 = {}",
                 key, i, was, now, key == old_key ? "" : " (record moved)",
                 Value(flags.get_key64(key)));
    }
    if (more != 0) {
        LOG_INFO(Core, "coop: flags debug: {} more key64 changes", more);
    }
    keys_.bytes = std::move(raw);
}

} // namespace CoopHook
