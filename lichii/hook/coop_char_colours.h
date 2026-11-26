// SPDX-License-Identifier: GPL-3.0-or-later
//
// Character Select's Customize options (hair, eye and clothing colours) on a puppet, done by co-op itself with the
// game's own model calls (docs/puppet.md, "Customize colours on puppets"). No emulator or Hookshot code here: the
// runtime gives the calls and the reads (Game).
//
// What the mod's options are, in the game (read from the mod's MIT source, OoT Character Select v0.2.1
// mods/customize.py; nothing of its code patch is used):
// - Hair: the shader parameters ns_param_color0 and ns_param_color1 of the hair materials (and of Mt_Eyebrow), set
//   through the model unit's own virtual functions (+0xd0 material by name, +0xe0 parameter by name, +0x158 set four
//   floats), and the material's palette entry (the textures behind its samplers' own) put in its first place with the
//   engine's own routine (kSetTexture).
// - Eyes: ns_param_color2 of Mt_Eye_Inner.
// - Clothes: every material whose palette is 7, 8 or 10 long (Clothing Color 1, 2, 3) gets its colour's entry.
// - An option at 0 is the model's own: with put_back the file's own values are set again.
// Why co-op does it (the root cause it fixes): the mod's code dresses every player-like actor with the VIEWER's own
// six options each frame, and its routine for another player's options was never released, so a puppet showed the
// viewer's colours or none of its player's.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <climits>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace CoopCharColours {

// Main offsets (game build e0f3ccfa..., the one the mod and the kit's game are for).
inline constexpr std::uint64_t kSetTexture = 0x34A5910;   // (material object, place, &{view, descriptor slot})
inline constexpr std::uint64_t kModelTypeGot = 0x4CB9D58; // what a player model component's +0xf8 is
// The actor and its model's units.
inline constexpr std::uint64_t kActorComponents = 0x270;  // [actor + 0x270] + 8: the model component
inline constexpr std::uint64_t kUnitMaterialCount = 0xc8, kUnitMaterialByName = 0xd0, kUnitParamByName = 0xe0,
                               kUnitSetFloats = 0x158;     // the unit's virtual functions
inline constexpr std::uint64_t kMatSamplers = 0xA2, kMatTextures = 0xA3, kMatViews = 0x18, kMatSlots = 0x98;
inline constexpr std::uint64_t kObjViews = 0x50;

inline constexpr std::array<const char*, 8> kHairNames = {"Mt_Hairs",    "Mt_HairLong", "Mt_Hair_Front",
                                                          "Mt_Hair1",    "Mt_Hair3",    "Mt_Ponytail",
                                                          "Mt_BodyHair", "Mt_Eyebrow"};
inline constexpr const char* kEyeName = "Mt_Eye_Inner";
inline constexpr std::array<const char*, 3> kParamNames = {"ns_param_color0", "ns_param_color1", "ns_param_color2"};
inline constexpr std::size_t kNames = kHairNames.size() + 1;   // the hair's, then the eye's
inline constexpr std::uint8_t kHairs = 9, kEyes = 7, kCloths = 7;

// The six options of CoopLookShare::kCustomFlags, one nibble each from bit 0.
struct Options {
    std::uint8_t hair = 0, eyes = 0, gear = 0;
    std::array<std::uint8_t, 3> clothes{};
    // 0.11.9: co-op's own eye colour, exact (EyeParam of the look's eyes part); over `eyes` when set.
    std::optional<std::array<float, 3>> eyes_exact;
    // 0.11.9: co-op's own hair colour (HairParam: colour 0, colour 1, the brows'); over `hair` when set. Link models
    // only (a character's stand-in hair keeps its own).
    std::optional<std::array<std::array<float, 3>, 3>> hair_exact;
    static Options Of(std::uint32_t custom);
    bool Colours() const {
        return hair != 0 || eyes != 0 || eyes_exact || hair_exact || clothes[0] != 0 || clothes[1] != 0 ||
               clothes[2] != 0;
    }
};

