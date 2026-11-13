// SPDX-License-Identifier: GPL-3.0-or-later
// item_get.hpp: another player's item, shown here with the game's own item-get moment (no Windows code,
// unit-tested; the game calls are in upstream.cpp, "The item-get moment").
//
// The game has no "give an item with its scene" call: an item actor that exists starts the get scene
// `Dm_SY_Get` through its PouchContent component (Link holds it up, the fanfare, the text box). So the receiving
// game makes that item's actor beside its Link, tells the component not to add it to the pouch again (the item
// came by the shared flags already) and asks for the scene with the request the component itself builds; the
// actor is deleted when the scene is over. Only when it is safe: our Link in control (no event, no menu, the
// player readable for 3 s), standing still in height for a second, not hurt in the last 8 s, no boss fight; one
// at a time, 3 s apart. An item that waits more than 12 s, or has no actor of its own (songs, medallions and
// stones have none here), gets the game's fanfare instead, and the large line with the item's icon when the game
// has one (romfs UI/Tex/Icon/<actor>).
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace CoopNx2navi::ItemGet {

// The item actor of a notice line ("Saria got the Hookshot - you got one too!", "Host got the map (Forest
// Temple)"); empty when it has none (a song, a heart container, a token).
std::string ActorFor(const std::string& line);
// The romfs path of the item's icon, "" when the game has none for it.
std::string IconPath(const std::string& actor);
// The game's fanfare size for it: 0 small, 1 medium, 2 large (PouchGetType; the map, the Hookshot and the
// medallions are large).
int FanfareType(const std::string& actor);

inline constexpr std::uint64_t kSafeAfterUs = 3'000'000;    // readable this long
inline constexpr std::uint64_t kStillUs = 1'000'000;        // height steady this long
inline constexpr float kStillM = 0.05f;
inline constexpr std::uint64_t kHurtQuietUs = 8'000'000;
inline constexpr std::uint64_t kWaitUs = 12'000'000;        // then the fanfare and the line instead
inline constexpr std::uint64_t kApartUs = 3'000'000;        // between two scenes

struct Look {  // our game, each frame
    bool in_game = false;
    bool event = false;
    bool menu = false;
    bool boss_fight = false;
    bool travelling = false;  // a follow, a join or a load of ours is under way
    float height = 0.0f;
    int life = -1;
};

struct Want {
    std::string line;
    std::string actor;  // empty: fanfare only
    std::uint64_t at_us = 0;
};

class Queue {
public:
    void Offer(const std::string& line, std::uint64_t now_us);
    // The present thread, each frame. Returns what to do now: a scene (`scene`), or the fanfare instead.
    struct Do {
        Want want;
        bool scene = false;
    };
    std::optional<Do> Frame(const Look& look, std::uint64_t now_us);
    // The scene asked for is over (or failed: `played` false gives its fanfare next).
    void Done(bool played, std::uint64_t now_us);
    bool Busy() const { return busy_; }
    std::size_t Waiting() const { return wants_.size(); }

private:
    std::deque<Want> wants_;
    bool busy_ = false;
    std::optional<Want> failed_;
    std::optional<Want> current_;  // the scene asked for
    std::uint64_t readable_since_ = 0;
    std::uint64_t still_since_ = 0;
    float still_height_ = 0.0f;
    int last_life_ = -1;
    std::uint64_t hurt_us_ = 0;
    std::uint64_t done_us_ = 0;
};

} // namespace CoopNx2navi::ItemGet
