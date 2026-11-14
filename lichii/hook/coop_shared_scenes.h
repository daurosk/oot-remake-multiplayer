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

using Vec3 = std::array<float, 3>;

// Our game this frame.
struct Here {
    std::string scene;          // empty: not known (a load)
    bool can_start = false;     // Link read, no event running, not paused, no menu
    std::optional<Vec3> link;   // our Link's place
    bool dungeon = false;       // the scene is a dungeon (rooms loaded one by one)
    std::optional<int> room;    // our Link's room there, if known
    std::uint64_t now_us = 0;
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
    // (it would come back to them) or already sent it this visit.
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
        while (!pending_.empty()) {
            std::string why = Why(pending_.front(), here, played(pending_.front().name));
            if (gated && (why.empty() || why == "wait") && !GateHolds(pending_.front().name, flag)) {
                why = "our story is not there yet";
            }
            if (why == "wait") {
                return std::nullopt;
            }
            Pending taken = std::move(pending_.front());
            pending_.erase(pending_.begin());
            if (why.empty()) {
                done_.insert(taken.name);
                joined_.insert(taken.name);
                notes_.push_back("player " + std::to_string(taken.player) + " started the story cutscene " +
                                 taken.name + " near us: playing it here too");
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
    };
    // "" to start now, "wait", or why it is skipped.
    std::string Why(const Pending& p, const Here& here, bool played) const;
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
