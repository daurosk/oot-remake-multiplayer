// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "coop_hit_replay.h"

namespace {

// Guest memory as a byte map: reads of unwritten bytes give 0.
class FakeGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* p = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            p[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) bytes[address + i] = p[i];
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T v) {
        Write(address, &v, sizeof v);
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kActor = 0x1000'0000;
constexpr std::uint64_t kTable = 0x2000'0000;
constexpr std::uint64_t kLife = 0x3000'0000;

FakeGuest WithCopy() {
    FakeGuest g;
    g.Put<std::uint64_t>(kActor + CoopHitReplay::kActorComponents, kTable);
    g.Put<std::uint64_t>(kTable + CoopHitReplay::kComponentLife, kLife);
    g.Put<std::uint32_t>(kLife + CoopHitReplay::kLastHitId, 0x20);
    return g;
}

} // namespace

TEST_CASE("hit replay: run under the caller's own lock, as the Mover's Step does, never takes it again") {
    // boss-intro run 080505: Replay locked the Mover's mutex_ inside Step, which holds it: the game ended
    // (resource_deadlock_would_occur). The replay takes only its own locks, so it runs fine under any outer one,
    // and its ids are read from another thread's call while it is held.
    CoopHitReplay::Replayer r;
    FakeGuest g = WithCopy();
    CoopHitReplay::Hit h;
    h.actor = kActor;
    h.attacker = 0x5000;
    h.amount = 3;
    h.at = {18.5f, 61.5f, 67.8f};
    h.exact = true;
    r.Add(h, 1000);
    CHECK(r.Waiting() == 1);
    std::mutex step_lock;  // stands for the Mover's mutex_
    std::vector<CoopHitReplay::Written> written;
    {
        std::lock_guard lock(step_lock);
        written = r.Run(g, 1050, [&](std::uint64_t a) {
            CHECK(r.ReplayedId(kLife) == 0xffffffffu);  // (its ids, read inside the run: no deadlock either)
            return a == kActor;
        });
    }
    REQUIRE(written.size() == 1);
    CHECK(written[0].id == 0x22);
    CHECK(written[0].life == kLife);
    CHECK(r.ReplayedId(kLife) == 0x22);
    CHECK(r.Waiting() == 0);
    // The record: our id, present, the hit landed exactly where it was asked (no +1 for an exact place).
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitRecord + 8) == 0x22);
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitRecord + 0xc) == 1);
    CHECK(g.Get<float>(kLife + 0x27c + 4) == doctest::Approx(61.5f));
    CHECK(g.Get<std::int32_t>(kLife + CoopHitReplay::kHitAmount + 4) == 3);
    r.Forget();
    CHECK(r.ReplayedId(kLife) == 0xffffffffu);
}

TEST_CASE("hit replay: a late hit or a gone actor is dropped, nothing written") {
    CoopHitReplay::Replayer r;
    FakeGuest g = WithCopy();
    CoopHitReplay::Hit h;
    h.actor = kActor;
    h.amount = 1;
    r.Add(h, 1000);
    CHECK(r.Run(g, 1000 + CoopHitReplay::kHitLateMs, [](std::uint64_t) { return true; }).empty());
    r.Add(h, 2000);
    CHECK(r.Run(g, 2010, [](std::uint64_t) { return false; }).empty());
    CHECK(r.ReplayedId(kLife) == 0xffffffffu);
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitRecord + 0xc) == 0);
    for (int i = 0; i < 70; ++i) r.Add(h, 3000);
    CHECK(r.Waiting() == CoopHitReplay::Replayer::kQueued);
}

