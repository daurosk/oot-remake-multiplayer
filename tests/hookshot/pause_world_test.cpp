// SPDX-License-Identifier: GPL-3.0-or-later
// Row 299's PauseMgr reader (hookshot/src/coop_pause_world.h) against a made-up game memory: the GOT slot, the global,
// the manager, its holds.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include "coop_pause_world.h"

namespace {

namespace P = CoopPauseWorld;
constexpr std::uint64_t kBase = 0x80020000;
constexpr std::uint64_t kGlobal = 0x85000000, kManager = 0x86000000, kArray = 0x86100000;

struct Memory {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void Put(std::uint64_t a, const void* v, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) bytes[a + i] = static_cast<const std::uint8_t*>(v)[i];
    }
    void Put32(std::uint64_t a, std::uint32_t v) { Put(a, &v, 4); }
    void Put64(std::uint64_t a, std::uint64_t v) { Put(a, &v, 8); }
    P::ReadFn Reader() {
        return [this](std::uint64_t a, void* out, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                const auto it = bytes.find(a + i);
                if (it == bytes.end()) return false;
                static_cast<std::uint8_t*>(out)[i] = it->second;
            }
            return true;
        };
    }
    // A manager with `n` holds, keys 0x100 + i, counts as given.
    void Manager(std::initializer_list<std::uint32_t> counts) {
        Put64(kBase + P::kPauseMgrSlot, kGlobal);
        Put64(kGlobal, kManager);
        Put32(kManager, static_cast<std::uint32_t>(counts.size()));
        Put64(kManager + P::kHolds, kArray);
        for (std::uint32_t w = 0; w < 4; ++w) Put32(kManager + P::kWords + 4 * w, w == 2 ? 1 : 0);
        std::uint32_t i = 0;
        for (const std::uint32_t c : counts) {
            const std::uint64_t at = kArray + P::kHoldSize * i;
            for (std::uint64_t b = 0; b < P::kHoldSize; b += 8) Put64(at + b, 0);
            Put32(at + P::kHoldKey, 0x100 + i);
            Put32(at + P::kHoldCount, c);
            ++i;
        }
    }
};

}  // namespace

TEST_CASE("pause world: the held holds and the manager's words") {
    Memory m;
    m.Manager({0, 2, 0, 1});
    const P::Snapshot s = P::Take(m.Reader(), kBase);
    REQUIRE(s.ok);
    CHECK(s.manager == kManager);
    CHECK(s.holds == 4);
    CHECK(s.words[2] == 1);
    REQUIRE(s.held.size() == 2);
    CHECK(s.held[0].index == 1);
    CHECK(s.held[0].key == 0x101);
    CHECK(s.held[0].count == 2);
    CHECK(s.held[1].index == 3);
    CHECK(P::Json(s) ==
          "{\"ok\":true,\"manager\":\"0x86000000\",\"holds\":4,\"words\":[0,0,1,0],\"held\":[{\"index\":1,\"key\":"
          "\"0x101\",\"count\":2},{\"index\":3,\"key\":\"0x103\",\"count\":1}]}");
    CHECK(P::AllHoldLines(m.Reader(), kBase).size() == 5);   // (the manager's line, then one per hold)
}

TEST_CASE("pause world: no manager yet, or a count no manager has, is not ok") {
    Memory m;
    CHECK_FALSE(P::Take(m.Reader(), kBase).ok);
    m.Put64(kBase + P::kPauseMgrSlot, kGlobal);
    m.Put64(kGlobal, 0);
    const P::Snapshot none = P::Take(m.Reader(), kBase);
    CHECK_FALSE(none.ok);
    CHECK(none.why == "no manager yet");
    CHECK(P::Json(none) == "{\"ok\":false,\"why\":\"no manager yet\"}");
    m.Manager({1});
    m.Put32(kManager, P::kMostHolds + 1);
    CHECK_FALSE(P::Take(m.Reader(), kBase).ok);
    CHECK_FALSE(P::Take(m.Reader(), 0).ok);
}
