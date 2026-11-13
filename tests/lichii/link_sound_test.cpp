// Tests for the other Links' sounds (lichii/hook/coop_link_sound.cpp): what the
// emit wrap keeps, the sender's and the listener's limits, and the game calls
// that start a sound on a puppet. Guest memory is fake pages.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_link_sound.h"
#include "coop_puppet.h"
#include "coop_puppet_sound.h"

namespace {

using namespace CoopLinkSound;

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
    CoopLinkSound::Read AsRead() {
        return [this](std::uint64_t a, void* out, std::size_t n) { return this->Read(a, out, n); };
    }

private:
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages_;
};

constexpr std::uint64_t kOurUser = 0x10000;
constexpr std::uint64_t kCell = 0x20000;   // the emit's asset cell
constexpr std::uint64_t kAsset = 0x21000;  // the asset: its name's char* first
constexpr std::uint64_t kName = 0x22000;

void PutAsset(FakeGuest& g, const std::string& name) {
    g.Put<std::uint64_t>(kCell, kAsset);
    g.Put<std::uint64_t>(kAsset, kName);
    g.PutText(kName, name);
}

void Reset() {
    SetOurUser(0);
    SetProbe(false);
    TakeCaught();
    TakeProbeLines();
}

struct Clean {  // the catcher is global: each test starts and ends with it empty
    Clean() { Reset(); }
    ~Clean() { Reset(); }
};

}  // namespace

TEST_CASE("link sound: the emit wrap keeps only our own Link's names, read from [[cell]]") {
    Clean clean;
    FakeGuest g;
    PutAsset(g, "Pl_Voice_Attack_00");
    CHECK_FALSE(OnEmit(kOurUser, kCell, g.AsRead()));  // our user not known yet
    SetOurUser(kOurUser);
    CHECK_FALSE(OnEmit(kOurUser + 8, kCell, g.AsRead()));  // another actor's user
    CHECK(OnEmit(kOurUser, kCell, g.AsRead()));
    CHECK_FALSE(OnEmit(kOurUser, 0, g.AsRead()));
    CHECK_FALSE(OnEmit(kOurUser, 0x90000, g.AsRead()));  // unreadable
    CHECK(TakeCaught() == std::vector<std::string>{"Pl_Voice_Attack_00"});
    CHECK(TakeCaught().empty());
}

TEST_CASE("link sound: names the protocol refuses are not kept") {
    Clean clean;
    FakeGuest g;
    SetOurUser(kOurUser);
    PutAsset(g, "Pl Voice");
    CHECK_FALSE(OnEmit(kOurUser, kCell, g.AsRead()));
    PutAsset(g, "");
    CHECK_FALSE(OnEmit(kOurUser, kCell, g.AsRead()));
    PutAsset(g, std::string(kNameMax + 1, 'a'));  // no end within the longest name
    CHECK(NameAt(kCell, g.AsRead()).empty());
    PutAsset(g, std::string(kNameMax, 'a'));
    CHECK(NameAt(kCell, g.AsRead()) == std::string(kNameMax, 'a'));
    CHECK(TakeCaught().empty());
}

TEST_CASE("link sound: at most kCaughtMax names wait between two frames") {
    Clean clean;
    FakeGuest g;
    SetOurUser(kOurUser);
    PutAsset(g, "Pl_FootStep");
    for (std::size_t i = 0; i < kCaughtMax + 10; ++i) {
        OnEmit(kOurUser, kCell, g.AsRead());
    }
    CHECK(TakeCaught().size() == kCaughtMax);
}

TEST_CASE("link sound: the probe names each user's sound once, ours marked") {
    Clean clean;
    FakeGuest g;
    SetOurUser(kOurUser);
    SetProbe(true);
    PutAsset(g, "Ocarina_D");
    CHECK_FALSE(OnEmit(0x30000, kCell, g.AsRead()));  // logged, not sent
    OnEmit(0x30000, kCell, g.AsRead());
    CHECK(OnEmit(kOurUser, kCell, g.AsRead()));
    const auto lines = TakeProbeLines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "coop: sound probe: user 0x30000 emits 'Ocarina_D'");
    CHECK(lines[1] == "coop: sound probe: user 0x10000 (our Link) emits 'Ocarina_D'");
    CHECK(TakeCaught().size() == 1);
}