// 0.11.9, co-op's eye colour (CoopLook's eyes part) as Mt_Eye_Inner's ns_param_color2: each of its three values moves
// the iris texture's hue, saturation and brightness by twice its distance from a half, clamped (the mod's notes,
// mods/customize.py EYE_COLOURS); the common iris is hue 0.608 (kIrisHue) at about kIrisSaturation / kIrisBrightness.
inline constexpr float kIrisHue = 0.608f, kIrisSaturation = 0.6f, kIrisBrightness = 0.55f;
std::array<float, 3> EyeParam(std::uint32_t rgb);
// 0.11.9, co-op's hair colour as the hair materials' ns_param_color0 and 1 and the brows' (linear, as the game's own
// hair values are; the mod's table, mods/customize.py HAIR_COLOURS, pairs a colour with a near one): colour 0 the
// colour, colour 1 a touch lighter, the brows darker (kBrowScale).
inline constexpr float kHairLighter = 1.12f, kBrowScale = 0.6f;
std::array<std::array<float, 3>, 3> HairParam(std::uint32_t rgb);

// The colours as the mod sets them (linear, as the game's own hair has them), 0..1. Nothing for 0 or out of range.
std::optional<std::array<float, 3>> HairColour(std::uint8_t option, int which);   // which: colour 0 or 1
std::optional<std::array<float, 3>> BrowColour(std::uint8_t option);
// `iris_shift`: what the character's iris texture adds to the hue, in 255ths (0 for the common iris).
std::optional<std::array<float, 3>> EyeColour(std::uint8_t option, int iris_shift);

// The place of a material's texture list an option puts first: the palette entry (option + samplers - 1, the
// palette being behind the samplers' own), or 0 (the material's own) for option 0 or one the palette lacks.
std::uint8_t PalettePlace(std::uint8_t samplers, std::uint8_t textures, std::uint8_t option);
// Which clothing colour a material's palette is for, by its length: 0, 1 or 2, or -1 (none, or the hair's).
int ClothRow(std::uint8_t samplers, std::uint8_t textures);

// The game, as the dresser sees it.
struct Game {
    std::uint64_t main_base = 0;
    std::function<bool(std::uint64_t address, void* out, std::size_t size)> read;
    std::function<bool(std::uint64_t address, const void* in, std::size_t size)> write;
    // A game function's call with x0-x7: its x0, or nothing when it could not be made.
    std::function<std::optional<std::uint64_t>(std::uint64_t function, const std::array<std::uint64_t, 8>& x)> call;
};

// Guest memory the dresser keeps its names and arguments in (a puppet's block, coop_puppet.h block::kDress).
inline constexpr std::uint64_t kCellBytes = 0x100;
inline constexpr std::uint64_t kCellNames = 0x00;   // kNames + 3 names of 16 bytes (to 0xc0)
inline constexpr std::uint64_t kCellName = 0xc0;    // {char*} for a by-name call
inline constexpr std::uint64_t kCellFloats = 0xd0;  // four floats
inline constexpr std::uint64_t kCellTexture = 0xe0; // {view, slot}

// One puppet's dressing state (the calc thread's).
struct State {
    std::uint64_t actor = 0;
    std::uint64_t cell = 0;          // kCellBytes of guest memory; 0: none, nothing is done
    bool names_written = false;
    struct Unit {
        std::uint64_t unit = 0;
        std::array<std::int16_t, kNames> material{};         // -1 none
        std::array<std::array<std::int16_t, 3>, kNames> param{};
        std::uint64_t materials = 0;   // the unit's material count (+0xc8) when they were looked up
        bool looked_up = false;
        // npc-customize: what the model has, read once at the look-up (reads only): real hair (Mt_Hairs with the mod's
        // palette, not a stand-in), and the clothing rows its materials' palettes are for (bit 0..2).
        bool hair_palette = false;
        std::uint8_t cloth_rows = 0;
    };
    std::vector<Unit> units;
    // npc-customize-2: the actor's model is there but not of the player model's kind (the mod's own check), so the
    // mod puts none of its options on it: no colourable parts, only the equipment row (Rows).
    bool other_kind = false;
};

