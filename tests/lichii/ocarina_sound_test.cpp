// Tests for the other players' Ocarina (issue 152): the sending side
// (lichii/hook/coop_ocarina_sound.cpp) and the calls that play a note on a
// puppet (lichii/hook/coop_puppet_sound.cpp). Guest memory is fake pages.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_ocarina_sound.h"
#include "coop_puppet.h"
#include "coop_puppet_sound.h"

namespace {

using namespace CoopOcarinaSound;
namespace snd = CoopPuppetSound;

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
    void PutText(std::uint64_t address, const std::string& text) {
        Map(address, text.size() + 1);
        Write(address, text.c_str(), text.size() + 1);
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

constexpr std::uint64_t kBase = 0x80000000;
constexpr std::uint64_t kActor = 0x400002000;
constexpr std::uint64_t kParts = 0x400010000;
constexpr std::uint64_t kXLink = 0x400020000;
constexpr std::uint64_t kUser = 0x400030000;
constexpr std::uint64_t kResource = 0x400040000;
constexpr std::uint64_t kAccessor = 0x400050000;
constexpr std::uint64_t kBlock = 0x200000000;

// A puppet whose XLink component has a sound user named Player_Child.
FakeGuest PuppetWithSound() {
    FakeGuest g;
    g.Map(kBlock, 0x700);
    g.Put<std::uint64_t>(kActor + snd::addr::kActorParts, kParts);
    g.Put<std::uint64_t>(kParts + snd::addr::kPartsXLink, kXLink);
    g.Put<std::uint64_t>(kXLink + snd::addr::kXLinkSoundUser, kUser);
    g.Put<std::uint32_t>(kUser + snd::addr::kUserFlags, 0);
    g.Put<std::uint64_t>(kUser + snd::addr::kUserResource, kResource);
    g.Put<std::uint64_t>(kResource + snd::addr::kResourceAccessor, kAccessor);
    g.Put<std::uint64_t>(kResource + snd::addr::kResourceName, kResource + 0x100);
    g.PutText(kResource + 0x100, "Player_Child");
    // Nothing playing: the list's first link is its head.
    g.Put<std::uint64_t>(kUser + snd::addr::kUserFirstLink, kUser + snd::addr::kUserEvents);
    g.Put<std::int32_t>(kUser + snd::addr::kUserLinkOffset, 0x10);
    return g;
}

struct Run {
    std::vector<std::string> said;
    snd::Say say() {
        return [this](const std::string& s) { said.push_back(s); };
    }
};

} // namespace

TEST_CASE("the ocarina's notes are the game's table") {
    CHECK(kNoteCount == coop::protocol::kOcarinaNotes);
    CHECK(AssetName(1) == "Ocarina_LowB");
    CHECK(AssetName(4) == "Ocarina_D");
    CHECK(AssetName(16) == "Ocarina_HighD");
    CHECK(AssetName(19) == "Ocarina_HighF");
    CHECK(AssetName(0).empty());
    CHECK(AssetName(20).empty());
    // L R Y X A, low to high, as the game's prompt lists them.
    CHECK(AssetName(NoteOf(kButtonL)) == "Ocarina_D");
    CHECK(AssetName(NoteOf(kButtonR)) == "Ocarina_F");
    CHECK(AssetName(NoteOf(kButtonY)) == "Ocarina_A");
    CHECK(AssetName(NoteOf(kButtonX)) == "Ocarina_B");
    CHECK(AssetName(NoteOf(kButtonA)) == "Ocarina_HighD");
    CHECK(NoteOf(1u << 1) == 0);  // B puts the Ocarina away
}

TEST_CASE("only the playing command counts as playing") {
    CHECK(Playing("DemoOcarina"));
    CHECK_FALSE(Playing("DemoOcarinaMelodyGet"));
    CHECK_FALSE(Playing("Wait"));
    CHECK_FALSE(Playing(""));
}

TEST_CASE("a note is sent when pressed and 0 when let go") {
    NoteSender s;
    CHECK_FALSE(s.Update(true, 0).has_value());
    CHECK(s.Update(true, kButtonL) == std::optional<std::uint8_t>(4));
    CHECK_FALSE(s.Update(true, kButtonL).has_value());  // held: nothing new
    CHECK(s.Update(true, 0) == std::optional<std::uint8_t>(0));
    CHECK_FALSE(s.Update(true, 0).has_value());
}

TEST_CASE("a newer press wins and letting it go goes back to the one held") {
    NoteSender s;
    CHECK(s.Update(true, kButtonY) == std::optional<std::uint8_t>(11));
    CHECK(s.Update(true, kButtonY | kButtonX) == std::optional<std::uint8_t>(13));
    CHECK(s.Update(true, kButtonY) == std::optional<std::uint8_t>(11));  // X let go, Y still down
    CHECK_FALSE(s.Update(true, kButtonY | (1u << 1)).has_value());     // B is no note
    CHECK(s.Update(true, 0) == std::optional<std::uint8_t>(0));
}

TEST_CASE("no notes unless the Ocarina is played, and stopping lets the note go") {
    NoteSender s;
    CHECK_FALSE(s.Update(false, kButtonA).has_value());
    CHECK(s.Update(true, kButtonA) == std::optional<std::uint8_t>(16));
    CHECK(s.Update(false, kButtonA) == std::optional<std::uint8_t>(0));
    // Pressed again only once playing again, and as a new press.
    CHECK(s.Update(true, kButtonA) == std::optional<std::uint8_t>(16));
}

TEST_CASE("a note on a puppet: search the asset, then emit it on the sound user") {
    FakeGuest g = PuppetWithSound();
    Run run;
    snd::State st;
    snd::Wanted w{4, 1, 1000};
    auto c = snd::Next(st, w, g, kBase, kActor, kBlock, 1100, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kSearchAsset);
    CHECK(c->x[0] == kAccessor);
    CHECK(c->x[1] == kBlock + snd::block::kAsset);
    CHECK(c->x[2] == kBlock + snd::block::kText);
    CHECK(g.TextAt(kBlock + snd::block::kText) == "Ocarina_D");
    std::array<std::uint8_t, 8> handle{};
    g.Read(kBlock + snd::block::kHandle, handle.data(), handle.size());
    CHECK(handle == std::array<std::uint8_t, 8>{0xff, 0, 0xff, 0xff, 0, 0, 0, 0});
    REQUIRE(run.said.size() == 2);
    CHECK(run.said[0].find("'Player_Child' (SLink") != std::string::npos);
    CHECK(run.said[1].find("no 'Ocarina' sound user found") != std::string::npos);
    snd::Finish(st, 1, "P1", run.say());
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 1110, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kEmit);
    CHECK(c->x[0] == kUser);
    CHECK(c->x[1] == kBlock + snd::block::kAsset);
    CHECK(c->x[2] == kBlock + snd::block::kHandle);
    CHECK(c->x[3] == 0);
    snd::Finish(st, 1, "P1", run.say());
    CHECK(st.sounding == 4);
    CHECK(run.said.back() == "P1 plays sound 'Ocarina_D' (returned 0x1)");
    CHECK_FALSE(snd::Next(st, w, g, kBase, kActor, kBlock, 1120, "P1", run.say()).has_value());

