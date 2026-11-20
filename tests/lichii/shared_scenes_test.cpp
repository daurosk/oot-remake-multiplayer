// Tests for shared story cutscenes (lichii/hook/coop_shared_scenes.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <ostream>   // (CHECK_MESSAGE prints a string_view: MSVC needs operator<< declared)
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/protocol/codec.hpp"
#include "coop_live_events.h"
#include "coop_shared_scenes.h"

namespace {

using CoopSharedScenes::Here;
using CoopSharedScenes::Joiner;
using CoopSharedScenes::Vec3;

constexpr Vec3 kBridge{10.0f, 0.0f, 20.0f};

Here Free(std::uint64_t now_us, Vec3 link = {15.0f, 0.0f, 22.0f}, std::string scene = "KokiriForest") {
    Here h;
    h.scene = std::move(scene);
    h.can_start = true;
    h.link = link;
    h.now_us = now_us;
    return h;
}

const auto kNotPlayed = [](const std::string&) { return false; };

} // namespace

TEST_CASE("the story cutscene list is sorted, has the story scenes and leaves out the rest") {
    const auto names = CoopSharedScenes::Names();
    CHECK(names.size() > 150);
    CHECK(std::is_sorted(names.begin(), names.end()));
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
    CHECK(CoopSharedScenes::IsShared("Dm_Kokiri_0030"));
    CHECK(CoopSharedScenes::IsShared("Meet_DekuTree"));
    CHECK(CoopSharedScenes::IsShared("Get_StoneOfForest"));
    CHECK_FALSE(CoopSharedScenes::IsShared("Dm_Kokiri_0000"));       // the opening
    CHECK_FALSE(CoopSharedScenes::IsShared("Dm_Deku_BossIn"));       // dungeons-boss
    CHECK_FALSE(CoopSharedScenes::IsShared("Dm_SY_BancChangeArrive"));
    CHECK_FALSE(CoopSharedScenes::IsShared("Dm_TimeTemple_0010"));   // time travel
    CHECK_FALSE(CoopSharedScenes::IsShared("Dm_Awake_Forest_Effect"));
    CHECK_FALSE(CoopSharedScenes::IsShared("Npc_Kokiri_Saria_Child"));  // talks stay local
    CHECK_FALSE(CoopSharedScenes::IsShared("InsideTheDekuTree_Gimmick_RoomD_PlatformUp"));
    // None of them is a gimmick event (those go by coop_live_events' rules).
    for (const auto& p : CoopLiveEvents::Places()) {
        CHECK_FALSE(CoopSharedScenes::IsShared(p.event));
    }
}

TEST_CASE("another player's story cutscene near us plays here once, at once") {
    Joiner j;
    CHECK_FALSE(j.Next(Free(0), kNotPlayed));
    j.Started(1, "Dm_Kokiri_0030", "KokiriForest", kBridge, 100);
    const auto got = j.Next(Free(120), kNotPlayed);
    REQUIRE(got);
    CHECK(*got == "Dm_Kokiri_0030");
    const auto notes = j.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("playing it here too") != std::string::npos);
    // Our watch then finds it running here: not sent back.
    CHECK_FALSE(j.Ours("Dm_Kokiri_0030", "KokiriForest"));
    // The same start again this visit: not played twice.
    j.Started(2, "Dm_Kokiri_0030", "KokiriForest", kBridge, 200);
    CHECK_FALSE(j.Next(Free(220), kNotPlayed));
    CHECK(j.TakeNotes()[0].find("played here already") != std::string::npos);
}

TEST_CASE("a story cutscene our game starts goes out once, a later start from another player is not replayed") {
    const std::string kMeet = "KokiriForest_Meet_DekuTree_MeetDekuTree_01";  // the Deku Tree's meeting (a setting)
    Joiner j;
    CHECK(j.Ours(kMeet, "KokiriForest"));
    CHECK_FALSE(j.Ours(kMeet, "KokiriForest"));  // the watch finds it again: sent once
    j.Started(1, kMeet, "KokiriForest", kBridge, 10);  // both started it together
    CHECK_FALSE(j.Next(Free(20), kNotPlayed));
    // A new visit of the scene: it may go out again.
    CHECK_FALSE(j.Next(Free(30, kBridge, "LostWoods"), kNotPlayed));
    CHECK(j.Ours(kMeet, "KokiriForest"));
}

