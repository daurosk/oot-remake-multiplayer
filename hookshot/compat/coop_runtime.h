// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module's runtime, behind the compat headers (hookshot/compat).
// The emulator's API slice that lichii/hook uses is answered from here, so the
// hook files compile unchanged for Hookshot (one copy of the co-op code).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace CoopRuntime {

// Set by the module's start (hookshot/src/coop_hookshot.cpp); false or zero
// before.
bool Running();                     // the game is up (the bridge's table is set)
std::uint64_t MainBase();
const std::string& MainBuildHex();  // as Hookshot gives it (32 hex digits)
std::filesystem::path GameDir();    // the folder of hookshot.exe
bool Valid(std::uint64_t address, std::size_t size);
bool Read(std::uint64_t address, void* out, std::size_t size);
bool Write(std::uint64_t address, const void* in, std::size_t size);
bool Cas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t& seen);
void Log(const std::string& line);
// Player 1's buttons as the game last got them (Switch NpadButton bits, the
// emulator's GetNpadButtons().raw). False when Hookshot's bridge is older
// than HosCoopApi::pad_buttons.
bool PadButtons(std::uint64_t& out);

}  // namespace CoopRuntime
