#pragma once

// Row 307: our Link's AS variables (the 'Move' command's walk/run blend reads them), read from guest memory with
// no game call. A port of the game's own code (main, ARM64; reports/puppet-run-307-5.md):
//   the AS instance (actor +0x270 > +0x40 > +0x18 > +0x18 > +0x40), vtable main+0x4c29818;
//   name -> handle: vt+0x2f8 = 0x37b9e80 (floats), vt+0x300 = 0x37ba140 (bools), a search of the AS resource's
//   name table; the value's address: vt+0x138 = 0x37ba6f0 (floats), vt+0x140 = 0x37ba7a0 (bools), which vt+0x1e0 /
//   vt+0x1e8 read. Each step checks that the vtable entry is that default function; a class that overrides it gives
//   nothing (its code is not ported).

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "coop/host/game_memory.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

namespace as_addr {
// main+ offsets of the game's functions named above.
inline constexpr std::uint64_t kF32Handle = 0x37b9e80;
inline constexpr std::uint64_t kBoolHandle = 0x37ba140;
inline constexpr std::uint64_t kF32HandleTable = 0x37bd980;   // the resource's vt+0x130 default
inline constexpr std::uint64_t kBoolHandleTable = 0x37bdbd0;  // the resource's vt+0x170 default
inline constexpr std::uint64_t kF32Address = 0x37ba6f0;
inline constexpr std::uint64_t kBoolAddress = 0x37ba7a0;
inline constexpr std::uint64_t kF32Get = 0x37bb3e0;
inline constexpr std::uint64_t kBoolGet = 0x37bb4c0;
// AS vtable slots (the call sites: 0x2e72e20 name lookups, 0x2e7584c f32 set, 0x2e763b0 bool set,
// 0x2e73684 f32 get, 0x2e75fc8 bool get).
inline constexpr std::uint64_t kVtF32Set = 0x80;
inline constexpr std::uint64_t kVtBoolSet = 0x88;
inline constexpr std::uint64_t kVtF32Address = 0x138;
inline constexpr std::uint64_t kVtBoolAddress = 0x140;
inline constexpr std::uint64_t kVtF32Get = 0x1e0;
inline constexpr std::uint64_t kVtBoolGet = 0x1e8;
inline constexpr std::uint64_t kVtF32Handle = 0x2f8;
inline constexpr std::uint64_t kVtBoolHandle = 0x300;
inline constexpr std::uint64_t kResourceVtF32Table = 0x130;
inline constexpr std::uint64_t kResourceVtBoolTable = 0x170;
// The AS from the controller (the actor's AS walk, addresses.hpp kAsInnerController).
inline constexpr std::uint64_t kControllerAs = 0x40;
}  // namespace as_addr

// The five names PLAYER_SAMPLE's AsVars carries (the game's own name tables: f32 0x3ee5990 + offsets at +0x6a0,
// bool 0x3ee5400 + offsets at +0x474; decoded in reports/puppet-run-307-4.md).
inline constexpr std::array<std::string_view, 3> kAsF32Names{"LeftStickLength", "LinearVelXZ", "XZSpeed"};
inline constexpr std::array<std::string_view, 2> kAsBoolNames{"IsLeftStickInput", "IsDash"};
inline constexpr std::array<std::uint8_t, 2> kAsBoolBits{protocol::kAsVarLeftStickInput, protocol::kAsVarDash};

// Handles found for one AS, kept from frame to frame: the search reads the name table.
struct AsVarsMemo {
  std::uint64_t as = 0;        // the AS the handles are for; 0: none yet
  std::uint64_t resource = 0;  // and its resource ([AS + 0xf0], whose name table they index)
  std::array<std::int32_t, 3> f32{-1, -1, -1};
  std::array<std::int32_t, 2> boolean{-1, -1};
};

// The handle the game's vt+0x2f8 (floats) or vt+0x300 (bools) would give for `name`; -1: not in the table.
// Nothing: a read failed, or the AS's class overrides the lookup.
std::optional<std::int32_t> as_var_handle(GameMemory& memory, std::uint64_t as, std::string_view name, bool boolean);
// The value vt+0x1e0 / vt+0x1e8 would give for a handle (>= 0). Nothing: a read failed or an override.
std::optional<float> as_f32(GameMemory& memory, std::uint64_t as, std::int32_t handle);
std::optional<bool> as_bool(GameMemory& memory, std::uint64_t as, std::int32_t handle);
// All five, for the AS at `as`. A missing name sends 0 (false); nothing when a read failed or no name is found.
std::optional<protocol::AsVars> read_as_vars(GameMemory& memory, std::uint64_t as, AsVarsMemo& memo);

}  // namespace coop::host
