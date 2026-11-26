// Tests for another player's lit Deku stick on their puppet (lichii/hook/
// coop_puppet_flame.cpp, issue 248): the game calls that start the stick's own
// flame effect and put it out, and when they are made. Guest memory is fake pages.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_puppet.h"
#include "coop_puppet_flame.h"
#include "coop_puppet_sound.h"

namespace {

using namespace CoopPuppetFlame;
using Step = State::Step;

class FakeGuest : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t address, std::size_t size) {
        for (std::uint64_t p = address & ~0xfffULL; p < address + size; p += 0x1000) {
            pages_[p];
        }
    }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) {
                return false;
            }
            o[i] = it->second[(address + i) & 0xfff];
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) {
                return false;
            }
            it->second[(address + i) & 0xfff] = p[i];
        }
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T value) {
        Map(address, sizeof(T));
        Write(address, &value, sizeof(T));
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    std::string TextAt(std::uint64_t address) {
        std::string out;
        char c = 0;
        while (Read(address + out.size(), &c, 1) && c != '\0') {
            out += c;
        }
        return out;
    }

private:
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages_;
};

namespace pa = CoopPuppet::addr;
namespace sa = CoopPuppetSound::addr;

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kPuppet = 0x100000;
constexpr std::uint64_t kParts = 0x110000;
constexpr std::uint64_t kEu = 0x120000;
constexpr std::uint64_t kHolder = 0x130000;
constexpr std::uint64_t kStick = 0x140000;
constexpr std::uint64_t kStickParts = 0x150000;
constexpr std::uint64_t kXLink = 0x160000;
constexpr std::uint64_t kUser = 0x170000;
constexpr std::uint64_t kResource = 0x180000;
constexpr std::uint64_t kAccessor = 0x190000;
constexpr std::uint64_t kBlock = 0x200000;
constexpr std::uint32_t kId = 0x1234;

// A puppet with a Deku stick in equipment slot 0, its ELink user readable.
FakeGuest PuppetWithStick(std::uint64_t stick = kStick) {
    FakeGuest g;
    g.Put<std::uint64_t>(kPuppet + pa::kActorParts, kParts);
    g.Put<std::uint64_t>(kParts + pa::kPartsEquipmentUser, kEu);
    g.Put<std::uint32_t>(kEu + pa::kEquipSlots + pa::kEquipSlotId, kId);
    g.Put<std::uint64_t>(kEu + pa::kEquipSlots + pa::kEquipSlotHolder, kHolder);
    g.Put<std::uint32_t>(kHolder + pa::kHolderId, kId);
    g.Put<std::uint64_t>(kHolder + pa::kHolderActor, stick);
    g.Put<std::uint32_t>(stick + pa::kActorState, 4);
    g.Put<std::uint64_t>(stick + sa::kActorParts, kStickParts);
    g.Put<std::uint64_t>(kStickParts + sa::kPartsXLink, kXLink);
    g.Put<std::uint64_t>(kXLink + sa::kXLinkEffectUser, kUser);
    g.Put<std::uint64_t>(kUser + sa::kUserResource, kResource);
    g.Put<std::uint64_t>(kResource + sa::kResourceAccessor, kAccessor);
    g.Map(kBlock, CoopPuppet::block::kSize);
    return g;
}

struct Run {
    FakeGuest g = PuppetWithStick();
    State s;
    std::vector<std::string> said;
    std::optional<CoopPuppet::Call> Next(std::uint8_t lit, std::uint8_t drawn = 1, bool shown = true) {
        return CoopPuppetFlame::Next(s, lit, drawn, shown, g, kMain, kPuppet, kBlock, "P2",
                                     [this](const std::string& l) { said.push_back(l); });
    }
    void Finish(std::uint64_t result) {
        CoopPuppetFlame::Finish(s, result, "P2", [this](const std::string& l) { said.push_back(l); });
    }
    // Lights it: FireStart found and played, Fire found and played.
    void Light() {
        REQUIRE(Next(1));
        Finish(1);
        REQUIRE(Next(1));
        Finish(0);
        REQUIRE(Next(1));
        Finish(1);
        REQUIRE(Next(1));
        Finish(0);
        REQUIRE(s.on);
    }
};

}  // namespace

TEST_CASE("puppet flame: block room after the link sounds, inside the block") {
    CHECK(block::kText >= CoopLinkSound::block::kHandle + 8 * CoopLinkSound::block::kHandleSlots);
    CHECK(block::kEnd <= CoopPuppet::block::kSize);
}

TEST_CASE("puppet flame: nothing is asked while their stick is not lit, or not in hand") {
    Run r;
    CHECK_FALSE(r.Next(0));
    CHECK_FALSE(r.Next(1, 0));         // lit but put away
    CHECK_FALSE(r.Next(1, 1, false));  // the puppet is hidden
    CHECK_FALSE(r.Next(2, 3));         // slot 1 lit, but the puppet has no piece there
    CHECK(r.s.pending == Step::none);
}

