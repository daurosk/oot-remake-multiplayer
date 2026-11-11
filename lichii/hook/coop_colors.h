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
//
// Where: on the tunic, as Ship of Harkinian's Anchor co-op colours the other
// players' tunics. An adult's tunic (Armor_Adult_Normal, _Goron, _Zora) is an
// actor of its own, worn through the EquipmentUser's upper armor link, so its
// model takes the colour alone. A child's tunic is part of the body model;
// a child puppet is tinted whole until a per-material colour is found
// (docs/puppet.md, "Colour", Not done).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
// The tunic: [[actor + 0x270] + 0x200] is the EquipmentUser (coop_puppet.h);
// its armor links at + 0xe0, 0x18 each, are {+0x08 holder, +0x10 id}; link 1
// is the upper body (kind 8, the tunic). The piece is [holder + 0x40].
inline constexpr std::uint64_t kPartsEquipmentUser = 0x200;
inline constexpr std::uint64_t kArmorLinks = 0xe0;
inline constexpr std::uint64_t kLinkStride = 0x18;
inline constexpr std::uint64_t kTunicLink = 1;
inline constexpr std::uint64_t kLinkHolder = 0x08;
inline constexpr std::uint64_t kLinkId = 0x10;
inline constexpr std::uint64_t kHolderActor = 0x40;
}  // namespace addr

using Rgba = std::array<float, 4>;

// The default strength (alpha) of a tint. LICHII_COOP_PUPPET_TINT overrides it.
inline constexpr float kDefaultStrength = 0.6f;

// A colour name (case does not matter) or #RRGGBB / RRGGBB, any value.
// Nothing for anything else, and for "" (no colour chosen).
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
// StrengthFrom the process's LICHII_COOP_PUPPET_TINT.
float StrengthFromEnvironment();

// --- Who has which colour (any thread). The one place to ask: the puppets,
// and the HUD for name tags.

// Our own choice (coop-options.ini `color`), set once at start.
void SetOwn(std::optional<std::uint32_t> rgb);
std::optional<std::uint32_t> Own();
// A remote player's choice from PLAYER_EQUIP; unset: none chosen.
void SetChosen(std::uint8_t player, std::optional<std::uint32_t> rgb);
std::optional<std::uint32_t> Chosen(std::uint8_t player);
void ForgetAll();  // disconnected
// The colour player `player` shows in: their choice, else their number's
// (coop::protocol::slot_color). `self`: our own number, so our own choice
// is used for it.
std::uint32_t ForPlayer(std::uint8_t player, std::uint8_t self = 0);

// --- Guest memory.

// The actor's model, or 0 when the actor has none (yet).
std::uint64_t ModelOf(CoopPuppet::Guest& guest, std::uint64_t actor);
// The tunic piece the actor wears (adult), or 0 when it wears none.
std::uint64_t TunicOf(CoopPuppet::Guest& guest, std::uint64_t actor);

enum class Applied { no_model, same, written };
// Writes `tint` into the actor's model as 0x2e1d200 does: the four floats,
// and the dirty bit, only when they differ from what is there.
Applied Apply(CoopPuppet::Guest& guest, std::uint64_t actor, const Rgba& tint);

// What one puppet was painted last; kept by the director per puppet.
struct Painted {
    std::uint64_t actor = 0;   // the puppet
    std::uint64_t target = 0;  // the actor that took the colour (its tunic, or itself)
    std::uint32_t color = 0;
};
// One game frame for one live puppet: the colour on its tunic if it wears
// one, else on its whole body. When the tunic appears after the body was
// painted, the body's colour is taken off once. Returns a log line when what
// was painted changed, "" otherwise.
std::string Paint(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t color,
                  float strength, Painted& painted);

}  // namespace CoopColors
