// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared story cutscenes (docs/scenes.md "Shared story cutscenes"):
// when a story cutscene starts in one player's game, every other player in
// the same room plays it too, started at once in their own game. Small talks
// stay local (npc-anim, issue 200). A player who cannot join (another event,
// a menu, a load, too far) is not pulled in; the result comes by the flags
// and the quest catch-up (issue 173).
//
// The pieces are deku-dungeon-2's (coop_live_events.h): the starter's game
// finds the cutscene's name in its event session (FindRunningEvent, with the
// story names here), sends it as DUNGEON_EVENT (type 355: the server relays
// it to the others in the sender's scene, never kept), and the others ask
// their game for the same event by name (Requester). Only the rules of who
// joins are here. Pure: tested in tests/lichii/shared_scenes_test.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopSharedScenes {

// The story cutscenes shared (event flow names, read from the romfs
// Event/EventFlow file names): every Dm_ cutscene except the system ones
// (Dm_SY_, Dm_MelodyGet, Dm_Title, Dm_StaffCredit, Dm_Ending), the opening
// (Dm_Kokiri_0000), the parts played inside another (_Effect, _Env, _Get,
// _move), boss intros and outs (dungeons-boss.md), the Chamber of Sages
// (Dm_Awake_, Dm_SageRoom_), and time travel (Dm_TimeTemple_: threads-of-time,
// issue 193); plus Meet_DekuTree, Meet_GreatFairy and Get_StoneOf...
std::span<const std::string_view> Names();
bool IsShared(std::string_view name);

// Player report 110 (Deku Tree): the game asks for an event by its *event
// setting* (RSDB EventSetting: a name, the flow and the entry it plays), and
// keeps "already played" per setting (HavePlayedEvent.<setting>; every one of
// the game's 2172 named settings has that flag). Most story cutscenes are a
// setting of their own name; many settings play a story flow under another
// name: the Great Deku Tree's meeting is the setting
// KokiriForest_Meet_DekuTree_MeetDekuTree_01 (flow Meet_DekuTree), the Fire
// Temple's freed Gorons Dm_FireTemple_0014.._0022 (flow Dm_FireTemple_0020).
// The game's request then named no story event, the story search's flow name
// counted as a leftover of another event (intro-ocarina) and was never sent:
// each player watched it alone. So the key of such a cutscene is its setting:
// the starter sends it, a game that joins asks for it and reads its played
// flag. coop_story_requests.inc (tools/puppet_pack/story_requests.py) holds
// every named setting of a story flow under another name, classified from the
// game's data: the shared ones here, the others (boss events, dungeon
// gimmicks, talks, arrivals, items, room events) in comments with why.
struct StoryOwn {
    std::string_view name;  // a story name
    std::string_view why;   // what of the player's own its flow changes
};
struct StorySetting {
    std::string_view request;  // the setting's name (what the game's request names)
    std::string_view flow;     // the shared story flow it plays
};
std::span<const StorySetting> StorySettings();
// The shared story flow a request of our game plays: `request` itself when it
// is a story name, the flow of a story setting in the table, else nothing.
std::optional<std::string_view> StoryOfRequest(std::string_view request);
// A setting in the table (sent and joined by that name).
bool IsStorySetting(std::string_view name);
// A story start to send or join: a story name or a story setting.
bool IsStory(std::string_view name);
// The names a request of our game counts as a story start by: the story
// names and the story settings (RequestedOf / RequestedOtherOf).
std::vector<std::string_view> RequestNames();
// Why `name` is neither sent nor joined, or "" when it is shared:
// - a story name the game has no event setting of (the quests Meet_DekuTree,
//   Meet_GreatFairy, Get_StoneOf{Forest,Fire,Water}, which also have no played
//   flag, and Dm_FinalBattle_0000, Dm_LostWood_0030, played inside other
//   settings): asking it plays nothing, and a game could not tell it played
//   it. Their cutscenes are settings of their own (the table above,
//   Dm_FairyFountain_*, the boss events);
// - a story name whose event flow changes what is the player's own (a trade
//   item taken, rupees, gear equipped from the pouch: Talon woken with the
//   Cucco Dm_HrlCastle_0020; kOwnStory with why, read from the game's flows by
//   story_requests.py): another game asked for it would be charged or changed
//   too. A gift of an item co-op shares stays shared (Zelda's letter);
// - a story name whose event flow turns a room's own state (kRoomStory: the
//   Forest Temple's twisted corridor Dm_ForestTemple_0005/_0007 toggles its
//   flags and turns round the player, row 173): a game that joined it would
//   turn its corridor once more, under its own player; the corridor's state
//   comes by its held flags (coop_dungeon.cpp) at that game's next load.
std::string_view NotShared(std::string_view name);
// NotShared(name) is "".
bool Askable(std::string_view name);

