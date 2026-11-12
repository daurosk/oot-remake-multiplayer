// Tests for each game's quests catching up with the shared story
// (lichii/hook/coop_quest.cpp, issue 173).
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>

#include "coop_quest.h"
#include "coop_story.h"

namespace {

// Murmur3 x86 32, seed 0 (as coop_story's names).
std::uint32_t Hash(const std::string& s) {
    const auto* d = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size() / 4;
    std::uint32_t h = 0;
    const auto rotl = [](std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t k = d[4 * i] | (d[4 * i + 1] << 8) | (d[4 * i + 2] << 16) |
                          (static_cast<std::uint32_t>(d[4 * i + 3]) << 24);
        k *= 0xcc9e2d51u;
        k = rotl(k, 15);
        k *= 0x1b873593u;
        h ^= k;
        h = rotl(h, 13);
        h = h * 5 + 0xe6546b64u;
    }
    std::uint32_t k = 0;
    const auto* t = d + 4 * n;
    switch (s.size() & 3) {
    case 3:
        k ^= t[2] << 16;
        [[fallthrough]];
    case 2:
        k ^= t[1] << 8;
        [[fallthrough]];
    case 1:
        k ^= t[0];
        k *= 0xcc9e2d51u;
        k = rotl(k, 15);
        k *= 0x1b873593u;
        h ^= k;
    }
    h ^= static_cast<std::uint32_t>(s.size());
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

const CoopQuest::Quest& Q(const char* name) {
    for (const auto& q : CoopQuest::Quests()) {
        if (std::string(q.name) == name) return q;
    }
    CHECK_MESSAGE(false, name);
    return CoopQuest::Quests().front();
}

struct Game {
    std::map<std::uint32_t, bool> bools;
    std::map<std::uint32_t, std::uint32_t> enums;
    CoopQuest::ReadBool flag = [this](std::uint32_t h) -> std::optional<bool> {
        auto it = bools.find(h);
        if (it == bools.end()) return std::nullopt;
        return it->second;
    };
    CoopQuest::ReadEnum value = [this](std::uint32_t h) -> std::optional<std::uint32_t> {
        auto it = enums.find(h);
        if (it == enums.end()) return std::nullopt;
        return it->second;
    };
    // One frame of the hook: every quest in order, written at once.
    void Frame() {
        for (const auto& q : CoopQuest::Quests()) {
            if (const auto r = CoopQuest::CatchUp(q, flag, value)) enums[q.hash] = *r;
        }
    }
    void Played(const char* scene) { bools[Hash(std::string("HavePlayedEvent.") + scene)] = true; }
};

const std::uint32_t kGoto = Hash("Goto_HyruleCastle");  // not the Enum's hash: see below

} // namespace

TEST_CASE("quest: the step hashes are the step names; each quest's steps are distinct") {
    for (const auto& q : CoopQuest::Quests()) {
        CHECK(q.steps.front() == Hash("NotReady"));
        CHECK(q.steps.back() == Hash("Complete"));
        for (std::size_t i = 0; i < q.steps.size(); ++i) {
            CHECK(CoopQuest::Rank(q, q.steps[i]) == i);
        }
        for (const auto& r : q.rules) {
            CHECK(CoopQuest::Rank(q, r.step).has_value());
            // Every rule flag is a shared story flag.
            CHECK(CoopStory::Find(r.flag) != nullptr);
        }
    }
    const auto& g = Q("Goto_HyruleCastle");
    CHECK(g.hash == 0xdb89c1a9u);
    CHECK(g.steps[3] == Hash("Step02"));
    CHECK(g.steps[8] == Hash("Step04"));
    const auto& d = Q("DoorOfTime");
    CHECK(d.hash == 0x378913cau);
    CHECK(d.steps[2] == Hash("Step01"));
    CHECK(d.steps[5] == Hash("Srep2_1_1"));
    CHECK(kGoto != g.hash);  // the Enum's hash is not its plain name's
}

TEST_CASE("quest: after A met Zelda, B's quests reach Complete and Step01 (Impa comes)") {
    const auto& g = Q("Goto_HyruleCastle");
    const auto& d = Q("DoorOfTime");
    Game b;
    b.enums[g.hash] = Hash("Step03_02");  // B is in the castle grounds
    b.enums[d.hash] = Hash("NotReady");
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Step03_02"));  // nothing shared yet
    CHECK(b.enums[d.hash] == Hash("NotReady"));
    // A's meeting: scenes 0030..0060 reach B as shared flags.
    for (const char* s : {"Dm_HrlCastle_0030", "Dm_HrlCastle_0035", "Dm_HrlCastle_0045",
                          "Dm_HrlCastle_0050", "Dm_HrlCastle_0060"}) {
        b.Played(s);
    }
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Complete"));
    CHECK(b.enums[d.hash] == Hash("Step01"));
    // Lullaby played by A: B stays at Step01, so B's Impa still takes B out.
    b.Played("Dm_MelodyGet_Zelda");
    b.Frame();
    CHECK(b.enums[d.hash] == Hash("Step01"));
}

TEST_CASE("quest: never lowered, never from before our own Deku Tree, unknown left alone") {
    const auto& g = Q("Goto_HyruleCastle");
    const auto& d = Q("DoorOfTime");
    Game ahead;
    ahead.enums[g.hash] = Hash("Complete");
    ahead.enums[d.hash] = Hash("Step2_2");
    ahead.Played("Dm_HrlCastle_0060");
    ahead.Played("Dm_HrlField_0010");
    ahead.Frame();
    CHECK(ahead.enums[g.hash] == Hash("Complete"));
    CHECK(ahead.enums[d.hash] == Hash("Step2_2"));

    Game early;  // a game still before the Deku Tree: Goto NotReady
    early.enums[g.hash] = Hash("NotReady");
    early.enums[d.hash] = Hash("NotReady");
    early.Played("Dm_HrlCastle_0060");
    early.Frame();
    CHECK(early.enums[g.hash] == Hash("NotReady"));
    CHECK(early.enums[d.hash] == Hash("NotReady"));  // needs our Goto complete

    Game odd;
    odd.enums[g.hash] = 0x12345678u;
    odd.enums[d.hash] = Hash("NotReady");
    odd.Played("Dm_HrlCastle_0060");
    odd.Frame();
    CHECK(odd.enums[g.hash] == 0x12345678u);
    CHECK(odd.enums[d.hash] == Hash("NotReady"));

    Game unread;  // nothing readable: nothing written
    unread.Played("Dm_HrlCastle_0060");
    unread.Frame();
    CHECK(unread.enums.empty());
}

TEST_CASE("quest: the owl and Talon move Goto_HyruleCastle on, the highest rule wins") {
    const auto& g = Q("Goto_HyruleCastle");
    Game b;
    b.enums[g.hash] = Hash("Step01");  // after B's Deku Tree
    b.Played("Dm_HrlField_0010");
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Step02"));
    b.Played("Dm_HrlCastle_0020");
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Step04"));

    Game c;
    c.enums[g.hash] = Hash("Step01");
    c.Played("Dm_HrlField_0010");
    c.Played("Dm_HrlCastle_0060");
    c.Frame();
    CHECK(c.enums[g.hash] == Hash("Complete"));
}