TEST_CASE("hit replay (protocol 14): only the other game's hit kind is taken, on the stand-in hit (crash guard)") {
    // Every hit shows on every screen: a bomb's (or arrow's, hammer's ...) kind reaches the damage step, so an
    // enemy that refuses a plain sword hit (King Dodongo unstunned, Gohma's eye) takes it. Nothing else of the
    // other process's record is written (review-pr-d): its pointers and links would crash our game.
    CoopHitReplay::Replayer r;
    FakeGuest g = WithCopy();
    CoopHitReplay::Record rec{};
    for (auto& w : rec) w = 0x71234560;  // pointer-looking words everywhere
    rec[CoopHitReplay::kRecKind] = 0x00090003;  // another attack kind than the sword's
    rec[CoopHitReplay::kRecKind + 1] = 0x10020000;
    rec[10] = 0xabcd;  // a hit-kind parameter: taken
    CoopHitReplay::Hit h;
    h.actor = kActor;
    h.attacker = 0x5000;
    h.amount = 8;
    h.at = {9.0f, 9.0f, 9.0f};
    h.from = {7.0f, 7.0f, 7.0f};
    h.record = rec;
    r.Add(h, 1000);
    const auto written = r.Run(g, 1010, [](std::uint64_t) { return true; });
    REQUIRE(written.size() == 1);
    CHECK(written[0].real);
    const std::uint64_t at = kLife + CoopHitReplay::kHitRecord;
    CHECK(g.Get<std::uint64_t>(at) == 0x5000);  // our attacker
    CHECK(g.Get<std::uint32_t>(at + 8) == 0x22);
    CHECK(g.Get<std::uint32_t>(at + 0xc) == 1);
    CHECK(g.Get<std::uint32_t>(kLife + 0x2c4) == 0x22);
    CHECK(g.Get<std::uint32_t>(kLife + 0x250) == 0x00090003);
    CHECK(g.Get<std::uint32_t>(at + 40) == 0xabcd);
    // Our places, not the record's.
    CHECK(g.Get<float>(kLife + 0x27c + 4) == doctest::Approx(10.0f));
    CHECK(g.Get<float>(kLife + 0x2a0 + 8) == doctest::Approx(7.0f));
    // Every word outside the hit kind is ours or the stand-in's, never the other game's.
    for (std::size_t i = 0; i < rec.size(); ++i) {
        if (i >= CoopHitReplay::kRecKind && i < CoopHitReplay::kRecKindEnd) continue;
        CHECK(g.Get<std::uint32_t>(at + 4 * i) != 0x71234560u);
    }
    // The attack header names the record's kind, our id.
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitHeader) == 0x00090003);
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitHeader + 4) == 0x10020000);
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitHeader + 8) == 0x22);
    CHECK(g.Get<std::int32_t>(kLife + CoopHitReplay::kHitAmount + 4) == 8);
}

TEST_CASE("hit replay (protocol 14): no record: the stand-in sword hit, as before") {
    CoopHitReplay::Replayer r;
    FakeGuest g = WithCopy();
    CoopHitReplay::Hit h;
    h.actor = kActor;
    h.amount = 1;
    h.at = {1.0f, 2.0f, 3.0f};
    r.Add(h, 1000);
    const auto written = r.Run(g, 1010, [](std::uint64_t) { return true; });
    REQUIRE(written.size() == 1);
    CHECK_FALSE(written[0].real);
    CHECK(g.Get<std::uint32_t>(kLife + 0x250) == CoopHitReplay::kHitTemplate[CoopHitReplay::kRecKind]);
    CHECK(g.Get<std::uint32_t>(kLife + CoopHitReplay::kHitHeader) == 0x00060001);
    CHECK(g.Get<float>(kLife + 0x27c + 4) == doctest::Approx(3.0f));  // the foot + 1
}

TEST_CASE("hit replay (row 177): another player's hit names their seen puppet as the attacker, else our Link") {
    constexpr std::uint64_t link = 0x1000;
    constexpr std::uint64_t puppet = 0x2000;
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, false) == puppet);
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, true) == link);  // downed, spectating or away
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, 0, false) == link);      // no seen puppet here (quiet, gone)
    CHECK(CoopHitReplay::ReplayAttacker(link, 0, puppet, false) == link); // the hitter is not known
}

TEST_CASE("hit replay (row 177 review MUST 2): the killing blow keeps our Link as the attacker") {
    constexpr std::uint64_t link = 0x1000;
    constexpr std::uint64_t puppet = 0x2000;
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, false, true) == link);
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, false, false) == puppet);
    CHECK(CoopHitReplay::LethalHit(8, 8, 0));    // takes the last of it
    CHECK(CoopHitReplay::LethalHit(8, 12, 0));   // more than is left
    CHECK(CoopHitReplay::LethalHit(-1, 4, 0));   // our life not read: the owner's life after is 0
    CHECK(CoopHitReplay::LethalHit(3, 4, 1));    // our copy is lower than the owner's: it dies here
    CHECK_FALSE(CoopHitReplay::LethalHit(8, 4, 4));
    CHECK_FALSE(CoopHitReplay::LethalHit(-1, 4, 4));
    CHECK_FALSE(CoopHitReplay::LethalHit(8, 0, 0));  // no hurt: not a hit
}

TEST_CASE("hit replay (row 178): a hit on a boss keeps our Link as the attacker, perception on or not") {
    constexpr std::uint64_t link = 0x1000;
    constexpr std::uint64_t puppet = 0x2000;
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, false, false, true) == link);
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, false, true, true) == link);
    CHECK(CoopHitReplay::ReplayAttacker(link, 2, puppet, false, false, false) == puppet);  // a plain enemy
}