TEST_CASE("link sound: the sender drops a quick repeat and keeps to kSendPerSecond") {
    Limiter l;
    CHECK(l.Allow("Pl_FootStep", 1000));
    CHECK_FALSE(l.Allow("Pl_FootStep", 1000 + kRepeatMs - 1));
    CHECK(l.Allow("Pl_FootStep", 1000 + kRepeatMs));
    CHECK(l.Allow("Pl_Voice", 1000 + kRepeatMs));  // another name: its own repeat time
    Limiter r;
    std::uint32_t sent = 0;
    for (std::uint32_t i = 0; i < 100; ++i) {
        sent += r.Allow("S" + std::to_string(i), 5000 + i) ? 1 : 0;  // 100 names in 100 ms
    }
    CHECK(sent == kSendPerSecond);
    CHECK_FALSE(r.Allow("late", 5999));
    CHECK(r.Allow("later", 6000));  // the first of that second has gone
}

TEST_CASE("link sound: heard only near our Link") {
    const std::array<float, 3> from{100.0f, 0.0f, 0.0f};
    CHECK_FALSE(InHearing(std::nullopt, from));
    CHECK(InHearing(std::array<float, 3>{100.0f, 0.0f, kHearDistance}, from));
    CHECK_FALSE(InHearing(std::array<float, 3>{100.0f, 0.0f, kHearDistance + 0.5f}, from));
}

TEST_CASE("link sound: a player's queue keeps the newest and drops stale ones") {
    Heard h;
    for (std::size_t i = 0; i < kHeardMax + 2; ++i) {
        h.Push("S" + std::to_string(i), 1000);
    }
    CHECK(h.Size() == kHeardMax);
    CHECK(h.Pop(1000)->name == "S2");  // the two oldest went
    Heard s;
    s.Push("old", 1000);
    s.Push("new", 1000 + kHeardStaleMs);
    CHECK(s.Pop(1000 + kHeardStaleMs + 1)->name == "new");
    CHECK_FALSE(s.Pop(1000 + kHeardStaleMs + 1));
}

TEST_CASE("link sound: the listener's budget is kPlayPerSecond for all puppets") {
    Budget b;
    std::uint32_t taken = 0;
    for (std::uint32_t i = 0; i < 100; ++i) {
        taken += b.Take(1000 + i) ? 1 : 0;
    }
    CHECK(taken == kPlayPerSecond);
    CHECK(b.Take(2000));
}

TEST_CASE("link sound: a list of playing names is matched name by name") {
    CHECK(AlreadyPlaying("Pl_FootStep, Pl_Voice", "Pl_Voice"));
    CHECK(AlreadyPlaying("Pl_FootStep", "Pl_FootStep"));
    CHECK_FALSE(AlreadyPlaying("Pl_FootStep_Grass", "Pl_FootStep"));
    CHECK_FALSE(AlreadyPlaying("", "Pl_FootStep"));
}

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kActor = 0x40000;
constexpr std::uint64_t kUser = 0x50000;
constexpr std::uint64_t kResource = 0x51000;
constexpr std::uint64_t kAccessor = 0x52000;
constexpr std::uint64_t kBlock = 0x60000;

FakeGuest Puppet() {
    FakeGuest g;
    g.Put<std::uint64_t>(kUser + addr::kUserResource, kResource);
    g.Put<std::uint64_t>(kResource + addr::kResourceAccessor, kAccessor);
    g.Map(kBlock, CoopPuppet::block::kSize);
    return g;
}

}  // namespace

