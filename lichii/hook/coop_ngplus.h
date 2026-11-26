// SPDX-License-Identifier: GPL-3.0-or-later
//
// (Navi) New Game+: beat Ganon, then play again with your hearts and tougher
// enemies. Navi's addition to oot-remake-multiplayer (UPSTREAM.txt).
//
// - Unlocked by Ganon's death: the game's own event request for it (taken),
//   Ganon's death setting or flow (coop_boss_scenes.cpp's table) or the
//   ending's Defeat_Ganon, seen by Hookshot's event request wrap
//   (CoopLiveEvents::TheGameRequests). Beating him at level L unlocks L + 1
//   (up to protocol kMaxNgPlus) and keeps the most hearts reached (MaxLife).
// - The level is chosen on the Multiplayer tab's Settings page (New Game+:
//   Off, NG+ 1 ... the highest unlocked) and kept in coop-ngplus.ini beside
//   coop.ini, with what was unlocked and the hearts.
// - With a level on: a NEW file (its opening, IsNewFileOpening) of the host
//   or of a game alone gets the kept hearts, through the game's own MaxLife
//   setter (never lowered; a loaded save and a guest's game get nothing of
//   their own: a guest has the host's save). Written while our Link is not
//   made (a load between scenes): the game makes Link from PlayerStatus.MaxLife
//   as it does for a loaded save, and while Link is there his Life component
//   writes its own max back into the flag (docs/items.md), so a write then
//   would not hold alone. The same one path alone and hosting. The difficulty is
//   ngplus_difficulty (coop/protocol/difficulty.hpp) of what is in force:
//   alone in CoopDifficulty::Now(), in a session by the host's server (the
//   host's level counts for everyone; a guest's own level is not used there,
//   the host's hearts reach the guests by the shared MaxLife).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopHook {
class EmulatorMemory;
}

namespace CoopNgPlus {

inline constexpr std::int32_t kMostQuarters = 80;  // 20 hearts, the most kept

// What coop-ngplus.ini keeps.
struct Saved {
    std::uint8_t level = 0;     // chosen: 0 off
    std::uint8_t unlocked = 0;  // the highest level that may be chosen
    std::int32_t hearts = 0;    // quarter hearts carried into New Game+ (0: none)
    bool operator==(const Saved&) const = default;
};
Saved Parse(std::string_view text);
std::string Format(const Saved& s);
// Ganon beaten with `max_life` (quarter hearts; 0 or less: not known).
Saved Beaten(Saved s, std::int32_t max_life);
// A level within what is unlocked.
Saved Choose(Saved s, int level);
// An event the game asked for that is Ganon's death.
bool IsGanonDeath(std::string_view event);
// An event the game asked for that is the opening of a new file.
bool IsNewFileOpening(std::string_view event);

// ngplus-hearts (results-0115-a: NG+ 1 on a real new file, no hearts given): the game does not ask its event
// manager for the opening. New Game's own flow plays the setting Opening_LoadingKokiriForest (romfs
// Event/EventFlow/OpeningEvent), which goes on to Dm_Kokiri_0000 by EventTransition, then Opening_Introduction
// and Dm_Kokiri_0030 (the same), so the event request wrap (IsNewFileOpening) never sees them. Ganon's death is
// played the same way (Defeat_Ganon's flow). The game keeps a played Bool per event setting
// (HavePlayedEvent.<setting>, GameDataList; docs/scenes.md), set whichever way the setting started: a file whose
// flag goes from unplayed to played has just played that event. The opening's flags are never shared (coop_story,
// coop_shared_scenes leave the opening out), and every save after it has them set (the opening ends with
// EventTriggerRequestAutoSave), so only a new file can see them rise.
inline constexpr std::uint32_t kOpeningPlayed[] = {
    0xa95bd438u,  // HavePlayedEvent.Opening_LoadingKokiriForest
    0x3b5ca6c2u,  // HavePlayedEvent.Dm_Kokiri_0000
    0x15b67d0eu,  // HavePlayedEvent.Opening_Introduction
    0xb041ae87u,  // HavePlayedEvent.Dm_Kokiri_0030
};
inline constexpr std::uint32_t kGanonPlayed[] = {
    0xa5e7a3ddu,  // HavePlayedEvent.InsideGanonCastle_Dungeon_BossDie_2 (Ganon's death setting)
    0x44db9a38u,  // HavePlayedEvent.Dm_FinalBattle_BossOut
};
// One played flag, followed within one file: rises once, when it is read unplayed and then played. The title or a
// save load forgets what was read (Reset), so a loaded save that has it set never rises.
struct Played {
    bool known = false;  // read since the last reset
    bool was = false;
};
// Takes this read (nothing: unread now, kept as it was) and says whether the flag rose with it.
bool Rose(Played& p, std::optional<bool> now);
inline void Reset(Played& p) { p = Played{}; }

// What decides the kept hearts for the loaded file.
struct Give {
    std::uint8_t level = 0;                // the level chosen (0 off)
    std::int32_t hearts = 0;               // kept (quarter hearts)
    std::optional<std::int32_t> max_life;  // the file's MaxLife now (nothing: unread)
    bool new_file = false;                 // the file began with the opening since the title
    bool may_give = false;                 // alone, or this game hosts (a guest plays the host's save)
    bool link_made = false;                // our Link is in the scene (his Life component owns the max then)
};
// Whether the file played is still a new one (its hearts may be given), from what this frame saw.
struct FileMark {
    bool was = false;           // a new file until now
    bool native_loads = false;  // the game's save loads are seen (CoopSave::NativeSaveLoads; false on the emulator)
    bool title = false;         // the title is up
    bool load_started = false;  // the game started a save load since the last frame (any load: title, System
                                // menu, Game Over's Try Again, the host's save)
    bool opening = false;       // a new file's opening (IsNewFileOpening) was taken since the last frame
};
// The title or a save load ends it; the opening (which comes after New Game's own load) begins it. Without the save
// loads seen (the emulator) a loaded save cannot be told from a new file, so nothing is given there.
bool NewFileNow(const FileMark& m);
// The MaxLife to set, or nothing (off, a loaded save, a guest, already that high: never lowered; our Link made:
// waits for the next load, where the game makes him from the flag).
std::optional<std::int32_t> HeartsToGive(const Give& g);

// Any thread.
std::uint8_t Level();
std::uint8_t Unlocked();
std::int32_t Hearts();
// The Settings page's choice: kept within what is unlocked, saved. False when nothing changed.
bool SetLevel(int level);

// The hook's thread, each frame (alone and in a session, also during loads): Ganon's death, and the hearts while a
// level is on. `in_game`: our Link is in the scene. `may_give`: alone, or this game hosts the session. `title`: the
// title screen is up (CoopSave::TitleUp): the next file is another one.
void Frame(CoopHook::EmulatorMemory& memory, bool in_game, bool may_give, bool title);

// Chat lines ("New Game+ 1 unlocked ...").
std::vector<std::string> TakeLines();

}  // namespace CoopNgPlus