    // Let go: the handle is stopped, nothing new is searched.
    w = snd::Wanted{0, 2, 1200};
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 1210, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kKill);
    CHECK(c->x[0] == kBlock + snd::block::kHandle);
    snd::Finish(st, 0, "P1", run.say());
    CHECK(st.sounding == 0);
    CHECK_FALSE(snd::Next(st, w, g, kBase, kActor, kBlock, 1220, "P1", run.say()).has_value());
}

TEST_CASE("a new note stops the one sounding first") {
    FakeGuest g = PuppetWithSound();
    Run run;
    snd::State st;
    snd::Wanted w{11, 1, 0};
    auto c = snd::Next(st, w, g, kBase, kActor, kBlock, 10, "P2", run.say());
    snd::Finish(st, 1, "P2", run.say());
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 20, "P2", run.say());
    snd::Finish(st, 1, "P2", run.say());
    REQUIRE(st.sounding == 11);
    w = snd::Wanted{13, 2, 30};
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 40, "P2", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kKill);
    snd::Finish(st, 0, "P2", run.say());
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 50, "P2", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kSearchAsset);
    CHECK(g.TextAt(kBlock + snd::block::kText) == "Ocarina_B");
}

TEST_CASE("an asset the user lacks, a puppet with no sound user, an old note") {
    Run run;
    {
        FakeGuest g = PuppetWithSound();
        snd::State st;
        snd::Wanted w{4, 1, 0};
        REQUIRE(snd::Next(st, w, g, kBase, kActor, kBlock, 10, "P1", run.say()).has_value());
        snd::Finish(st, 0, "P1", run.say());  // not found
        CHECK(run.said.back() == "P1: the sound 'Ocarina_D' is not in its sound user");
        CHECK_FALSE(snd::Next(st, w, g, kBase, kActor, kBlock, 20, "P1", run.say()).has_value());
        CHECK(st.sounding == 0);
    }
    {
        FakeGuest g = PuppetWithSound();
        g.Put<std::uint64_t>(kXLink + snd::addr::kXLinkSoundUser, 0);
        snd::State st;
        CHECK_FALSE(snd::Next(st, snd::Wanted{4, 1, 0}, g, kBase, kActor, kBlock, 10, "P1",
                              run.say()).has_value());
        CHECK(run.said.back().find("has no sound user") != std::string::npos);
    }
    {
        FakeGuest g = PuppetWithSound();
        snd::State st;
        // Received 2 s before the puppet could play it: skipped.
        CHECK_FALSE(snd::Next(st, snd::Wanted{4, 1, 1000}, g, kBase, kActor, kBlock, 3000, "P1",
                              run.say()).has_value());
        CHECK(st.serial_done == 1);
    }
}

