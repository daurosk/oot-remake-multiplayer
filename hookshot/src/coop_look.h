// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's look, changed while the game runs (docs/puppet.md, "The Link
// customizer"). The Multiplayer tab's Customize page (coop_look_menu.h) shows
// a look and sets the one the player chose (SetPending); when the pause
// screen closes it calls ApplyOnUnpause, and on the next frame:
//   - your own Link wears it at once, no restart and no scene load: the
//     textures are rewritten in the game's memory (the tunic: coop_look_tunic.h
//     with the pack's colours; the gear: coop_look_parts.h, any colour);
//   - the others get it: the tunic's colour in PLAYER_EQUIP as at the start
//     (CoopHook::SetColor), every part in PLAYER_LOOK (CoopHook::SetLook); a
//     newcomer gets both;
//   - it is written to the settings files, so the next start keeps it.
// Co-op need not be running: alone, your Link changes and the look is saved.
//
// A look is one colour per part (docs/puppet.md, "The Link customizer"): the
// tunic, the Goron and Zora tunics' cloth, and the parts of the shields,
// swords, boots and gauntlets that their textures keep apart. Each part has
// a number that never changes (the settings and the network use it).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopLook {

enum class Part : std::uint8_t {
    tunic = 0,            // the Kokiri tunic and cap (one texture on the adult: the cap is not apart)
    goron_tunic = 1,      // the Goron Tunic's red cloth
    zora_tunic = 2,       // the Zora Tunic's blue cloth
    hylian_frame = 3,     // the Hylian Shield's metal rim and the cross on its back
    hylian_face = 4,      // its blue face
    hylian_emblem = 5,    // its red bird (the gold Triforce stays)
    mirror_rim = 6,       // the Mirror Shield's red rim
    mirror_face = 7,      // its mirror
    deku_shield = 8,      // the Deku Shield (one colour: it is all wood)
    kokiri_blade = 9,
    kokiri_hilt = 10,
    master_blade = 11,
    master_hilt = 12,     // the Master Sword's blue grip and wings (the gold and the gem stay)
    biggoron_blade = 13,
    biggoron_hilt = 14,
    boots = 15,           // the leather of every pair of boots
    gauntlets = 16,       // the metal of the Silver and the Golden Gauntlets
};
inline constexpr std::size_t kParts = 17;

// A part as the menu and the settings name it.
struct PartInfo {
    Part part;
    const char* key;    // in the settings files: look_<key> (the tunic: `color`)
    const char* item;   // the menu's heading: "Hylian Shield"
    const char* name;   // the row: "Frame"
};
// Every part, in the menu's order (by item).
const std::array<PartInfo, kParts>& Parts();
const PartInfo& InfoOf(Part part);
// A part by its number; nothing for a number this build does not know.
std::optional<Part> PartOf(std::uint8_t number);

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

// The two settings files with the look in them: coop-options.ini when the
// game belongs to a kit (`options_ini` given; the kit's launcher hands its
// `color` on as LICHII_COOP_COLOR, which counts before coop.ini), else
// coop.ini. The tunic is `color`, every other part `look_<key>`; an unset
// part comments its line out. True when a text changed.
bool WriteSettings(const Look& look, std::string& coop_ini, std::string* options_ini);
// The parts other than the tunic as a settings text has them (the tunic comes
// with the co-op settings: CoopHook::LoadSettings). A value that is not a
// colour is left unset, and said in `errors`.
Look ReadParts(std::string_view text, std::vector<std::string>* errors = nullptr);

// The look as the others get it (PLAYER_LOOK, docs/protocol.md): the set
// parts, by number.
std::vector<std::pair<std::uint8_t, std::uint32_t>> Wire(const Look& look);
// A received look; parts this build does not know are passed over.
Look FromWire(const std::vector<std::pair<std::uint8_t, std::uint32_t>>& parts);

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
