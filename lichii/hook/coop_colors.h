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
// Where: the whole puppet. The body model takes the colour, and so does
// every armor piece it wears (an adult's tunic, Armor_Adult_Normal, _Goron,
// _Zora, is an actor of its own, worn through the EquipmentUser's armor
// links, as are boots, masks and gauntlets). So an adult is tinted whole, as
// a child is (Caleb 2026-11-12: "for adult link the whole model needs the
// color not just tunic").
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
// Worn armor: [[actor + 0x270] + 0x200] is the EquipmentUser (coop_puppet.h);
// its kArmorLinkCount armor links at + 0xe0, 0x18 each, are {+0x08 holder,
// +0x10 id} (link 1 is the tunic). The piece is [holder + 0x40].
inline constexpr std::uint64_t kPartsEquipmentUser = 0x200;
inline constexpr std::uint64_t kArmorLinks = 0xe0;
inline constexpr std::uint64_t kLinkStride = 0x18;
inline constexpr std::uint64_t kTunicLink = 1;
inline constexpr std::uint32_t kArmorLinkCount = 13;
inline constexpr std::uint64_t kLinkHolder = 0x08;
inline constexpr std::uint64_t kLinkId = 0x10;
inline constexpr std::uint64_t kHolderActor = 0x40;
}  // namespace addr

using Rgba = std::array<float, 4>;

// The default strength (alpha) of a tint. LICHII_COOP_PUPPET_TINT overrides it.
inline constexpr float kDefaultStrength = 0.0f;  // off: the cloth colours do it

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
// The armor piece in the actor's armor link `link`, or 0 when it wears none.
std::uint64_t PieceOf(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t link);
// The tunic piece the actor wears (adult), or 0 when it wears none.
inline std::uint64_t TunicOf(CoopPuppet::Guest& guest, std::uint64_t actor) {
    return PieceOf(guest, actor, static_cast<std::uint32_t>(addr::kTunicLink));
}

enum class Applied { no_model, same, written };
// Writes `tint` into the actor's model as 0x2e1d200 does: the four floats,
// and the dirty bit, only when they differ from what is there.
Applied Apply(CoopPuppet::Guest& guest, std::uint64_t actor, const Rgba& tint);

// What one puppet was painted last; kept by the director per puppet.
struct Painted {
    std::uint64_t actor = 0;   // the puppet
    std::uint32_t color = 0;
    std::uint32_t models = 0;  // how many models took it: the body and its pieces
};
// One game frame for one live puppet: the colour on its body and on every
// armor piece it wears. Returns a log line when the colour or the number of
// painted models changed, "" otherwise.
std::string Paint(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t color,
                  float strength, Painted& painted);


// --- Cloth colours (Caleb 2026-11-12: "All colors should make the tunic and
// cap what the user sets it to, but lighter shades for those areas", the
// undershirt, sleeves and leggings; skin, hair, leather and gear keep theirs).
//
// Per material, through the game's own material parameters. The model set is
// [[[actor + 0x270] + 8] + 0x18] (0x974de0; the holder's virtual +0xf8 is
// 0x9d7160, which returns +0x18); +0x20 the unit count (s32), +0x28 the units
// (8 bytes each, the unit at [entry]). A unit's virtuals, as the game calls
// them (0x358f200, 0x358f290, 0x212e48, 0x68b148):
//   +0xc8 (unit) -> material count
//   +0xd0 (unit, const char** name) -> material index, -1 none
//   +0xe0 (unit, material, const char** name) -> parameter index, -1 none
//   +0x158 (unit, material, parameter, const float rgba[4]) sets a colour
// The colour parameters the game sets by name are ns_param_color0 to 5.
namespace cloth {
inline constexpr std::uint64_t kPartsHolder = 0x8;
inline constexpr std::uint64_t kHolderGetSet = 0xf8;   // virtual
inline constexpr std::uint64_t kHolderGetSetDefault = 0x9d7160;
inline constexpr std::uint64_t kHolderSet = 0x18;
inline constexpr std::uint64_t kSetCount = 0x20;
inline constexpr std::uint64_t kSetUnits = 0x28;
inline constexpr std::uint64_t kFindMaterial = 0xd0;
inline constexpr std::uint64_t kFindParam = 0xe0;
inline constexpr std::uint64_t kSetColor = 0x158;
inline constexpr std::uint32_t kMaxUnits = 4;
// Inside the unit, as +0x158 (0x3595f60) reads it: the material slot is
// [[unit + 0x110] + 8 m] when that array is there, else [unit + 0x108] +
// 0x80 m. The slot's resource [slot], whose [[[res + 0x10]] + 0x20] is the
// shader's parameter table (0x18 bytes each, +0x10 the u16 offset); the
// values are at [slot + 0x48] + offset. +0x158 copies 16 bytes there and
// marks the parameter dirty (bits at [slot + 0x10], byte [slot + 0x2c]).
inline constexpr std::uint64_t kUnitSlotArray = 0x110;
inline constexpr std::uint64_t kUnitSlots = 0x108;
inline constexpr std::uint64_t kSlotStride = 0x80;
inline constexpr std::uint64_t kSlotValues = 0x48;
inline constexpr std::uint64_t kResShader = 0x10;
inline constexpr std::uint64_t kShaderParams = 0x20;
inline constexpr std::uint64_t kParamStride = 0x18;
inline constexpr std::uint64_t kParamOffset = 0x10;
}  // namespace cloth