TEST_CASE("a puppet made again starts silent") {
    FakeGuest g = PuppetWithSound();
    Run run;
    snd::State st;
    snd::Wanted w{4, 1, 0};
    snd::Next(st, w, g, kBase, kActor, kBlock, 10, "P1", run.say());
    snd::Finish(st, 1, "P1", run.say());
    snd::Next(st, w, g, kBase, kActor, kBlock, 20, "P1", run.say());
    snd::Finish(st, 1, "P1", run.say());
    REQUIRE(st.sounding == 4);
    // The same note on a new actor is not played again, and no stop is sent
    // to the old actor's handle.
    CHECK_FALSE(snd::Next(st, w, g, kBase, kActor + 0x1000, kBlock, 30, "P1", run.say()).has_value());
    CHECK(st.sounding == 0);
}

TEST_CASE("what a sound user plays is read from its event list") {
    FakeGuest g = PuppetWithSound();
    CHECK(snd::Playing(g, kUser).empty());
    // Two events: links at E + 0x10 (the list offset), names via +0x30.
    const std::uint64_t e1 = 0x400060000, e2 = 0x400061000;
    g.Put<std::uint64_t>(kUser + snd::addr::kUserFirstLink, e1 + 0x10);
    g.Put<std::uint64_t>(e1 + 0x10 + snd::addr::kLinkNext, e2 + 0x10);
    g.Put<std::uint64_t>(e2 + 0x10 + snd::addr::kLinkNext, kUser + snd::addr::kUserEvents);
    g.Put<std::uint64_t>(e1 + snd::addr::kEventAsset, e1 + 0x200);
    g.Put<std::uint64_t>(e1 + 0x200, e1 + 0x300);
    g.PutText(e1 + 0x300, "Ocarina_D");
    g.Put<std::uint64_t>(e2 + snd::addr::kEventAsset, e2 + 0x200);
    g.Put<std::uint64_t>(e2 + 0x200, e2 + 0x300);
    g.PutText(e2 + 0x300, "Player_FootStep");
    CHECK(snd::Playing(g, kUser) == "Ocarina_D, Player_FootStep");
    Run run;
    snd::State st;
    snd::LogPlaying(st, g, kActor, 1000, "P1", run.say());
    REQUIRE(run.said.size() == 1);
    CHECK(run.said[0] == "P1 sounds: Ocarina_D, Player_FootStep");
    snd::LogPlaying(st, g, kActor, 1100, "P1", run.say());  // too soon, and the same
    CHECK(run.said.size() == 1);
    g.Put<std::uint64_t>(kUser + snd::addr::kUserFirstLink, kUser + snd::addr::kUserEvents);
    snd::LogPlaying(st, g, kActor, 1300, "P1", run.say());
    CHECK(run.said.back() == "P1 sounds: none");
}

