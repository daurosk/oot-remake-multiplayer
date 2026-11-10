#include <doctest/doctest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "../host/fake_memory.hpp"
#include "coop/host/game_flags.hpp"
#include "coop_cheat_items.h"

// The cheat (issue 125) on a fake GameDataMgr laid out as docs/flags.md says,
// with the flags a real save and the autotest save differ in.
namespace {

using coop::host::GameFlags;
using coop::host::test::FakeMemory;

class Writable final : public coop::host::WritableGameMemory {
public:
    std::uint64_t main_base() const override { return mem.main_base(); }
    bool read(std::uint64_t a, std::span<std::uint8_t> out) override { return mem.read(a, out); }
    bool write(std::uint64_t a, std::span<const std::uint8_t> data) override {
        std::vector<std::uint8_t> probe(data.size());
        if (!mem.read(a, probe)) return false;
        mem.put(a, data.data(), data.size());
        return true;
    }
    std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t a, std::uint32_t expected,
                                                      std::uint32_t desired) override {
        const auto v = coop::host::read_u32(mem, a);
        if (v && *v == expected) mem.put_u32(a, desired);
        return v;
    }
    FakeMemory mem;
};

constexpr std::uint64_t kMgr = 0x8000000000ull;
constexpr std::uint32_t kCap = 131, kQueueCap = 32;

// Pouch counts, capacities and IsGet of the cheat's items (docs/items.md).
constexpr std::uint32_t kStockBomb = 0xdb1bcd4f, kStockBag = 0xb0a1bf51, kCapBomb = 0xe753508b,
                        kIsGetBomb = 0x2bb00768, kIsGetBag = 0xa63f1e2d;
// What a real bomb bag also sets (coop_cheat_items.h).
constexpr std::uint32_t kHelpA = 0x6e5e7d9a, kHelpB = 0xfced7932, kBagGot = 0x4d345bbe,
                        kOrderBomb = 0xc7ecb4a1, kOrderSlingshot = 0xcba88c4f,
                        kOrderStick = 0xaf5154ab;

const std::vector<std::uint32_t> kInts{0x938d56bf, 0xa8c3e1fb, 0x518ec74b, 0x418db791, kStockBag,
                                       kStockBomb, kCapBomb,   0x80d34ce9, 0x5fadf7fc, 0xae8cc742};
const std::vector<std::uint32_t> kBools{0x69ec28be, 0x0a550531, 0x572701ea, kIsGetBag, kIsGetBomb,
                                        0x0ec22ee1, 0x29888e1e, kHelpA,     kHelpB,    kBagGot};
const std::vector<std::uint32_t> kUInts{kOrderBomb, kOrderSlingshot, kOrderStick};

struct Layout {
    std::uint64_t base, entries, slots, queue;
    std::uint64_t stride, value, slot;
};
constexpr Layout kBool{0x10, 0x8100000000ull, 0x8110000000ull, 0x8120000000ull, 0x18, 0x16, 8};
constexpr Layout kInt{0xd0, 0x8200000000ull, 0x8210000000ull, 0x8220000000ull, 0x28, 0x1c, 12};
constexpr Layout kUInt{0x790, 0x8500000000ull, 0x8510000000ull, 0x8520000000ull, 0x28, 0x1c, 12};

void zero(FakeMemory& m, std::uint64_t a, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) m.put_u8(a + i, 0);
}

void table(FakeMemory& m, const Layout& t, const std::vector<std::uint32_t>& hashes) {
    m.put_u32(kMgr + t.base, static_cast<std::uint32_t>(hashes.size()));
    m.put_u64(kMgr + t.base + 8, t.entries);
    m.put_u64(kMgr + t.base + 0x10, t.slots);
    m.put_u32(kMgr + t.base + 0x1c, kCap);
    zero(m, t.slots, 8 * kCap);
    zero(m, t.entries, t.stride * hashes.size());
    std::uint32_t index = 0;
    for (const auto h : hashes) {
        std::uint32_t s = h % kCap;
        while (*coop::host::read_u32(m, t.slots + 8 * s) != 0) s = (s + 1) % kCap;
        m.put_u32(t.slots + 8 * s, h);
        m.put_u32(t.slots + 8 * s + 4, index++);
    }
    m.put_u32(kMgr + t.base + 0x20, kQueueCap);
    m.put_u64(kMgr + t.base + 0x28, t.queue);
    m.put_u32(kMgr + t.base + 0x30, 0x30000000);
    zero(m, t.queue, t.slot * kQueueCap);
}