struct Result {
    std::size_t calls = 0;
    bool failed = false;   // a call could not be made (the frame stops there)
    bool cut = false;      // the call budget ran out: not all done (the rest next frame)
    std::size_t units = 0; // model units found (0: the model is not there yet)
};

// npc-customize: the rows a character's Customize page offers, from its model (State after a look-up): bit 0 hair,
// 1 eyes, 2..4 Clothing Color 1..3, 5 equipment (every model). kRowsKnown: every unit of the model was looked up (a
// model with no colourable parts, or not of the player model's kind: kRowGear | kRowsKnown, the equipment only); 0:
// not known yet (the page shows every row).
inline constexpr std::uint8_t kRowHair = 1, kRowEyes = 2, kRowCloth1 = 4, kRowGear = 32, kRowsKnown = 0x80;
std::uint8_t Rows(const State& state);
// The equipment component's handles hidden by "Hidden" (the mod's GEAR_HANDLES: every piece but the item in hand,
// the head, the adult's tunics/boots/gauntlets drawn in place of the body, the bracelet and the Stone of Agony) and
// the actor's shown bit (the word and bit the game's set-visible changes).
inline constexpr std::uint64_t kActorEquipment = 0x200, kGearFirst = 0x20, kGearStride = 0x18, kActorFlags = 0x4AC;
inline constexpr std::uint32_t kGearHandles = 0x11F0FD, kGearCount = 21, kShownBit = 0x10000;
// Shows (shown) or hides the actor's equipment pieces; the count of pieces changed.
std::size_t SetGearShown(const Game& game, std::uint64_t actor, bool shown);

// Puts `custom`'s colours on the actor's model. put_back: an option at 0 sets the file's own values again (after
// another hand changed them: the mod's own per-frame code with the viewer's options). `iris_shift` as EyeColour.
// At most `max_calls` calls are made (Result::cut when it stopped there).
Result Dress(const Game& game, State& state, std::uint32_t custom, bool put_back, int iris_shift = 0,
             std::size_t max_calls = SIZE_MAX);
// npc-customize: only looks the model's materials up (no colour put on), for Rows.
Result Learn(const Game& game, State& state, std::size_t max_calls = SIZE_MAX);
// The same with the options given (co-op's exact eyes among them).
Result Dress(const Game& game, State& state, const Options& options, bool put_back, int iris_shift = 0,
             std::size_t max_calls = SIZE_MAX);

// Which puppets are dressed when, each frame (the calc thread).
// - The mod's own per-frame code puts the VIEWER's options (the six Ints of this game) on every player-like actor,
//   so while any of them is a colour every live puppet is dressed every frame, with put_back: a character's puppet
//   in its player's options, a Link puppet (custom 0) in its model's own colours.
// - Otherwise a puppet is dressed when its options change or its actor is new, and again every kAgainMs while it
//   has any (a model not loaded at the first try), with put_back once it was ever dressed.
// - A puppet the mod's own released routine dresses (Director::Dress, verified) is left to it.
// The eyes' and hair's colours of a player's look (CoopLook's parts 18 and 19) go on a Link model only: Customize offers
// them on Link alone, and a character's model has its own (review 0.11.9: a character picked, or a picker of others,
// got your eyes). Every site that dresses a model takes them through this: the puppets, your own Link, the menu's.
// npc-customize (owner 2026-11-23): a character's model takes the character parts instead (CoopLook 20..25, one set
// for every character): ForModel is the one rule at every site that dresses a model.
inline constexpr std::uint8_t kLinkEyesPart = 18, kLinkHairPart = 19, kCharHairPart = 20, kCharEyesPart = 21,
                              kCharCloth1Part = 22, kCharGearPart = 25;
struct ModelColours {
    std::uint32_t custom = 0;                 // Options' nibbles: hair palette, gear, the three clothing palettes
    std::optional<std::uint32_t> eyes{}, hair{};  // exact colours (EyeParam / HairParam)
    bool operator==(const ModelColours&) const = default;
};
// `part(number)`: the look's colour of that part, if set. A Link model: Link's eyes and hair only. A character's: its
// character parts; with none of them set, `legacy_custom` (a 0.11.8 peer's Character Select options, part 62).
ModelColours ForModel(bool link_model, const std::function<std::optional<std::uint32_t>(std::uint8_t)>& part,
                      std::uint32_t legacy_custom = 0);