TEST_CASE("the notes are searched and played on the game's Ocarina user when a list reaches it") {
    FakeGuest g = PuppetWithSound();
    // A circular list through both users at +0x40, with the system's head between them.
    constexpr std::uint64_t kOcarina = 0x400060000;
    constexpr std::uint64_t kOcarinaResource = 0x400070000;
    constexpr std::uint64_t kOcarinaAccessor = 0x400080000;
    constexpr std::uint64_t kHead = 0x400090000;
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserResource, kOcarinaResource);
    g.Put<std::uint64_t>(kOcarinaResource + snd::addr::kResourceAccessor, kOcarinaAccessor);
    g.Put<std::uint64_t>(kOcarinaResource + snd::addr::kResourceName, kOcarinaResource + 0x100);
    g.PutText(kOcarinaResource + 0x100, "Ocarina");
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserFirstLink, kOcarina + snd::addr::kUserEvents);
    g.Put<std::int32_t>(kOcarina + snd::addr::kUserLinkOffset, 0x10);
    g.Put<std::uint64_t>(kUser + 0x40, kHead);
    g.Put<std::uint64_t>(kHead, kOcarina + 0x40);
    g.Put<std::uint64_t>(kOcarina + 0x40, kUser + 0x40);
    std::uint64_t link = 0;
    CHECK(snd::FindUser(g, kUser, "Ocarina", &link) == kOcarina);
    CHECK(link == 0x40);
    CHECK(snd::FindUser(g, kUser, "Nobody") == 0);
    // Something that only points at the resource (no event list) is not a user.
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserFirstLink, 0);
    CHECK(snd::FindUser(g, kUser, "Ocarina") == 0);
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserFirstLink, kOcarina + snd::addr::kUserEvents);

    Run run;
    snd::State st;
    snd::Wanted w{16, 1, 1000};
    auto c = snd::Next(st, w, g, kBase, kActor, kBlock, 1100, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kSearchAsset);
    CHECK(c->x[0] == kOcarinaAccessor);
    CHECK(run.said.back().find("Ocarina notes go to sound user") != std::string::npos);
    snd::Finish(st, 1, "P1", run.say());
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 1110, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->function == kBase + snd::addr::kEmit);
    CHECK(c->x[0] == kOcarina);
    snd::Finish(st, 1, "P1", run.say());
    // The next note: no new lookup, no new user lines.
    const auto lines = run.said.size();
    w = snd::Wanted{4, 2, 1200};
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 1210, "P1", run.say());  // kill
    snd::Finish(st, 0, "P1", run.say());
    c = snd::Next(st, w, g, kBase, kActor, kBlock, 1220, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->x[0] == kOcarinaAccessor);
    CHECK(run.said.size() == lines);
}

TEST_CASE("the Ocarina user is found in a module of the sound root's array") {
    FakeGuest g = PuppetWithSound();
    constexpr std::uint64_t kRoot = 0x4000a0000;
    constexpr std::uint64_t kModules = 0x4000b0000;
    constexpr std::uint64_t kModule = 0x4000c0000;
    constexpr std::uint64_t kOcarina = 0x400060000;
    constexpr std::uint64_t kOcarinaResource = 0x400070000;
    g.Put<std::uint64_t>(kBase + snd::addr::kSoundRootGot, kBase + 0x100000);
    g.Put<std::uint64_t>(kBase + 0x100000, kRoot);
    g.Put<std::int32_t>(kRoot + 0xc0, 2);
    g.Put<std::int32_t>(kRoot + 0xc4, 4);
    g.Put<std::uint64_t>(kRoot + 0xc8, kModules);
    g.Put<std::uint64_t>(kModules, kModule + 0x800);  // another module, nothing in it
    g.Put<std::uint64_t>(kModules + 8, kModule);
    g.Put<std::uint64_t>(kModule + 0x40, kOcarina);
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserResource, kOcarinaResource);
    g.Put<std::uint64_t>(kOcarinaResource + snd::addr::kResourceAccessor, 0x400080000);
    g.Put<std::uint64_t>(kOcarinaResource + snd::addr::kResourceName, kOcarinaResource + 0x100);
    g.PutText(kOcarinaResource + 0x100, "Ocarina");
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserFirstLink, kOcarina + snd::addr::kUserEvents);
    g.Put<std::int32_t>(kOcarina + snd::addr::kUserLinkOffset, 0x10);
    CHECK(snd::SoundRoot(g, kBase) == kRoot);
    std::string how;
    CHECK(snd::FindUserInModules(g, kRoot, "Ocarina", &how) == kOcarina);
    CHECK(how == "module 1 of the array at root +0xc0, field +0x40");

    Run run;
    snd::State st;
    auto c = snd::Next(st, snd::Wanted{4, 1, 0}, g, kBase, kActor, kBlock, 10, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->x[0] == 0x400080000);
    CHECK(run.said.back().find("module 1 of the array at root +0xc0") != std::string::npos);
}

