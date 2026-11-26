// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/protocol/race.hpp"

#include <string>
#include <type_traits>

namespace coop::protocol {

std::optional<bool> race_of(const std::vector<ModSetting>& settings) {
  for (const ModSetting& s : settings) {
    if (s.mod_id == kRaceModId && s.key == kRaceKey) return s.value == "on";
  }
  return std::nullopt;
}

ModSetting race_setting(bool on) {
  return ModSetting{std::string(kRaceModId), std::string(kRaceKey), on ? "on" : "off"};
}

namespace {
template <typename T>
constexpr bool kWorld =
    // Flags and saves (the host's table, its save for a joiner, who made a change).
    std::is_same_v<T, FlagSet> || std::is_same_v<T, FlagState> || std::is_same_v<T, FlagSnapshot> ||
    std::is_same_v<T, FlagReset> || std::is_same_v<T, FlagOrigin> || std::is_same_v<T, SaveAsk> ||
    std::is_same_v<T, SavePart> ||
    // Enemies, bosses and what players make: owners, copies, hits, hand-overs.
    std::is_same_v<T, ObjectPresent> || std::is_same_v<T, ObjectOwner> || std::is_same_v<T, HandoverAsk> ||
    std::is_same_v<T, HandoverState> || std::is_same_v<T, ObjectSpawn> || std::is_same_v<T, ObjectSample> ||
    std::is_same_v<T, ObjectRemove> || std::is_same_v<T, ObjectGroup> || std::is_same_v<T, Damage> ||
    std::is_same_v<T, BossRoom> || std::is_same_v<T, BossScene> || std::is_same_v<T, BossHeart> ||
    std::is_same_v<T, BossAlive> || std::is_same_v<T, FishLure> ||
    // The world: breakables, grass, signs, drops, doors and switches, platforms, the clock.
    std::is_same_v<T, WorldBreak> || std::is_same_v<T, WorldMove> || std::is_same_v<T, WorldPlace> ||
    std::is_same_v<T, WorldHit> || std::is_same_v<T, SignCut> || std::is_same_v<T, GrassCut> ||
    std::is_same_v<T, DropSpawn> || std::is_same_v<T, DropTake> || std::is_same_v<T, DropAsk> ||
    std::is_same_v<T, DungeonEvent> || std::is_same_v<T, SwitchState> || std::is_same_v<T, PlatformClock> ||
    std::is_same_v<T, PlatformCounter> || std::is_same_v<T, TimeOfDay> ||
    // Things carried between games (bombs, lifted objects) and the warp to a partner.
    std::is_same_v<T, BombTake> || std::is_same_v<T, BombAnswer> || std::is_same_v<T, BombShare> ||
    std::is_same_v<T, LiftAsk> || std::is_same_v<T, LiftAnswer> || std::is_same_v<T, LiftPlace> ||
    std::is_same_v<T, LiftShare> || std::is_same_v<T, JoinPartner>;
}  // namespace

bool race_private(const Message& message) {
  if (std::holds_alternative<LocalSample>(message) || std::holds_alternative<LocalMessage>(message)) {
    const auto inner = local_inner(message);
    return inner && race_private(*inner);
  }
  return std::visit([](const auto& m) { return kWorld<std::decay_t<decltype(m)>>; }, message);
}

}  // namespace coop::protocol