// The mod's hair palette entry (1..kHairs) nearest a colour, and its clothing palette entry (1..kCloths, the colours of
// `cloth_rgb`, its CLOTH_VALUES in order).
std::uint8_t NearestHair(std::uint32_t rgb);
inline constexpr std::array<std::uint32_t, kCloths> kClothRgb = {0x00C000, 0xFF0000, 0x0000FF, 0x800080,
                                                                 0xFFFF00, 0x000000, 0xFFFFFF};
std::uint8_t NearestCloth(std::uint32_t rgb);

// What a dressed model is, so an age or character switch starts anew at once (its materials are another model's): the
// field player's actor name (Player_Child, Player_Adult) and the character shown (0 Link). Never 0 (0 is "unknown").
inline std::uint64_t ModelKey(std::string_view actor_name, std::uint16_t character) {
    std::uint64_t h = 0xcbf29ce484222325ull;   // (FNV-1a: the same key on every run, for the log and tests)
    for (const char c : actor_name) h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
    h = (h ^ character) * 0x100000001b3ull;
    return h == 0 ? 1 : h;
}

// The pause menu's model of your Link: the game's menu shows the player as its own actor, named "Pouch" + the field
// player's actor name (PouchPlayer_Child for Player_Child, PouchPlayer_Adult for Player_Adult: RSDB ActorInfo's
// PouchPlayer rows; lane 0.11.9: menu "0: PouchPlayer_Child", field "Player_Child"). A model of any other name (a
// picker's character, the menu's tapestry) is not your Link.
inline bool MenuModelOfPlayer(std::string_view menu_name, std::string_view player_name) {
    constexpr std::string_view kMenu = "Pouch";
    if (player_name.empty() || menu_name.size() != kMenu.size() + player_name.size()) return false;
    return menu_name.substr(0, kMenu.size()) == kMenu && menu_name.substr(kMenu.size()) == player_name;
}

struct Target {
    std::uint64_t actor = 0;
    std::uint64_t cell = 0;
    std::uint32_t custom = 0;
    bool mods_routine = false;
    int iris_shift = 0;
    // 0.11.9: the player's eye colour (CoopLook's eyes part, 0xRRGGBB): dressed on any model, Link's too.
    std::optional<std::uint32_t> eyes{};
    // 0.11.9: the player's hair colour (ForModel: Link's hair part on a Link model, the character's on a character's,
    // with the nearest palette entry in custom).
    std::optional<std::uint32_t> hair{};
    // What the puppet is (a hash of its actor's pack name): a puppet made again as another actor at the same
    // address, or in the same block, starts anew (its old model's material numbers are not this one's).
    std::uint64_t model = 0;
    // npc-customize: your own character, its number (its model's rows are kept for the Customize page: OwnRows);
    // learn: look its model up even with nothing to put on.
    bool own = false;
    std::uint16_t character = 0;
    // npc-customize-2: the menu's model of your pick on Character Select's Customize screen (the picture). Its rows are
    // the page's while it is there: the field model may still be the earlier pick's until the menu closes.
    bool picture = false;
};