TEST_CASE("the Ocarina module is found by its vtable one object below the sound root") {
    FakeGuest g = PuppetWithSound();
    constexpr std::uint64_t kRoot = 0x4000a0000;
    constexpr std::uint64_t kChild = 0x4000d0000;
    constexpr std::uint64_t kModules = 0x4000b0000;
    constexpr std::uint64_t kModule = 0x4000c0000;
    constexpr std::uint64_t kOcarina = 0x400060000;
    constexpr std::uint64_t kOcarinaResource = 0x400070000;
    constexpr std::uint64_t kVtableBase = kBase + 0x4b00000;
    g.Put<std::uint64_t>(kBase + snd::addr::kSoundRootGot, kBase + 0x100000);
    g.Put<std::uint64_t>(kBase + 0x100000, kRoot);
    g.Put<std::uint64_t>(kBase + snd::addr::kOcarinaModuleVtableGot, kVtableBase);
    g.Put<std::uint64_t>(kRoot + 0x48, kChild);
    g.Put<std::int32_t>(kChild + 0x10, 1);
    g.Put<std::int32_t>(kChild + 0x14, 2);
    g.Put<std::uint64_t>(kChild + 0x18, kModules);
    g.Put<std::uint64_t>(kModules, kModule);
    g.Put<std::uint64_t>(kModule, kVtableBase + 0x10);
    g.Put<std::uint64_t>(kModule + snd::addr::kModuleUser, kOcarina);
    // The user's name is not readable: found by the module anyway.
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserResource, kOcarinaResource);
    g.Put<std::uint64_t>(kOcarinaResource + snd::addr::kResourceAccessor, 0x400080000);
    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserModuleBack, kModule + 0x38);
    std::string how;
    CHECK(snd::FindOcarinaModule(g, kBase, kRoot, &how) == kModule);
    CHECK(how == "root +0x48 -> array +0x10 [0]");
    CHECK(snd::DescribeModule(g, kModule).find("back link ok") != std::string::npos);

    Run run;
    snd::State st;
    auto c = snd::Next(st, snd::Wanted{4, 1, 0}, g, kBase, kActor, kBlock, 10, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->x[0] == 0x400080000);
    CHECK(run.said.back().find("Ocarina module at root +0x48 -> array +0x10 [0]") != std::string::npos);
}

TEST_CASE("the Ocarina module's user is taken on a later note once it has its resource") {
    FakeGuest g = PuppetWithSound();
    constexpr std::uint64_t kRoot = 0x4000a0000;
    constexpr std::uint64_t kModules = 0x4000b0000;
    constexpr std::uint64_t kModule = 0x4000c0000;
    constexpr std::uint64_t kOcarina = 0x400060000;
    constexpr std::uint64_t kOcarinaResource = 0x400070000;
    constexpr std::uint64_t kVtableBase = kBase + 0x4b00000;
    g.Put<std::uint64_t>(kBase + snd::addr::kSoundRootGot, kBase + 0x100000);
    g.Put<std::uint64_t>(kBase + 0x100000, kRoot);
    g.Put<std::uint64_t>(kBase + snd::addr::kOcarinaModuleVtableGot, kVtableBase);
    g.Put<std::int32_t>(kRoot + 0xc0, 1);
    g.Put<std::int32_t>(kRoot + 0xc4, 1);
    g.Put<std::uint64_t>(kRoot + 0xc8, kModules);
    g.Put<std::uint64_t>(kModules, kModule);
    g.Put<std::uint64_t>(kModule, kVtableBase + 0x10);
    g.Put<std::uint64_t>(kModule + snd::addr::kModuleUser, kOcarina);  // no resource yet

    Run run;
    snd::State st;
    auto c = snd::Next(st, snd::Wanted{4, 1, 0}, g, kBase, kActor, kBlock, 10, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->x[0] != 0x400080000);  // its own user's accessor
    snd::Finish(st, 0, "P1", run.say());

    g.Put<std::uint64_t>(kOcarina + snd::addr::kUserResource, kOcarinaResource);
    g.Put<std::uint64_t>(kOcarinaResource + snd::addr::kResourceAccessor, 0x400080000);
    c = snd::Next(st, snd::Wanted{6, 2, 500}, g, kBase, kActor, kBlock, 510, "P1", run.say());
    REQUIRE(c.has_value());
    CHECK(c->x[0] == 0x400080000);
    CHECK(run.said.back().find("on a later note") != std::string::npos);
}