TEST_CASE("players not in the room, in another scene, or with it played are not pulled in") {
    Joiner j;
    j.Started(1, "Dm_Kokiri_0030", "KokiriForest", Vec3{100.0f, 0.0f, 20.0f}, 0);
    CHECK_FALSE(j.Next(Free(10), kNotPlayed));
    CHECK(j.TakeNotes()[0].find("too far") != std::string::npos);

    j.Started(1, "Dm_Kokiri_0045", "KokiriForest", kBridge, 0);
    CHECK_FALSE(j.Next(Free(10, kBridge, "LostWoods"), kNotPlayed));
    CHECK(j.Waiting() == 0);  // another scene: dropped

    j.Started(1, "Dm_LostWood_0010", "LostWoods", kBridge, 20);
    CHECK_FALSE(j.Next(Free(30, kBridge, "LostWoods"), [](const std::string&) { return true; }));
    CHECK(j.TakeNotes().back().find("has it played already") != std::string::npos);

    j.Started(1, "Dm_LostWood_0021", "LostWoods", std::nullopt, 40);
    CHECK_FALSE(j.Next(Free(50, kBridge, "LostWoods"), kNotPlayed));
    CHECK(j.TakeNotes().back().find("not known") != std::string::npos);
}

TEST_CASE("a busy game waits up to 3 s, then skips the cutscene") {
    Joiner j;
    j.Started(1, "Dm_Kokiri_0030", "KokiriForest", kBridge, 1'000'000);
    Here busy = Free(1'500'000);
    busy.can_start = false;  // our own talk, a menu or a pause
    CHECK_FALSE(j.Next(busy, kNotPlayed));
    CHECK(j.Waiting() == 1);
    CHECK(j.TakeNotes().empty());
    // Free again within the time: joined late.
    REQUIRE(j.Next(Free(2'000'000), kNotPlayed));

    j.Started(1, "Dm_Kokiri_0045", "KokiriForest", kBridge, 5'000'000);
    Here loading;
    loading.now_us = 6'000'000;  // a load: no scene, no Link
    CHECK_FALSE(j.Next(loading, kNotPlayed));
    CHECK(j.Waiting() == 1);
    busy.now_us = 8'000'000;
    CHECK_FALSE(j.Next(busy, kNotPlayed));
    CHECK(j.Waiting() == 0);
    CHECK(j.TakeNotes().back().find("busy for 3 s") != std::string::npos);
}

TEST_CASE("LICHII_COOP_SHARED_CUTSCENES: on unless 0") {
    CHECK(CoopSharedScenes::OnFromEnv(""));
    CHECK(CoopSharedScenes::OnFromEnv("1"));
    CHECK_FALSE(CoopSharedScenes::OnFromEnv("0"));
}

TEST_CASE("in a dungeon a story cutscene started in another room is not played here") {
    // 12 m apart, but through a wall: the starter's room is not loaded in our
    // game, and the cutscene would show a black picture until it ends.
    Joiner j;
    Here h = Free(120, {15.0f, 0.0f, 22.0f}, "ForestTemple");
    h.dungeon = true;
    h.room = 4;
    j.Started(1, "Dm_ForestTemple_0010", "ForestTemple", Vec3{15.0f, 0.0f, 10.0f}, 100, 7);
    CHECK_FALSE(j.Next(h, kNotPlayed));
    CHECK(j.Waiting() == 1);  // not played from here, held for when we come into their room (row 176)
    const auto notes = j.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("another room of this dungeon") != std::string::npos);
    CHECK(notes[0].find("held") != std::string::npos);
    // The same room: played (the held one in the other room does not block it).
    j.Started(1, "Dm_ForestTemple_0011", "ForestTemple", Vec3{15.0f, 0.0f, 10.0f}, 200, 4);
    const auto got = j.Next(h, kNotPlayed);
    REQUIRE(got);
    CHECK(*got == "Dm_ForestTemple_0011");
}