// The wardrobe's targets are built here, field by name, never by position. A positional Target{...} in the frame
// glue put the model key into `eyes` (the sixth field), so `model` stayed 0: a puppet made again as another actor, and
// your own model after an age or character switch, kept the old model's material numbers (review of the
// dress-colours branch; the puppets' key was broken that way since it was added). These are what the glue calls.
// A puppet (Director::DressView): its model key is its actor's pack name (CoopPuppet_Adult_C007: age and character).
inline Target PuppetTarget(std::uint64_t actor, std::uint64_t cell, bool mods_routine, std::string_view actor_name,
                           const ModelColours& colours) {
    return Target{.actor = actor,
                  .cell = cell,
                  .custom = colours.custom,
                  .mods_routine = mods_routine,
                  .eyes = colours.eyes,
                  .hair = colours.hair,
                  .model = ModelKey(actor_name, 0)};
}
// Your own model (cell 0, `own`) or the pause menu's model of it (its cell; `picture`: Character Select's Customize
// screen's model of your pick), keyed by ModelKey of its actor name and the character shown.
inline Target OwnTarget(std::uint64_t actor, std::uint64_t cell, std::uint64_t model, const ModelColours& colours,
                        std::uint16_t character, bool own, bool picture) {
    return Target{.actor = actor,
                  .cell = cell,
                  .custom = colours.custom,
                  .eyes = colours.eyes,
                  .hair = colours.hair,
                  .model = model,
                  .own = own,
                  .character = character,
                  .picture = picture};
}

class Wardrobe {
public:
    static constexpr std::uint64_t kAgainMs = 3000;
    static constexpr std::size_t kCallsPerFrame = 600;
    // The six Ints of this game (CoopLookShare::kCustomFlags' nibbles): what the mod's code puts on every model.
    // Any thread.
    void SetViewer(std::uint32_t custom) { viewer_.store(custom & 0xFFFFFFu, std::memory_order_relaxed); }
    std::uint32_t Viewer() const { return viewer_.load(std::memory_order_relaxed); }
    // The rows of your own character's model (Rows, with kRowsKnown) and its character number, once looked up; any
    // thread. 0 before.
    std::uint8_t OwnRows(std::uint16_t character) const {
        const std::uint32_t v = own_rows_.load(std::memory_order_relaxed);
        return (v >> 8) == character ? static_cast<std::uint8_t>(v & 0xFF) : std::uint8_t{0};
    }
    // One frame; returns the calls made. `say`: a line for the log (a puppet's first dressing, a failure).
    std::size_t Frame(const Game& game, const std::vector<Target>& targets, std::uint64_t now_ms,
                      const std::function<void(const std::string&)>& say);

private:
    struct Kept {
        State state;
        std::optional<std::uint32_t> dressed;   // the options last put on (nothing: never)
        std::optional<std::uint32_t> dressed_eyes;   // and the exact eye colour
        std::optional<std::uint32_t> dressed_hair;   // and hair colour
        bool ever = false;                      // a colour was ever put on this actor
        std::uint64_t at_ms = 0;
        std::uint64_t model = 0;
        std::uint64_t retry_ms = 0;            // after a failed call: not before then (kAgainMs)
        bool said_failed = false;              // a failure is said once per puppet
        bool seen = false;
        bool gear_hidden = false;              // its equipment hidden by us (shown again when that ends)
    };
    std::vector<Kept> kept_;
    std::size_t first_ = 0;                    // the target dressed first: one further each frame (the budget)
    std::atomic<std::uint32_t> viewer_{0};
    std::atomic<std::uint32_t> own_rows_{0};   // (character << 8) | rows
};

// The character this game shows for its own player (the save's CharacterMenu_SelectedCharacter, 0: Link), as
// coop_hook reads it twice a second. Any thread.
void SetShownCharacter(std::uint16_t number);
std::uint16_t ShownCharacter();
// Character Select's code and data are in this game (its saved Int CharacterMenu_SelectedCharacter exists; the hook
// reads it twice a second). Any thread. Row 207 (0.11.9): co-op then never opens its own Customize on System, whose
// live picture forces the menu's Link model, which that mod's code patches; Customize is in its screen.
void SetModPresent(bool present);
bool ModPresent();
// Your own pick as the save holds it (CharacterMenu_SelectedCharacter; none: no Character Select), read every 30th
// frame of the module, co-op on or off (coop_hook.cpp OwnCharacterFrame): sets ModPresent and ShownCharacter (a
// number from 1 to `max`; anything else is Link, 0). Returns the character shown.
std::uint16_t NoteOwnPick(std::optional<std::int32_t> pick, std::uint16_t max);

// The one wardrobe of the game (the runtime's frame and coop_hook's reading of the six Ints).
Wardrobe& TheWardrobe();

}  // namespace CoopCharColours
