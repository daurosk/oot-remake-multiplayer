// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 248: another player's lit Deku stick burns on their puppet too, as a
// picture only (docs/puppet.md, "A lit Deku stick"). No emulator code here:
// the puppet director (coop_puppet.h) asks this for its calls and passes the
// results back.
//
// In the owner's game the stick burns through the game's chemistry: its
// chemical body (ChemicalParam Weapon_Chemical_Wood) catches fire at a torch,
// lights what it touches, burns out after FixedBurningDuration and breaks.
// That stays the owner's: a puppet's stick given a burning chemical body
// would be a second fire in the follower's world (torches, webs, grass lit
// twice). So the follower plays only what the chemistry shows: the stick's
// own effects from its ELink user Item_DekuStick (read from the romfs's
// ELink2 file, elink2.Product.110.belnk): `Chemical_FireStart` (the flash as
// it catches) and `Chemical_Fire` (the flame, heat haze, smoke, and embers
// while it swings). No chemical body is touched.
//
// The game's own calls (main offsets; the same xlink2 code CoopPuppetSound
// uses, coop_puppet_sound.h):
// - start: 0x43db00(accessor, &asset, key) finds the key, then
//   0x440630(user, &asset, &handle, 0) plays it (OneShotXLinkSearchAndEmit).
// - stop: 0x37416c0(user, &handle, key) finds what the user plays for that
//   key and fills an 8-byte handle (u8 0xff, u16 0xffff at +2 before), then
//   0x7cc120(&handle) kills it. This is OneShotXLinkFadeByKey's run
//   (main+0x295af30) with its kill switch set: read from the game's code.
// The sender (coop::host::PlayerReader::read_lit) sees the same key playing
// on its own stick's user, and PLAYER_EQUIP component 17 (`lit`) says so.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace CoopPuppet {
class Guest;
struct Call;
}  // namespace CoopPuppet

namespace CoopPuppetFlame {

namespace addr {
inline constexpr std::uint64_t kFindPlaying = 0x37416c0;  // (user, &handle, key) -> bool
inline constexpr std::uint64_t kKill = 0x7cc120;          // (&handle)
inline constexpr std::uint64_t kSearchAsset = 0x43db00;   // (accessor, &asset, key) -> bool
inline constexpr std::uint64_t kEmit = 0x440630;          // (user, &asset, &handle, 0)
}  // namespace addr

inline constexpr char kStartKey[] = "Chemical_FireStart";
inline constexpr char kFireKey[] = "Chemical_Fire";

// Room in the puppet's block (CoopPuppet::block, after coop_link_sound.h's handles).
namespace block {
inline constexpr std::uint64_t kText = 0x740;      // the key, 0x20 bytes
inline constexpr std::uint64_t kTextCapacity = 0x20;
inline constexpr std::uint64_t kAsset = 0x760;     // 8 bytes
inline constexpr std::uint64_t kHandle = 0x768;    // 0x10 bytes
inline constexpr std::uint64_t kEnd = 0x778;
}  // namespace block

struct State {
    enum class Step { none, start_search, start_emit, fire_search, fire_emit, find, kill };
    Step pending = Step::none;  // the call made, waiting for Finish
    Step next = Step::none;     // the call to make next (a start or a stop under way)
    int slot = -1;              // the equipment slot of the piece it burns on
    std::uint64_t actor = 0;    // that piece
    bool on = false;            // Chemical_Fire was started on `actor`
    std::uint64_t refused = 0;  // a piece whose user has no Chemical_Fire: not asked again
    unsigned notes = 0;         // failure lines said (capped)
};

// The piece actor in the puppet's equipment slot `slot`; 0 when there is none
// or it is gone (the same test of the link as PlayerReader::read_drawn).
std::uint64_t PieceIn(CoopPuppet::Guest& guest, std::uint64_t puppet, int slot);

// The calc thread, once the puppet is live. `lit` and `drawn` are the
// player's PLAYER_EQUIP bits (unknown: 0); `shown` false while the puppet is
// hidden. The next call, or nothing.
std::optional<CoopPuppet::Call> Next(State& s, std::uint8_t lit, std::uint8_t drawn, bool shown,
                                     CoopPuppet::Guest& guest, std::uint64_t main_base,
                                     std::uint64_t puppet, std::uint64_t block, const std::string& who,
                                     const std::function<void(const std::string&)>& say);
// The call Next gave was made; `result` is its x0.
void Finish(State& s, std::uint64_t result, const std::string& who,
            const std::function<void(const std::string&)>& say);
// The call Next gave was not made (the cave's own limit): asked again next frame.
void Dropped(State& s);
// The puppet was made again or deleted: its pieces (and their effects) went with it.
void Forget(State& s);

}  // namespace CoopPuppetFlame
