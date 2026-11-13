// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's look, changed while the game runs (docs/puppet.md, "Changing
// your colour in the game"). The pause menu's Settings page shows a look and
// sets the one the player chose (SetPending); when the pause screen closes it
// calls ApplyOnUnpause, and on the next frame:
//   - your own Link wears it at once (coop_look_tunic.h: the tunic's textures
//     are rewritten in the game's memory, no restart and no scene load);
//   - the other players' puppet of you changes (the colour goes out in
//     PLAYER_EQUIP as at the start: CoopHook::SetColor; a newcomer gets it
//     with the equipment);
//   - it is written to the settings files, so the next start keeps it.
// Co-op need not be running: alone, your Link changes and the look is saved.
//
// A look is one colour per part. Today the only part is the tunic (with the
// cap); more parts can follow, each under its own number, which never
// changes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace CoopLook {

enum class Part : std::uint8_t {
    tunic = 0,
    // later: shield = 1, sword = 2, boots = 3, gauntlets = 4
};
inline constexpr std::size_t kParts = 1;

struct Look {
    // 0xRRGGBB for each part, by its number. Unset: the game's own look (for
    // the tunic the others then see your player number's colour).
    std::array<std::optional<std::uint32_t>, kParts> colour{};

    std::optional<std::uint32_t>& operator[](Part p) { return colour[static_cast<std::size_t>(p)]; }
    const std::optional<std::uint32_t>& operator[](Part p) const { return colour[static_cast<std::size_t>(p)]; }
    bool operator==(const Look&) const = default;
};

// A part's colour as the settings files have it (coop-options.ini `color`):
// a colour name or #RRGGBB (CoopColors::Parse); "" for unset.
std::string ColourText(const std::optional<std::uint32_t>& colour);

// The look worn and the one waiting for the pause screen to close. No game,
// no files: the rules alone.
class Choice {
public:
    explicit Choice(Look worn = {}) : worn_(worn) {}
    const Look& Worn() const { return worn_; }
    // What a menu row shows: the waiting look, else the worn one.
    const Look& Shown() const { return pending_ ? *pending_ : worn_; }
    // The player chose `look` in the menu. Nothing changes yet. Choosing the
    // worn look again takes a waiting one away.
    void SetPending(const Look& look);
    // The pause screen closed: the waiting look becomes the worn one and is
    // returned, for the caller to apply. Nothing when none waits.
    std::optional<Look> Unpause();

private:
    Look worn_;
    std::optional<Look> pending_;
};

// The two settings files with the look in them: `color` in coop-options.ini
// when the game belongs to a kit (`options_ini` given; the kit's launcher
// hands it on as LICHII_COOP_COLOR, which counts before coop.ini), else in
// coop.ini. An unset tunic comments the line out. True when a text changed.
bool WriteSettings(const Look& look, std::string& coop_ini, std::string* options_ini);

// The tunic's colour as the puppet pack names it (coop_look/<name>.bin): the
// pack colour nearest it (CoopColors::TunicOf), lower case; "green", the
// game's own, when unset.
std::string TunicName(const std::optional<std::uint32_t>& colour);

// ---------------------------------------------------------------------------
// The module's one look. Any thread, but Frame.

// What applying a look does (the module's: coop_look_live.cpp). Called on
// the game's thread, from Frame.
using Apply = std::function<void(const Look&)>;

// At the module's start: the look of the settings (LICHII_COOP_COLOR, else
// coop-options.ini's or coop.ini's `color`) and what applying one does.
// The worn look is not applied again: the game started in it.
void Init(const Look& worn, Apply apply);
Look Worn();
Look Shown();
void SetPending(const Look& look);
// The pause screen closed. The look is applied on the game's next frame.
void ApplyOnUnpause();
// hos_coop_frame's every-frame site: applies a look ApplyOnUnpause asked
// for (Init's `apply`).
void Frame();


// The Hookshot module's look (coop_look_live.cpp): Start, after the kit's
// switches are set, takes the settings' look (Init), and starts the worker
// that keeps your own Link's tunic in the worn colour; Stop ends it.
void StartModule();
void StopModule();

}  // namespace CoopLook
