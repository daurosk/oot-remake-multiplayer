// SPDX-License-Identifier: GPL-3.0-or-later
// The puppets' settings from the LICHII_COOP_* variables, one copy for both
// runtimes: the emulator's cave (coop_svc.cpp) and Hookshot's module
// (hookshot/src/coop_hookshot.cpp) call Configure() when the puppets start.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_puppet.h"

namespace CoopPuppetConfig {

// One variable's value, "" when it is not set.
using EnvFn = std::function<std::string(const char*)>;

// Whether the puppet pack has tunic puppets (CoopPuppet::PackHasTunicPuppets);
// std::nullopt when no pack manifest was found (nothing to judge by).
using PackTunicsFn = std::function<std::optional<bool>()>;
// The pack's per-slot item copies (CoopPuppet::PackLookItems of its manifest).
using PackLookFn = std::function<CoopPuppet::LookItems()>;

// The settings the variables ask for. pack_green answers whether the puppet
// pack has both green puppets; asked once, always (then no puppet is the
// plain one, whose model is your own Link's copy). pack_tunics (optional): a pack without tunic puppets
// turns the tunics off, so every puppet is the plain one the pack has.
// pack_look (optional): the pack's item copies for gear colours on the
// puppets (LICHII_COOP_LOOK_ITEMS=0: none). Each line for the log goes into
// *log.
CoopPuppet::PlannerSettings Settings(const EnvFn& env, const std::function<bool()>& pack_green,
                                     std::vector<std::string>* log, const PackTunicsFn& pack_tunics = {},
                                     const PackLookFn& pack_look = {});

#ifdef LICHII_COOP
// Settings() from the process's variables and the puppet pack under
// LoadDir/<title id>, then CoopPuppet::Configure. perceived_safe: false when
// the runtime cannot make a perceived puppet's null reads give 0 (issue 150,
// hookshot/src/coop_null_page.h Armable): a pack with perception then makes
// every puppet its quiet copy.
void Configure(bool perceived_safe = true);
// A coop_puppet.json is under LoadDir/<title id> (the puppet pack is installed).
bool HasPack();
std::map<std::uint16_t, std::uint16_t> CharacterFiles();
// Rows 239/240/241: CharacterFiles() with a pack; without one, an empty map when Character Select has no NPC model
// file at all (its UI-only part), nothing when it has some (unknown: picks are kept). coop_puppet_config.cpp.
std::optional<std::map<std::uint16_t, std::uint16_t>> KnownCharacterFiles();
// The Character Select mod's characters.json text, or empty (coop_char_ages.h ParseCharacters reads it).
std::string CharacterListText();
#endif

}  // namespace CoopPuppetConfig