// A saved value, as the game holds it after the load.
void saved(FakeMemory& m, const Layout& t, const std::vector<std::uint32_t>& hashes,
           std::uint32_t hash, std::uint32_t value) {
    const auto i = static_cast<std::uint64_t>(
        std::find(hashes.begin(), hashes.end(), hash) - hashes.begin());
    REQUIRE(i < hashes.size());
    if (t.slot == 8) {
        m.put_u8(t.entries + t.stride * i + t.value, static_cast<std::uint8_t>(value));
    } else {
        m.put_u32(t.entries + t.stride * i + t.value, value);
    }
}

// The autotest save: no bombs, the slingshot got 18th, the Deku Stick 15th.
Writable game() {
    Writable w;
    w.mem.put_u64(FakeMemory::kBase + 0x4e842a8, kMgr);
    table(w.mem, kBool, kBools);
    table(w.mem, kInt, kInts);
    table(w.mem, kUInt, kUInts);
    saved(w.mem, kUInt, kUInts, kOrderSlingshot, 18);
    saved(w.mem, kUInt, kUInts, kOrderStick, 15);
    return w;
}

bool Has(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("cheat items: a save without bombs gets everything a real bomb bag sets") {
    auto w = game();
    const auto lines = CoopCheatItems::TopUp(w);
    GameFlags f(w);
    CHECK(f.get_int(kStockBomb) == 20);
    CHECK(f.get_int(kCapBomb) == 20);
    CHECK(f.get_int(kStockBag) == 1);
    CHECK(f.get_bool(kIsGetBomb) == true);
    CHECK(f.get_bool(kIsGetBag) == true);
    CHECK(f.get_bool(kHelpA) == true);
    CHECK(f.get_bool(kHelpB) == true);
    CHECK(f.get_bool(kBagGot) == true);
    CHECK(f.get_uint(kOrderBomb) == 19u);  // after the slingshot, the highest
    CHECK(f.get_uint(kOrderSlingshot) == 18u);
    CHECK(lines.size() == 14);
    CHECK(Has(lines, "cheat Item_Bomb 0 -> 20"));
    CHECK(Has(lines, "cheat 0x541947d0.Bomb 0 -> 1"));
    CHECK(Has(lines, "cheat get order Item_Bomb 0 -> 19"));
    CHECK_FALSE(Has(lines, "queue busy"));

    // The game has not applied the queues yet; the next pass sees them anyway.
    CHECK(CoopCheatItems::TopUp(w).empty());
}

TEST_CASE("cheat items: a save with real bombs keeps its own values") {
    auto w = game();
    saved(w.mem, kInt, kInts, kStockBomb, 14);
    saved(w.mem, kInt, kInts, kCapBomb, 20);
    saved(w.mem, kInt, kInts, kStockBag, 1);
    for (const auto h : {kIsGetBomb, kIsGetBag, kHelpA, kHelpB, kBagGot}) {
        saved(w.mem, kBool, kBools, h, 1);
    }
    saved(w.mem, kUInt, kUInts, kOrderBomb, 35);
    const auto lines = CoopCheatItems::TopUp(w);
    GameFlags f(w);
    CHECK(Has(lines, "cheat Item_Bomb 14 -> 20"));
    CHECK_FALSE(Has(lines, "Bomb 0 -> 1"));
    CHECK_FALSE(Has(lines, "get order"));
    CHECK(f.get_uint(kOrderBomb) == 35u);
}

TEST_CASE("cheat items: nothing before the flag manager is up, and a full queue is retried") {
    auto w = game();
    w.mem.put_u64(FakeMemory::kBase + 0x4e842a8, 0);
    CHECK(CoopCheatItems::TopUp(w).empty());

    auto v = game();
    v.mem.put_u32(kMgr + kUInt.base + 0x30, 0x30000000 | kQueueCap);  // UInt queue full
    const auto lines = CoopCheatItems::TopUp(v);
    CHECK(Has(lines, "cheat get order Item_Bomb 0 -> 19 (queue busy, again next second)"));
    v.mem.put_u32(kMgr + kUInt.base + 0x30, 0x40000000);  // the game emptied it
    const auto again = CoopCheatItems::TopUp(v);
    REQUIRE(again.size() == 1);
    CHECK(again[0] == "cheat get order Item_Bomb 0 -> 19");
}
