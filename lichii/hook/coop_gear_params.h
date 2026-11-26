// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gear parts whose colour is a material parameter, not a texture (row 277: "Master sword handle customization doesn't
// work"). Read from the game's own files (docs/puppet.md, "Gear colours that are a material's parameter"):
// - Model/Weapon_Sword_Master draws its grip (shape Grip) with the material Mt_Weapon_Sword_Master, which samples no
//   colour texture (its samplers: _n0 _rgh0 _mt0 _ao0; the blade's Mt_Weapon_Sword_Master_Metal has _a0). The grip's
//   colour is that material's shader parameter ns_param_color0 (linear 0.022, 0.318, 0.402; every material of the
//   model that samples the colour texture has 1, 1, 1 there). The blue blocks of Mt_Weapon_Sword_Master_Alb that the
//   customizer painted lie under the grip's texture coordinates only (2 of 2693 under a shape that samples it), so
//   painting the texture never showed.
// - Of every other gear texture's parts, all are drawn by a material that samples that texture (offline audit of the
//   21 textures of CoopLookParts::Gear on the game's models), so only the Master Sword's hilt is here.
// So the hilt's colour is put on the equipped piece's own model with the game's model calls (the unit's +0xd0
// material by name, +0xe0 parameter by name, +0x158 four floats: the calls CoopCharColours uses for the hair and
// eyes), on your Link's sword and on every puppet's (its copy `_Look<k>` / `_Blade<n>` has the same material).
// No emulator or Hookshot code here: the runtime gives the calls and the reads (CoopCharColours::Game).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_char_colours.h"

namespace CoopGearParams {

// A gear part that is a material's parameter: the CoopLook part's number, the item actors that wear that material
// (an actor whose name starts with `actor`, not followed by `_Dark`: Dark Link's sword is another material), the
// material and the parameter (its first three floats are the colour, linear; the fourth stays the file's).
struct ParamPart {
    std::uint8_t part;
    const char* actor;
    const char* material;
    const char* param;
};
inline constexpr std::array<ParamPart, 1> kParts = {{
    {12, "Weapon_Sword_Master", "Mt_Weapon_Sword_Master", "ns_param_color0"},   // CoopLook::Part::master_hilt
}};
inline constexpr std::size_t kPartCount = kParts.size();

// Whether an item actor of this name wears a part's material: the game's item, or its co-op copies
// (Weapon_Sword_Master_Look3, Weapon_Sword_Master_Blade12), not another item that starts so (_Dark).
bool WearsPart(std::string_view actor_name, const ParamPart& part);
// A colour (0xRRGGBB, sRGB as Customize shows it) as the parameter's linear floats.
std::array<float, 3> Linear(std::uint32_t rgb);

// The guest memory a dresser keeps its names and arguments in (CoopCharColours::kCellBytes, its kCellName and
// kCellFloats): the names from 0 (32 bytes each: material, then parameter, for each part).
inline constexpr std::uint64_t kCellPartNames = 0x00, kNameBytes = 0x20;
static_assert(kCellPartNames + 2 * kNameBytes * kPartCount <= CoopCharColours::kCellName);

// One actor wearing gear: your Link or a puppet. `colours[i]`: the player's colour of kParts[i], or nothing for the
// game's own.
struct Target {
    std::uint64_t actor = 0;
    std::array<std::optional<std::uint32_t>, kPartCount> colours{};
};

// Each frame (the calc thread): the targets' equipped pieces that wear a part's material get its colour, when it
// changes, when the piece is new (equipped, made again) and again every kAgainMs while a colour is set (the game may
// set its own values again); a colour taken away puts the file's own back once. At most kCallsPerFrame calls.
class Dresser {
public:
    static constexpr std::uint64_t kAgainMs = 500;
    static constexpr std::size_t kCallsPerFrame = 48;
    // `cell`: kCellBytes of guest memory (0: nothing is done). Returns the calls made. `say`: a log line.
    std::size_t Frame(const CoopCharColours::Game& game, std::uint64_t cell, const std::vector<Target>& targets,
                      std::uint64_t now_ms, const std::function<void(const std::string&)>& say);

private:
    struct Piece {
        std::uint64_t actor = 0;   // the target wearing it
        std::uint64_t piece = 0;   // the item actor
        std::uint64_t unit = 0;    // its model unit holding the material (0: none of its units has it)
        std::vector<std::uint64_t> units;   // the model's units when it was looked up
        std::size_t part = 0;
        std::int16_t material = -1, param = -1;
        bool looked_up = false;
        std::optional<std::uint32_t> put;   // the colour put on last (nothing: the file's own, or never)
        bool ever = false;                  // a colour was put on it
        std::uint64_t at_ms = 0;
        bool seen = false;
        bool said = false;
    };
    std::vector<Piece> pieces_;
    bool names_written_ = false;
    std::uint64_t cell_ = 0;
    std::uint64_t retry_ms_ = 0;   // after a failed call: not before then
    bool said_failed_ = false;
};

}  // namespace CoopGearParams
