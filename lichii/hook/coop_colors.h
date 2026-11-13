// Player colours (docs/puppet.md, "Colour"): each player's puppet is tinted in
// that player's colour, so players tell each other apart. No emulator code
// here, so it is tested on its own.
//
// The tint is the game's own hit flash. The AI node
// ExecuteSetAndCountModelDamageColor (vtable 0x49d4a30, run 0x283f5b0) takes
// the actor's model with 0xc71c20 (actor -> [[actor + 0x270] + 0x260]) and
// sets its colour with 0x2e1d200(model, float rgba[4]): that function only
// copies the four floats to model + 0x20 and, when they changed, ors 4 into
// the model's dirty bits at + 0x120. The node writes (1, 0, 0, 1) for the red
// flash (alpha 0.25 for a softer one) and (0, 0, 0, 0) to end it, so alpha is
// the strength. Being a plain store, the director does it from the SVC
// handler, with no call into the game.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopPuppet {
class Guest;
}

namespace CoopColors {

namespace addr {
inline constexpr std::uint64_t kActorParts = 0x270;    // actor -> parts
inline constexpr std::uint64_t kPartsModel = 0x260;    // parts -> model (0xc71c20)
inline constexpr std::uint64_t kModelColor = 0x20;     // 4 x f32: r, g, b, strength
inline constexpr std::uint64_t kModelDirty = 0x120;    // u32 bits
inline constexpr std::uint32_t kDirtyColor = 0x4;      // 0x2e1d200 sets it
}  // namespace addr

using Rgba = std::array<float, 4>;

// The default strength (alpha) of a tint: enough to tell players apart and
// keep the tunic's own colours readable. LICHII_COOP_PUPPET_TINT overrides it.
inline constexpr float kDefaultStrength = 0.45f;

// A colour name (case does not matter) or #RRGGBB / RRGGBB. Nothing for
// anything else, and for "" (no colour chosen).
std::optional<std::uint32_t> Parse(std::string_view text);
// The names Parse knows, comma-separated, for error messages and the docs.
const char* Names();
// "#RRGGBB".
std::string Text(std::uint32_t rgb);
// The tint written for a colour: its channels as 0..1 and `strength` as alpha
// (clamped to 0..1). Strength 0 is no tint at all.
Rgba TintOf(std::uint32_t rgb, float strength);
// LICHII_COOP_PUPPET_TINT as a strength: a number 0 to 1, "0" or "off" turns
// the colours off. Unset or bad: kDefaultStrength.
float StrengthFrom(const char* text);

// The tunic a colour gets (docs/puppet.md, "Colour"): the preset of the
// puppet pack nearest it, as the actor suffix "Red", "Blue", ... (actor
// CoopPuppet_<Age>_<Suffix>, tools/puppet_pack/tunic.py COLORS), or "" for
// green, the game's own tunic (the plain CoopPuppet_<Age>).
std::string_view TunicOf(std::uint32_t rgb);
// Every suffix TunicOf returns besides "".
const std::vector<std::string_view>& Tunics();

// The actor's model, or 0 when the actor has none (yet).
std::uint64_t ModelOf(CoopPuppet::Guest& guest, std::uint64_t actor);

enum class Applied { no_model, same, written };
// Writes `tint` into the actor's model as 0x2e1d200 does: the four floats,
// and the dirty bit, only when they differ from what is there.
Applied Apply(CoopPuppet::Guest& guest, std::uint64_t actor, const Rgba& tint);

}  // namespace CoopColors