// How near the starter must be to our Link for us to join: same room. The
// Kokiri bridge and Saria are about 20 m apart; the Deku Tree's clearing is
// about 40 m across.
inline constexpr float kRoomM = 30.0f;
// In a dungeon 30 m reaches through walls into other rooms, and the game has
// only the rooms around Link loaded: a cutscene started from another room
// shows a black picture until it ends (a player report, 2026-11-12). There the
// starter must be in our room (Here::room, Started's room: coop_rooms.h);
// where a room is not known (no room table), only this near.
inline constexpr float kNoRoomM = 8.0f;
// How long a start waits while our game cannot take it (our own event, a
// menu, a pause, a load) before it is skipped: a cutscene joined later than
// this would be far behind the starter's.
inline constexpr std::uint64_t kJoinUs = 3'000'000;
// Row 176 (Meg, run 20261119-222229): a start skipped only because we were not
// near (too far, another room or scene, our room not known) is held this long,
// or until our save has it played (the starter's result came by the flags), and
// joined once we come near: player 2 walked into Meg's area while player 1's
// Dm_ForestTemple_0015 still played, its own room logic started a second
// Dm_ForestTemple_0015, and each game then ran its own Meg (two owners). Joined,
// our game plays the starter's event (its actors act for the starter) and the
// room logic does not start it again (its played flag is then set here).
// Row 176 again (the Forest playtest on 0.11.7): Meg's fight ran past 2 min
// (Dm_ForestTemple_0017 to her death), so a partner who came after kHoldUs
// still had its own room logic start a second Dm_0017 and a second Meg. So a
// held start is also kept past kHoldUs while its starter is still in that scene
// (Here::in_scene): their fight goes on there, and our late start is theirs. Once
// they left the scene (or the session), what they started is gone with them, and
// our own start of it is a new one of ours.
inline constexpr std::uint64_t kHoldUs = 120'000'000;
// The reasons Why gives for a start we are only not near (held, kHoldUs).
bool NotNearYet(std::string_view why);

using Vec3 = std::array<float, 3>;

// Our game this frame.
struct Here {
    std::string scene;          // empty: not known (a load)
    bool can_start = false;     // Link read, no event running, not paused, no menu
    std::optional<Vec3> link;   // our Link's place
    bool dungeon = false;       // the scene is a dungeon (rooms loaded one by one)
    std::optional<int> room;    // our Link's room there, if known
    std::uint64_t now_us = 0;
    // The other players in our scene now (their numbers); read only while a start
    // waits. A held start whose starter is here is kept past kHoldUs.
    std::vector<std::uint8_t> in_scene;
};

// Story order (intro-ocarina, a player report 2026-11-13): a story cutscene
// that only fits a later part of the story is never started in a game that is
// not there yet, nor sent from one. Saria's gift on the bridge
// (Dm_Kokiri_0150: it gives the Fairy Ocarina) asked in a game still in the
// opening gave the ocarina and froze in the cutscene (Saria is not at the
// bridge then). A gate holds when any of its flags has its value; a flag that
// cannot be read does not hold it.
struct Need {
    std::uint32_t hash;
    bool value;
    std::string_view what;
};
// The flags of which one must hold for `name`; empty: no gate.
std::span<const Need> Needs(std::string_view name);
// Reads a saved Bool by hash (nothing: not readable).
template <typename Flag>
bool GateHolds(std::string_view name, Flag&& flag) {
    const auto needs = Needs(name);
    if (needs.empty()) {
        return true;
    }
    for (const Need& n : needs) {
        const std::optional<bool> v = flag(n.hash);
        if (v && *v == n.value) {
            return true;
        }
    }
    return false;
}

