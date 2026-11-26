// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_switch_stand_mode.h"

#include <atomic>
#include <string>

namespace CoopSwitchStandMode {
namespace {
std::atomic<bool> g_choice{true};
std::atomic<bool> g_in_force{true};
std::atomic<bool> g_told{false};
}  // namespace

void SetChoice(bool on) { g_choice.store(on); }
bool Choice() { return g_choice.load(); }

void OnWelcomed(bool hosting) {
    g_told.store(false);
    g_in_force.store(true);  // (an older host sends nothing: on)
    if (hosting) OnHosting();
}

void OnHosting() {
    // A host after a migration keeps the setting its old host told it.
    if (!g_told.exchange(true)) g_in_force.store(g_choice.load());
}

void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings) {
    if (const auto on = Of(settings)) {
        g_in_force.store(*on);
        g_told.store(true);
    }
}

bool InForce() { return g_in_force.load(std::memory_order_relaxed); }

coop::protocol::ModSetting AsSetting() {
    return coop::protocol::ModSetting{std::string(kModId), std::string(kKey), InForce() ? "on" : "off"};
}

std::optional<bool> Of(const std::vector<coop::protocol::ModSetting>& settings) {
    for (const auto& s : settings) {
        if (s.mod_id == kModId && s.key == kKey) return s.value != "off";
    }
    return std::nullopt;
}

}  // namespace CoopSwitchStandMode
