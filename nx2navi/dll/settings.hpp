// SPDX-License-Identifier: GPL-3.0-or-later
// The DLL's settings: coop.ini in the mod folder. The four keys of the
// emulator's coop.ini (server, port, name, password) are read by the same code
// with the same rules (lichii/hook/coop_settings.cpp); this adds the keys of
// this port, share_progress, overlay and puppet. No Windows code: tested on its own.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "coop_settings.h"

namespace CoopNx2navi {

struct Settings {
    CoopHook::Settings coop;      // server, port, name, password
    // Shared progress and world state (progress.hpp): items, songs, keys, chests, one-time
    // pickups, dungeon switches, doors, cleared rooms, story flags, as the emulator build shares
    // them. On unless the file says 0 (since coop-dll 0.9).
    bool share_progress = true;
    // The in-game panel and chat box, in a game with NX2navi's plugin
    // interface (overlay.hpp); on unless the file says 0.
    bool overlay = true;
    // The other players shown as a second Link (puppet.hpp), in a game with
    // the plugin interface and the puppet pack; on unless the file says 0.
    bool puppet = true;
    // Who this game follows through loading zones (teleport.hpp): anyone it
    // shares a scene with (the default since 2026-11-13: nobody is left in
    // another area), the host only, or nobody.
    enum class Follow { off, host, anyone };
    Follow follow = Follow::anyone;
    // Once per run, when this game first gets into the game while the host
    // is in it: go to where the host is. On unless the file says 0.
    bool join_at_host = true;
    // The other players as dots on the game's minimap (overlay.hpp); on unless 0.
    bool minimap = true;
    // The other players' names over their heads (overlay.hpp); on unless 0.
    bool name_tags = true;
    // Shared enemies and boss rooms (objects/objects.hpp); on unless the file says 0
    // (since coop-dll 0.7: README, "Shared enemies").
    bool objects = true;
    bool objects_boss = true;   // objects_boss = 0: boss rooms not shared
    // How enemy copies are kept from acting by themselves: 3 leaves their AI
    // update out (objects/brain.hpp; the default with objects = 1), 1 and 2 set
    // or clear the brain skip bit (objects/hold.hpp: neither stops the AI), 0
    // nothing.
    int objects_ai_hold = 3;
    bool objects_world = true;  // objects_world = 0: no field-grass game calls
    // The join password of a session whose host set none. The shared server and protocol give
    // every session one (it lets players in and makes the traffic keys), so "no password" on
    // the home network is this fixed, public word on both sides: the host's server is started
    // with it and a joiner whose password is empty sends it. Anyone who can reach the host's
    // address and port can then join; a host who sets a password of its own is as before.
    static constexpr const char* kOpenPassword = "oot-coop-open";
    static std::string JoinPassword(const std::string& typed) {
        return typed.empty() ? std::string(kOpenPassword) : typed;
    }
    // time = 0: each game keeps its own clock. On: every game shows the host's time of day
    // (lichii/hook/coop_time, docs/time.md). Needs objects = 1 (the frame step's callers).
    bool time = true;
    // drops = 0: each game rolls its own drops. On: what an enemy, a pot or grass leaves is the
    // same in every game and goes to the player who walks on it (lichii/hook/coop_drops).
    bool drops = true;
    // objects_friendly = 1: townspeople and animals (Npc_, Animal_: Malon, the
    // horses, the cows) are shared too, as in the emulator build. Off unless
    // the file says 1: their copies are not held, so a player could not talk
    // to, ride or milk one another game owns; each game runs its own.
    bool objects_friendly = false;
    // objects_rolls_apart = <metres>, 0 to 200: a game that does not decide the
    // scene's rolled enemies (the night's Stalchildren) keeps and shares its
    // own when the deciding player's Link is further than this from them, so a
    // player away from the host has enemies of its own. 0: only the first
    // player's game rolls, as in the emulator build.
    int objects_rolls_apart = 30;
    // host_save = 1: this game joins someone's session and takes the host's
    // save first (save_sync.hpp): it is fetched into the co-op profile and the
    // game starts again on it. Off unless the file says 1 (the launcher's Join
    // and the in-game Join write it; a host never has it).
    bool host_save = false;
    bool enabled() const { return coop.enabled(); }
};

// Reads the text of a coop.ini. Any problem is added to `errors` with its
// line number and turns co-op off (the server is cleared), as in the emulator.
Settings ParseSettings(std::string_view text, std::vector<std::string>& errors);

// The text of a coop.ini that ParseSettings reads back to `settings` (CRLF
// lines). Without the password when `with_password` is false: the line is
// left out, for a file written by the game itself (the password then travels
// in the environment, OOT_COOP_PASSWORD).
std::string SettingsIni(const Settings& settings, bool with_password);

}  // namespace CoopNx2navi