TEST_CASE("puppet flame: a lit stick plays the stick's own FireStart, then Fire, on its ELink user") {
    Run r;
    auto c = r.Next(1);
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kSearchAsset);
    CHECK(c->x[0] == kAccessor);
    CHECK(c->x[1] == kBlock + block::kAsset);
    CHECK(c->x[2] == kBlock + block::kText);
    CHECK(r.g.TextAt(kBlock + block::kText) == "Chemical_FireStart");
    CHECK_FALSE(r.Next(1));  // one call at a time
    r.Finish(1);
    c = r.Next(1);
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kEmit);
    CHECK(c->x[0] == kUser);
    CHECK(c->x[1] == kBlock + block::kAsset);
    CHECK(c->x[2] == kBlock + block::kHandle);
    CHECK(c->x[3] == 0);
    CHECK(r.g.Get<std::uint8_t>(kBlock + block::kHandle) == 0xff);
    CHECK(r.g.Get<std::uint16_t>(kBlock + block::kHandle + 2) == 0xffff);
    r.Finish(0);
    c = r.Next(1);
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kSearchAsset);
    CHECK(r.g.TextAt(kBlock + block::kText) == "Chemical_Fire");
    r.Finish(1);
    c = r.Next(1);
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kEmit);
    r.Finish(0);
    CHECK(r.s.on);
    CHECK(r.s.actor == kStick);
    CHECK_FALSE(r.Next(1));  // burning as wanted: nothing more
    REQUIRE_FALSE(r.said.empty());
    CHECK(r.said.back().find("a picture only") != std::string::npos);
}

TEST_CASE("puppet flame: a piece without the flash key gets the flame alone") {
    Run r;
    REQUIRE(r.Next(1));
    r.Finish(0);  // no Chemical_FireStart
    auto c = r.Next(1);
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kSearchAsset);
    CHECK(r.g.TextAt(kBlock + block::kText) == "Chemical_Fire");
}

TEST_CASE("puppet flame: a piece without Chemical_Fire is left unlit and not asked again") {
    Run r;
    REQUIRE(r.Next(1));
    r.Finish(0);
    REQUIRE(r.Next(1));
    r.Finish(0);  // no Chemical_Fire either
    CHECK_FALSE(r.s.on);
    CHECK(r.s.refused == kStick);
    CHECK_FALSE(r.Next(1));
    CHECK_FALSE(r.Next(1));
}

TEST_CASE("puppet flame: put out with the game's find by key and kill, as OneShotXLinkFadeByKey") {
    struct Case {
        std::uint8_t lit;
        std::uint8_t drawn;
        bool shown;
    };
    // their stick went out; put away; the puppet hidden
    for (const Case k : {Case{0, 1, true}, Case{1, 0, true}, Case{1, 1, false}}) {
        const std::uint8_t lit = k.lit;
        const std::uint8_t drawn = k.drawn;
        const bool shown = k.shown;
        CAPTURE(lit);
        CAPTURE(drawn);
        CAPTURE(shown);
        Run r;
        r.Light();
        auto c = r.Next(lit, drawn, shown);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kFindPlaying);
        CHECK(c->x[0] == kUser);
        CHECK(c->x[1] == kBlock + block::kHandle);
        CHECK(c->x[2] == kBlock + block::kText);
        CHECK(r.g.TextAt(kBlock + block::kText) == "Chemical_Fire");
        CHECK(r.g.Get<std::uint16_t>(kBlock + block::kHandle + 2) == 0xffff);
        r.Finish(1);
        c = r.Next(lit, drawn, shown);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kKill);
        CHECK(c->x[0] == kBlock + block::kHandle);
        r.Finish(0);
        CHECK_FALSE(r.s.on);
        CHECK_FALSE(r.Next(lit, drawn, shown));
    }
}

TEST_CASE("puppet flame: a flame the game already ended needs no kill") {
    Run r;
    r.Light();
    REQUIRE(r.Next(0));
    r.Finish(0);  // the find found nothing playing
    CHECK_FALSE(r.s.on);
    CHECK_FALSE(r.Next(0));
}

TEST_CASE("puppet flame: a piece made again starts unlit, and is lit again while theirs burns") {
    Run r;
    r.Light();
    constexpr std::uint64_t kNewStick = 0x400000;
    FakeGuest fresh = PuppetWithStick(kNewStick);
    r.g = fresh;
    auto c = r.Next(1);  // the old piece's flame went with it: the new one is lit from the start
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kSearchAsset);
    CHECK(r.s.actor == kNewStick);
    CHECK(r.said.front().find("their stick burns") != std::string::npos);
    bool forgot = false;
    for (const auto& l : r.said) {
        forgot = forgot || l.find("gone or made again") != std::string::npos;
    }
    CHECK(forgot);
}

TEST_CASE("puppet flame: a call the cave did not make is asked again") {
    Run r;
    REQUIRE(r.Next(1));
    Dropped(r.s);
    auto c = r.Next(1);
    REQUIRE(c);
    CHECK(c->function == kMain + sa::kSearchAsset);
    CHECK(r.g.TextAt(kBlock + block::kText) == "Chemical_FireStart");
}

TEST_CASE("puppet flame: Forget drops it all (a new puppet actor)") {
    Run r;
    r.Light();
    Forget(r.s);
    CHECK_FALSE(r.s.on);
    CHECK(r.s.actor == 0);
    CHECK(r.s.pending == Step::none);
}