TEST_CASE("row 176: a start we were too far from is held and joined when we come near while it is not played here") {
    // Run 20261119-222229: player 2 was 81 m away when player 1 started Dm_ForestTemple_0015 (Meg); it walked into
    // her area while the cutscene still played and its own room logic started a second one: two Megs, two owners.
    const Vec3 kMeg{0.0f, 100.0f, -3.5f};
    Joiner j;
    Here far = Free(1'000'000, Vec3{0.0f, 101.0f, 77.0f}, "ForestTemple");  // the entrance, 81 m
    far.dungeon = true;
    far.room = 1;
    j.Started(1, "Dm_ForestTemple_0015", "ForestTemple", kMeg, 900'000, 2);
    CHECK_FALSE(j.Next(far, kNotPlayed));
    CHECK(j.Waiting() == 1);
    CHECK(j.TakeNotes().at(0).find("too far") != std::string::npos);
    far.now_us = 5'000'000;
    CHECK_FALSE(j.Next(far, kNotPlayed));
    CHECK(j.TakeNotes().empty());  // noted once
    // In the room now, but our game busy (a talk): it waits 3 s from when we came near, not from the start.
    Here near = Free(20'000'000, Vec3{12.0f, 100.0f, -3.5f}, "ForestTemple");
    near.dungeon = true;
    near.room = 2;
    near.can_start = false;
    CHECK_FALSE(j.Next(near, kNotPlayed));
    CHECK(j.Waiting() == 1);
    near.can_start = true;
    near.now_us = 21'000'000;
    CHECK(j.Next(near, kNotPlayed) == std::optional<std::string>("Dm_ForestTemple_0015"));
    CHECK(j.TakeNotes().back().find("now near us: playing it here too (late)") != std::string::npos);
    CHECK_FALSE(j.Ours("Dm_ForestTemple_0015", "ForestTemple"));  // joined: not sent back
}

TEST_CASE("row 176: a held start is dropped once our save has it played, or after kHoldUs") {
    const Vec3 kFar{0.0f, 0.0f, 200.0f};
    Joiner j;
    j.Started(1, "Dm_Kokiri_0030", "KokiriForest", kFar, 0);
    CHECK_FALSE(j.Next(Free(10), kNotPlayed));
    CHECK(j.Waiting() == 1);
    // The starter's result came by the flags: never played here.
    CHECK_FALSE(j.Next(Free(20), [](const std::string&) { return true; }));
    CHECK(j.Waiting() == 0);
    CHECK(j.TakeNotes().back().find("has it played already") != std::string::npos);

    j.Started(1, "Dm_Kokiri_0045", "KokiriForest", kFar, 0);
    CHECK_FALSE(j.Next(Free(10), kNotPlayed));
    CHECK(j.Waiting() == 1);
    CHECK_FALSE(j.Next(Free(CoopSharedScenes::kHoldUs + 1), kNotPlayed));
    CHECK(j.Waiting() == 0);
    CHECK(j.TakeNotes().back().find("too far") != std::string::npos);
}

TEST_CASE("row 176: our game starting a held start of another player is theirs late, not a new start sent out") {
    Joiner j;
    j.Started(2, "Dm_ForestTemple_0015", "ForestTemple", Vec3{0.0f, 100.0f, -3.5f}, 0);
    Here far = Free(10, Vec3{0.0f, 101.0f, 77.0f}, "ForestTemple");
    CHECK_FALSE(j.Next(far, kNotPlayed));
    j.TakeNotes();
    // A place into Meg's area: our room logic starts it before the join is asked.
    CHECK_FALSE(j.Ours("Dm_ForestTemple_0015", "ForestTemple"));
    CHECK(j.Waiting() == 0);
    CHECK(j.TakeNotes().at(0).find("which player 2 started here earlier (held for us): theirs, not sent") !=
          std::string::npos);
    // Without a held start ours goes out as before.
    CHECK(j.Ours("Dm_ForestTemple_0016", "ForestTemple"));
}

TEST_CASE("in a dungeon with no room known only a very near starter is joined") {
    Joiner j;
    Here h = Free(120, {15.0f, 0.0f, 22.0f}, "ForestTemple");
    h.dungeon = true;  // no room: no table
    j.Started(1, "Dm_ForestTemple_0010", "ForestTemple", Vec3{15.0f, 0.0f, 10.0f}, 100);  // 12 m
    CHECK_FALSE(j.Next(h, kNotPlayed));
    CHECK(j.TakeNotes().at(0).find("room in this dungeon is not known") != std::string::npos);
    j.Started(1, "Dm_ForestTemple_0011", "ForestTemple", Vec3{15.0f, 0.0f, 17.0f}, 100);  // 5 m
    CHECK(j.Next(h, kNotPlayed) == std::optional<std::string>("Dm_ForestTemple_0011"));
    // Our room known, theirs not (or the other way): the same short reach.
    h.room = 4;
    j.Started(1, "Dm_ForestTemple_0012", "ForestTemple", Vec3{15.0f, 0.0f, 10.0f}, 100);
    CHECK_FALSE(j.Next(h, kNotPlayed));
}

TEST_CASE("outside a dungeon rooms do not count: the 30 m rule alone") {
    Joiner j;
    Here h = Free(120);  // KokiriForest, not a dungeon
    h.room = 1;          // ignored
    j.Started(1, "Dm_Kokiri_0030", "KokiriForest", kBridge, 100, 2);
    CHECK(j.Next(h, kNotPlayed) == std::optional<std::string>("Dm_Kokiri_0030"));
}

TEST_CASE("intro-ocarina: Saria's gift is never started in a game still in the opening") {
    // A player's report (0.10.1, Longshot, hosting): a rupee taken in the intro gave the Fairy Ocarina and froze in
    // the cutscene: Dm_Kokiri_0150 (Saria on the bridge) asked in a game where Saria is not at the bridge yet.
    constexpr std::uint32_t kDekuTreeLast = 0xebaec07fu;  // HavePlayedEvent.Dm_Kokiri_0140
    constexpr std::uint32_t kExitBlocker = 0xe5dc5674u;   // KokiriForest_Npc_Kokiri_007_Child_Blocker_00
    std::optional<bool> deku_tree = false, blocker = true;
    const auto flag = [&](std::uint32_t h) -> std::optional<bool> {
        if (h == kDekuTreeLast) return deku_tree;
        if (h == kExitBlocker) return blocker;
        return std::nullopt;
    };
    CHECK(coop::protocol::flag_hash("HavePlayedEvent.Dm_Kokiri_0140") == kDekuTreeLast);
    CHECK(coop::protocol::flag_hash("KokiriForest_Npc_Kokiri_007_Child_Blocker_00") == kExitBlocker);
    CHECK(CoopSharedScenes::Needs("Dm_Kokiri_0150").size() == 2);
    for (const auto& n : CoopSharedScenes::Needs("Dm_Kokiri_0150")) {
        CHECK_MESSAGE(coop::protocol::flag_hash(n.what) == n.hash, n.what);
    }
    CHECK(CoopSharedScenes::Needs("Dm_Kokiri_0030").empty());
    CHECK_FALSE(CoopSharedScenes::GateHolds("Dm_Kokiri_0150", flag));
    CHECK(CoopSharedScenes::GateHolds("Dm_Kokiri_0030", flag));  // no gate

    // The opening: another player's (wrongly found) Saria's gift is skipped, not waited for.
    Joiner j;
    j.Started(1, "Dm_Kokiri_0150", "KokiriForest", kBridge, 100);
    CHECK_FALSE(j.Next(Free(120), kNotPlayed, flag));
    CHECK(j.Waiting() == 0);
    CHECK(j.TakeNotes().back().find("our story is not there yet") != std::string::npos);
    // Unreadable flags hold nothing either.
    deku_tree.reset();
    blocker.reset();
    CHECK_FALSE(CoopSharedScenes::GateHolds("Dm_Kokiri_0150", flag));

    // After the Deku Tree's last scene: played here too (a new visit of the forest).
    deku_tree = true;
    Joiner after;
    after.Started(1, "Dm_Kokiri_0150", "KokiriForest", kBridge, 100);
    const auto got = after.Next(Free(120), kNotPlayed, flag);
    REQUIRE(got);
    CHECK(*got == "Dm_Kokiri_0150");
    // Or the exit blocker gone (an open forest): it holds as well.
    deku_tree = false;
    blocker = false;
    CHECK(CoopSharedScenes::GateHolds("Dm_Kokiri_0150", flag));
    // Other scenes are not gated.
    blocker = true;
    Joiner other;
    other.Started(1, "Dm_Kokiri_0050", "KokiriForest", kBridge, 100);
    CHECK(other.Next(Free(120), kNotPlayed, flag) == std::optional<std::string>("Dm_Kokiri_0050"));
}

TEST_CASE("a story cutscene asked by its event setting is sent and joined by that setting (player report 110)") {
    using CoopSharedScenes::Askable;
    using CoopSharedScenes::IsStory;
    using CoopSharedScenes::IsStorySetting;
    using CoopSharedScenes::StoryOfRequest;
    // The Deku Tree's meeting, as the EventRequestActor in KokiriForest_Child asks it.
    constexpr std::string_view kEntry = "KokiriForest_Meet_DekuTree_MeetDekuTree_01";
    CHECK(StoryOfRequest(kEntry) == std::optional<std::string_view>("Meet_DekuTree"));
    CHECK(StoryOfRequest("Get_StoneOfFire_MeetDarnia") == std::optional<std::string_view>("Get_StoneOfFire"));
    CHECK(StoryOfRequest("Dm_Kokiri_0070") == std::optional<std::string_view>("Dm_Kokiri_0070"));
    CHECK_FALSE(StoryOfRequest("Npc_Kokiri_Mido_Child").has_value());                     // a talk
    CHECK_FALSE(StoryOfRequest("GoronCity_SecretHole_AppearTreasureBox").has_value());    // a gimmick
    CHECK_FALSE(StoryOfRequest("Get_StoneOfFire_AppearBoss").has_value());                // a boss event
    // The key is the setting: Ruto's meeting and her taking the stone are two shared settings of
    // Get_StoneOfWater in InsideJabuJabuBelly, not one flow name.
    CHECK(IsStorySetting("Get_StoneOfWater_MeetRuto"));
    CHECK(IsStorySetting("Get_StoneOfWater_RutoGetStone"));
    // Every named setting of a story flow is classified from the game's data (story_requests.py): shared ones
    // whether placed or asked another way (Impa at the castle, the Fire Temple's Gorons) ...
    CHECK(IsStorySetting("Get_StoneOfWater_TalkWithImpa"));
    CHECK(StoryOfRequest("Dm_FireTemple_0015") == std::optional<std::string_view>("Dm_FireTemple_0020"));
    // ... and not: a boss event, a dungeon gimmick, the player's own talk, a room event, and the ones whose flow
    // changes what is the player's own (read from the flow: the letter shown from a bottle, the fish given from
    // one, the ocarina input at the waterfall and Darunia's door, the bomb bag's chest, the Kokiri Sword equipped
    // from the pouch).
    for (const std::string_view n : {"Get_StoneOfWater_DestroyBoss", "StoneOfWater_ElevatedFloor",
                                     "Meet_Darunia_PlayOcarina", "Check_DaruniaDoor", "Talk_Deku_01_Attacked",
                                     "Get_StoneOfWater_ShowLetter", "Get_StoneOfWater_JabuJabuEat",
                                     "Get_StoneOfWater_WaterfallSplit", "GoronCity_PlayZeldasLullaby_OpenDaruniaRoom",
                                     "Get_StoneOfFire_GetBombBag", "Get_StoneOfForest_GetSword"}) {
        CHECK_MESSAGE(!IsStory(n), std::string(n));
    }
    // A story name whose flow changes what is the player's own (Talon woken: the Cucco taken from the pouch) is
    // neither sent nor joined; a gift of an item co-op shares is (Zelda's letter, Saria's ocarina).
    CHECK(CoopSharedScenes::IsShared("Dm_HrlCastle_0020"));
    CHECK_FALSE(Askable("Dm_HrlCastle_0020"));
    CHECK(CoopSharedScenes::NotShared("Dm_HrlCastle_0020") == "it changes the player's own things");
    CHECK(Askable("Dm_HrlCastle_0050"));
    CHECK(Askable("Dm_Kokiri_0155"));
    CHECK(CoopSharedScenes::NotShared("Meet_GreatFairy") == "the game has no event setting of that name");
    CHECK(CoopSharedScenes::NotShared("Dm_Kokiri_0070").empty());
    // A Great Fairy's fountain: its flow has the paid heal (Dm_FairyFountain_Din:Pay, 500 rupees), read once the
    // reader took a switch's cases right (8 bytes each; the old 0x10 never reached it): the player's own.
    CHECK_FALSE(Askable("Dm_FairyFountain_Din"));
    CHECK(CoopSharedScenes::NotShared("Dm_FairyFountain_Din") == "it changes the player's own things");
    CHECK(IsStory(kEntry));
    CHECK(IsStory("Dm_Kokiri_0070"));
    CHECK_FALSE(IsStory("Npc_Kokiri_Mido_Child"));
    // The story names with no setting: no game can be asked for them.
    for (const std::string_view n : {"Meet_DekuTree", "Meet_GreatFairy", "Get_StoneOfForest", "Get_StoneOfFire",
                                     "Get_StoneOfWater", "Dm_FinalBattle_0000", "Dm_LostWood_0030"}) {
        CHECK_MESSAGE(CoopSharedScenes::IsShared(n), std::string(n));
        CHECK_MESSAGE(!Askable(n), std::string(n));
    }
    CHECK(Askable(kEntry));
    CHECK(Askable("Dm_Kokiri_0150"));  // a movie only (no flowchart to read): shared, story order gated
    for (const auto& r : CoopSharedScenes::StorySettings()) {
        CHECK_MESSAGE(CoopSharedScenes::IsShared(r.flow), std::string(r.flow));
        CHECK(Askable(r.request));
        CHECK(StoryOfRequest(r.request) == std::optional<std::string_view>(r.flow));
    }

    // The guard (intro-ocarina): the setting is a story start, not "another event".
    auto request = [](std::string_view name) {
        CoopLiveEvents::GameRequest r;
        std::copy(name.begin(), name.end(), r.name);
        r.taken = true;
        return r;
    };
    const auto names_span = CoopSharedScenes::Names();
    const std::vector<std::string_view> flows(names_span.begin(), names_span.end());
    const std::vector<std::string_view> names = CoopSharedScenes::RequestNames();
    const std::vector<CoopLiveEvents::GameRequest> asked{request(kEntry)};
    CHECK(CoopLiveEvents::RequestedOtherOf(asked, flows));  // by flow names only (before): refused
    CHECK_FALSE(CoopLiveEvents::RequestedOtherOf(asked, names));
    CHECK(CoopLiveEvents::RequestedOf(asked, names) == std::optional<std::string>(kEntry));
    // A talk setting of the same quest (a row id) is another event: the search's flow name is refused.
    const std::vector<CoopLiveEvents::GameRequest> talk{request("178488356_1871805950_2638247227_D_T")};
    CHECK(CoopLiveEvents::RequestedOtherOf(talk, names));

    // Through the start source: the setting given once, by the request; the search's flow name after it is
    // not given again (it is no setting: a leftover of the running event).
    CoopLiveEvents::StartSource source;
    source.Scene("KokiriForest_Child");
    const auto find = [] { return std::optional<std::string>("Meet_DekuTree"); };
    CHECK(source.Frame(CoopLiveEvents::RequestedOf(asked, names), CoopLiveEvents::RequestedOtherOf(asked, names),
                       true, true, find) == std::optional<std::string>(kEntry));
    CHECK_FALSE(source.Frame(std::nullopt, false, true, true, find).has_value());
    CHECK(source.Refused() == 0);
}

TEST_CASE("a joined story setting reads its own played flag, and a story name with no setting is not joined") {
    // The flags the game has (GameDataList.Product.110): HavePlayedEvent per setting; none for the quests' flows.
    CHECK(coop::host::flag_hash("HavePlayedEvent.KokiriForest_Meet_DekuTree_MeetDekuTree_01") == 0xef98d829u);
    CHECK(coop::host::flag_hash("HavePlayedEvent.Get_StoneOfFire_MeetDarnia") == 0x507d99c9u);
    CHECK(coop::host::flag_hash("HavePlayedEvent.Get_StoneOfFire_DodongoCavernOpne") == 0x516ed011u);
    CHECK(coop::host::flag_hash("HavePlayedEvent.Get_StoneOfWater_MeetRuto") == 0x64d3877fu);
    CHECK(coop::host::flag_hash("HavePlayedEvent.Get_StoneOfWater_JabuJabuEat") == 0xfb7c9b2cu);
    CHECK(coop::host::flag_hash("HavePlayedEvent.Dm_Kokiri_0140") == 0xebaec07fu);

    const std::string kEntry = "KokiriForest_Meet_DekuTree_MeetDekuTree_01";
    // The hook's played check: HavePlayedEvent.<name joined>.
    const auto saw = [&](bool met) {
        return [&, met](const std::string& n) { return met && "HavePlayedEvent." + n == "HavePlayedEvent." + kEntry; };
    };
    const Here here = Free(120, {15.0f, 0.0f, 22.0f}, "KokiriForest_Child");
    // B has met the Deku Tree; A starts the meeting near B: B is not asked to play it again.
    Joiner seen;
    seen.Started(1, kEntry, "KokiriForest_Child", kBridge, 100);
    CHECK_FALSE(seen.Next(here, saw(true)).has_value());
    // A B that has not met it is asked for that same setting.
    Joiner fresh;
    fresh.Started(1, kEntry, "KokiriForest_Child", kBridge, 100);
    CHECK(fresh.Next(here, saw(false)) == std::optional<std::string>(kEntry));
    // A quest's flow name (an older game, or the search with no request) is never joined: nothing to ask.
    Joiner quest;
    quest.Started(1, "Get_StoneOfForest", "KokiriForest_Child", kBridge, 100);
    CHECK(quest.Waiting() == 0);
    CHECK_FALSE(quest.Next(here, kNotPlayed).has_value());
    const auto notes = quest.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("no event setting") != std::string::npos);
    // Nor a story cutscene that takes the player's own things (Talon woken with the Cucco).
    Joiner cucco;
    cucco.Started(1, "Dm_HrlCastle_0020", "LonLonRanch", kBridge, 100);
    CHECK(cucco.Waiting() == 0);
    CHECK(cucco.TakeNotes()[0].find("the player's own things") != std::string::npos);
}

