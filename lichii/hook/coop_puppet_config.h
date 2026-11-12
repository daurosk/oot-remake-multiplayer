// SPDX-License-Identifier: GPL-3.0-or-later
// The puppets' settings from the LICHII_COOP_* variables, one copy for both
// runtimes: the emulator's cave (coop_svc.cpp) and Hookshot's module
// (hookshot/src/coop_hookshot.cpp) call Configure() when the puppets start.
#pragma once

#include <functional>
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

// The settings the variables ask for. pack_green answers whether the puppet
// pack has both green puppets; it is asked only when the player's own Link
// wears another colour. pack_tunics (optional): a pack without tunic puppets
// turns the tunics off, so every puppet is the plain one the pack has. Each
// line for the log goes into *log.
CoopPuppet::PlannerSettings Settings(const EnvFn& env, const std::function<bool()>& pack_green,
                                     std::vector<std::string>* log, const PackTunicsFn& pack_tunics = {});

#ifdef LICHII_COOP
// Settings() from the process's variables and the puppet pack under
// LoadDir/<title id>, then CoopPuppet::Configure.
void Configure();
#endif

}  // namespace CoopPuppetConfig