// Which materials take the colour, and which parameter. LICHII_COOP_COLOR_STRONG,
// _LIGHT (comma lists of material names) and _PARAM override the defaults.
struct ClothSettings {
    std::vector<std::string> strong{"Mt_Angin", "Mt_Cloth"};
    std::vector<std::string> light{"Mt_Inner", "Mt_Cloth_Pants"};
    std::string param = "ns_param_color0";
    float lighten = 0.55f;   // how far the light shade goes toward white
    bool probe = false;      // LICHII_COOP_DEBUG_COLOR: log every colour parameter found
    // LICHII_COOP_COLOR_SWEEP: debug, every 5 s the next of ns_param_color1 to 5 takes the
    // colour on the strong materials and the others go back to white (finds the one shown).
    bool sweep = false;
    static ClothSettings FromEnvironment();
};
// Other materials the probe looks at (not painted).
inline constexpr std::array<const char*, 10> kProbeMaterials{
    "Mt_Angin_Acs", "Mt_Vest", "Mt_Feather", "Mt_Hairs", "Mt_Face", "Mt_Cloth_Parts",
    "Mt_PantsParts", "Mt_Leather", "Mt_Leater_Upper", "Mt_Skin"};

// A material parameter's 4 floats as they are now, read from memory.
std::optional<Rgba> ReadParam(CoopPuppet::Guest& guest, std::uint64_t unit, std::int32_t material,
                              std::int32_t param);
// Debug: up to `count` parameters of a material from `first`: index, the
// name if the table entry starts with one, the u16 offset and the 4 floats.
std::string DumpParams(CoopPuppet::Guest& guest, std::uint64_t unit, std::int32_t material,
                       std::int32_t first, std::int32_t count);

// The colour as the material takes it: linear floats, alpha 1. `lighten` 0
// is the colour itself, 1 white.
Rgba ClothColor(std::uint32_t rgb, float lighten);

// One game call the planner wants (the director's cave runs it).
struct ClothCall {
    std::uint64_t function = 0;
    std::array<std::uint64_t, 4> x{};
};

// Paints one puppet's cloth, one call at a time. Scratch: 0x60 bytes of guest
// memory the planner may write (a name, its pointer, a colour).
class Cloth {
public:
    // The next call, or nothing (done, or nothing to do yet). Resets itself
    // for a new actor; paints again when the colour changes. `line` gets a
    // log line when a pass ends without a call.
    std::optional<ClothCall> Next(CoopPuppet::Guest& guest, std::uint64_t main_base,
                                  std::uint64_t actor, std::uint32_t color,
                                  const ClothSettings& settings, std::uint64_t scratch,
                                  std::string& line, std::uint64_t now_ms = 0);
    // The call's x0. Returns a log line when a pass ends, "" otherwise.
    std::string Done(std::uint64_t result, const ClothSettings& settings);
    bool Waiting() const { return waiting_; }

private:
    struct Unit {
        std::uint64_t object = 0;
        std::uint64_t vtable = 0;
    };
    struct Found {
        std::uint32_t unit = 0;
        std::string material;
        std::int32_t index = -1;
        std::vector<std::int32_t> params;  // per name in ParamNames
        bool light = false;
        bool painted = false;
    };
    enum class Step { units, material, param, paint, done };
    std::vector<std::string> MaterialNames(const ClothSettings& settings) const;
    std::vector<std::string> ParamNames(const ClothSettings& settings) const;
    std::string Report(const ClothSettings& settings) const;

    std::uint64_t actor_ = 0;
    std::uint32_t color_ = 0;
    Step step_ = Step::units;
    std::vector<Unit> units_;
    std::vector<Found> found_;
    std::size_t unit_i_ = 0, name_i_ = 0, found_i_ = 0, param_i_ = 0;
    std::uint32_t paint_color_ = 0;
    std::size_t pk_ = 0;          // the parameter (ParamNames index) being painted
    int sweep_ = 1;
    std::uint64_t sweep_ms_ = 0;
    std::vector<std::size_t> Kinds(const ClothSettings& settings) const;
    bool waiting_ = false;
};

}  // namespace CoopColors