TEST_CASE("no story cutscene shared in 0.11.3 stops being shared unless the game's data gives a reason") {
    // Regression (review of report 110): every name of the story list is still sent and joined, but the
    // ones the game has no event setting of and the ones whose flow changes what is the player's own (a
    // trade, rupees, gear from the pouch). A new name here needs its reason in coop_story_requests.inc.
    const std::vector<std::string_view> allowed = {
        // no event setting (kNoSetting)
        "Dm_FinalBattle_0000", "Dm_LostWood_0030", "Get_StoneOfFire", "Get_StoneOfForest", "Get_StoneOfWater",
        "Meet_DekuTree", "Meet_GreatFairy",
        // the player's own (kOwnStory)
        "Dm_DeathMtTrail_0050",  // takes the Eye Drops (Biggoron's trade)
        "Dm_HrlCastle_0020",     // takes the Cucco (Talon)
        "Dm_Kokiri_0063",        // equips the Kokiri Sword from the pouch
        "Dm_LonLon_0030",        // rupees
        "Dm_LonLon_0040",        // Ingo beaten: moves the player's own Get_Epona to AfterWonGambleRace (row 150)
        "Dm_LostWood_0015",      // Biggoron's sword trade challenge removed (not the Pocket Egg: the old
                                 // reader's misread case reached an egg taken that its real cases never do)
        // the Great Fairies' paid heal (Dm_FairyFountain_Din:Pay, 500 rupees)
        "Dm_FairyFountain_Courage", "Dm_FairyFountain_Din", "Dm_FairyFountain_Frore", "Dm_FairyFountain_Nayru",
        "Dm_FairyFountain_Power", "Dm_FairyFountain_Wisdom",
    };
    std::size_t dropped = 0;
    for (const std::string_view n : CoopSharedScenes::Names()) {
        if (!CoopSharedScenes::Askable(n)) {
            ++dropped;
            CHECK_MESSAGE(std::find(allowed.begin(), allowed.end(), n) != allowed.end(), std::string(n));
            CHECK_MESSAGE(!CoopSharedScenes::NotShared(n).empty(), std::string(n));
        }
    }
    CHECK(dropped == allowed.size());
}
