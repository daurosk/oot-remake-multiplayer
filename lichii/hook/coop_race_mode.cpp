// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_race_mode.h"

#include <atomic>

#include "coop/protocol/race.hpp"

namespace CoopRaceMode {
namespace {
std::atomic<bool> g_choice{false};
std::atomic<bool> g_in_force{false};
std::atomic<bool> g_told{false};
}  // namespace

void SetChoice(bool on) { g_choice.store(on); }
bool Choice() { return g_choice.load(); }

void OnWelcomed(bool hosting) {
    g_told.store(false);
    g_in_force.store(false);
    if (hosting) OnHosting();
}

void OnHosting() {
    // A host after a migration keeps the mode its old host told it.
    if (!g_told.exchange(true)) g_in_force.store(g_choice.load());
}

void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings) {
    if (const auto race = coop::protocol::race_of(settings)) {
        g_in_force.store(*race);
        g_told.store(true);
    }
}

bool InForce() { return g_in_force.load(std::memory_order_relaxed); }

coop::protocol::ModSetting AsSetting() { return coop::protocol::race_setting(InForce()); }

}  // namespace CoopRaceMode