class Joiner {
public:
    // Another player's game started `name` in `scene`; `starter` is where
    // that player was last seen (nothing: not known, so not joined), and
    // `room` the dungeon room of that place, if known.
    void Started(std::uint8_t player, const std::string& name, const std::string& scene,
                 std::optional<Vec3> starter, std::uint64_t now_us, std::optional<int> room = std::nullopt);
    // Our own game started `name` (found by our watch). Returns whether to
    // send it to the others: not when we only joined another player's start
    // (it would come back to them) or already sent it this visit, nor when
    // another player's start of it is held for us here (kHoldUs): ours is then
    // a late start of theirs (a place into the area beat the join), not a new one.
    bool Ours(const std::string& name, const std::string& scene);
    // Once a frame: the cutscene to ask our game for now, if any. `played`
    // says whether our game already has it played (HavePlayedEvent.<name>):
    // then it is skipped. Each name at most once per visit of the scene.
    template <typename Played>
    std::optional<std::string> Next(const Here& here, Played&& played) {
        return Next(here, std::forward<Played>(played), [](std::uint32_t) { return std::optional<bool>(true); },
                    false);
    }
    // The same, with the story order (Needs) read from our game's flags.
    template <typename Played, typename Flag>
    std::optional<std::string> Next(const Here& here, Played&& played, Flag&& flag) {
        return Next(here, std::forward<Played>(played), std::forward<Flag>(flag), true);
    }

private:
    template <typename Played, typename Flag>
    std::optional<std::string> Next(const Here& here, Played&& played, Flag&& flag, bool gated) {
        Visit(here.scene);
        for (std::size_t i = 0; i < pending_.size();) {
            Pending& p = pending_[i];
            std::string why = Why(p, here, played(p.name));
            if (p.held && p.near_us == 0 && !NotNearYet(why)) {
                p.near_us = here.now_us;  // near now: a busy game gets its 3 s from here
                why = Why(p, here, played(p.name));
            }
            if (gated && (why.empty() || why == "wait") && !GateHolds(p.name, flag)) {
                why = "our story is not there yet";
            }
            if (NotNearYet(why) && (here.now_us < p.at_us + kHoldUs || StarterStays(p, here))) {
                if (!p.held) {
                    p.held = true;
                    notes_.push_back("player " + std::to_string(p.player) + "'s story cutscene " + p.name +
                                     " not played here yet (" + why + "): held, played here if we come near "
                                     "while it is not played here");
                }
                ++i;
                continue;
            }
            if (why == "wait") {
                return std::nullopt;
            }
            Pending taken = std::move(p);
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            if (why.empty()) {
                done_.insert(taken.name);
                joined_.insert(taken.name);
                notes_.push_back("player " + std::to_string(taken.player) + " started the story cutscene " +
                                 taken.name + (taken.held ? " earlier, now near us: playing it here too (late)"
                                                          : " near us: playing it here too"));
                return taken.name;
            }
            notes_.push_back("player " + std::to_string(taken.player) + "'s story cutscene " + taken.name +
                             " not played here (" + why + "); its result comes by the flags");
        }
        return std::nullopt;
    }

public:
    // Log lines since the last call.
    std::vector<std::string> TakeNotes();
    std::size_t Waiting() const { return pending_.size(); }

private:
    struct Pending {
        std::uint8_t player;
        std::string name;
        std::string scene;
        std::optional<Vec3> starter;
        std::uint64_t at_us;
        std::optional<int> room;
        bool held = false;           // skipped only for not being near: kept (kHoldUs)
        std::uint64_t near_us = 0;   // when a held start was first near (its busy wait counts from then)
    };
    // "" to start now, "wait", or why it is skipped.
    std::string Why(const Pending& p, const Here& here, bool played) const;
    // A held start whose starter is still in its scene (kept past kHoldUs).
    static bool StarterStays(const Pending& p, const Here& here);
    void Visit(const std::string& scene);
    std::string scene_;
    std::vector<Pending> pending_;
    std::set<std::string> done_;    // asked, sent or played here this visit
    std::set<std::string> joined_;  // asked because another player started it
    std::vector<std::string> notes_;
};

// LICHII_COOP_SHARED_CUTSCENES: unset or anything but "0": on; "0": off.
bool OnFromEnv(std::string_view text);

} // namespace CoopSharedScenes