TEST_CASE("link sound: a puppet looks the asset up once, then starts it with the game's emit") {
    FakeGuest g = Puppet();
    Player p;
    Budget b;
    std::vector<std::string> said;
    const auto say = [&said](const std::string& s) { said.push_back(s); };
    const auto none = [] { return std::string(); };
    p.heard.Push("Pl_Voice_Attack_00", 1000);
    // 1. the search, in its sound user's accessor, the name in the block's text
    auto c = Next(p, b, g, kMain, kActor, kUser, kBlock, 1000, none, "P2", say, true);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSearchAsset);
    CHECK(c->x[0] == kAccessor);
    CHECK(c->x[1] == kBlock + CoopPuppetSound::block::kAsset);
    CHECK(c->x[2] == kBlock + CoopPuppetSound::block::kText);
    CHECK(g.TextAt(kBlock + CoopPuppetSound::block::kText) == "Pl_Voice_Attack_00");
    CHECK_FALSE(Next(p, b, g, kMain, kActor, kUser, kBlock, 1000, none, "P2", say, true));  // one at a time
    g.Put<std::uint64_t>(kBlock + CoopPuppetSound::block::kAsset, 0xa55e7);  // what the game's search wrote
    Finish(p, 1, g, kBlock, "P2", say, true);
    // 2. the emit at once, on the puppet's user, with a fresh handle
    c = Next(p, b, g, kMain, kActor, kUser, kBlock, 1001, none, "P2", say, true);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kEmit);
    CHECK(c->x[0] == kUser);
    CHECK(c->x[1] == kBlock + CoopPuppetSound::block::kAsset);
    CHECK(c->x[2] == kBlock + block::kHandle);
    CHECK(c->x[3] == 0);
    CHECK(g.Get<std::uint32_t>(kBlock + block::kHandle) == 0xffff00ffu);
    Finish(p, 1, g, kBlock, "P2", say, true);
    CHECK_FALSE(Next(p, b, g, kMain, kActor, kUser, kBlock, 1002, none, "P2", say, true));
    // 3. the same name again: no search, the asset kept
    g.Put<std::uint64_t>(kBlock + CoopPuppetSound::block::kAsset, 0);
    p.heard.Push("Pl_Voice_Attack_00", 2000);
    c = Next(p, b, g, kMain, kActor, kUser, kBlock, 2000, none, "P2", say, true);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kEmit);
    CHECK(g.Get<std::uint64_t>(kBlock + CoopPuppetSound::block::kAsset) == 0xa55e7);
    Finish(p, 1, g, kBlock, "P2", say, true);
    CHECK(said == std::vector<std::string>{"P2 sound 'Pl_Voice_Attack_00'", "P2 sound 'Pl_Voice_Attack_00'"});
}

TEST_CASE("link sound: a sound its own animation already plays, or one not in its user, is not started") {
    FakeGuest g = Puppet();
    Player p;
    Budget b;
    const auto say = [](const std::string&) {};
    const auto none = [] { return std::string(); };
    p.heard.Push("Missing", 1000);
    REQUIRE(Next(p, b, g, kMain, kActor, kUser, kBlock, 1000, none, "P2", say, false));
    Finish(p, 0, g, kBlock, "P2", say, false);  // the search found nothing
    p.heard.Push("Missing", 1100);
    CHECK_FALSE(Next(p, b, g, kMain, kActor, kUser, kBlock, 1100, none, "P2", say, false));  // not looked for again
    p.assets["Pl_FootStep"] = 0xf00;
    p.heard.Push("Pl_FootStep", 1200);
    CHECK_FALSE(Next(p, b, g, kMain, kActor, kUser, kBlock, 1200,
                     [] { return std::string("Pl_FootStep, Pl_Voice"); }, "P2", say, false));
    CHECK(p.heard.Empty());
}

TEST_CASE("link sound: a new puppet forgets the assets, and no sound user plays nothing") {
    FakeGuest g = Puppet();
    Player p;
    Budget b;
    std::vector<std::string> said;
    const auto say = [&said](const std::string& s) { said.push_back(s); };
    const auto none = [] { return std::string(); };
    p.actor = kActor;
    p.user = kUser;
    p.assets["Pl_FootStep"] = 0xf00;
    p.heard.Push("Pl_FootStep", 1000);
    auto c = Next(p, b, g, kMain, kActor + 0x1000, kUser, kBlock, 1000, none, "P2", say, false);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSearchAsset);  // looked up again for the new puppet
    Finish(p, 0, g, kBlock, "P2", say, false);
    p.heard.Push("Pl_FootStep", 1100);
    CHECK_FALSE(Next(p, b, g, kMain, kActor + 0x1000, 0, kBlock, 1100, none, "P2", say, false));
    CHECK(p.heard.Empty());
    CHECK(said == std::vector<std::string>{"P2 has no sound user: the player's sounds are not played"});
}

TEST_CASE("link sound: past the budget a sound waits for a later frame, its age kept") {
    FakeGuest g = Puppet();
    Player p;
    Budget b;
    for (std::uint32_t i = 0; i < kPlayPerSecond; ++i) {
        b.Take(1000);
    }
    const auto say = [](const std::string&) {};
    const auto none = [] { return std::string(); };
    p.assets["Pl_FootStep"] = 0xf00;
    p.heard.Push("Pl_FootStep", 1000);
    CHECK_FALSE(Next(p, b, g, kMain, kActor, kUser, kBlock, 1100, none, "P2", say, false));
    CHECK(p.heard.Size() == 1);
    CHECK(Next(p, b, g, kMain, kActor, kUser, kBlock, 2000, none, "P2", say, false) == std::nullopt);  // stale by then
    CHECK(p.heard.Empty());
}
